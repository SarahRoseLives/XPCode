package main

import (
	"bufio"
	"bytes"
	"context"
	"encoding/json"
	"flag"
	"fmt"
	"io"
	"log"
	"net/http"
	"os"
	"sort"
	"strings"
	"sync"
	"time"
)

type toolCall struct {
	ID       string `json:"id"`
	Type     string `json:"type"`
	Function struct {
		Name      string `json:"name"`
		Arguments string `json:"arguments"`
	} `json:"function"`
}

type oaiMessage struct {
	Role       string     `json:"role"`
	Content    *string    `json:"content,omitempty"`
	ToolCalls  []toolCall `json:"tool_calls,omitempty"`
	ToolCallID string     `json:"tool_call_id,omitempty"`
}

type upstreamRequest struct {
	Model    string           `json:"model"`
	Messages []oaiMessage     `json:"messages"`
	Stream   bool             `json:"stream"`
	Tools    []map[string]any `json:"tools,omitempty"`
}

type clientRequest struct {
	Session     string       `json:"session"`
	Model       string       `json:"model"`
	Workdir     string       `json:"workdir"`
	Message     string       `json:"message"`
	ToolResults []toolResult `json:"tool_results"`
}

type toolResult struct {
	ID      string `json:"id"`
	Content string `json:"content"`
}

type streamChunk struct {
	Choices []struct {
		Delta struct {
			Content   string `json:"content"`
			ToolCalls []struct {
				Index    int    `json:"index"`
				ID       string `json:"id"`
				Function struct {
					Name      string `json:"name"`
					Arguments string `json:"arguments"`
				} `json:"function"`
			} `json:"tool_calls"`
		} `json:"delta"`
	} `json:"choices"`
}

type session struct {
	mu       sync.Mutex
	messages []oaiMessage
	pending  bool
	updated  time.Time
}

type kvp struct {
	k string
	v string
}

var (
	listenAddr = flag.String("listen", "0.0.0.0:8080", "address to listen on")
	upstream   = flag.String("upstream", "https://api.deepseek.com/chat/completions", "OpenAI-compatible chat completions URL")
	apiKey     = flag.String("api-key", "", "API key (defaults to DEEPSEEK_API_KEY or OPENAI_API_KEY)")
	apiKeyFile = flag.String("api-key-file", "", "read the API key from this file")
	modelFlag  = flag.String("model", "", "force this model for every request")
	systemFlag = flag.String("system", "You are XPCode, a coding assistant running inside Windows XP. Use the provided tools to inspect files and run commands on the XP machine. Prefer small, safe steps. Be concise.", "system prompt added when the client sends none (empty disables)")
	mockMode   = flag.Bool("mock", false, "stream a canned reply instead of calling the upstream")
	verbose    = flag.Bool("verbose", false, "log each request and upstream status")
	defaultMdl = "deepseek-chat"
)

var (
	sessionsMu sync.Mutex
	sessions   = map[string]*session{}
)

