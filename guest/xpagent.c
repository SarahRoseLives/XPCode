#define WINVER 0x0501
#define _WIN32_WINNT 0x0501
#define WIN32_LEAN_AND_MEAN

#include <winsock2.h>
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>

#define XPAGENT_PORT 4444
#define XPAGENT_VERSION "0.2"
#define MAX_FIELDS 16
#define MAX_FIELD (64L * 1024 * 1024)

typedef struct {
    char *data;
    long len;
} Field;

static LONG g_seq;

static void xlog(const char *fmt, ...)
{
    char path[MAX_PATH];
    DWORD n = GetTempPathA(MAX_PATH, path);
    FILE *fp;
    va_list ap;

    if (n == 0 || n + 16 >= MAX_PATH)
        return;
    strcat(path, "xpagent.log");
    fp = fopen(path, "a");
    if (!fp)
        return;
    va_start(ap, fmt);
    vfprintf(fp, fmt, ap);
    va_end(ap);
    fputc('\n', fp);
    fclose(fp);
}

static int send_all(SOCKET s, const void *buf, int n)
{
    const char *p = (const char *)buf;

    while (n > 0) {
        int r = send(s, p, n, 0);
        if (r <= 0)
            return -1;
        p += r;
        n -= r;
    }
    return 0;
}

static int recv_all(SOCKET s, char *buf, int n)
{
    int off = 0;

    while (off < n) {
        int r = recv(s, buf + off, n - off, 0);
        if (r <= 0)
            return -1;
        off += r;
    }
    return 0;
}

static int recv_line(SOCKET s, char *buf, int cap)
{
    int i = 0;

    while (i < cap - 1) {
        char c;
        if (recv(s, &c, 1, 0) <= 0)
            return -1;
        if (c == '\n') {
            if (i > 0 && buf[i - 1] == '\r')
                i--;
            buf[i] = '\0';
            return i;
        }
        buf[i++] = c;
    }
    return -1;
}

static int recv_field(SOCKET s, Field *f)
{
    char line[32];
    long n;
    char *p;

    f->data = NULL;
    f->len = 0;
    if (recv_line(s, line, sizeof line) < 0)
        return -1;
    n = atol(line);
    if (n < 0 || n > MAX_FIELD)
        return -1;
    p = (char *)malloc((size_t)n + 1);
    if (!p)
        return -1;
    if (n > 0 && recv_all(s, p, (int)n) < 0) {
        free(p);
        return -1;
    }
    p[n] = '\0';
    f->data = p;
    f->len = n;
    return 0;
}

static int send_resp(SOCKET s, const char *status, Field *f, int n)
{
    char hdr[64];
    int k = sprintf(hdr, "%s %d\n", status, n);
    int i;

    if (send_all(s, hdr, k) < 0)
        return -1;
    for (i = 0; i < n; i++) {
        char lh[32];
        int m = sprintf(lh, "%ld\n", f[i].len);
        if (send_all(s, lh, m) < 0)
            return -1;
        if (f[i].len > 0 && send_all(s, f[i].data, (int)f[i].len) < 0)
            return -1;
    }
    return 0;
}

static void err_resp(SOCKET s, const char *msg)
{
    Field o[1];
    o[0].data = (char *)msg;
    o[0].len = (long)strlen(msg);
    send_resp(s, "ERR", o, 1);
}

static void ok_text(SOCKET s, const char *msg)
{
    Field o[1];
    o[0].data = (char *)msg;
    o[0].len = (long)strlen(msg);
    send_resp(s, "OK", o, 1);
}

