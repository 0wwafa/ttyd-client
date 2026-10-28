#!/bin/bash
#
# Quick end to end smoke test without Python:
# starts a ttyd server, connects with ttyd-client through a pipe and checks the
# output. Requires `ttyd` to be installed (or TTYD=<path>).
#
# Usage: ./test/smoke.sh [path-to-ttyd-client]
#
set -eo pipefail

CLIENT="${1:-${CLIENT:-./build/ttyd-client}}"
TTYD="${TTYD:-ttyd}"
PORT="${PORT:-7679}"

if [ ! -x "$CLIENT" ]; then
  echo "client not found: $CLIENT" >&2
  exit 1
fi

command -v "$TTYD" > /dev/null || { echo "ttyd not found, set TTYD=<path>" >&2; exit 1; }

cleanup() {
  [ -n "${SERVER_PID:-}" ] && kill "$SERVER_PID" 2> /dev/null || true
  wait "$SERVER_PID" 2> /dev/null || true
}
trap cleanup EXIT

echo "=== starting ttyd on port ${PORT}"
"$TTYD" -p "$PORT" -W bash -c 'echo SMOKE_$((6*7)); exit 0' > /tmp/ttyd-smoke.log 2>&1 &
SERVER_PID=$!

for _ in $(seq 50); do
  grep -q "Listening on port" /tmp/ttyd-smoke.log 2> /dev/null && break
  sleep 0.1
done

echo "=== running $CLIENT"
OUTPUT=$(printf 'exit\r' | "$CLIENT" -d 6 --no-reconnect "localhost:${PORT}" 2>&1) || {
  echo "client failed:" >&2
  echo "$OUTPUT" >&2
  exit 1
}

echo "$OUTPUT" | sed 's/\r$//'

if ! echo "$OUTPUT" | grep -q "SMOKE_42"; then
  echo "FAILED: expected remote output" >&2
  exit 1
fi

echo "=== OK"