func toolDefs() []map[string]any {
	str := func(desc string) map[string]any {
		return map[string]any{"type": "string", "description": desc}
	}
	return []map[string]any{
		{"type": "function", "function": map[string]any{
			"name":        "read_file",
			"description": "Read a text file from the XP machine.",
			"parameters": map[string]any{
				"type":       "object",
				"properties": map[string]any{"path": str("Path relative to the project root, or absolute")},
				"required":   []string{"path"},
			},
		}},
		{"type": "function", "function": map[string]any{
			"name":        "write_file",
			"description": "Write text to a file on the XP machine.",
			"parameters": map[string]any{
				"type": "object",
				"properties": map[string]any{
					"path":    str("Path relative to the project root, or absolute"),
					"content": str("File contents"),
				},
				"required": []string{"path", "content"},
			},
		}},
		{"type": "function", "function": map[string]any{
			"name":        "edit_file",
			"description": "Replace text in a file. Fails unless old occurs exactly once, unless all is true.",
			"parameters": map[string]any{
				"type": "object",
				"properties": map[string]any{
					"path": str("Path relative to the project root, or absolute"),
					"old":  str("Exact text to replace"),
					"new":  str("Replacement text"),
					"all":  map[string]any{"type": "boolean", "description": "Replace every occurrence"},
				},
				"required": []string{"path", "old", "new"},
			},
		}},
		{"type": "function", "function": map[string]any{
			"name":        "search",
			"description": "Search files under the project root (or a given path) for a literal string.",
			"parameters": map[string]any{
				"type": "object",
				"properties": map[string]any{
					"pattern": str("Literal text to find"),
					"path":    str("Directory relative to the project root"),
					"max":     map[string]any{"type": "integer", "description": "Maximum matches (default 100)"},
				},
				"required": []string{"pattern"},
			},
		}},
		{"type": "function", "function": map[string]any{
			"name":        "make_dir",
			"description": "Create a directory (and any missing parents) on the XP machine.",
			"parameters": map[string]any{
				"type":       "object",
				"properties": map[string]any{"path": str("Directory relative to the project root, or absolute")},
				"required":   []string{"path"},
			},
		}},
		{"type": "function", "function": map[string]any{
			"name":        "list_dir",
			"description": "List a directory on the XP machine.",
			"parameters": map[string]any{
				"type":       "object",
				"properties": map[string]any{"path": str("Directory relative to the project root, or absolute")},
				"required":   []string{"path"},
			},
		}},
		{"type": "function", "function": map[string]any{
			"name":        "run_command",
			"description": "Run a shell command on the XP machine and return its output.",
			"parameters": map[string]any{
				"type": "object",
				"properties": map[string]any{
					"command": str("Command line"),
					"timeout": map[string]any{"type": "integer", "description": "Seconds before killing the command (default 120)"},
				},
				"required": []string{"command"},
			},
		}},
	}
}

func writeFrame(w io.Writer, kind byte, data []byte) error {
	if _, err := fmt.Fprintf(w, "%c%d\n", kind, len(data)); err != nil {
		return err
	}
	_, err := w.Write(data)
	return err
}

func flushWriter(w io.Writer) {
	if f, ok := w.(http.Flusher); ok {
		f.Flush()
	}
}

func pickModel(reqModel string) string {
	if *modelFlag != "" {
		return *modelFlag
	}
	if reqModel != "" {
		return reqModel
	}
	return defaultMdl
}

func resolveKey() string {
	if *apiKey != "" {
		return *apiKey
	}
	if *apiKeyFile != "" {
		if b, err := os.ReadFile(*apiKeyFile); err == nil {
			if k := strings.TrimSpace(string(b)); k != "" {
				return k
			}
		}
	}
	if k := os.Getenv("DEEPSEEK_API_KEY"); k != "" {
		return k
	}
	return os.Getenv("OPENAI_API_KEY")
}

func getSession(id, workdir string) *session {
	sessionsMu.Lock()
	defer sessionsMu.Unlock()
	s := sessions[id]
	if s == nil {
		sys := *systemFlag
		if workdir != "" {
			sys += "\n\nThe project root is " + workdir + ". Prefer paths relative to it."
		}
		s = &session{messages: []oaiMessage{{Role: "system", Content: &sys}}}
		sessions[id] = s
	}
	s.updated = time.Now()
	return s
}

func appendToolResults(s *session, results []toolResult) {
	byID := map[string]string{}
	for _, r := range results {
		byID[r.ID] = r.Content
	}
	for i := len(s.messages) - 1; i >= 0; i-- {
		if s.messages[i].Role == "assistant" && len(s.messages[i].ToolCalls) > 0 {
			for _, tc := range s.messages[i].ToolCalls {
				content, ok := byID[tc.ID]
				if !ok {
					content = "(no result)"
				}
				s.messages = append(s.messages, oaiMessage{
					Role:       "tool",
					ToolCallID: tc.ID,
					Content:    &content,
				})
			}
			return
		}
	}
}

func parseArgs(raw string) []kvp {
	var m map[string]any
	if json.Unmarshal([]byte(raw), &m) != nil {
		return nil
	}
	keys := make([]string, 0, len(m))
	for k := range m {
		keys = append(keys, k)
	}
	sort.Strings(keys)
	out := make([]kvp, 0, len(m))
	for _, k := range keys {
		switch v := m[k].(type) {
		case string:
			out = append(out, kvp{k, v})
		default:
			b, _ := json.Marshal(v)
			out = append(out, kvp{k, string(b)})
		}
	}
	return out
}