static void do_exec(SOCKET s, Field *f, int nf)
{
    char tmpdir[MAX_PATH];
    char batpath[MAX_PATH * 2];
    char outpath[MAX_PATH * 2];
    char cmdline[MAX_PATH * 2 + 64];
    SECURITY_ATTRIBUTES sa;
    HANDLE hout = INVALID_HANDLE_VALUE;
    HANDLE nul = INVALID_HANDLE_VALUE;
    HANDLE hf;
    STARTUPINFOA si;
    PROCESS_INFORMATION pi;
    DWORD wrote = 0;
    DWORD seq;
    DWORD code = 0;
    char codebuf[32];
    Field out[2];
    char *buf = NULL;
    DWORD size = 0, off = 0;

    if (nf < 1) {
        err_resp(s, "exec: command required");
        return;
    }
    if (GetTempPathA(MAX_PATH, tmpdir) == 0) {
        err_resp(s, "temp path unavailable");
        return;
    }
    seq = (DWORD)InterlockedIncrement(&g_seq);
    sprintf(batpath, "%sxpagent_%lu_%lu.bat", tmpdir,
            (unsigned long)GetCurrentProcessId(), (unsigned long)seq);
    sprintf(outpath, "%sxpagent_%lu_%lu.out", tmpdir,
            (unsigned long)GetCurrentProcessId(), (unsigned long)seq);

    hf = CreateFileA(batpath, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS,
                     FILE_ATTRIBUTE_NORMAL, NULL);
    if (hf == INVALID_HANDLE_VALUE) {
        err_resp(s, "cannot write temp batch");
        return;
    }
    WriteFile(hf, "@echo off\r\n", 11, &wrote, NULL);
    WriteFile(hf, f[0].data, (DWORD)f[0].len, &wrote, NULL);
    WriteFile(hf, "\r\n", 2, &wrote, NULL);
    CloseHandle(hf);

    sa.nLength = sizeof sa;
    sa.lpSecurityDescriptor = NULL;
    sa.bInheritHandle = TRUE;

    hout = CreateFileA(outpath, GENERIC_WRITE,
                       FILE_SHARE_READ | FILE_SHARE_WRITE, &sa,
                       CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (hout == INVALID_HANDLE_VALUE) {
        DeleteFileA(batpath);
        err_resp(s, "cannot create temp output");
        return;
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

    sprintf(cmdline, "cmd.exe /c \"%s\"", batpath);
    memset(&pi, 0, sizeof pi);

    if (!CreateProcessA(NULL, cmdline, NULL, NULL, TRUE, CREATE_NO_WINDOW,
                        NULL, NULL, &si, &pi)) {
        CloseHandle(hout);
        if (nul != INVALID_HANDLE_VALUE)
            CloseHandle(nul);
        DeleteFileA(batpath);
        DeleteFileA(outpath);
        err_resp(s, "CreateProcess failed");
        return;
    }
    CloseHandle(hout);
    if (nul != INVALID_HANDLE_VALUE)
        CloseHandle(nul);

    WaitForSingleObject(pi.hProcess, INFINITE);
    GetExitCodeProcess(pi.hProcess, &code);
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    DeleteFileA(batpath);

    {
        HANDLE hr = CreateFileA(outpath, GENERIC_READ,
                                FILE_SHARE_READ | FILE_SHARE_WRITE, NULL,
                                OPEN_EXISTING, 0, NULL);
        if (hr != INVALID_HANDLE_VALUE) {
            size = GetFileSize(hr, NULL);
            if (size == INVALID_FILE_SIZE || size > MAX_FIELD)
                size = 0;
            buf = (char *)malloc(size ? (size_t)size : 1);
            if (buf) {
                while (off < size) {
                    DWORD got = 0;
                    if (!ReadFile(hr, buf + off, size - off, &got, NULL) ||
                        got == 0)
                        break;
                    off += got;
                }
            }
            CloseHandle(hr);
        }
        DeleteFileA(outpath);
    }

    if (!buf) {
        buf = (char *)malloc(1);
        off = 0;
    }
    sprintf(codebuf, "%lu", (unsigned long)code);
    out[0].data = codebuf;
    out[0].len = (long)strlen(codebuf);
    out[1].data = buf ? buf : (char *)"";
    out[1].len = (long)off;
    send_resp(s, "OK", out, 2);
    free(buf);
}

static void do_put(SOCKET s, Field *f, int nf)
{
    HANDLE h;
    DWORD wrote = 0;

    if (nf < 2) {
        err_resp(s, "put: path and data required");
        return;
    }
    h = CreateFileA(f[0].data, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS,
                    FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE) {
        err_resp(s, "cannot open destination");
        return;
    }
    if (f[1].len > 0 && !WriteFile(h, f[1].data, (DWORD)f[1].len, &wrote, NULL)) {
        CloseHandle(h);
        err_resp(s, "write failed");
        return;
    }
    CloseHandle(h);
    ok_text(s, "ok");
}

static void do_get(SOCKET s, Field *f, int nf)
{
    HANDLE h;
    DWORD size, off = 0;
    char *b;

    if (nf < 1) {
        err_resp(s, "get: path required");
        return;
    }
    h = CreateFileA(f[0].data, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
                    NULL, OPEN_EXISTING, 0, NULL);
    if (h == INVALID_HANDLE_VALUE) {
        err_resp(s, "cannot open file");
        return;
    }
    size = GetFileSize(h, NULL);
    if (size == INVALID_FILE_SIZE || size > MAX_FIELD) {
        CloseHandle(h);
        err_resp(s, "file too large");
        return;
    }
    b = (char *)malloc(size ? (size_t)size : 1);
    if (!b) {
        CloseHandle(h);
        err_resp(s, "out of memory");
        return;
    }
    while (off < size) {
        DWORD got = 0;
        if (!ReadFile(h, b + off, size - off, &got, NULL) || got == 0)
            break;
        off += got;
    }
    CloseHandle(h);
    {
        Field o[1];
        o[0].data = b;
        o[0].len = (long)off;
        send_resp(s, "OK", o, 1);
    }
    free(b);
}

static void do_tail(SOCKET s, Field *f, int nf)
{
    HANDLE h;
    DWORD size, want, start;
    char *b;

    if (nf < 1) {
        err_resp(s, "tail: path required");
        return;
    }
    want = (nf > 1) ? (DWORD)atol(f[1].data) : 65536;
    if (want > MAX_FIELD)
        want = MAX_FIELD;
    h = CreateFileA(f[0].data, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
                    NULL, OPEN_EXISTING, 0, NULL);
    if (h == INVALID_HANDLE_VALUE) {
        err_resp(s, "cannot open file");
        return;
    }
    size = GetFileSize(h, NULL);
    start = (size > want) ? size - want : 0;
    SetFilePointer(h, (LONG)start, NULL, FILE_BEGIN);
    b = (char *)malloc((size - start) ? (size_t)(size - start) : 1);
    if (!b) {
        CloseHandle(h);
        err_resp(s, "out of memory");
        return;
    }
    {
        DWORD got = 0, off = 0;
        while (off < size - start) {
            if (!ReadFile(h, b + off, size - start - off, &got, NULL) || got == 0)
                break;
            off += got;
        }
        CloseHandle(h);
        {
            Field o[1];
            o[0].data = b;
            o[0].len = (long)off;
            send_resp(s, "OK", o, 1);
        }
    }
    free(b);
}

static void put16(BYTE *p, unsigned v)
{
    p[0] = (BYTE)(v & 0xff);
    p[1] = (BYTE)((v >> 8) & 0xff);
}

static void put32(BYTE *p, unsigned long v)
{
    p[0] = (BYTE)(v & 0xff);
    p[1] = (BYTE)((v >> 8) & 0xff);
    p[2] = (BYTE)((v >> 16) & 0xff);
    p[3] = (BYTE)((v >> 24) & 0xff);
}

static void do_shot(SOCKET s, Field *f, int nf)
{
    int x, y;
    DWORD w, h, imgsize;
    HDC sdc, mdc;
    BITMAPINFO bi;
    void *bits = NULL;
    HBITMAP bmp;
    HGDIOBJ old;
    BYTE *out;
    DWORD total;

    (void)f;
    (void)nf;

    x = GetSystemMetrics(SM_XVIRTUALSCREEN);
    y = GetSystemMetrics(SM_YVIRTUALSCREEN);
    w = (DWORD)GetSystemMetrics(SM_CXVIRTUALSCREEN);
    h = (DWORD)GetSystemMetrics(SM_CYVIRTUALSCREEN);
    if (w == 0 || h == 0) {
        err_resp(s, "no screen");
        return;
    }
    sdc = GetDC(NULL);
    if (!sdc) {
        err_resp(s, "GetDC failed");
        return;
    }
    mdc = CreateCompatibleDC(sdc);
    memset(&bi, 0, sizeof bi);
    bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bi.bmiHeader.biWidth = (LONG)w;
    bi.bmiHeader.biHeight = -(LONG)h;
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    bi.bmiHeader.biCompression = BI_RGB;
    bmp = CreateDIBSection(sdc, &bi, DIB_RGB_COLORS, &bits, NULL, 0);
    if (!bmp || !bits) {
        DeleteDC(mdc);
        ReleaseDC(NULL, sdc);
        err_resp(s, "CreateDIBSection failed");
        return;
    }
    old = SelectObject(mdc, bmp);
    BitBlt(mdc, 0, 0, (int)w, (int)h, sdc, x, y, SRCCOPY);

    imgsize = w * h * 4;
    total = 54 + imgsize;
    out = (BYTE *)malloc(total);
    if (!out) {
        SelectObject(mdc, old);
        DeleteObject(bmp);
        DeleteDC(mdc);
        ReleaseDC(NULL, sdc);
        err_resp(s, "out of memory");
        return;
    }
    out[0] = 'B';
    out[1] = 'M';
    put32(out + 2, total);
    put16(out + 6, 0);
    put16(out + 8, 0);
    put32(out + 10, 54);
    put32(out + 14, 40);
    put32(out + 18, w);
    put32(out + 22, (unsigned long)(-(long)h));
    put16(out + 26, 1);
    put16(out + 28, 32);
    put32(out + 30, 0);
    put32(out + 34, imgsize);
    put32(out + 38, 2835);
    put32(out + 42, 2835);
    put32(out + 46, 0);
    put32(out + 50, 0);
    memcpy(out + 54, bits, imgsize);

    {
        Field o[1];
        o[0].data = (char *)out;
        o[0].len = (long)total;
        send_resp(s, "OK", o, 1);
    }
    free(out);
    SelectObject(mdc, old);
    DeleteObject(bmp);
    DeleteDC(mdc);
    ReleaseDC(NULL, sdc);
}

static void do_move(SOCKET s, Field *f, int nf)
{
    if (nf < 2) {
        err_resp(s, "move: x and y required");
        return;
    }
    SetCursorPos(atoi(f[0].data), atoi(f[1].data));
    ok_text(s, "ok");
}

static void do_click(SOCKET s, Field *f, int nf)
{
    const char *btn = "left";
    DWORD down = MOUSEEVENTF_LEFTDOWN, up = MOUSEEVENTF_LEFTUP;
    int twice = 0;

    if (nf >= 3) {
        SetCursorPos(atoi(f[0].data), atoi(f[1].data));
        btn = f[2].data;
    } else if (nf >= 1) {
        btn = f[0].data;
    }
    if (strcmp(btn, "right") == 0) {
        down = MOUSEEVENTF_RIGHTDOWN;
        up = MOUSEEVENTF_RIGHTUP;
    } else if (strcmp(btn, "middle") == 0) {
        down = MOUSEEVENTF_MIDDLEDOWN;
        up = MOUSEEVENTF_MIDDLEUP;
    } else if (strcmp(btn, "double") == 0) {
        twice = 1;
    }
    mouse_event(down, 0, 0, 0, 0);
    mouse_event(up, 0, 0, 0, 0);
    if (twice) {
        Sleep(60);
        mouse_event(down, 0, 0, 0, 0);
        mouse_event(up, 0, 0, 0, 0);
    }
    ok_text(s, "ok");
}

static void send_unicode(wchar_t ch, WORD vk)
{
    INPUT in[2];
    memset(in, 0, sizeof in);
    in[0].type = INPUT_KEYBOARD;
    if (vk) {
        in[0].ki.wVk = vk;
    } else {
        in[0].ki.wScan = ch;
        in[0].ki.dwFlags = KEYEVENTF_UNICODE;
    }
    in[1] = in[0];
    in[1].ki.dwFlags |= KEYEVENTF_KEYUP;
    SendInput(2, in, sizeof(INPUT));
}

static void do_key(SOCKET s, Field *f, int nf)
{
    int wlen, i;
    wchar_t *w;

    if (nf < 1) {
        err_resp(s, "key: text required");
        return;
    }
    wlen = MultiByteToWideChar(CP_UTF8, 0, f[0].data, (int)f[0].len, NULL, 0);
    if (wlen <= 0) {
        ok_text(s, "ok");
        return;
    }
    w = (wchar_t *)malloc((size_t)(wlen + 1) * sizeof(wchar_t));
    if (!w) {
        err_resp(s, "out of memory");
        return;
    }
    MultiByteToWideChar(CP_UTF8, 0, f[0].data, (int)f[0].len, w, wlen);
    for (i = 0; i < wlen; i++) {
        if (w[i] == L'\n')
            send_unicode(w[i], VK_RETURN);
        else if (w[i] == L'\t')
            send_unicode(w[i], VK_TAB);
        else if (w[i] == L'\r')
            continue;
        else
            send_unicode(w[i], 0);
    }
    free(w);
    ok_text(s, "ok");
}

static int serve(SOCKET s)
{
    char hdr[128];
    int quit = 0;

    while (!quit) {
        char verb[32];
        int nf = 0, i;
        Field f[MAX_FIELDS];

        if (recv_line(s, hdr, sizeof hdr) <= 0)
            return -1;
        if (sscanf(hdr, "%31s %d", verb, &nf) != 2)
            return -1;
        if (nf < 0 || nf > MAX_FIELDS)
            return -1;

        for (i = 0; i < nf; i++) {
            if (recv_field(s, &f[i]) < 0) {
                int j;
                for (j = 0; j < i; j++)
                    free(f[j].data);
                return -1;
            }
        }

        if (strcmp(verb, "PING") == 0) {
            char pong[32];
            sprintf(pong, "PONG %s", XPAGENT_VERSION);
            ok_text(s, pong);
        } else if (strcmp(verb, "VERSION") == 0) {
            ok_text(s, XPAGENT_VERSION);
        } else if (strcmp(verb, "EXEC") == 0) {
            do_exec(s, f, nf);
        } else if (strcmp(verb, "PUT") == 0) {
            do_put(s, f, nf);
        } else if (strcmp(verb, "GET") == 0) {
            do_get(s, f, nf);
        } else if (strcmp(verb, "TAIL") == 0) {
            do_tail(s, f, nf);
        } else if (strcmp(verb, "SHOT") == 0) {
            do_shot(s, f, nf);
        } else if (strcmp(verb, "MOVE") == 0) {
            do_move(s, f, nf);
        } else if (strcmp(verb, "CLICK") == 0) {
            do_click(s, f, nf);
        } else if (strcmp(verb, "KEY") == 0) {
            do_key(s, f, nf);
        } else if (strcmp(verb, "QUIT") == 0) {
            send_resp(s, "OK", NULL, 0);
            quit = 1;
        } else {
            err_resp(s, "unknown verb");
        }

        for (i = 0; i < nf; i++)
            free(f[i].data);
    }
    return 0;
}

static DWORD WINAPI client_thread(LPVOID param)
{
    SOCKET c = (SOCKET)(UINT_PTR)param;
    serve(c);
    closesocket(c);
    return 0;
}

static void set_autostart(const char *exe)
{
    HKEY k;
    if (RegCreateKeyExA(HKEY_CURRENT_USER,
                        "Software\\Microsoft\\Windows\\CurrentVersion\\Run",
                        0, NULL, 0, KEY_SET_VALUE, NULL, &k, NULL) ==
        ERROR_SUCCESS) {
        char q[MAX_PATH + 4];
        sprintf(q, "\"%s\"", exe);
        RegSetValueExA(k, "xpagent", 0, REG_SZ, (const BYTE *)q,
                       (DWORD)strlen(q) + 1);
        RegCloseKey(k);
    }
}

static void clear_autostart(void)
{
    HKEY k;
    if (RegOpenKeyExA(HKEY_CURRENT_USER,
                      "Software\\Microsoft\\Windows\\CurrentVersion\\Run",
                      0, KEY_SET_VALUE, &k) == ERROR_SUCCESS) {
        RegDeleteValueA(k, "xpagent");
        RegCloseKey(k);
    }
}

int WINAPI WinMain(HINSTANCE hi, HINSTANCE hp, LPSTR cmdline, int show)
{
    char exe[MAX_PATH];
    WSADATA wsa;
    SOCKET ls;
    struct sockaddr_in a;
    BOOL yes = 1;
    HANDLE mutex;

    (void)hi;
    (void)hp;
    (void)show;

    GetModuleFileNameA(NULL, exe, MAX_PATH);

    if (cmdline && strstr(cmdline, "uninstall")) {
        clear_autostart();
        return 0;
    }
    if (cmdline && strstr(cmdline, "install")) {
        set_autostart(exe);
        return 0;
    }

    mutex = CreateMutexA(NULL, FALSE, "xpagent_singleton");
    if (mutex && GetLastError() == ERROR_ALREADY_EXISTS)
        return 0;

    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) {
        xlog("WSAStartup failed");
        return 1;
    }

    ls = socket(AF_INET, SOCK_STREAM, 0);
    if (ls == INVALID_SOCKET) {
        xlog("socket failed %d", WSAGetLastError());
        return 1;
    }
    setsockopt(ls, SOL_SOCKET, SO_REUSEADDR, (const char *)&yes, sizeof yes);

    memset(&a, 0, sizeof a);
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = htonl(INADDR_ANY);
    a.sin_port = htons(XPAGENT_PORT);
    if (bind(ls, (struct sockaddr *)&a, sizeof a) == SOCKET_ERROR) {
        xlog("bind failed %d", WSAGetLastError());
        return 1;
    }
    if (listen(ls, 4) == SOCKET_ERROR) {
        xlog("listen failed %d", WSAGetLastError());
        return 1;
    }

    xlog("listening on %d", XPAGENT_PORT);
    for (;;) {
        SOCKET c = accept(ls, NULL, NULL);
        if (c == INVALID_SOCKET)
            continue;
        CloseHandle(CreateThread(NULL, 0, client_thread,
                                 (LPVOID)(UINT_PTR)c, 0, NULL));
    }
    return 0;
}
