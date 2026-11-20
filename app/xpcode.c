#define WINVER 0x0501
#define _WIN32_WINNT 0x0501
#define WIN32_LEAN_AND_MEAN

#include <windows.h>
#include <wininet.h>
#include <shellapi.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define IDC_TRANSCRIPT 1001
#define IDC_INPUT 1002
#define IDC_SEND 1003
#define IDC_STATUS 1004
#define IDM_EXIT 2001
#define IDM_SETTINGS 2002
#define IDM_ABOUT 2003

#define WM_APP_CHUNK (WM_APP + 1)
#define WM_APP_DONE (WM_APP + 2)
#define WM_APP_ERROR (WM_APP + 3)

#define MAX_HISTORY 64
#define MAX_FRAME (64L * 1024 * 1024)

typedef struct {
	char *role;
	char *content;
} Msg;

typedef struct {
	char *p;
	size_t len;
	size_t cap;
} SB;

typedef struct {
	HWND hwnd;
	char *body;
} Job;

static HWND g_hwnd;
static HWND g_transcript;
static HWND g_input;
static HWND g_send;
static HWND g_status;
static HFONT g_font;
static WNDPROC g_edit_proc;
static int g_busy;
static Msg g_msgs[MAX_HISTORY];
static int g_nmsgs;
static SB g_reply;
static char g_host[128] = "Garden.lan";
static int g_port = 8080;
static char g_model[128] = "deepseek-chat";

static char *dupstr(const char *s)
{
	size_t n = strlen(s) + 1;
	char *p = (char *)malloc(n);
	if (p)
		memcpy(p, s, n);
	return p;
}

static void sb_reserve(SB *b, size_t extra)
{
	size_t need = b->len + extra + 1;
	size_t cap;
	char *np;

	if (need <= b->cap)
		return;
	cap = b->cap ? b->cap : 256;
	while (cap < need)
		cap *= 2;
	np = (char *)realloc(b->p, cap);
	if (!np)
		return;
	b->p = np;
	b->cap = cap;
}

static void sb_add(SB *b, const char *s, size_t n)
{
	sb_reserve(b, n);
	if (!b->p)
		return;
	memcpy(b->p + b->len, s, n);
	b->len += n;
	b->p[b->len] = 0;
}

static void sb_str(SB *b, const char *s)
{
	sb_add(b, s, strlen(s));
}

static void sb_free(SB *b)
{
	free(b->p);
	b->p = NULL;
	b->len = 0;
	b->cap = 0;
}

static void sb_json(SB *b, const char *s)
{
	const unsigned char *p;

	sb_str(b, "\"");
	for (p = (const unsigned char *)s; *p; p++) {
		switch (*p) {
		case '"':
			sb_str(b, "\\\"");
			break;
		case '\\':
			sb_str(b, "\\\\");
			break;
		case '\n':
			sb_str(b, "\\n");
			break;
		case '\r':
			sb_str(b, "\\r");
			break;
		case '\t':
			sb_str(b, "\\t");
			break;
		default:
			if (*p < 0x20) {
				char tmp[8];
				sprintf(tmp, "\\u%04x", *p);
				sb_str(b, tmp);
			} else {
				sb_add(b, (const char *)p, 1);
			}
		}
	}
	sb_str(b, "\"");
}

static char *build_body(void)
{
	SB b = {0};
	int i;

	sb_str(&b, "{\"model\":");
	sb_json(&b, g_model);
	sb_str(&b, ",\"messages\":[");
	for (i = 0; i < g_nmsgs; i++) {
		if (i)
			sb_str(&b, ",");
		sb_str(&b, "{\"role\":");
		sb_json(&b, g_msgs[i].role);
		sb_str(&b, ",\"content\":");
		sb_json(&b, g_msgs[i].content);
		sb_str(&b, "}");
	}
	sb_str(&b, "]}");
	return b.p;
}

