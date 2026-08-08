# Streamable HTTP 测试

## 自动测试

```bash
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Debug
cmake --build build
ctest --test-dir build --output-on-failure -V
```

集成测试会临时启动一个监听 `127.0.0.1:18089` 的服务器，并覆盖：

- health check
- MCP 2026-07-28 `server/discover`
- Origin 拒绝
- `Mcp-Method` 与 body 不一致
- `subscriptions/listen` 参数校验和 acknowledged 过滤条件回显
- POST SSE 最终响应
- 并发相同 JSON-RPC ID 的隔离
- Legacy initialize/session/ping/delete

可用 `MCP_TEST_PORT` 覆盖测试端口。

## 手工启动

```bash
mkdir -p logs
./build/mcp_server -m -a 127.0.0.1 -P 8080 -p ./build/plugins -l ./logs
```

## Modern discovery

```bash
curl -sS -X POST http://127.0.0.1:8080/mcp \
  -H 'Content-Type: application/json' \
  -H 'Accept: application/json, text/event-stream' \
  -H 'MCP-Protocol-Version: 2026-07-28' \
  -H 'Mcp-Method: server/discover' \
  --data '{
    "jsonrpc":"2.0",
    "id":1,
    "method":"server/discover",
    "params":{"_meta":{
      "io.modelcontextprotocol/protocolVersion":"2026-07-28",
      "io.modelcontextprotocol/clientInfo":{"name":"curl","version":"1.0"},
      "io.modelcontextprotocol/clientCapabilities":{}
    }}
  }'
```

## Modern tools/call SSE

```bash
curl -sS -N -X POST http://127.0.0.1:8080/mcp \
  -H 'Content-Type: application/json' \
  -H 'Accept: application/json, text/event-stream' \
  -H 'MCP-Protocol-Version: 2026-07-28' \
  -H 'Mcp-Method: tools/call' \
  -H 'Mcp-Name: sleep' \
  --data '{
    "jsonrpc":"2.0",
    "id":2,
    "method":"tools/call",
    "params":{
      "name":"sleep",
      "arguments":{"milliseconds":100},
      "_meta":{
        "io.modelcontextprotocol/protocolVersion":"2026-07-28",
        "io.modelcontextprotocol/clientInfo":{"name":"curl","version":"1.0"},
        "io.modelcontextprotocol/clientCapabilities":{}
      }
    }
  }'
```

预期至少收到一个 `event: message`，最终 JSON-RPC result 包含 `resultType: complete`，随后连接结束。

## Legacy initialize

```bash
curl -sS -D /tmp/mcp-headers -X POST http://127.0.0.1:8080/mcp \
  -H 'Content-Type: application/json' \
  -H 'Accept: application/json' \
  --data '{"jsonrpc":"2.0","id":10,"method":"initialize","params":{"protocolVersion":"2025-03-26","capabilities":{},"clientInfo":{"name":"curl","version":"1.0"}}}'

session_id=$(awk 'BEGIN{IGNORECASE=1} /^Mcp-Session-Id:/{gsub("\r", "", $2); print $2}' /tmp/mcp-headers)

curl -sS -X POST http://127.0.0.1:8080/mcp \
  -H 'Content-Type: application/json' \
  -H 'Accept: application/json' \
  -H "Mcp-Session-Id: $session_id" \
  --data '{"jsonrpc":"2.0","id":11,"method":"ping"}'
```