func writeToolFrame(w io.Writer, tc toolCall) {
	args := parseArgs(tc.Function.Arguments)
	var b bytes.Buffer
	fmt.Fprintf(&b, "%d\n%s", len(tc.ID), tc.ID)
	fmt.Fprintf(&b, "%d\n%s", len(tc.Function.Name), tc.Function.Name)
	fmt.Fprintf(&b, "%d\n", len(args))
	for _, a := range args {
		fmt.Fprintf(&b, "%d\n%s%d\n%s", len(a.k), a.k, len(a.v), a.v)
	}
	writeFrame(w, 'T', b.Bytes())
}

func upstreamStream(ctx context.Context, w http.ResponseWriter, modelName string, msgs []oaiMessage, key string) (string, []toolCall, error) {
	body, err := json.Marshal(upstreamRequest{
		Model:    modelName,
		Messages: msgs,
		Stream:   true,
		Tools:    toolDefs(),
	})
	if err != nil {
		return "", nil, err
	}
	hreq, err := http.NewRequestWithContext(ctx, http.MethodPost, *upstream, bytes.NewReader(body))
	if err != nil {
		return "", nil, err
	}
	hreq.Header.Set("Content-Type", "application/json")
	hreq.Header.Set("Authorization", "Bearer "+key)
	if *verbose {
		log.Printf("upstream model=%s messages=%d", modelName, len(msgs))
	}
	resp, err := http.DefaultClient.Do(hreq)
	if err != nil {
		return "", nil, err
	}
	defer resp.Body.Close()
	if resp.StatusCode != http.StatusOK {
		b, _ := io.ReadAll(io.LimitReader(resp.Body, 8192))
		return "", nil, fmt.Errorf("upstream %d: %s", resp.StatusCode, strings.TrimSpace(string(b)))
	}
	sc := bufio.NewScanner(resp.Body)
	sc.Buffer(make([]byte, 0, 64*1024), 4*1024*1024)
	var text strings.Builder
	acc := map[int]*toolCall{}
	for sc.Scan() {
		line := sc.Text()
		if !strings.HasPrefix(line, "data:") {
			continue
		}
		payload := strings.TrimSpace(strings.TrimPrefix(line, "data:"))
		if payload == "[DONE]" {
			break
		}
		var chunk streamChunk
		if json.Unmarshal([]byte(payload), &chunk) != nil {
			continue
		}
		if len(chunk.Choices) == 0 {
			continue
		}
		delta := chunk.Choices[0].Delta
		if delta.Content != "" {
			text.WriteString(delta.Content)
			writeFrame(w, 'D', []byte(delta.Content))
			flushWriter(w)
		}
		for _, tc := range delta.ToolCalls {
			a := acc[tc.Index]
			if a == nil {
				a = &toolCall{Type: "function"}
				acc[tc.Index] = a
			}
			if tc.ID != "" {
				a.ID = tc.ID
			}
			if tc.Function.Name != "" {
				a.Function.Name = tc.Function.Name
			}
			a.Function.Arguments += tc.Function.Arguments
		}
	}
	if err := sc.Err(); err != nil {
		return text.String(), nil, err
	}
	idx := make([]int, 0, len(acc))
	for k := range acc {
		idx = append(idx, k)
	}
	sort.Ints(idx)
	tcs := make([]toolCall, 0, len(acc))
	for _, k := range idx {
		t := *acc[k]
		if t.Function.Name == "" {
			continue
		}
		if t.ID == "" {
			t.ID = fmt.Sprintf("call_%d", k)
		}
		tcs = append(tcs, t)
	}
	return text.String(), tcs, nil
}

func mockStream(w io.Writer, msgs []oaiMessage) string {
	last := ""
	for i := len(msgs) - 1; i >= 0; i-- {
		if msgs[i].Role == "user" && msgs[i].Content != nil {
			last = *msgs[i].Content
			break
		}
	}
	reply := "This is a mock XPCode reply. You said: " + last + "\n\nPlain-HTTP streaming through the Go bridge works."
	for _, part := range strings.SplitAfter(reply, " ") {
		writeFrame(w, 'D', []byte(part))
		flushWriter(w)
		time.Sleep(25 * time.Millisecond)
	}
	return reply
}

