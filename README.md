# XPCode

A native Windows XP AI coding client. XPCode runs as a plain Win32 app inside Windows XP and talks to modern LLMs through a small Go bridge on the host, which handles TLS and keeps the API key off the XP machine.

![XPCode running on Windows XP](docs/screenshot.png)

## How it works

```text
Windows XP                        Host (Linux/macOS)
+----------------------+  plain  +---------------------+  HTTPS  +----------------+
| xpcode.exe (Win32)   | ------> | xpproxy (Go)        | ------> | DeepSeek /     |
| WinInet, RichEdit    | frames  | TLS, upstream, key  |   TLS   | OpenAI         |
+----------------------+         +---------------------+         +----------------+
```

XP only speaks TLS 1.0, so it never calls the provider directly. The bridge streams replies back as tiny length-prefixed frames, which keeps the XP side simple. `--mock` streams canned replies for offline UI work.

Tool calls arrive as `T` frames, XPCode executes them in the guest, and the results are posted back for the next round.

## Layout

- `app/` — native Win32 client (C, MinGW)
- `proxy/` — Go TLS bridge with an OpenAI-compatible upstream
- `guest/` — `xpagent.exe`, a small control agent used during development
- `tools/` — `xpctl` VM/agent CLI and `run-proxy.sh`

## Build

```sh
make              # proxy + app + guest agent
make proxy        # Go bridge -> build/xpproxy
make app          # app/xpcode.exe
make agent        # guest/xpagent.exe
```

The XP binaries are built with `i686-w64-mingw32-gcc -mcrtdll=msvcrt-os` so they link against `msvcrt.dll` and run on XP.

## Run

On the host:

```sh
tools/run-proxy.sh
```

On the XP machine:

```text
C:\xpcode.exe
```

Deploying during development (VirtualBox + `tools/xpctl`):

```sh
tools/xpctl put app/xpcode.exe 'C:\xpcode.exe'
tools/xpctl exec 'start "" C:\xpcode.exe'
```

## Configuration

`C:\xpcode.ini`:

```ini
[server]
host=Garden.lan
port=8080
model=deepseek-chat
workdir=C:\xpcode-demo
```

Set `host` to an address the guest can reach. Under VirtualBox NAT, the host's LAN name or IP works; `10.0.2.2` may not reach host services. Relative tool paths resolve against `workdir`.

Proxy:

```sh
xpproxy --listen 0.0.0.0:8080 --api-key-file ~/.config/xpcode/api_key
xpproxy --mock
xpproxy --upstream https://api.openai.com/v1/chat/completions --model gpt-4o-mini
```

## Security

The API key lives only on the host: a mode-600 file or `DEEPSEEK_API_KEY` / `OPENAI_API_KEY`. It is never written into XPCode or committed to this repo. Keep the proxy off untrusted networks.

## Status

Chat, streaming, and a local tool loop work end-to-end on Windows XP. The model can call `read_file`, `write_file`, `edit_file`, `list_dir`, `search`, and `run_command`; XPCode executes them in the guest and feeds the results back. Relative paths resolve against `workdir`.

Tools run with the XP user's privileges, so only use this on a machine and with prompts you trust.