static void push_msg(const char *role, const char *content)
{
	if (g_nmsgs >= MAX_HISTORY) {
		free(g_msgs[0].role);
		free(g_msgs[0].content);
		memmove(&g_msgs[0], &g_msgs[1], sizeof(Msg) * (MAX_HISTORY - 1));
		g_nmsgs = MAX_HISTORY - 1;
	}
	g_msgs[g_nmsgs].role = dupstr(role);
	g_msgs[g_nmsgs].content = dupstr(content);
	g_nmsgs++;
}

static void get_ini_path(char *buf)
{
	char exe[MAX_PATH];
	char *slash;

	GetModuleFileNameA(NULL, exe, MAX_PATH);
	slash = strrchr(exe, '\\');
	if (slash)
		slash[1] = 0;
	strcpy(buf, exe);
	strcat(buf, "xpcode.ini");
}

static void load_settings(void)
{
	char path[MAX_PATH];

	get_ini_path(path);
	if (GetFileAttributesA(path) == INVALID_FILE_ATTRIBUTES) {
		WritePrivateProfileStringA("server", "host", "Garden.lan", path);
		WritePrivateProfileStringA("server", "port", "8080", path);
		WritePrivateProfileStringA("server", "model", "deepseek-chat", path);
	}
	GetPrivateProfileStringA("server", "host", "Garden.lan", g_host, sizeof g_host, path);
	GetPrivateProfileStringA("server", "model", "deepseek-chat", g_model, sizeof g_model, path);
	g_port = GetPrivateProfileIntA("server", "port", 8080, path);
}

static char *get_input_text(void)
{
	int n = GetWindowTextLengthA(g_input);
	char *s = (char *)malloc((size_t)n + 1);

	if (!s)
		return NULL;
	if (n > 0)
		GetWindowTextA(g_input, s, n + 1);
	else
		s[0] = 0;
	return s;
}

static void set_status(const char *s)
{
	SetWindowTextA(g_status, s);
}

static void transcript_append(const char *s)
{
	int len;

	SendMessageA(g_transcript, EM_SETREADONLY, FALSE, 0);
	len = GetWindowTextLengthA(g_transcript);
	SendMessageA(g_transcript, EM_SETSEL, (WPARAM)len, (LPARAM)len);
	SendMessageA(g_transcript, EM_REPLACESEL, FALSE, (LPARAM)s);
	SendMessageA(g_transcript, EM_SCROLLCARET, 0, 0);
	SendMessageA(g_transcript, EM_SETREADONLY, TRUE, 0);
}

static int read_exact(HINTERNET h, char *buf, DWORD n)
{
	DWORD off = 0;

	while (off < n) {
		DWORD got = 0;
		if (!InternetReadFile(h, buf + off, n - off, &got) || got == 0)
			return -1;
		off += got;
	}
	return 0;
}

static void post_error(HWND hwnd, const char *msg)
{
	PostMessageA(hwnd, WM_APP_ERROR, 0, (LPARAM)dupstr(msg));
}

static void post_wininet_error(HWND hwnd, const char *what)
{
	char msg[512];
	sprintf(msg, "%s (wininet error %lu)", what, (unsigned long)GetLastError());
	post_error(hwnd, msg);
}

