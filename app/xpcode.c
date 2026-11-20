#define WINVER 0x0501
#define _WIN32_WINNT 0x0501
#define WIN32_LEAN_AND_MEAN

#include <windows.h>
#include <wininet.h>
#include <shellapi.h>
#include <shlobj.h>
#include <richedit.h>
#include <objbase.h>
#include "resource.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

#define IDC_TRANSCRIPT 1001
#define IDC_INPUT 1002
#define IDC_SEND 1003
#define IDC_STATUS 1004
#define IDC_STOP 1005
#define IDM_EXIT 2001
#define IDM_SETTINGS 2002
#define IDM_ABOUT 2003
#define IDM_NEW 2004
#define IDM_WORKDIR 2005
#define IDM_SETTINGS_FILE 2006
#define IDM_LOG 2007
#define IDM_CONTINUE 2008

#define WM_APP_CHUNK (WM_APP + 1)
#define WM_APP_DONE (WM_APP + 2)
#define WM_APP_ERROR (WM_APP + 3)
#define WM_APP_LOG (WM_APP + 4)
#define WM_APP_STATUS (WM_APP + 5)
#define WM_APP_REASON (WM_APP + 6)

#define MAX_FRAME (64L * 1024 * 1024)
#define MAX_TOOL_ROUNDS 80

typedef struct {
	char *p;
	size_t len;
	size_t cap;
} SB;

typedef struct {
	HWND hwnd;
	char *body;
} Job;

typedef struct {
	char *k;
	char *v;
} KV;

typedef struct {
	char *id;
	char *name;
	KV *args;
	int nargs;
} ToolCall;

typedef struct {
	ToolCall *items;
	int n;
} ToolList;

typedef struct {
	const char *p;
	long len;
	long off;
} Cur;

static HWND g_hwnd;
static HWND g_transcript;
static HWND g_input;
static HWND g_send;
static HWND g_stop;
static HWND g_status;
static HFONT g_font;
static WNDPROC g_edit_proc;
static int g_busy;
static int g_reason_started;
static volatile LONG g_cancel;
static int g_approve;
static SB g_reply;
static char g_host[128] = "Garden.lan";
static int g_port = 8080;
static char g_model[128] = "deepseek-flash";
static char g_workdir[MAX_PATH];
static char g_session[64];

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

static char *normalize_lf(const char *s)
{
	size_t n = strlen(s);
	char *out = (char *)malloc(n + 1);
	size_t i, j = 0;

	if (!out)
		return NULL;
	for (i = 0; i < n; i++) {
		if (s[i] == '\r' && s[i + 1] == '\n')
			continue;
		out[j++] = s[i];
	}
	out[j] = 0;
	return out;
}

static char *to_crlf(const char *s)
{
	SB b = {0};
	size_t i;

	for (i = 0; s[i]; i++) {
		if (s[i] == '\r')
			continue;
		if (s[i] == '\n')
			sb_str(&b, "\r\n");
		else
			sb_add(&b, s + i, 1);
	}
	if (!b.p)
		return dupstr("");
	return b.p;
}

