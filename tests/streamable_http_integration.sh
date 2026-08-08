#!/usr/bin/env bash
set -euo pipefail

server_bin="$1"
port="${MCP_TEST_PORT:-18089}"
test_root="$(mktemp -d /tmp/mcp-streamable-test.XXXXXX)"
mkdir -p "$test_root/logs" "$test_root/plugins"
plugin_dir="$(dirname "$server_bin")/plugins"

server_pid=""
cleanup() {
  if [[ -n "$server_pid" ]]; then
    kill "$server_pid" 2>/dev/null || true
    wait "$server_pid" 2>/dev/null || true
  fi
}
trap cleanup EXIT

"$server_bin" -m -a 127.0.0.1 -P "$port" \
  -p "$plugin_dir" -l "$test_root/logs" \
  >"$test_root/server.out" 2>&1 &
server_pid=$!

base_url="http://127.0.0.1:${port}"
for _ in {1..50}; do
  if curl -fsS "$base_url/health" >/dev/null 2>&1; then
    break
  fi
  sleep 0.1
done
curl -fsS "$base_url/health" | grep -q 'streamable-http'

modern_meta='{"io.modelcontextprotocol/protocolVersion":"2026-07-28","io.modelcontextprotocol/clientInfo":{"name":"integration-test","version":"1.0"},"io.modelcontextprotocol/clientCapabilities":{}}'

discover_response=$(curl -fsS -X POST "$base_url/mcp" \
  -H 'Content-Type: application/json' \
  -H 'Accept: application/json, text/event-stream' \
  -H 'MCP-Protocol-Version: 2026-07-28' \
  -H 'Mcp-Method: server/discover' \
  --data "{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"server/discover\",\"params\":{\"_meta\":${modern_meta}}}")
printf '%s' "$discover_response" | grep -q '2026-07-28'

bad_origin_status=$(curl -sS -o /dev/null -w '%{http_code}' -X POST "$base_url/mcp" \
  -H 'Origin: https://attacker.example' \
  -H 'Content-Type: application/json' \
  -H 'Accept: application/json, text/event-stream' \
  --data '{}')
[[ "$bad_origin_status" == "403" ]]

header_mismatch_status=$(curl -sS -o /dev/null -w '%{http_code}' -X POST "$base_url/mcp" \
  -H 'Content-Type: application/json' \
  -H 'Accept: application/json, text/event-stream' \
  -H 'MCP-Protocol-Version: 2026-07-28' \
  -H 'Mcp-Method: tools/list' \
  --data "{\"jsonrpc\":\"2.0\",\"id\":2,\"method\":\"server/discover\",\"params\":{\"_meta\":${modern_meta}}}")
[[ "$header_mismatch_status" == "400" ]]

invalid_subscription_status=$(curl -sS -o /dev/null -w '%{http_code}' -X POST "$base_url/mcp" \
  -H 'Content-Type: application/json' \
  -H 'Accept: application/json, text/event-stream' \
  -H 'MCP-Protocol-Version: 2026-07-28' \
  -H 'Mcp-Method: subscriptions/listen' \
  --data "{\"jsonrpc\":\"2.0\",\"id\":21,\"method\":\"subscriptions/listen\",\"params\":{\"_meta\":${modern_meta}}}")
[[ "$invalid_subscription_status" == "400" ]]

set +e
subscription_ack=$(curl -fsS -N --max-time 1 -X POST "$base_url/mcp" \
  -H 'Content-Type: application/json' \
  -H 'Accept: application/json, text/event-stream' \
  -H 'MCP-Protocol-Version: 2026-07-28' \
  -H 'Mcp-Method: subscriptions/listen' \
  --data "{\"jsonrpc\":\"2.0\",\"id\":22,\"method\":\"subscriptions/listen\",\"params\":{\"notifications\":{\"toolsListChanged\":true},\"_meta\":${modern_meta}}}")