static DWORD WINAPI net_thread(LPVOID param)
{
	Job *job = (Job *)param;
	HINTERNET inet = NULL, conn = NULL, req = NULL;
	DWORD status = 0, sz;
	int done = 0;

	inet = InternetOpenA("XPCode/0.1", INTERNET_OPEN_TYPE_DIRECT, NULL, NULL, 0);
	if (!inet) {
		post_error(job->hwnd, "InternetOpen failed");
		goto finish;
	}
	conn = InternetConnectA(inet, g_host, (INTERNET_PORT)g_port, NULL, NULL,
				INTERNET_SERVICE_HTTP, 0, 0);
	if (!conn) {
		post_wininet_error(job->hwnd, "cannot reach proxy");
		goto finish;
	}
	req = HttpOpenRequestA(conn, "POST", "/api/chat", NULL, NULL, NULL,
			       INTERNET_FLAG_NO_CACHE_WRITE | INTERNET_FLAG_RELOAD, 0);
	if (!req) {
		post_error(job->hwnd, "HttpOpenRequest failed");
		goto finish;
	}
	if (!HttpSendRequestA(req, "Content-Type: application/json\r\n", -1,
			      job->body, (DWORD)strlen(job->body))) {
		post_wininet_error(job->hwnd, "HTTP send failed");
		goto finish;
	}
	sz = sizeof status;
	HttpQueryInfoA(req, HTTP_QUERY_STATUS_CODE | HTTP_QUERY_FLAG_NUMBER, &status, &sz, NULL);
	if (status != 200) {
		SB eb = {0};
		char tmp[1024];
		DWORD got = 0;
		char msg[1200];

		while (InternetReadFile(req, tmp, sizeof tmp, &got) && got > 0)
			sb_add(&eb, tmp, got);
		sprintf(msg, "proxy returned HTTP %lu %s", (unsigned long)status, eb.p ? eb.p : "");
		post_error(job->hwnd, msg);
		sb_free(&eb);
		goto finish;
	}
	while (!done) {
		char type, ch;
		char lenbuf[24];
		int li = 0;
		long n;
		char *data;

		if (read_exact(req, &ch, 1) != 0)
			break;
		type = ch;
		for (;;) {
			if (read_exact(req, &ch, 1) != 0) {
				done = 1;
				break;
			}
			if (ch == '\n')
				break;
			if (li < (int)sizeof lenbuf - 1)
				lenbuf[li++] = ch;
		}
		if (done)
			break;
		lenbuf[li] = 0;
		n = atol(lenbuf);
		if (n < 0 || n > MAX_FRAME)
			break;
		data = (char *)malloc((size_t)n + 1);
		if (!data)
			break;
		if (n > 0 && read_exact(req, data, (DWORD)n) != 0) {
			free(data);
			break;
		}
		data[n] = 0;
		if (type == 'D') {
			PostMessageA(job->hwnd, WM_APP_CHUNK, 0, (LPARAM)data);
		} else if (type == 'E') {
			PostMessageA(job->hwnd, WM_APP_ERROR, 0, (LPARAM)data);
		} else {
			free(data);
			if (type == 'Z')
				done = 1;
		}
	}
finish:
	if (req)
		InternetCloseHandle(req);
	if (conn)
		InternetCloseHandle(conn);
	if (inet)
		InternetCloseHandle(inet);
	PostMessageA(job->hwnd, WM_APP_DONE, 0, 0);
	free(job->body);
	free(job);
	return 0;
}

static void do_send(void)
{
	char *text, *body;
	Job *job;
	HANDLE th;

	if (g_busy)
		return;
	load_settings();
	text = get_input_text();
	if (!text || !*text) {
		free(text);
		return;
	}
	g_busy = 1;
	EnableWindow(g_send, FALSE);
	SetWindowTextA(g_input, "");
	transcript_append("You: ");
	transcript_append(text);
	transcript_append("\r\n\r\nXPCode: ");
	push_msg("user", text);
	sb_free(&g_reply);
	body = build_body();
	set_status("Thinking...");
	free(text);

	job = (Job *)malloc(sizeof *job);
	if (!job) {
		free(body);
		g_busy = 0;
		EnableWindow(g_send, TRUE);
		set_status("Out of memory");
		return;
	}
	job->hwnd = g_hwnd;
	job->body = body;
	th = CreateThread(NULL, 0, net_thread, job, 0, NULL);
	if (th)
		CloseHandle(th);
	else {
		free(body);
		free(job);
		g_busy = 0;
		EnableWindow(g_send, TRUE);
		set_status("Cannot start worker thread");
	}
}

