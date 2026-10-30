#!/usr/bin/env bash
# Starts the compiler workbench and opens it in the browser.
#
#   ./run.sh          # first free port from 8000
#   ./run.sh 9000     # first free port from 9000
#
# Ctrl+C stops the server.
set -euo pipefail
cd "$(dirname "$0")"

port="${1:-8000}"
while lsof -iTCP:"$port" -sTCP:LISTEN >/dev/null 2>&1; do   # skip ports another app holds
    port=$((port + 1))
done
url="http://127.0.0.1:$port/"

python3 visualize/serve.py --port "$port" --no-open &
server=$!
trap 'kill "$server" 2>/dev/null' EXIT INT TERM

# serve.py builds build/rcc first, so wait until the workbench itself answers.
for _ in $(seq 60); do
    if curl -fs "${url}api/health" 2>/dev/null | grep -q '"languages"'; then
        open "$url"
        wait "$server"
        exit
    fi
    kill -0 "$server" 2>/dev/null || { echo "The server stopped before it was ready." >&2; exit 1; }
    sleep 0.5
done
echo "The server did not answer at $url within 30 s." >&2
exit 1