subscription_curl_status=$?
set -e
[[ "$subscription_curl_status" == "28" ]]
printf '%s' "$subscription_ack" | grep -q 'notifications/subscriptions/acknowledged'
printf '%s' "$subscription_ack" | grep -q '"toolsListChanged":true'

sse_response=$(curl -fsS -N -X POST "$base_url/mcp" \
  -H 'Content-Type: application/json' \
  -H 'Accept: application/json, text/event-stream' \
  -H 'MCP-Protocol-Version: 2026-07-28' \
  -H 'Mcp-Method: tools/call' \
  -H 'Mcp-Name: missing-tool' \
  --data "{\"jsonrpc\":\"2.0\",\"id\":3,\"method\":\"tools/call\",\"params\":{\"name\":\"missing-tool\",\"arguments\":{},\"_meta\":${modern_meta}}}")
printf '%s' "$sse_response" | grep -q 'event: message'
printf '%s' "$sse_response" | grep -q '"id":3'

# Two concurrent requests intentionally reuse the same JSON-RPC id. They must
# remain isolated by their transport exchange ids and execute concurrently.
concurrent_request() {
  local milliseconds="$1"
  local output="$2"
  curl -fsS -N -X POST "$base_url/mcp" \
    -H 'Content-Type: application/json' \
    -H 'Accept: application/json, text/event-stream' \
    -H 'MCP-Protocol-Version: 2026-07-28' \
    -H 'Mcp-Method: tools/call' \
    -H 'Mcp-Name: sleep' \
    --data "{\"jsonrpc\":\"2.0\",\"id\":99,\"method\":\"tools/call\",\"params\":{\"name\":\"sleep\",\"arguments\":{\"milliseconds\":${milliseconds}},\"_meta\":${modern_meta}}}" \
    >"$output"
}

concurrency_start=$(python3 -c 'import time; print(time.monotonic())')
concurrent_request 700 "$test_root/concurrent-a.out" &
request_a_pid=$!
concurrent_request 700 "$test_root/concurrent-b.out" &
request_b_pid=$!
wait "$request_a_pid"
wait "$request_b_pid"
concurrency_end=$(python3 -c 'import time; print(time.monotonic())')
python3 - "$concurrency_start" "$concurrency_end" <<'PY'
import sys
elapsed = float(sys.argv[2]) - float(sys.argv[1])
assert elapsed < 1.30, f"requests ran serially: {elapsed:.3f}s"
PY
grep -q '"id":99' "$test_root/concurrent-a.out"
grep -q 'Waited for 700 milliseconds' "$test_root/concurrent-a.out"
grep -q '"id":99' "$test_root/concurrent-b.out"
grep -q 'Waited for 700 milliseconds' "$test_root/concurrent-b.out"

legacy_headers="$test_root/legacy.headers"
legacy_response=$(curl -fsS -D "$legacy_headers" -X POST "$base_url/mcp" \
  -H 'Content-Type: application/json' \
  -H 'Accept: application/json' \
  --data '{"jsonrpc":"2.0","id":10,"method":"initialize","params":{"protocolVersion":"2025-03-26","capabilities":{},"clientInfo":{"name":"legacy-test","version":"1.0"}}}')
printf '%s' "$legacy_response" | grep -q '"id":10'
session_id=$(awk 'BEGIN{IGNORECASE=1} /^Mcp-Session-Id:/{gsub("\r", "", $2); print $2}' "$legacy_headers")
[[ -n "$session_id" ]]

legacy_ping=$(curl -fsS -X POST "$base_url/mcp" \
  -H 'Content-Type: application/json' \
  -H 'Accept: application/json' \
  -H "Mcp-Session-Id: $session_id" \
  --data '{"jsonrpc":"2.0","id":11,"method":"ping"}')
printf '%s' "$legacy_ping" | grep -q '"id":11'

curl -fsS -X DELETE "$base_url/mcp" -H "Mcp-Session-Id: $session_id" \
  | grep -q 'session terminated'

echo "Streamable HTTP integration tests passed"
