#!/bin/sh
# Headless browser smoke test for the embedded workbench. Requires Chrome and
# curl on the developer machine; neither is a runtime dependency of Glassbox.
set -eu

BUILD=${1:-build}
ROOT=$(cd "$(dirname "$0")/.." && pwd)
CHROME=${CHROME:-/Applications/Google Chrome.app/Contents/MacOS/Google Chrome}
if [ ! -x "$CHROME" ]; then
  echo "Chrome not found at $CHROME; set CHROME=/path/to/chrome" >&2
  exit 2
fi
PORT=$((7400 + ($$ % 200)))
LOG=$(mktemp)
TMPDIR_SMOKE=$(mktemp -d)
PID=""
cleanup() {
  if [ -n "$PID" ] && kill -0 "$PID" 2>/dev/null; then kill "$PID" 2>/dev/null || true; fi
  rm -rf "$TMPDIR_SMOKE" "$LOG"
}
trap cleanup EXIT

"$ROOT/$BUILD/glassbox" --trace "$ROOT/examples/traces/demo.gbt" \
  --no-browser --no-record --port "$PORT" >"$LOG" 2>&1 &
PID=$!
URL=""
for _ in $(seq 1 100); do
  URL=$(sed -n 's/.*serving \(http:\/\/[^ ]*\).*/\1/p' "$LOG" | head -1)
  [ -n "$URL" ] && break
  sleep 0.1
done
if [ -z "$URL" ]; then
  echo "launcher did not report a URL:" >&2
  cat "$LOG" >&2
  exit 1
fi
TOKEN=$(printf '%s' "$URL" | sed -n 's/.*[?&]t=\([0-9a-f]*\).*/\1/p')
if [ -z "$TOKEN" ]; then echo "no token in $URL" >&2; exit 1; fi
echo "smoke: $URL"

dom() {
  # $1 = URL suffix appended after the tokenized base
  "$CHROME" --headless=new --disable-gpu --no-sandbox \
    --virtual-time-budget=6000 --user-data-dir="$TMPDIR_SMOKE/profile-$RANDOM" \
    --dump-dom "$URL$1" 2>/dev/null
}

check_view() {
  view=$1
  needle=$2
  body=$(dom "#$view")
  case "$body" in
    *"No trace open"*)
      echo "smoke: $view rendered an empty workbench" >&2
      exit 1
      ;;
  esac
  case "$body" in
    *"$needle"*) ;;
    *)
      echo "smoke: $view missing marker: $needle" >&2
      exit 1
      ;;
  esac
  case "$body" in
    *btn-quit*) ;;
    *)
      echo "smoke: $view missing quit control" >&2
      exit 1
      ;;
  esac
  echo "smoke: $view ok"
}

check_view overview 'Live heap at T'
check_view flame 'Flame graph'
check_view heap 'Allocation sites'
check_view spans 'Waterfall'
check_view logs 'demo workload'
check_view state 'inventory_count'
check_view diff 'Diff A/B'

query_body=$(dom '&q=type%3Aalloc%20AND%20size%3E1024#query')
case "$query_body" in
  *records*) ;;
  *)
    echo "smoke: query summary missing" >&2
    exit 1
    ;;
esac
case "$query_body" in
  *alloc*) ;;
  *)
    echo "smoke: query view did not render results" >&2
    exit 1
    ;;
esac
echo "smoke: query ok"

# Quit through the API; the UI quit button uses the same route.
curl -fsS -X POST -H "X-Glassbox-Token: $TOKEN" -H 'Content-Type: application/json' \
  --data '{}' "http://127.0.0.1:$PORT/api/shutdown" >/dev/null
PID=""
echo "smoke: shutdown ok"