static LRESULT CALLBACK edit_proc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
	if (msg == WM_KEYDOWN && wParam == VK_RETURN) {
		do_send();
		return 0;
	}
	return CallWindowProcA(g_edit_proc, hwnd, msg, wParam, lParam);
}

static HMENU build_menu(void)
{
	HMENU menu = CreateMenu();
	HMENU file = CreatePopupMenu();
	HMENU settings = CreatePopupMenu();
	HMENU help = CreatePopupMenu();

	AppendMenuA(file, MF_STRING, IDM_EXIT, "E&xit");
	AppendMenuA(menu, MF_POPUP, (UINT_PTR)file, "&File");
	AppendMenuA(settings, MF_STRING, IDM_SETTINGS, "&Open Settings File");
	AppendMenuA(menu, MF_POPUP, (UINT_PTR)settings, "&Settings");
	AppendMenuA(help, MF_STRING, IDM_ABOUT, "&About");
	AppendMenuA(menu, MF_POPUP, (UINT_PTR)help, "&Help");
	return menu;
}

static void open_settings(void)
{
	char path[MAX_PATH];

	get_ini_path(path);
	ShellExecuteA(g_hwnd, "open", "notepad.exe", path, NULL, SW_SHOW);
}

static LRESULT CALLBACK wnd_proc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
	switch (msg) {
	case WM_CREATE:
		LoadLibraryA("riched20.dll");
		g_font = CreateFontA(-12, 0, 0, 0, FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET,
				     OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, DEFAULT_QUALITY,
				     DEFAULT_PITCH | FF_SWISS, "Tahoma");
		g_transcript = CreateWindowExA(WS_EX_CLIENTEDGE, "RichEdit20A", "",
					       WS_CHILD | WS_VISIBLE | WS_VSCROLL | ES_MULTILINE |
					       ES_READONLY | ES_AUTOVSCROLL | ES_NOHIDESEL,
					       0, 0, 10, 10, hwnd, (HMENU)IDC_TRANSCRIPT,
					       GetModuleHandle(NULL), NULL);
		g_input = CreateWindowExA(WS_EX_CLIENTEDGE, "EDIT", "",
					  WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL,
					  0, 0, 10, 10, hwnd, (HMENU)IDC_INPUT,
					  GetModuleHandle(NULL), NULL);
		g_send = CreateWindowExA(0, "BUTTON", "Send",
					 WS_CHILD | WS_VISIBLE | BS_DEFPUSHBUTTON,
					 0, 0, 10, 10, hwnd, (HMENU)IDC_SEND,
					 GetModuleHandle(NULL), NULL);
		g_status = CreateWindowExA(0, "STATIC", "Ready",
					   WS_CHILD | WS_VISIBLE | SS_LEFT,
					   0, 0, 10, 10, hwnd, (HMENU)IDC_STATUS,
					   GetModuleHandle(NULL), NULL);
		if (g_font) {
			SendMessageA(g_transcript, WM_SETFONT, (WPARAM)g_font, TRUE);
			SendMessageA(g_input, WM_SETFONT, (WPARAM)g_font, TRUE);
			SendMessageA(g_send, WM_SETFONT, (WPARAM)g_font, TRUE);
			SendMessageA(g_status, WM_SETFONT, (WPARAM)g_font, TRUE);
		}
		g_edit_proc = (WNDPROC)SetWindowLongPtrA(g_input, GWLP_WNDPROC,
							 (LONG_PTR)edit_proc);
		return 0;
	case WM_SIZE: {
		int w = LOWORD(lParam);
		int h = HIWORD(lParam);
		int margin = 8;
		int input_h = 26;
		int btn_w = 74;
		int status_h = 20;

		MoveWindow(g_transcript, margin, margin, w - 2 * margin,
			   h - 2 * margin - input_h - status_h - 8, TRUE);
		MoveWindow(g_input, margin, h - margin - status_h - input_h,
			   w - 2 * margin - btn_w - 8, input_h, TRUE);
		MoveWindow(g_send, w - margin - btn_w,
			   h - margin - status_h - input_h, btn_w, input_h, TRUE);
		MoveWindow(g_status, margin, h - margin - status_h + 2,
			   w - 2 * margin, status_h, TRUE);
		return 0;
	}
	case WM_GETMINMAXINFO: {
		MINMAXINFO *mmi = (MINMAXINFO *)lParam;
		mmi->ptMinTrackSize.x = 480;
		mmi->ptMinTrackSize.y = 320;
		return 0;
	}
	case WM_COMMAND:
		switch (LOWORD(wParam)) {
		case IDC_SEND:
			do_send();
			return 0;
		case IDM_EXIT:
			DestroyWindow(hwnd);
			return 0;
		case IDM_SETTINGS:
			open_settings();
			return 0;
		case IDM_ABOUT:
			MessageBoxA(hwnd,
				    "XPCode 0.1\n\nA native Windows XP AI coding client.\n\n"
				    "Streams over plain HTTP from the Go bridge.",
				    "About XPCode", MB_OK | MB_ICONINFORMATION);
			return 0;
		}
		break;
	case WM_APP_CHUNK: {
		char *s = (char *)lParam;
		if (s) {
			transcript_append(s);
			sb_str(&g_reply, s);
			free(s);
		}
		return 0;
	}
	case WM_APP_ERROR: {
		char *s = (char *)lParam;
		transcript_append("\r\n[error] ");
		if (s) {
			transcript_append(s);
			free(s);
		}
		set_status("Error");
		return 0;
	}
	case WM_APP_DONE:
		transcript_append(g_reply.len ? "\r\n\r\n" : "\r\n");
		if (g_reply.len)
			push_msg("assistant", g_reply.p);
		sb_free(&g_reply);
		g_busy = 0;
		EnableWindow(g_send, TRUE);
		set_status("Ready");
		SetFocus(g_input);
		return 0;
	case WM_DESTROY:
		PostQuitMessage(0);
		return 0;
	}
	return DefWindowProcA(hwnd, msg, wParam, lParam);
}