static int wildcard_match(const char *pat, const char *str)
{
	const char *p = pat, *s = str, *star = NULL, *ss = NULL;

	while (*s) {
		if (*p == '?' || (*p && tolower((unsigned char)*p) == tolower((unsigned char)*s))) {
			p++;
			s++;
		} else if (*p == '*') {
			star = p++;
			ss = s;
		} else if (star) {
			p = star + 1;
			s = ++ss;
		} else {
			return 0;
		}
	}
	while (*p == '*')
		p++;
	return *p == 0;
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

static void get_log_path(char *buf)
{
	char exe[MAX_PATH];
	char *slash;

	GetModuleFileNameA(NULL, exe, MAX_PATH);
	slash = strrchr(exe, '\\');
	if (slash)
		slash[1] = 0;
	strcpy(buf, exe);
	strcat(buf, "xpcode.log");
}

static void log_append(const char *s)
{
	char path[MAX_PATH];
	FILE *fp;

	if (!s || !*s)
		return;
	get_log_path(path);
	fp = fopen(path, "ab");
	if (!fp)
		return;
	fputs(s, fp);
	fclose(fp);
}

static void load_settings(void)
{
	char path[MAX_PATH], exe[MAX_PATH], exe_dir[MAX_PATH];
	char *slash;

	GetModuleFileNameA(NULL, exe, MAX_PATH);
	slash = strrchr(exe, '\\');
	if (slash)
		slash[1] = 0;
	strcpy(exe_dir, exe);
	get_ini_path(path);
	if (GetFileAttributesA(path) == INVALID_FILE_ATTRIBUTES) {
		WritePrivateProfileStringA("server", "host", "Garden.lan", path);
		WritePrivateProfileStringA("server", "port", "8080", path);
		WritePrivateProfileStringA("server", "model", "deepseek-flash", path);
		WritePrivateProfileStringA("server", "workdir", exe_dir, path);
		WritePrivateProfileStringA("server", "approve", "0", path);
	}
	GetPrivateProfileStringA("server", "host", "Garden.lan", g_host, sizeof g_host, path);
	GetPrivateProfileStringA("server", "model", "deepseek-flash", g_model, sizeof g_model, path);
	GetPrivateProfileStringA("server", "workdir", exe_dir, g_workdir, sizeof g_workdir, path);
	if (GetFileAttributesA(g_workdir) == INVALID_FILE_ATTRIBUTES)
		strcpy(g_workdir, exe_dir);
	g_approve = GetPrivateProfileIntA("server", "approve", 0, path);
	g_port = GetPrivateProfileIntA("server", "port", 8080, path);
}

static char *get_input_text(void)
{
	int n = GetWindowTextLengthA(g_input);
	char *a, *u;
	wchar_t *w;
	int wl, ul;

	a = (char *)malloc((size_t)n + 1);
	if (!a)
		return NULL;
	if (n > 0)
		GetWindowTextA(g_input, a, n + 1);
	else
		a[0] = 0;
	wl = MultiByteToWideChar(CP_ACP, 0, a, -1, NULL, 0);
	if (wl <= 0) {
		free(a);
		return dupstr("");
	}
	w = (wchar_t *)malloc((size_t)wl * sizeof(wchar_t));
	if (!w) {
		free(a);
		return NULL;
	}
	MultiByteToWideChar(CP_ACP, 0, a, -1, w, wl);
	ul = WideCharToMultiByte(CP_UTF8, 0, w, -1, NULL, 0, NULL, NULL);
	u = (char *)malloc((size_t)(ul > 0 ? ul : 1));
	if (u) {
		if (ul > 0)
			WideCharToMultiByte(CP_UTF8, 0, w, -1, u, ul, NULL, NULL);
		else
			u[0] = 0;
	}
	free(w);
	free(a);
	return u;
}

static void set_status(const char *s)
{
	SetWindowTextA(g_status, s);
}

static void transcript_append_styled(const char *s, COLORREF color, int bold, int italic)
{
	int wl, start, end;
	wchar_t *w;
	CHARFORMAT2W cf;

	if (!s)
		return;
	wl = MultiByteToWideChar(CP_UTF8, 0, s, -1, NULL, 0);
	if (wl <= 0)
		return;
	w = (wchar_t *)malloc((size_t)wl * sizeof(wchar_t));
	if (!w)
		return;
	MultiByteToWideChar(CP_UTF8, 0, s, -1, w, wl);

	SendMessageW(g_transcript, EM_SETREADONLY, FALSE, 0);
	start = GetWindowTextLengthW(g_transcript);
	SendMessageW(g_transcript, EM_SETSEL, (WPARAM)start, (LPARAM)start);
	SendMessageW(g_transcript, EM_REPLACESEL, FALSE, (LPARAM)w);
	end = GetWindowTextLengthW(g_transcript);
	SendMessageW(g_transcript, EM_SETSEL, (WPARAM)start, (LPARAM)end);

	memset(&cf, 0, sizeof cf);
	cf.cbSize = sizeof cf;
	cf.dwMask = CFM_COLOR | CFM_BOLD | CFM_ITALIC;
	cf.crTextColor = color;
	cf.dwEffects = (bold ? CFE_BOLD : 0) | (italic ? CFE_ITALIC : 0);
	SendMessageW(g_transcript, EM_SETCHARFORMAT, SCF_SELECTION, (LPARAM)&cf);

	SendMessageW(g_transcript, EM_SETSEL, (WPARAM)end, (LPARAM)end);
	SendMessageW(g_transcript, EM_SCROLLCARET, 0, 0);
	SendMessageW(g_transcript, EM_SETREADONLY, TRUE, 0);
	free(w);
}

static void transcript_append(const char *s)
{
	transcript_append_styled(s, RGB(0, 0, 0), 0, 0);
}

static int read_exact(HINTERNET h, char *buf, DWORD n)
{
	DWORD off = 0;

	while (off < n) {
		DWORD got = 0;
		if (g_cancel)
			return -1;
		if (InternetReadFile(h, buf + off, n - off, &got)) {
			if (got == 0)
				return -1;
			off += got;
		} else {
			DWORD e = GetLastError();
			if (e == ERROR_INTERNET_TIMEOUT) {
				if (g_cancel)
					return -1;
				continue;
			}
			return -1;
		}
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

static int cur_line(Cur *c, long *v)
{
	long start = c->off;
	long n;
	char tmp[32];

	while (c->off < c->len && c->p[c->off] != '\n')
		c->off++;
	if (c->off >= c->len)
		return -1;
	n = c->off - start;
	if (n > (long)sizeof tmp - 1)
		n = (long)sizeof tmp - 1;
	memcpy(tmp, c->p + start, (size_t)n);
	tmp[n] = 0;
	c->off++;
	*v = atol(tmp);
	return (*v < 0) ? -1 : 0;
}

static char *cur_blob(Cur *c, long n)
{
	char *s;

	if (n < 0 || c->off + n > c->len)
		return NULL;
	s = (char *)malloc((size_t)n + 1);
	if (!s)
		return NULL;
	memcpy(s, c->p + c->off, (size_t)n);
	s[n] = 0;
	c->off += n;
	return s;
}

static void free_tool_call(ToolCall *tc)
{
	int i;

	free(tc->id);
	free(tc->name);
	for (i = 0; i < tc->nargs; i++) {
		free(tc->args[i].k);
		free(tc->args[i].v);
	}
	free(tc->args);
	memset(tc, 0, sizeof *tc);
}

static void free_tool_list(ToolList *tl)
{
	int i;

	for (i = 0; i < tl->n; i++)
		free_tool_call(&tl->items[i]);
	free(tl->items);
	tl->items = NULL;
	tl->n = 0;
}

static int parse_tool_payload(const char *payload, long len, ToolCall *tc)
{
	Cur c;
	long n, nargs;
	int i;

	memset(tc, 0, sizeof *tc);
	c.p = payload;
	c.len = len;
	c.off = 0;
	if (cur_line(&c, &n)) {
		free_tool_call(tc);
		return -1;
	}
	tc->id = cur_blob(&c, n);
	if (cur_line(&c, &n)) {
		free_tool_call(tc);
		return -1;
	}
	tc->name = cur_blob(&c, n);
	if (cur_line(&c, &nargs)) {
		free_tool_call(tc);
		return -1;
	}
	if (nargs < 0 || nargs > 64) {
		free_tool_call(tc);
		return -1;
	}
	tc->args = (KV *)calloc((size_t)nargs, sizeof(KV));
	if (nargs > 0 && !tc->args) {
		free_tool_call(tc);
		return -1;
	}
	tc->nargs = (int)nargs;
	for (i = 0; i < nargs; i++) {
		if (cur_line(&c, &n)) {
			free_tool_call(tc);
			return -1;
		}
		tc->args[i].k = cur_blob(&c, n);
		if (cur_line(&c, &n)) {
			free_tool_call(tc);
			return -1;
		}
		tc->args[i].v = cur_blob(&c, n);
	}
	if (!tc->id || !tc->name) {
		free_tool_call(tc);
		return -1;
	}
	return 0;
}

static const char *arg_of(ToolCall *tc, const char *key)
{
	int i;

	for (i = 0; i < tc->nargs; i++) {
		if (!strcmp(tc->args[i].k, key))
			return tc->args[i].v ? tc->args[i].v : "";
	}
	return "";
}

static const char *arg_find(ToolCall *tc, const char *key)
{
	int i;

	for (i = 0; i < tc->nargs; i++) {
		if (!strcmp(tc->args[i].k, key))
			return tc->args[i].v ? tc->args[i].v : "";
	}
	return NULL;
}

static char *tool_read_file(const char *path, int start, int lines)
{
	HANDLE h;
	DWORD full, size, got = 0;
	char *buf, *p, *end, *out;
	const DWORD readcap = 4 * 1024 * 1024;
	const DWORD wholecap = 512 * 1024;

	h = CreateFileA(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
			NULL, OPEN_EXISTING, 0, NULL);
	if (h == INVALID_HANDLE_VALUE)
		return dupstr("error: cannot open file");
	full = GetFileSize(h, NULL);
	if (full == INVALID_FILE_SIZE) {
		CloseHandle(h);
		return dupstr("error: cannot stat file");
	}
	size = full > readcap ? readcap : full;
	buf = (char *)malloc((size_t)size + 64);
	if (!buf) {
		CloseHandle(h);
		return dupstr("error: out of memory");
	}
	if (size > 0)
		ReadFile(h, buf, size, &got, NULL);
	CloseHandle(h);
	buf[got] = 0;
	{
		char *lf = normalize_lf(buf);
		if (lf) {
			free(buf);
			buf = lf;
			got = (DWORD)strlen(buf);
		}
	}

	if (start <= 0 && lines <= 0) {
		if (got > wholecap) {
			buf[wholecap] = 0;
			strcpy(buf + wholecap, "\n[truncated]");
		}
		return buf;
	}

	p = buf;
	{
		int line = 1;
		while (line < start && *p) {
			char *nl = strchr(p, '\n');
			if (!nl) {
				p += strlen(p);
				break;
			}
			p = nl + 1;
			line++;
		}
	}
	if (!*p) {
		free(buf);
		return dupstr("(empty range)");
	}
	end = p;
	{
		int n = 0;
		while (*end && (lines <= 0 || n < lines)) {
			char *nl = strchr(end, '\n');
			if (!nl) {
				end += strlen(end);
				break;
			}
			end = nl + 1;
			n++;
		}
	}
	{
		size_t len = (size_t)(end - p);
		if (len > 256 * 1024)
			len = 256 * 1024;
		out = (char *)malloc(len + 32);
		if (!out) {
			free(buf);
			return dupstr("error: out of memory");
		}
		memcpy(out, p, len);
		out[len] = 0;
		if ((size_t)(end - p) > len)
			strcpy(out + len, "\n[truncated]");
	}
	free(buf);
	return out;
}

static char *tool_write_file(const char *path, const char *content)
{
	HANDLE h;
	DWORD wrote = 0;
	char *crlf = to_crlf(content);
	const char *data = crlf ? crlf : content;
	size_t len = strlen(data);

	h = CreateFileA(path, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS,
			FILE_ATTRIBUTE_NORMAL, NULL);
	if (h == INVALID_HANDLE_VALUE) {
		free(crlf);
		return dupstr("error: cannot open file for writing");
	}
	if (len > 0 && !WriteFile(h, data, (DWORD)len, &wrote, NULL)) {
		CloseHandle(h);
		free(crlf);
		return dupstr("error: write failed");
	}
	CloseHandle(h);
	free(crlf);
	return dupstr("ok");
}

static char *tool_list_dir(const char *path)
{
	SB b = {0};
	char pattern[MAX_PATH];
	WIN32_FIND_DATAA fd;
	HANDLE h;
	size_t n = strlen(path);

	if (n == 0 || n >= MAX_PATH - 2)
		return dupstr("error: bad path");
	strcpy(pattern, path);
	if (pattern[n - 1] != '\\' && pattern[n - 1] != '/')
		strcat(pattern, "\\");
	strcat(pattern, "*");
	h = FindFirstFileA(pattern, &fd);
	if (h == INVALID_HANDLE_VALUE)
		return dupstr("error: cannot list directory");
	do {
		char line[600];
		if (!strcmp(fd.cFileName, ".") || !strcmp(fd.cFileName, ".."))
			continue;
		sprintf(line, "%s%s\t%lu\r\n", fd.cFileName,
			(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) ? "\\" : "",
			(unsigned long)fd.nFileSizeLow);
		sb_str(&b, line);
	} while (FindNextFileA(h, &fd));
	FindClose(h);
	if (!b.p)
		return dupstr("(empty)");
	return b.p;
}

static char *tool_run_command(const char *command, int timeout_sec)
{
	char tmpdir[MAX_PATH], bat[MAX_PATH * 2], outp[MAX_PATH * 2];
	DWORD tick = GetTickCount();
	SECURITY_ATTRIBUTES sa;
	HANDLE hf, hout, hr, nul;
	STARTUPINFOA si;
	PROCESS_INFORMATION pi;
	DWORD wrote = 0, code = 0, size = 0, off = 0, got = 0;
	DWORD fullsize = 0;
	DWORD wait;
	LONG deadline;
	int timed_out;
	int cancelled;
	char *body = NULL;
	SB b = {0};
	char head[64];

	if (GetTempPathA(MAX_PATH, tmpdir) == 0)
		return dupstr("error: no temp path");
	sprintf(bat, "%sxpcode_%lu_%lu.bat", tmpdir,
		(unsigned long)GetCurrentProcessId(), (unsigned long)tick);
	sprintf(outp, "%sxpcode_%lu_%lu.out", tmpdir,
		(unsigned long)GetCurrentProcessId(), (unsigned long)tick);

	hf = CreateFileA(bat, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS,
			 FILE_ATTRIBUTE_NORMAL, NULL);
	if (hf == INVALID_HANDLE_VALUE)
		return dupstr("error: cannot write temp batch");
	WriteFile(hf, "@echo off\r\n", 11, &wrote, NULL);
	WriteFile(hf, command, (DWORD)strlen(command), &wrote, NULL);
	WriteFile(hf, "\r\n", 2, &wrote, NULL);
	CloseHandle(hf);

	sa.nLength = sizeof sa;
	sa.lpSecurityDescriptor = NULL;
	sa.bInheritHandle = TRUE;
	hout = CreateFileA(outp, GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE,
			   &sa, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
	if (hout == INVALID_HANDLE_VALUE) {
		DeleteFileA(bat);
		return dupstr("error: cannot create temp output");
	}
	nul = CreateFileA("NUL", GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
			  &sa, OPEN_EXISTING, 0, NULL);

	memset(&si, 0, sizeof si);
	si.cb = sizeof si;
	si.dwFlags = STARTF_USESTDHANDLES | STARTF_USESHOWWINDOW;
	si.wShowWindow = SW_HIDE;
	si.hStdOutput = hout;
	si.hStdError = hout;
	si.hStdInput = (nul == INVALID_HANDLE_VALUE) ? NULL : nul;

	{
		char cmdline[MAX_PATH * 2 + 32];
		sprintf(cmdline, "cmd.exe /c \"%s\"", bat);
		memset(&pi, 0, sizeof pi);
		if (!CreateProcessA(NULL, cmdline, NULL, NULL, TRUE, CREATE_NO_WINDOW,
				    NULL, g_workdir[0] ? g_workdir : NULL, &si, &pi)) {
			CloseHandle(hout);
			if (nul != INVALID_HANDLE_VALUE)
				CloseHandle(nul);
			DeleteFileA(bat);
			DeleteFileA(outp);
			return dupstr("error: CreateProcess failed");
		}
	}
	CloseHandle(hout);
	if (nul != INVALID_HANDLE_VALUE)
		CloseHandle(nul);
	deadline = (LONG)(GetTickCount() + (DWORD)timeout_sec * 1000);
	timed_out = 0;
	cancelled = 0;
	for (;;) {
		wait = WaitForSingleObject(pi.hProcess, 250);
		if (wait == WAIT_OBJECT_0 || wait == WAIT_FAILED)
			break;
		if (g_cancel) {
			cancelled = 1;
			break;
		}
		if ((LONG)(deadline - (LONG)GetTickCount()) <= 0) {
			timed_out = 1;
			break;
		}
	}
	if (timed_out || cancelled) {
		TerminateProcess(pi.hProcess, 1);
		WaitForSingleObject(pi.hProcess, 5000);
	}
	GetExitCodeProcess(pi.hProcess, &code);
	CloseHandle(pi.hThread);
	CloseHandle(pi.hProcess);
	DeleteFileA(bat);

	hr = CreateFileA(outp, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
			 NULL, OPEN_EXISTING, 0, NULL);
	if (hr != INVALID_HANDLE_VALUE) {
		size = GetFileSize(hr, NULL);
		fullsize = size;
		if (size == INVALID_FILE_SIZE || size > 256 * 1024)
			size = 256 * 1024;
		body = (char *)malloc((size_t)size + 1);
		if (body) {
			while (off < size) {
				if (!ReadFile(hr, body + off, size - off, &got, NULL) || got == 0)
					break;
				off += got;
			}
			body[off] = 0;
		}
		CloseHandle(hr);
	}
	DeleteFileA(outp);

	sprintf(head, "exit=%lu\r\n", (unsigned long)code);
	sb_str(&b, head);
	sb_str(&b, body ? body : "");
	if (fullsize != INVALID_FILE_SIZE && fullsize > 256 * 1024)
		sb_str(&b, "\r\n[output truncated]");
	if (cancelled) {
		sb_str(&b, "\r\n[cancelled]");
	} else if (timed_out) {
		char note[64];
		sprintf(note, "\r\n[timed out after %d s]", timeout_sec);
		sb_str(&b, note);
	}
	free(body);
	return b.p ? b.p : dupstr("");
}

static char *tool_make_dir(const char *path)
{
	int r = SHCreateDirectoryExA(NULL, path, NULL);

	if (r == ERROR_SUCCESS || r == ERROR_ALREADY_EXISTS || r == ERROR_FILE_EXISTS)
		return dupstr("ok");
	{
		char m[128];
		sprintf(m, "error: cannot create directory (%d)", r);
		return dupstr(m);
	}
}

static void resolve_path(const char *in, char *out, size_t cap)
{
	size_t n;

	if (!in || !*in) {
		snprintf(out, cap, "%s", g_workdir);
		return;
	}
	if (in[1] == ':' || (in[0] == '\\' && in[1] == '\\') || in[0] == '/') {
		snprintf(out, cap, "%s", in);
		return;
	}
	n = strlen(g_workdir);
	if (n && (g_workdir[n - 1] == '\\' || g_workdir[n - 1] == '/'))
		snprintf(out, cap, "%s%s", g_workdir, in);
	else
		snprintf(out, cap, "%s\\%s", g_workdir, in);
}

static int should_skip_dir(const char *name)
{
	static const char *skip[] = { ".git", ".svn", "node_modules", "build", "dist", "target", ".cache" };
	size_t i;

	for (i = 0; i < sizeof skip / sizeof skip[0]; i++) {
		if (!_stricmp(name, skip[i]))
			return 1;
	}
	return 0;
}

static void build_tree(SB *b, const char *dir, int depth, int *count)
{
	char spec[MAX_PATH * 2];
	WIN32_FIND_DATAA fd;
	HANDLE h;

	if (depth > 2 || *count >= 80 || b->len > 3500)
		return;
	snprintf(spec, sizeof spec, "%s\\*", dir);
	h = FindFirstFileA(spec, &fd);
	if (h == INVALID_HANDLE_VALUE)
		return;
	do {
		char full[MAX_PATH * 2];
		int i;
		if (!strcmp(fd.cFileName, ".") || !strcmp(fd.cFileName, ".."))
			continue;
		if ((fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) &&
		    should_skip_dir(fd.cFileName))
			continue;
		for (i = 0; i < depth; i++)
			sb_str(b, "  ");
		sb_str(b, fd.cFileName);
		if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
			sb_str(b, "\\");
		sb_str(b, "\r\n");
		(*count)++;
		if (*count >= 80 || b->len > 3500)
			break;
		if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
			snprintf(full, sizeof full, "%s\\%s", dir, fd.cFileName);
			build_tree(b, full, depth + 1, count);
		}
	} while (FindNextFileA(h, &fd));
	FindClose(h);
}

static void search_file(const char *path, const char *pattern, int max, SB *out, int *count)
{
	HANDLE h;
	DWORD size, got = 0, off = 0;
	char *buf;
	int line = 1;
	char *p;

	h = CreateFileA(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
			NULL, OPEN_EXISTING, 0, NULL);
	if (h == INVALID_HANDLE_VALUE)
		return;
	size = GetFileSize(h, NULL);
	if (size == INVALID_FILE_SIZE || size > 2 * 1024 * 1024) {
		CloseHandle(h);
		return;
	}
	buf = (char *)malloc((size_t)size + 1);
	if (!buf) {
		CloseHandle(h);
		return;
	}
	while (off < size) {
		if (!ReadFile(h, buf + off, size - off, &got, NULL) || got == 0)
			break;
		off += got;
	}
	CloseHandle(h);
	buf[off] = 0;
	if (off > 0 && memchr(buf, 0, off)) {
		free(buf);
		return;
	}
	{
		char *lf = normalize_lf(buf);
		if (lf) {
			free(buf);
			buf = lf;
		}
	}
	p = buf;
	while (p && *p && *count < max) {
		char *nl = strchr(p, '\n');
		char linebuf[1800];
		if (nl)
			*nl = 0;
		if (strstr(p, pattern)) {
			snprintf(linebuf, sizeof linebuf, "%s:%d: %.900s\r\n", path, line, p);
			sb_str(out, linebuf);
			(*count)++;
		}
		if (!nl)
			break;
		p = nl + 1;
		line++;
	}
	free(buf);
}

static void search_dir(const char *dir, const char *pattern, int max, int depth, SB *out, int *count)
{
	char spec[MAX_PATH * 2];
	WIN32_FIND_DATAA fd;
	HANDLE h;

	if (depth > 12 || *count >= max)
		return;
	snprintf(spec, sizeof spec, "%s\\*", dir);
	h = FindFirstFileA(spec, &fd);
	if (h == INVALID_HANDLE_VALUE)
		return;
	do {
		char full[MAX_PATH * 2];
		if (!strcmp(fd.cFileName, ".") || !strcmp(fd.cFileName, ".."))
			continue;
		snprintf(full, sizeof full, "%s\\%s", dir, fd.cFileName);
		if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
			if (!should_skip_dir(fd.cFileName))
				search_dir(full, pattern, max, depth + 1, out, count);
		} else {
			if (fd.nFileSizeHigh == 0 && fd.nFileSizeLow <= 2 * 1024 * 1024)
				search_file(full, pattern, max, out, count);
		}
		if (*count >= max)
			break;
	} while (FindNextFileA(h, &fd));
	FindClose(h);
}

static char *tool_search(const char *pattern, const char *path, int max)
{
	SB out = {0};
	int count = 0;
	char dir[MAX_PATH * 2];

	if (!pattern || !*pattern)
		return dupstr("error: pattern is required");
	if (path && *path)
		resolve_path(path, dir, sizeof dir);
	else
		snprintf(dir, sizeof dir, "%s", g_workdir);
	search_dir(dir, pattern, max, 0, &out, &count);
	if (!out.p)
		return dupstr("(no matches)");
	return out.p;
}

static void glob_dir(const char *dir, const char *pattern, int max, int depth, SB *out, int *count)
{
	char spec[MAX_PATH * 2];
	WIN32_FIND_DATAA fd;
	HANDLE h;

	if (depth > 12 || *count >= max)
		return;
	snprintf(spec, sizeof spec, "%s\\*", dir);
	h = FindFirstFileA(spec, &fd);
	if (h == INVALID_HANDLE_VALUE)
		return;
	do {
		char full[MAX_PATH * 2];
		if (!strcmp(fd.cFileName, ".") || !strcmp(fd.cFileName, ".."))
			continue;
		snprintf(full, sizeof full, "%s\\%s", dir, fd.cFileName);
		if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
			if (!should_skip_dir(fd.cFileName))
				glob_dir(full, pattern, max, depth + 1, out, count);
		} else if (wildcard_match(pattern, fd.cFileName)) {
			sb_str(out, full);
			sb_str(out, "\r\n");
			(*count)++;
		}
		if (*count >= max)
			break;
	} while (FindNextFileA(h, &fd));
	FindClose(h);
}

static char *tool_glob(const char *pattern, const char *path, int max)
{
	SB out = {0};
	int count = 0;
	char dir[MAX_PATH * 2];

	if (!pattern || !*pattern)
		return dupstr("error: pattern is required");
	if (path && *path)
		resolve_path(path, dir, sizeof dir);
	else
		snprintf(dir, sizeof dir, "%s", g_workdir);
	glob_dir(dir, pattern, max, 0, &out, &count);
	if (!out.p)
		return dupstr("(no matches)");
	return out.p;
}

static char *tool_edit_file(const char *path, const char *old, const char *neu, int all)
{
	HANDLE h;
	DWORD size, got = 0, off = 0;
	char *buf, *p;
	size_t olen;
	int count = 0, done = 0;
	SB out = {0};
	char *w;

	if (!old || !*old)
		return dupstr("error: old text is empty");
	h = CreateFileA(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
			NULL, OPEN_EXISTING, 0, NULL);
	if (h == INVALID_HANDLE_VALUE)
		return dupstr("error: cannot open file");
	size = GetFileSize(h, NULL);
	if (size == INVALID_FILE_SIZE || size > 4 * 1024 * 1024) {
		CloseHandle(h);
		return dupstr("error: file too large");
	}
	buf = (char *)malloc((size_t)size + 1);
	if (!buf) {
		CloseHandle(h);
		return dupstr("error: out of memory");
	}
	while (off < size) {
		if (!ReadFile(h, buf + off, size - off, &got, NULL) || got == 0)
			break;
		off += got;
	}
	CloseHandle(h);
	buf[off] = 0;
	{
		char *lf = normalize_lf(buf);
		if (lf) {
			free(buf);
			buf = lf;
		}
	}
	olen = strlen(old);
	p = buf;
	while ((p = strstr(p, old)) != NULL) {
		count++;
		p += olen;
	}
	if (count == 0) {
		free(buf);
		return dupstr("error: old text not found");
	}
	if (count > 1 && !all) {
		char m[128];
		sprintf(m, "error: %d matches; pass all=true or add more context", count);
		free(buf);
		return dupstr(m);
	}
	p = buf;
	for (;;) {
		char *q = strstr(p, old);
		if (!q) {
			sb_str(&out, p);
			break;
		}
		sb_add(&out, p, (size_t)(q - p));
		sb_str(&out, neu ? neu : "");
		p = q + olen;
		done++;
		if (!all) {
			sb_str(&out, p);
			break;
		}
	}
	free(buf);
	if (!out.p) {
		sb_free(&out);
		return dupstr("error: out of memory");
	}
	w = tool_write_file(path, out.p);
	sb_free(&out);
	if (strncmp(w, "ok", 2) != 0)
		return w;
	free(w);
	{
		char m[64];
		sprintf(m, "ok: replaced %d", done);
		return dupstr(m);
	}
}

static char *tool_apply_patch(const char *path, ToolCall *tc)
{
	HANDLE h;
	DWORD size, got = 0, off = 0;
	char *cur;
	int i, edits = 0;
	char *w;

	h = CreateFileA(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
			NULL, OPEN_EXISTING, 0, NULL);
	if (h == INVALID_HANDLE_VALUE)
		return dupstr("error: cannot open file");
	size = GetFileSize(h, NULL);
	if (size == INVALID_FILE_SIZE || size > 4 * 1024 * 1024) {
		CloseHandle(h);
		return dupstr("error: file too large");
	}
	cur = (char *)malloc((size_t)size + 1);
	if (!cur) {
		CloseHandle(h);
		return dupstr("error: out of memory");
	}
	while (off < size) {
		if (!ReadFile(h, cur + off, size - off, &got, NULL) || got == 0)
			break;
		off += got;
	}
	CloseHandle(h);
	cur[off] = 0;
	{
		char *lf = normalize_lf(cur);
		if (lf) {
			free(cur);
			cur = lf;
		}
	}
	for (i = 1;; i++) {
		char key[16];
		const char *old, *neu, *p;
		int cnt = 0;
		SB nb = {0};

		sprintf(key, "old%d", i);
		old = arg_find(tc, key);
		if (!old)
			break;
		sprintf(key, "new%d", i);
		neu = arg_find(tc, key);
		if (!*old) {
			free(cur);
			return dupstr("error: old text is empty");
		}
		p = cur;
		while ((p = strstr(p, old)) != NULL) {
			cnt++;
			p += strlen(old);
		}
		if (cnt != 1) {
			char m[160];
			sprintf(m, "error: edit %d matched %d times; each old must match exactly once", i, cnt);
			free(cur);
			return dupstr(m);
		}
		p = strstr(cur, old);
		sb_add(&nb, cur, (size_t)(p - cur));
		sb_str(&nb, neu ? neu : "");
		sb_str(&nb, p + strlen(old));
		free(cur);
		cur = nb.p ? nb.p : dupstr("");
		edits++;
	}
	if (edits == 0) {
		free(cur);
		return dupstr("error: no edits provided");
	}
	w = tool_write_file(path, cur);
	free(cur);
	if (strncmp(w, "ok", 2) != 0)
		return w;
	free(w);
	{
		char m[64];
		sprintf(m, "ok: %d edits", edits);
		return dupstr(m);
	}
}

static int proxy_open(const char *api_path, const char *body,
		      HINTERNET *inet, HINTERNET *conn, HINTERNET *req, DWORD *status)
{
	HINTERNET in, co, re;
	DWORD sz;
	DWORD rto = 120000;

	*inet = NULL;
	*conn = NULL;
	*req = NULL;
	*status = 0;
	in = InternetOpenA("XPCode/0.3", INTERNET_OPEN_TYPE_DIRECT, NULL, NULL, 0);
	if (!in)
		return -1;
	co = InternetConnectA(in, g_host, (INTERNET_PORT)g_port, NULL, NULL,
			      INTERNET_SERVICE_HTTP, 0, 0);
	if (!co) {
		InternetCloseHandle(in);
		return -1;
	}
	re = HttpOpenRequestA(co, "POST", api_path, NULL, NULL, NULL,
			      INTERNET_FLAG_NO_CACHE_WRITE | INTERNET_FLAG_RELOAD, 0);
	if (!re) {
		InternetCloseHandle(co);
		InternetCloseHandle(in);
		return -1;
	}
	InternetSetOptionA(re, INTERNET_OPTION_RECEIVE_TIMEOUT, &rto, sizeof rto);
	InternetSetOptionA(re, INTERNET_OPTION_SEND_TIMEOUT, &rto, sizeof rto);
	if (!HttpSendRequestA(re, "Content-Type: application/json\r\n", -1,
			      (LPVOID)body, (DWORD)strlen(body))) {
		InternetCloseHandle(re);
		InternetCloseHandle(co);
		InternetCloseHandle(in);
		return -1;
	}
	sz = sizeof *status;
	HttpQueryInfoA(re, HTTP_QUERY_STATUS_CODE | HTTP_QUERY_FLAG_NUMBER, status, &sz, NULL);
	*inet = in;
	*conn = co;
	*req = re;
	return 0;
}

static char *proxy_post_text(const char *api_path, const char *body, int *status)
{
	HINTERNET inet, conn, req;
	DWORD code = 0, got = 0;
	SB out = {0};
	char tmp[4096];

	if (status)
		*status = 0;
	if (proxy_open(api_path, body, &inet, &conn, &req, &code) != 0) {
		if (status)
			*status = -1;
		return dupstr("error: cannot reach proxy");
	}
	if (status)
		*status = (int)code;
	while (InternetReadFile(req, tmp, sizeof tmp, &got) && got > 0) {
		sb_add(&out, tmp, got);
		if (out.len > 256 * 1024)
			break;
	}
	InternetCloseHandle(req);
	InternetCloseHandle(conn);
	InternetCloseHandle(inet);
	return out.p ? out.p : dupstr("");
}

static char *tool_web_search(const char *query, int max)
{
	SB b = {0};
	char m[32];
	int status = 0;
	char *res;

	if (!query || !*query)
		return dupstr("error: query is required");
	sb_str(&b, "{\"query\":");
	sb_json(&b, query);
	sprintf(m, ",\"max\":%d}", max);
	sb_str(&b, m);
	res = proxy_post_text("/web/search", b.p, &status);
	sb_free(&b);
	if (!res)
		return dupstr("error: out of memory");
	if (status != 200) {
		SB e = {0};
		sb_str(&e, "error: web search failed");
		if (*res) {
			sb_str(&e, ": ");
			sb_str(&e, res);
		}
		free(res);
		return e.p;
	}
	return res;
}

static char *tool_web_fetch(const char *url, int max)
{
	SB b = {0};
	char m[32];
	int status = 0;
	char *res;

	if (!url || !*url)
		return dupstr("error: url is required");
	sb_str(&b, "{\"url\":");
	sb_json(&b, url);
	sprintf(m, ",\"max\":%d}", max);
	sb_str(&b, m);
	res = proxy_post_text("/web/fetch", b.p, &status);
	sb_free(&b);
	if (!res)
		return dupstr("error: out of memory");
	if (status != 200) {
		SB e = {0};
		sb_str(&e, "error: web fetch failed");
		if (*res) {
			sb_str(&e, ": ");
			sb_str(&e, res);
		}
		free(res);
		return e.p;
	}
	return res;
}

static char *tool_web_download(const char *url, const char *dest)
{
	SB b = {0};
	SB err = {0};
	HINTERNET inet, conn, req;
	DWORD code = 0, got = 0;
	HANDLE h;
	char tmp[8192];
	DWORD wrote, total = 0;
	int failed = 0;
	char msg[512];

	if (!url || !*url)
		return dupstr("error: url is required");
	sb_str(&b, "{\"url\":");
	sb_json(&b, url);
	sb_str(&b, "}");
	if (proxy_open("/web/raw", b.p, &inet, &conn, &req, &code) != 0) {
		sb_free(&b);
		return dupstr("error: cannot reach proxy");
	}
	sb_free(&b);
	if (code != 200) {
		while (InternetReadFile(req, tmp, sizeof tmp, &got) && got > 0)
			sb_add(&err, tmp, got);
		InternetCloseHandle(req);
		InternetCloseHandle(conn);
		InternetCloseHandle(inet);
		sprintf(msg, "error: download failed (HTTP %lu) %.400s",
			(unsigned long)code, err.p ? err.p : "");
		sb_free(&err);
		return dupstr(msg);
	}
	h = CreateFileA(dest, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS,
			FILE_ATTRIBUTE_NORMAL, NULL);
	if (h == INVALID_HANDLE_VALUE) {
		InternetCloseHandle(req);
		InternetCloseHandle(conn);
		InternetCloseHandle(inet);
		return dupstr("error: cannot create destination file");
	}
	for (;;) {
		if (!InternetReadFile(req, tmp, sizeof tmp, &got)) {
			failed = 1;
			break;
		}
		if (got == 0)
			break;
		if (!WriteFile(h, tmp, got, &wrote, NULL)) {
			failed = 1;
			break;
		}
		total += wrote;
		if (total > 128 * 1024 * 1024) {
			failed = 1;
			break;
		}
	}
	CloseHandle(h);
	InternetCloseHandle(req);
	InternetCloseHandle(conn);
	InternetCloseHandle(inet);
	if (failed) {
		DeleteFileA(dest);
		return dupstr("error: download interrupted");
	}
	sprintf(msg, "downloaded %lu bytes to %s", (unsigned long)total, dest);
	return dupstr(msg);
}

typedef struct {
	const char *question;
	char answer[1024];
	int ok;
} AskCtx;

static INT_PTR CALLBACK ask_proc(HWND dlg, UINT msg, WPARAM wParam, LPARAM lParam)
{
	AskCtx *ctx = (AskCtx *)GetWindowLongPtrA(dlg, GWLP_USERDATA);

	switch (msg) {
	case WM_INITDIALOG:
		ctx = (AskCtx *)lParam;
		SetWindowLongPtrA(dlg, GWLP_USERDATA, (LONG_PTR)ctx);
		SetDlgItemTextA(dlg, IDC_ASK_QUESTION, ctx->question ? ctx->question : "");
		SetDlgItemTextA(dlg, IDC_ASK_ANSWER, "");
		SetFocus(GetDlgItem(dlg, IDC_ASK_ANSWER));
		return FALSE;
	case WM_COMMAND:
		if (!ctx)
			break;
		switch (LOWORD(wParam)) {
		case IDC_ASK_OK:
			GetDlgItemTextA(dlg, IDC_ASK_ANSWER, ctx->answer, sizeof ctx->answer);
			ctx->ok = 1;
			EndDialog(dlg, 1);
			return TRUE;
		case IDC_ASK_CANCEL:
			ctx->ok = 0;
			EndDialog(dlg, 0);
			return TRUE;
		}
		break;
	case WM_CLOSE:
		if (ctx)
			ctx->ok = 0;
		EndDialog(dlg, 0);
		return TRUE;
	}
	return FALSE;
}

static char *tool_ask_user(const char *question)
{
	AskCtx ctx;
	INT_PTR r;
	char log[1200];

	memset(&ctx, 0, sizeof ctx);
	ctx.question = question;
	r = DialogBoxParamA(GetModuleHandle(NULL), MAKEINTRESOURCEA(IDD_ASK),
			    g_hwnd, ask_proc, (LPARAM)&ctx);
	if (r == 1) {
		snprintf(log, sizeof log, "\r\n[answer] %.900s\r\n", ctx.answer);
		PostMessageA(g_hwnd, WM_APP_LOG, 0, (LPARAM)dupstr(log));
		if (ctx.answer[0] == 0)
			return dupstr("(the user sent an empty answer)");
		return dupstr(ctx.answer);
	}
	PostMessageA(g_hwnd, WM_APP_LOG, 0, (LPARAM)dupstr("\r\n[answer] (canceled)\r\n"));
	return dupstr("(the user canceled the question)");
}

static int needs_approval(const char *name)
{
	return !strcmp(name, "write_file") || !strcmp(name, "edit_file") ||
	       !strcmp(name, "apply_patch") || !strcmp(name, "make_dir") ||
	       !strcmp(name, "run_command") || !strcmp(name, "download");
}

static void approval_text(ToolCall *tc, char *out, size_t cap)
{
	const char *path = arg_of(tc, "path");
	const char *cmd = arg_of(tc, "command");

	if (!*path)
		path = arg_of(tc, "pattern");
	snprintf(out, cap, "XPCode wants to run:\n\n%s\n\n%s\n\nAllow?",
		 tc->name, *cmd ? cmd : path);
}

static char *execute_tool(ToolCall *tc)
{
	char path[MAX_PATH * 2];

	if (g_approve && needs_approval(tc->name)) {
		char msg[1200];
		approval_text(tc, msg, sizeof msg);
		if (MessageBoxA(g_hwnd, msg, "XPCode: approve tool call",
				MB_YESNO | MB_ICONWARNING | MB_DEFBUTTON2) != IDYES)
			return dupstr("denied by user");
	}

	if (!strcmp(tc->name, "web_search")) {
		const char *mp = arg_of(tc, "max");
		int max = *mp ? atoi(mp) : 5;
		if (max < 1)
			max = 5;
		if (max > 10)
			max = 10;
		return tool_web_search(arg_of(tc, "query"), max);
	}
	if (!strcmp(tc->name, "web_fetch")) {
		const char *mp = arg_of(tc, "max");
		int max = *mp ? atoi(mp) : 20000;
		if (max < 1000)
			max = 20000;
		if (max > 200000)
			max = 200000;
		return tool_web_fetch(arg_of(tc, "url"), max);
	}
	if (!strcmp(tc->name, "download")) {
		resolve_path(arg_of(tc, "path"), path, sizeof path);
		return tool_web_download(arg_of(tc, "url"), path);
	}
	if (!strcmp(tc->name, "ask_user"))
		return tool_ask_user(arg_of(tc, "question"));
	if (!strcmp(tc->name, "read_file")) {
		const char *ss = arg_of(tc, "start");
		const char *ls = arg_of(tc, "lines");
		int start = *ss ? atoi(ss) : 0;
		int cnt = *ls ? atoi(ls) : 0;
		if (start < 0)
			start = 0;
		if (cnt < 0)
			cnt = 0;
		resolve_path(arg_of(tc, "path"), path, sizeof path);
		return tool_read_file(path, start, cnt);
	}
	if (!strcmp(tc->name, "write_file")) {
		resolve_path(arg_of(tc, "path"), path, sizeof path);
		return tool_write_file(path, arg_of(tc, "content"));
	}
	if (!strcmp(tc->name, "apply_patch")) {
		resolve_path(arg_of(tc, "path"), path, sizeof path);
		return tool_apply_patch(path, tc);
	}
	if (!strcmp(tc->name, "edit_file")) {
		const char *all = arg_of(tc, "all");
		int do_all = (!strcmp(all, "true") || !strcmp(all, "1") || !strcmp(all, "yes"));
		resolve_path(arg_of(tc, "path"), path, sizeof path);
		return tool_edit_file(path, arg_of(tc, "old"), arg_of(tc, "new"), do_all);
	}
	if (!strcmp(tc->name, "make_dir")) {
		resolve_path(arg_of(tc, "path"), path, sizeof path);
		return tool_make_dir(path);
	}
	if (!strcmp(tc->name, "list_dir")) {
		resolve_path(arg_of(tc, "path"), path, sizeof path);
		return tool_list_dir(path);
	}
	if (!strcmp(tc->name, "glob")) {
		const char *mp = arg_of(tc, "max");
		int max = *mp ? atoi(mp) : 200;
		if (max < 1)
			max = 200;
		if (max > 1000)
			max = 1000;
		return tool_glob(arg_of(tc, "pattern"), arg_of(tc, "path"), max);
	}
	if (!strcmp(tc->name, "search")) {
		const char *mp = arg_of(tc, "max");
		int max = *mp ? atoi(mp) : 100;
		if (max < 1)
			max = 100;
		if (max > 500)
			max = 500;
		return tool_search(arg_of(tc, "pattern"), arg_of(tc, "path"), max);
	}
	if (!strcmp(tc->name, "run_command")) {
		const char *ts = arg_of(tc, "timeout");
		int sec = *ts ? atoi(ts) : 120;
		if (sec < 1)
			sec = 1;
		if (sec > 600)
			sec = 600;
		return tool_run_command(arg_of(tc, "command"), sec);
	}
	return dupstr("error: unknown tool");
}

static char *build_start_body(const char *text)
{
	SB b = {0};

	sb_str(&b, "{\"session\":");
	sb_json(&b, g_session);
	sb_str(&b, ",\"model\":");
	sb_json(&b, g_model);
	sb_str(&b, ",\"workdir\":");
	sb_json(&b, g_workdir);
	{
		SB tree = {0};
		int tcount = 0;
		build_tree(&tree, g_workdir, 0, &tcount);
		sb_str(&b, ",\"context\":");
		sb_json(&b, tree.p ? tree.p : "");
		sb_free(&tree);
	}
	sb_str(&b, ",\"message\":");
	sb_json(&b, text);
	sb_str(&b, "}");
	return b.p;
}

static char *execute_and_build(ToolList *tl, HWND hwnd)
{
	SB b = {0};
	int i;

	PostMessageA(hwnd, WM_APP_STATUS, 0, (LPARAM)dupstr("Running tools..."));
	sb_str(&b, "{\"session\":");
	sb_json(&b, g_session);
	sb_str(&b, ",\"model\":");
	sb_json(&b, g_model);
	sb_str(&b, ",\"tool_results\":[");
	for (i = 0; i < tl->n; i++) {
		char *res;
		char logline[512];
		const char *detail = arg_of(&tl->items[i], "path");
		if (!*detail)
			detail = arg_of(&tl->items[i], "command");
		if (!*detail)
			detail = arg_of(&tl->items[i], "pattern");
		snprintf(logline, sizeof logline, "\r\n[tool] %s %.100s\r\n",
			 tl->items[i].name ? tl->items[i].name : "?", detail);
		PostMessageA(hwnd, WM_APP_LOG, 0, (LPARAM)dupstr(logline));
		res = execute_tool(&tl->items[i]);
		if (i)
			sb_str(&b, ",");
		sb_str(&b, "{\"id\":");
		sb_json(&b, tl->items[i].id ? tl->items[i].id : "");
		sb_str(&b, ",\"content\":");
		sb_json(&b, res ? res : "");
		sb_str(&b, "}");
		free(res);
	}
	sb_str(&b, "]}");
	return b.p;
}

static int do_round(HWND hwnd, const char *body, ToolList *tl, int *need_tools)
{
	HINTERNET inet = NULL, conn = NULL, req = NULL;
	DWORD status = 0, sz;
	int ok = -1;

	*need_tools = 0;
	PostMessageA(hwnd, WM_APP_STATUS, 0, (LPARAM)dupstr("Thinking..."));
	inet = InternetOpenA("XPCode/0.3", INTERNET_OPEN_TYPE_DIRECT, NULL, NULL, 0);
	if (!inet) {
		post_wininet_error(hwnd, "InternetOpen failed");
		return -1;
	}
	conn = InternetConnectA(inet, g_host, (INTERNET_PORT)g_port, NULL, NULL,
				INTERNET_SERVICE_HTTP, 0, 0);
	if (!conn) {
		if (!g_cancel)
			post_wininet_error(hwnd, "cannot reach proxy");
		goto out;
	}
	req = HttpOpenRequestA(conn, "POST", "/api/chat", NULL, NULL, NULL,
			       INTERNET_FLAG_NO_CACHE_WRITE | INTERNET_FLAG_RELOAD, 0);
	if (!req) {
		if (!g_cancel)
			post_wininet_error(hwnd, "HttpOpenRequest failed");
		goto out;
	}
	{
		DWORD rto = 1000;
		DWORD sto = 120000;
		InternetSetOptionA(req, INTERNET_OPTION_RECEIVE_TIMEOUT, &rto, sizeof rto);
		InternetSetOptionA(req, INTERNET_OPTION_SEND_TIMEOUT, &sto, sizeof sto);
	}
	if (!HttpSendRequestA(req, "Content-Type: application/json\r\n", -1,
			      (LPVOID)body, (DWORD)strlen(body))) {
		if (!g_cancel)
			post_wininet_error(hwnd, "HTTP send failed");
		goto out;
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
		sprintf(msg, "proxy HTTP %lu %s", (unsigned long)status, eb.p ? eb.p : "");
		if (!g_cancel)
			post_error(hwnd, msg);
		sb_free(&eb);
		goto out;
	}
	for (;;) {
		char type, ch;
		char lenbuf[24];
		int li = 0;
		long n;
		char *data;

		if (g_cancel) {
			ok = -1;
			goto out;
		}
		if (read_exact(req, &ch, 1) != 0) {
			ok = 0;
			break;
		}
		type = ch;
		for (;;) {
			if (read_exact(req, &ch, 1) != 0) {
				ok = -1;
				goto out;
			}
			if (ch == '\n')
				break;
			if (li < (int)sizeof lenbuf - 1)
				lenbuf[li++] = ch;
		}
		lenbuf[li] = 0;
		n = atol(lenbuf);
		if (n < 0 || n > MAX_FRAME) {
			ok = -1;
			goto out;
		}
		data = (char *)malloc((size_t)n + 1);
		if (!data) {
			ok = -1;
			goto out;
		}
		if (n > 0 && read_exact(req, data, (DWORD)n) != 0) {
			free(data);
			ok = -1;
			goto out;
		}
		data[n] = 0;
		if (type == 'D') {
			PostMessageA(hwnd, WM_APP_CHUNK, 0, (LPARAM)data);
		} else if (type == 'R') {
			PostMessageA(hwnd, WM_APP_REASON, 0, (LPARAM)data);
		} else if (type == 'E') {
			PostMessageA(hwnd, WM_APP_ERROR, 0, (LPARAM)data);
		} else if (type == 'T') {
			ToolCall tc;
			if (parse_tool_payload(data, n, &tc) == 0) {
				ToolCall *ni = (ToolCall *)realloc(tl->items,
								  sizeof(ToolCall) * (size_t)(tl->n + 1));
				if (ni) {
					tl->items = ni;
					tl->items[tl->n++] = tc;
				} else {
					free_tool_call(&tc);
				}
			}
			free(data);
		} else if (type == 'Z') {
			if (n == 5 && !memcmp(data, "tools", 5))
				*need_tools = 1;
			free(data);
			ok = 0;
			break;
		} else {
			free(data);
		}
	}
out:
	if (req)
		InternetCloseHandle(req);
	if (conn)
		InternetCloseHandle(conn);
	if (inet)
		InternetCloseHandle(inet);
	return ok;
}

static DWORD WINAPI net_thread(LPVOID param)
{
	Job *job = (Job *)param;
	char *body = job->body;
	int round;

	for (round = 0; round < MAX_TOOL_ROUNDS; round++) {
		ToolList tl = {0};
		int need = 0;
		int rc;

		if (g_cancel)
			break;
		rc = do_round(job->hwnd, body, &tl, &need);
		free(body);
		body = NULL;
		if (rc != 0 || g_cancel) {
			free_tool_list(&tl);
			break;
		}
		if (!need || tl.n == 0) {
			free_tool_list(&tl);
			break;
		}
		body = execute_and_build(&tl, job->hwnd);
		free_tool_list(&tl);
		if (!body)
			break;
		if (g_cancel) {
			free(body);
			body = NULL;
			break;
		}
	}
	if (g_cancel)
		PostMessageA(job->hwnd, WM_APP_LOG, 0, (LPARAM)dupstr("\r\n[stopped]\r\n"));
	if (body && round >= MAX_TOOL_ROUNDS)
		post_error(job->hwnd, "tool loop limit reached");
	free(body);
	PostMessageA(job->hwnd, WM_APP_DONE, 0, 0);
	free(job);
	return 0;
}

static void do_send(void);

static void do_continue(void)
{
	if (g_busy)
		return;
	SetWindowTextA(g_input, "Continue with the task. Do not stop until it is complete; use tools, or use ask_user if you need input.");
	do_send();
}

static void do_stop(void)
{
	if (!g_busy)
		return;
	InterlockedExchange(&g_cancel, 1);
	set_status("Stopping...");
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
	InterlockedExchange(&g_cancel, 0);
	g_reason_started = 0;
	EnableWindow(g_send, FALSE);
	if (g_stop)
		EnableWindow(g_stop, TRUE);
	SetWindowTextA(g_input, "");
	transcript_append_styled("You: ", RGB(0, 0, 180), 1, 0);
	transcript_append(text);
	log_append("You: ");
	log_append(text);
	log_append("\r\n");
	transcript_append("\r\n\r\n");
	transcript_append_styled("XPCode: ", RGB(0, 128, 0), 1, 0);
	sb_free(&g_reply);
	body = build_start_body(text);
	set_status("Thinking...");
	free(text);
	if (!body) {
		g_busy = 0;
		EnableWindow(g_send, TRUE);
		set_status("Out of memory");
		return;
	}

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
	if (msg == WM_KEYDOWN && wParam == VK_RETURN &&
	    (GetKeyState(VK_CONTROL) & 0x8000)) {
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

	AppendMenuA(file, MF_STRING, IDM_NEW, "&New Chat");
	AppendMenuA(file, MF_STRING, IDM_CONTINUE, "&Continue\tF5");
	AppendMenuA(file, MF_SEPARATOR, 0, NULL);
	AppendMenuA(file, MF_STRING, IDM_EXIT, "E&xit");
	AppendMenuA(menu, MF_POPUP, (UINT_PTR)file, "&File");
	AppendMenuA(settings, MF_STRING, IDM_SETTINGS, "&Settings...");
	AppendMenuA(settings, MF_STRING, IDM_SETTINGS_FILE, "Open Settings &File");
	AppendMenuA(settings, MF_STRING, IDM_WORKDIR, "Choose &Working Folder...");
	AppendMenuA(menu, MF_POPUP, (UINT_PTR)settings, "&Settings");
	AppendMenuA(help, MF_STRING, IDM_LOG, "Open &Log File");
	AppendMenuA(help, MF_SEPARATOR, 0, NULL);
	AppendMenuA(help, MF_STRING, IDM_ABOUT, "&About");
	AppendMenuA(menu, MF_POPUP, (UINT_PTR)help, "&Help");
	return menu;
}

static void open_log(void)
{
	char path[MAX_PATH];

	get_log_path(path);
	ShellExecuteA(g_hwnd, "open", "notepad.exe", path, NULL, SW_SHOW);
}

static void open_settings_file(void)
{
	char path[MAX_PATH];

	get_ini_path(path);
	ShellExecuteA(g_hwnd, "open", "notepad.exe", path, NULL, SW_SHOW);
}

static void new_session_id(void)
{
	GUID g;

	if (CoCreateGuid(&g) == S_OK) {
		sprintf(g_session, "%08lx-%04x-%04x-%02x%02x-%02x%02x%02x%02x%02x%02x",
			(unsigned long)g.Data1, g.Data2, g.Data3,
			g.Data4[0], g.Data4[1], g.Data4[2], g.Data4[3],
			g.Data4[4], g.Data4[5], g.Data4[6], g.Data4[7]);
	} else {
		sprintf(g_session, "xp-%lu-%lu", (unsigned long)GetCurrentProcessId(),
			(unsigned long)GetTickCount());
	}
}

static void update_title(void)
{
	char title[MAX_PATH + 16];

	snprintf(title, sizeof title, "XPCode - %s", g_workdir[0] ? g_workdir : "");
	SetWindowTextA(g_hwnd, title);
}

static void new_chat(void)
{
	if (g_busy)
		return;
	SetWindowTextW(g_transcript, L"");
	sb_free(&g_reply);
	new_session_id();
	log_append("\r\n=== new chat ===\r\n");
	set_status("New chat");
	SetFocus(g_input);
}

static void choose_workdir(void)
{
	BROWSEINFOA bi;
	LPITEMIDLIST pidl;
	char path[MAX_PATH];
	char ini[MAX_PATH];

	if (g_busy)
		return;
	memset(&bi, 0, sizeof bi);
	bi.hwndOwner = g_hwnd;
	bi.lpszTitle = "Choose the XPCode project folder";
	bi.ulFlags = BIF_RETURNONLYFSDIRS;
	pidl = SHBrowseForFolderA(&bi);
	if (!pidl)
		return;
	if (SHGetPathFromIDListA(pidl, path)) {
		strncpy(g_workdir, path, sizeof g_workdir - 1);
		g_workdir[sizeof g_workdir - 1] = 0;
		get_ini_path(ini);
		WritePrivateProfileStringA("server", "workdir", g_workdir, ini);
		new_chat();
		update_title();
		set_status(g_workdir);
	}
	ILFree(pidl);
}

static INT_PTR CALLBACK settings_proc(HWND dlg, UINT msg, WPARAM wParam, LPARAM lParam)
{
	(void)lParam;
	switch (msg) {
	case WM_INITDIALOG: {
		char port[16];
		SetDlgItemTextA(dlg, IDC_SET_HOST, g_host);
		sprintf(port, "%d", g_port);
		SetDlgItemTextA(dlg, IDC_SET_PORT, port);
		SetDlgItemTextA(dlg, IDC_SET_MODEL, g_model);
		SetDlgItemTextA(dlg, IDC_SET_WORKDIR, g_workdir);
		CheckDlgButton(dlg, IDC_SET_APPROVE, g_approve ? BST_CHECKED : BST_UNCHECKED);
		return TRUE;
	}
	case WM_COMMAND:
		switch (LOWORD(wParam)) {
		case IDC_SET_BROWSE: {
			BROWSEINFOA bi;
			LPITEMIDLIST pidl;
			char path[MAX_PATH];

			memset(&bi, 0, sizeof bi);
			bi.hwndOwner = dlg;
			bi.lpszTitle = "Choose the XPCode project folder";
			bi.ulFlags = BIF_RETURNONLYFSDIRS;
			pidl = SHBrowseForFolderA(&bi);
			if (pidl) {
				if (SHGetPathFromIDListA(pidl, path))
					SetDlgItemTextA(dlg, IDC_SET_WORKDIR, path);
				ILFree(pidl);
			}
			return TRUE;
		}
		case IDC_SET_OK: {
			char host[128], model[128], work[MAX_PATH], port[16];
			char ini[MAX_PATH];
			int p;

			GetDlgItemTextA(dlg, IDC_SET_HOST, host, sizeof host);
			GetDlgItemTextA(dlg, IDC_SET_PORT, port, sizeof port);
			GetDlgItemTextA(dlg, IDC_SET_MODEL, model, sizeof model);
			GetDlgItemTextA(dlg, IDC_SET_WORKDIR, work, sizeof work);
			g_approve = IsDlgButtonChecked(dlg, IDC_SET_APPROVE) == BST_CHECKED;
			p = atoi(port);
			if (p < 1 || p > 65535)
				p = 8080;
			get_ini_path(ini);
			WritePrivateProfileStringA("server", "host", host[0] ? host : "Garden.lan", ini);
			sprintf(port, "%d", p);
			WritePrivateProfileStringA("server", "port", port, ini);
			WritePrivateProfileStringA("server", "model", model[0] ? model : "deepseek-flash", ini);
			WritePrivateProfileStringA("server", "workdir", work[0] ? work : g_workdir, ini);
			WritePrivateProfileStringA("server", "approve", g_approve ? "1" : "0", ini);
			EndDialog(dlg, 1);
			return TRUE;
		}
		case IDC_SET_CANCEL:
			EndDialog(dlg, 0);
			return TRUE;
		}
		break;
	case WM_CLOSE:
		EndDialog(dlg, 0);
		return TRUE;
	}
	return FALSE;
}

static void show_settings(void)
{
	INT_PTR r;

	if (g_busy)
		return;
	r = DialogBoxParamA(GetModuleHandle(NULL), MAKEINTRESOURCEA(IDD_SETTINGS),
			    g_hwnd, settings_proc, 0);
	if (r == 1) {
		load_settings();
		new_chat();
		update_title();
		set_status("Settings saved");
	}
}

static LRESULT CALLBACK wnd_proc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
	switch (msg) {
	case WM_CREATE:
		LoadLibraryA("riched20.dll");
		g_font = CreateFontA(-12, 0, 0, 0, FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET,
				     OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, DEFAULT_QUALITY,
				     DEFAULT_PITCH | FF_SWISS, "Tahoma");
		g_transcript = CreateWindowExA(WS_EX_CLIENTEDGE, "RichEdit20W", "",
					       WS_CHILD | WS_VISIBLE | WS_VSCROLL | ES_MULTILINE |
					       ES_READONLY | ES_AUTOVSCROLL | ES_NOHIDESEL,
					       0, 0, 10, 10, hwnd, (HMENU)IDC_TRANSCRIPT,
					       GetModuleHandle(NULL), NULL);
		g_input = CreateWindowExA(WS_EX_CLIENTEDGE, "EDIT", "",
					  WS_CHILD | WS_VISIBLE | WS_VSCROLL | ES_MULTILINE |
					  ES_AUTOVSCROLL, 0, 0, 10, 10, hwnd, (HMENU)IDC_INPUT,
					  GetModuleHandle(NULL), NULL);
		g_send = CreateWindowExA(0, "BUTTON", "Send",
					 WS_CHILD | WS_VISIBLE | BS_DEFPUSHBUTTON,
					 0, 0, 10, 10, hwnd, (HMENU)IDC_SEND,
					 GetModuleHandle(NULL), NULL);
		g_stop = CreateWindowExA(0, "BUTTON", "Stop",
					 WS_CHILD | WS_VISIBLE,
					 0, 0, 10, 10, hwnd, (HMENU)IDC_STOP,
					 GetModuleHandle(NULL), NULL);
		if (g_stop)
			EnableWindow(g_stop, FALSE);
		g_status = CreateWindowExA(0, "STATIC", "Ready (Ctrl+Enter sends)",
					   WS_CHILD | WS_VISIBLE | SS_LEFT,
					   0, 0, 10, 10, hwnd, (HMENU)IDC_STATUS,
					   GetModuleHandle(NULL), NULL);
		if (g_font) {
			SendMessageA(g_transcript, WM_SETFONT, (WPARAM)g_font, TRUE);
			SendMessageA(g_input, WM_SETFONT, (WPARAM)g_font, TRUE);
			SendMessageA(g_send, WM_SETFONT, (WPARAM)g_font, TRUE);
			SendMessageA(g_stop, WM_SETFONT, (WPARAM)g_font, TRUE);
			SendMessageA(g_status, WM_SETFONT, (WPARAM)g_font, TRUE);
		}
		g_edit_proc = (WNDPROC)SetWindowLongPtrA(g_input, GWLP_WNDPROC,
							 (LONG_PTR)edit_proc);
		return 0;
	case WM_SIZE: {
		int w = LOWORD(lParam);
		int h = HIWORD(lParam);
		int margin = 8;
		int input_h = 64;
		int send_w = 74;
		int stop_w = 60;
		int status_h = 20;

		MoveWindow(g_transcript, margin, margin, w - 2 * margin,
			   h - 2 * margin - input_h - status_h - 8, TRUE);
		MoveWindow(g_input, margin, h - margin - status_h - input_h,
			   w - 2 * margin - send_w - stop_w - 8, input_h, TRUE);
		MoveWindow(g_send, w - margin - send_w,
			   h - margin - status_h - 26, send_w, 26, TRUE);
		MoveWindow(g_stop, w - margin - send_w - stop_w - 4,
			   h - margin - status_h - 26, stop_w, 26, TRUE);
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
		case IDC_STOP:
			do_stop();
			return 0;
		case IDM_NEW:
			new_chat();
			return 0;
		case IDM_CONTINUE:
			do_continue();
			return 0;
		case IDM_SETTINGS:
			show_settings();
			return 0;
		case IDM_SETTINGS_FILE:
			open_settings_file();
			return 0;
		case IDM_WORKDIR:
			choose_workdir();
			return 0;
		case IDM_EXIT:
			DestroyWindow(hwnd);
			return 0;
		case IDM_LOG:
			open_log();
			return 0;
		case IDM_ABOUT:
			MessageBoxA(hwnd,
				    "XPCode 0.3\n\nA native Windows XP AI coding client.\n\n"
				    "Streams over plain HTTP from the Go bridge and runs tools locally.",
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
	case WM_APP_REASON: {
		char *s = (char *)lParam;
		if (s) {
			if (!g_reason_started) {
				transcript_append_styled("\r\n[thinking]\r\n", RGB(130, 130, 130), 1, 0);
				g_reason_started = 1;
			}
			transcript_append_styled(s, RGB(130, 130, 130), 0, 1);
			free(s);
		}
		return 0;
	}
	case WM_APP_LOG: {
		char *s = (char *)lParam;
		if (s) {
			transcript_append_styled(s, RGB(0, 110, 130), 1, 0);
			log_append(s);
			free(s);
		}
		return 0;
	}
	case WM_APP_ERROR: {
		char *s = (char *)lParam;
		log_append("[error] ");
		if (s)
			log_append(s);
		log_append("\r\n");
		transcript_append_styled("\r\n[error] ", RGB(180, 0, 0), 1, 0);
		if (s) {
			transcript_append_styled(s, RGB(180, 0, 0), 0, 0);
			free(s);
		}
		set_status("Error");
		return 0;
	}
	case WM_APP_STATUS: {
		char *s = (char *)lParam;
		if (s) {
			set_status(s);
			free(s);
		}
		return 0;
	}
	case WM_APP_DONE:
		InterlockedExchange(&g_cancel, 0);
		EnableWindow(g_stop, FALSE);
		if (g_reply.p) {
			log_append("XPCode: ");
			log_append(g_reply.p);
			log_append("\r\n\r\n");
		}
		transcript_append(g_reply.len ? "\r\n\r\n" : "\r\n");
		sb_free(&g_reply);
		g_busy = 0;
		EnableWindow(g_send, TRUE);
		set_status("Ready (Ctrl+Enter sends)");
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
	ACCEL acc[5];
	HACCEL accel;
	int ww = 760, wh = 560, wx, wy;

	(void)prev;
	(void)show;

	load_settings();
	new_session_id();

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
	update_title();
	ShowWindow(g_hwnd, SW_SHOW);
	UpdateWindow(g_hwnd);

	if (cmdline && strstr(cmdline, "selftest")) {
		SetWindowTextA(g_input, "Hello from XPCode. Reply in one short sentence.");
		do_send();
	}

	acc[0].fVirt = FVIRTKEY | FCONTROL;
	acc[0].key = 'N';
	acc[0].cmd = IDM_NEW;
	acc[1].fVirt = FVIRTKEY | FCONTROL;
	acc[1].key = 'O';
	acc[1].cmd = IDM_WORKDIR;
	acc[2].fVirt = FVIRTKEY | FCONTROL;
	acc[2].key = VK_OEM_COMMA;
	acc[2].cmd = IDM_SETTINGS;
	acc[3].fVirt = FVIRTKEY;
	acc[3].key = VK_ESCAPE;
	acc[3].cmd = IDC_STOP;
	acc[4].fVirt = FVIRTKEY;
	acc[4].key = VK_F5;
	acc[4].cmd = IDM_CONTINUE;
	accel = CreateAcceleratorTableA(acc, 5);

	while (GetMessageA(&msg, NULL, 0, 0) > 0) {
		if (!TranslateAcceleratorA(g_hwnd, accel, &msg)) {
			TranslateMessage(&msg);
			DispatchMessageA(&msg);
		}
	}
	return (int)msg.wParam;
}