func chatHandler(w http.ResponseWriter, r *http.Request) {
	if r.Method != http.MethodPost {
		http.Error(w, "POST only", http.StatusMethodNotAllowed)
		return
	}
	var req clientRequest
	if err := json.NewDecoder(io.LimitReader(r.Body, 16<<20)).Decode(&req); err != nil {
		http.Error(w, "invalid json", http.StatusBadRequest)
		return
	}
	if req.Session == "" {
		req.Session = fmt.Sprintf("anon-%d", time.Now().UnixNano())
	}
	if *verbose {
		log.Printf("chat session=%s model=%q message=%d results=%d", req.Session, req.Model, len(req.Message), len(req.ToolResults))
	}

	w.Header().Set("Content-Type", "text/plain; charset=utf-8")
	w.Header().Set("Cache-Control", "no-cache")
	w.Header().Set("X-Accel-Buffering", "no")
	w.WriteHeader(http.StatusOK)
	flushWriter(w)

	s := getSession(req.Session, req.Workdir)
	s.mu.Lock()
	defer s.mu.Unlock()

	if len(req.ToolResults) > 0 {
		if !s.pending {
			writeFrame(w, 'E', []byte("no pending tool calls for this session"))
			writeFrame(w, 'Z', nil)
			flushWriter(w)
			return
		}
		appendToolResults(s, req.ToolResults)
		s.pending = false
	} else if req.Message != "" {
		if s.pending {
			sys := *systemFlag
			s.messages = []oaiMessage{{Role: "system", Content: &sys}}
			s.pending = false
		}
		m := req.Message
		s.messages = append(s.messages, oaiMessage{Role: "user", Content: &m})
	} else {
		writeFrame(w, 'E', []byte("empty request"))
		writeFrame(w, 'Z', nil)
		flushWriter(w)
		return
	}

	modelName := pickModel(req.Model)
	if *mockMode {
		text := mockStream(w, s.messages)
		s.messages = append(s.messages, oaiMessage{Role: "assistant", Content: &text})
		writeFrame(w, 'Z', nil)
		flushWriter(w)
		return
	}
	key := resolveKey()
	if key == "" {
		writeFrame(w, 'E', []byte("no API key configured on the proxy"))
		writeFrame(w, 'Z', nil)
		flushWriter(w)
		return
	}

	text, tcs, err := upstreamStream(r.Context(), w, modelName, s.messages, key)
	if err != nil {
		errText := "error: " + err.Error()
		writeFrame(w, 'E', []byte(err.Error()))
		s.messages = append(s.messages, oaiMessage{Role: "assistant", Content: &errText})
		writeFrame(w, 'Z', nil)
		flushWriter(w)
		return
	}
	asst := oaiMessage{Role: "assistant", ToolCalls: tcs}
	if text != "" {
		asst.Content = &text
	}
	s.messages = append(s.messages, asst)
	if len(tcs) > 0 {
		s.pending = true
		for _, tc := range tcs {
			writeToolFrame(w, tc)
		}
		if *verbose {
			log.Printf("session=%s tool_calls=%d", req.Session, len(tcs))
		}
		writeFrame(w, 'Z', []byte("tools"))
	} else {
		writeFrame(w, 'Z', nil)
	}
	flushWriter(w)
}

func healthHandler(w http.ResponseWriter, r *http.Request) {
	w.Header().Set("Content-Type", "text/plain")
	fmt.Fprintln(w, "ok")
}

func main() {
	flag.Parse()
	if !*mockMode && resolveKey() == "" {
		log.Fatal("no API key: set DEEPSEEK_API_KEY or OPENAI_API_KEY, pass --api-key or --api-key-file, or use --mock")
	}
	go func() {
		for {
			time.Sleep(10 * time.Minute)
			sessionsMu.Lock()
			for id, s := range sessions {
				if time.Since(s.updated) > time.Hour {
					delete(sessions, id)
				}
			}
			sessionsMu.Unlock()
		}
	}()
	http.HandleFunc("/healthz", healthHandler)
	http.HandleFunc("/api/chat", chatHandler)
	log.Printf("listening on %s upstream=%s mock=%v", *listenAddr, *upstream, *mockMode)
	if err := http.ListenAndServe(*listenAddr, nil); err != nil {
		log.Fatal(err)
	}
}
