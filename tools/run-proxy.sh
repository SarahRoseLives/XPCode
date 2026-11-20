#!/bin/sh
set -eu
ROOT=$(cd "$(dirname "$0")/.." && pwd)
KEY=${XPCODE_API_KEY_FILE:-$HOME/.config/xpcode/api_key}
LISTEN=${XPCODE_PROXY_LISTEN:-0.0.0.0:8080}
if [ ! -x "$ROOT/build/xpproxy" ]; then
    (cd "$ROOT/proxy" && go build -o "$ROOT/build/xpproxy" .)
fi
exec "$ROOT/build/xpproxy" --listen "$LISTEN" --api-key-file "$KEY" "$@"
