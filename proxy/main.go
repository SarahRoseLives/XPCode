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
	"strings"
	"time"
)

type message struct {
	Role    string `json:"role"`
	Content string `json:"content"`
}

type chatRequest struct {
	Model    string    `json:"model"`
	Messages []message `json:"messages"`
}

type upstreamRequest struct {
	Model    string    `json:"model"`
	Messages []message `json:"messages"`
	Stream   bool      `json:"stream"`
}

type streamChunk struct {
	Choices []struct {
		Delta struct {
			Content string `json:"content"`
		} `json:"delta"`
	} `json:"choices"`
}

var (
	listenAddr = flag.String("listen", "0.0.0.0:8080", "address to listen on")
	upstream   = flag.String("upstream", "https://api.deepseek.com/chat/completions", "OpenAI-compatible chat completions URL")
	apiKey     = flag.String("api-key", "", "API key (defaults to DEEPSEEK_API_KEY or OPENAI_API_KEY)")
	apiKeyFile = flag.String("api-key-file", "", "read the API key from this file")
	modelFlag  = flag.String("model", "", "force this model for every request")
	systemFlag = flag.String("system", "You are XPCode, a concise coding assistant running on Windows XP.", "system prompt added when the client sends none (empty disables)")
	mockMode   = flag.Bool("mock", false, "stream a canned reply instead of calling the upstream")
	verbose    = flag.Bool("verbose", false, "log each request and upstream status")
	defaultMdl = "deepseek-chat"
)

func writeFrame(w io.Writer, kind byte, data []byte) error {
	if _, err := fmt.Fprintf(w, "%c%d\n", kind, len(data)); err != nil {
		return err
	}
	_, err := w.Write(data)
	return err
}

func flush(w http.ResponseWriter) {
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

func withSystem(msgs []message) []message {
	if *systemFlag == "" {
		return msgs
	}
	for _, m := range msgs {
		if m.Role == "system" {
			return msgs
		}
	}
	out := make([]message, 0, len(msgs)+1)
	out = append(out, message{Role: "system", Content: *systemFlag})
	out = append(out, msgs...)
	return out
}

func mockStream(w io.Writer, msgs []message) {
	last := ""
	for i := len(msgs) - 1; i >= 0; i-- {
		if msgs[i].Role == "user" {
			last = msgs[i].Content
			break
		}
	}
	reply := "This is a mock XPCode reply. You said: " + last + "\n\nPlain-HTTP streaming through the Go bridge works."
	for _, part := range strings.SplitAfter(reply, " ") {
		if err := writeFrame(w, 'D', []byte(part)); err != nil {
			return
		}
		flushWriter(w)
		time.Sleep(25 * time.Millisecond)
	}
	writeFrame(w, 'Z', nil)
	flushWriter(w)
}

func flushWriter(w io.Writer) {
	if f, ok := w.(http.Flusher); ok {
		f.Flush()
	}
}

func upstreamStream(ctx context.Context, w http.ResponseWriter, req chatRequest, modelName, key string) error {
	body, err := json.Marshal(upstreamRequest{Model: modelName, Messages: req.Messages, Stream: true})
	if err != nil {
		return err
	}
	hreq, err := http.NewRequestWithContext(ctx, http.MethodPost, *upstream, bytes.NewReader(body))
	if err != nil {
		return err
	}
	hreq.Header.Set("Content-Type", "application/json")
	hreq.Header.Set("Authorization", "Bearer "+key)
	if *verbose {
		log.Printf("upstream model=%s messages=%d url=%s", modelName, len(req.Messages), *upstream)
	}
	resp, err := http.DefaultClient.Do(hreq)
	if err != nil {
		return err
	}
	defer resp.Body.Close()
	if resp.StatusCode != http.StatusOK {
		b, _ := io.ReadAll(io.LimitReader(resp.Body, 8192))
		writeFrame(w, 'E', []byte(fmt.Sprintf("upstream %d: %s", resp.StatusCode, strings.TrimSpace(string(b)))))
		return nil
	}
	sc := bufio.NewScanner(resp.Body)
	sc.Buffer(make([]byte, 0, 64*1024), 4*1024*1024)
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
		text := chunk.Choices[0].Delta.Content
		if text == "" {
			continue
		}
		if err := writeFrame(w, 'D', []byte(text)); err != nil {
			return err
		}
		flushWriter(w)
	}
	return sc.Err()
}

func chatHandler(w http.ResponseWriter, r *http.Request) {
	if r.Method != http.MethodPost {
		http.Error(w, "POST only", http.StatusMethodNotAllowed)
		return
	}
	var req chatRequest
	if err := json.NewDecoder(io.LimitReader(r.Body, 8<<20)).Decode(&req); err != nil {
		http.Error(w, "invalid json", http.StatusBadRequest)
		return
	}
	msgs := withSystem(req.Messages)
	modelName := pickModel(req.Model)
	if *verbose {
		log.Printf("chat model=%s messages=%d mock=%v", modelName, len(msgs), *mockMode)
	}
	w.Header().Set("Content-Type", "text/plain; charset=utf-8")
	w.Header().Set("Cache-Control", "no-cache")
	w.Header().Set("X-Accel-Buffering", "no")
	w.WriteHeader(http.StatusOK)
	flush(w)

	if *mockMode {
		mockStream(w, msgs)
		return
	}
	key := resolveKey()
	if key == "" {
		writeFrame(w, 'E', []byte("no API key configured on the proxy"))
		writeFrame(w, 'Z', nil)
		flush(w)
		return
	}
	if err := upstreamStream(r.Context(), w, chatRequest{Model: modelName, Messages: msgs}, modelName, key); err != nil {
		writeFrame(w, 'E', []byte(err.Error()))
	}
	writeFrame(w, 'Z', nil)
	flush(w)
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

func healthHandler(w http.ResponseWriter, r *http.Request) {
	w.Header().Set("Content-Type", "text/plain")
	fmt.Fprintln(w, "ok")
}

func main() {
	flag.Parse()
	if !*mockMode && resolveKey() == "" {
		log.Fatal("no API key: set DEEPSEEK_API_KEY or OPENAI_API_KEY, pass --api-key, or use --mock")
	}
	http.HandleFunc("/healthz", healthHandler)
	http.HandleFunc("/api/chat", chatHandler)
	log.Printf("listening on %s upstream=%s mock=%v", *listenAddr, *upstream, *mockMode)
	if err := http.ListenAndServe(*listenAddr, nil); err != nil {
		log.Fatal(err)
	}
}