int WINAPI WinMain(HINSTANCE inst, HINSTANCE prev, LPSTR cmdline, int show)
{
	WNDCLASSA wc;
	MSG msg;
	RECT wa;
	int ww = 760, wh = 560, wx, wy;

	(void)prev;
	(void)show;

	load_settings();
	memset(&wc, 0, sizeof wc);
	wc.lpfnWndProc = wnd_proc;
	wc.hInstance = inst;
	wc.hCursor = LoadCursor(NULL, IDC_ARROW);
	wc.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);
	wc.lpszClassName = "XPCodeWnd";
	wc.hIcon = LoadIcon(NULL, IDI_APPLICATION);
	RegisterClassA(&wc);

	SystemParametersInfoA(SPI_GETWORKAREA, 0, &wa, 0);
	if (ww > (wa.right - wa.left) - 16)
		ww = (wa.right - wa.left) - 16;
	if (wh > (wa.bottom - wa.top) - 16)
		wh = (wa.bottom - wa.top) - 16;
	wx = wa.left + ((wa.right - wa.left) - ww) / 2;
	wy = wa.top + ((wa.bottom - wa.top) - wh) / 2;

	g_hwnd = CreateWindowExA(0, "XPCodeWnd", "XPCode",
				 WS_OVERLAPPEDWINDOW, wx, wy,
				 ww, wh, NULL, build_menu(), inst, NULL);
	if (!g_hwnd)
		return 1;
	ShowWindow(g_hwnd, SW_SHOW);
	UpdateWindow(g_hwnd);

	if (cmdline && strstr(cmdline, "selftest")) {
		SetWindowTextA(g_input, "Hello from XPCode. Reply in one short sentence.");
		do_send();
	}

	while (GetMessageA(&msg, NULL, 0, 0) > 0) {
		TranslateMessage(&msg);
		DispatchMessageA(&msg);
	}
	return (int)msg.wParam;
}
