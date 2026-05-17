# Streamable HTTP 传输层测试文档

本文档提供完整的 curl 命令，用于测试 Streamable HTTP 传输层的各项功能。

## 前置条件

### 1. 编译并启动服务器

```bash
cd /path/to/mcp-server
mkdir -p build && cd build
cmake .. && make -j$(nproc)

# 启动 Streamable HTTP 模式（默认端口 8080）
./mcp_server -m
```

### 2. 环境变量（简化 curl 命令）

```bash
export MCP_URL="http://localhost:8080/mcp"
```

---

## 测试 1：健康检查

验证服务器是否正常运行。

```bash
curl -s http://localhost:8080/health | python3 -m json.tool
```

**预期响应：**
```json
{
    "status": "ok",
    "transport": "streamable-http"
}
```

---

## 测试 2：Initialize 握手

建立会话，获取 `MCP-Session-Id`。这是所有后续请求的前提。

```bash
curl -s -D - -X POST http://localhost:8080/mcp \
  -H "Content-Type: application/json" \
  -H "Accept: application/json" \
  -d '{
    "jsonrpc": "2.0",
    "id": 1,
    "method": "initialize",
    "params": {
      "protocolVersion": "2025-03-26",
      "capabilities": {},
      "clientInfo": {
        "name": "test-client",
        "version": "1.0.0"
      }
    }
  }'
```

**预期响应头（重要部分）：**
```
HTTP/1.1 200 OK
Content-Type: application/json
MCP-Session-Id: <生成的会话ID>
```

**预期响应体：**
```json
{
    "jsonrpc": "2.0",
    "id": 1,
    "result": {
        "protocolVersion": "2025-03-26",
        "capabilities": {
            "tools": {"listChanged": true},
            "prompts": {"listChanged": true},
            "resources": {"subscribe": true, "listChanged": true},
            "logging": {}
        },
        "serverInfo": {
            "name": "mcp-server",
            "version": "0.7.0"
        }
    }
}
```

**记录会话 ID（后续命令需要）：**
```bash
# 自动提取 Session-Id
SESSION_ID=$(curl -s -D - -X POST http://localhost:8080/mcp \
  -H "Content-Type: application/json" \
  -H "Accept: application/json" \
  -d '{"jsonrpc":"2.0","id":1,"method":"initialize","params":{"protocolVersion":"2025-03-26","capabilities":{},"clientInfo":{"name":"test","version":"1.0.0"}}}' \
  | grep -i "MCP-Session-Id" | awk '{print $2}' | tr -d '\r')

echo "Session ID: $SESSION_ID"
```

---

## 测试 3：发送 initialized 通知

握手完成后发送通知（无需 id 字段），服务器应返回 202。

```bash
curl -s -D - -X POST http://localhost:8080/mcp \
  -H "Content-Type: application/json" \
  -H "Accept: application/json" \
  -H "MCP-Session-Id: $SESSION_ID" \
  -d '{
    "jsonrpc": "2.0",
    "method": "notifications/initialized"
  }'
```

**预期响应：**
```
HTTP/1.1 202 Accepted
```

---

## 测试 4：列出可用工具（tools/list）

获取所有已加载插件提供的工具列表。

```bash
curl -s -X POST http://localhost:8080/mcp \
  -H "Content-Type: application/json" \
  -H "Accept: application/json" \
  -H "MCP-Session-Id: $SESSION_ID" \
  -d '{
    "jsonrpc": "2.0",
    "id": 2,
    "method": "tools/list"
  }' | python3 -m json.tool
```

**预期响应（应包含 weather 等插件）：**
```json
{
    "jsonrpc": "2.0",
    "id": 2,
    "result": {
        "tools": [
            {
                "name": "get_weather",
                "description": "Get weather forecast of a city...",
                "inputSchema": {...}
            },
            {
                "name": "sleep",
                "description": "...",
                "inputSchema": {...}
            }
        ]
    }
}
```

---

## 测试 5：调用天气插件（tools/call）

查询北京天气（纬度 39.9，经度 116.4）。

### 5.1 JSON 模式（Accept: application/json）

```bash
curl -s -X POST http://localhost:8080/mcp \
  -H "Content-Type: application/json" \
  -H "Accept: application/json" \
  -H "MCP-Session-Id: $SESSION_ID" \
  -d '{
    "jsonrpc": "2.0",
    "id": 3,
    "method": "tools/call",
    "params": {
      "name": "get_weather",
      "arguments": {
        "latitude": "39.9042",
        "longitude": "116.4074",
        "city": "Beijing"
      }
    }
  }' | python3 -m json.tool
```

**预期响应：**
```json
{
    "jsonrpc": "2.0",
    "id": 3,
    "result": {
        "content": [
            {
                "type": "text",
                "text": "Weather Forecast for Beijing:\n\nToday's Temperature Forecast:\n🌅 Morning: XX.X°C\n☀️ Afternoon: XX.X°C\n🌙 Evening: XX.X°C\n\n🔼 Highest: XX.X°C at HH:MM\n🔽 Lowest: XX.X°C at HH:MM\n\n📊 Daily Summary: ..."
            }
        ],
        "isError": false
    }
}
```

### 5.2 查询上海天气

```bash
curl -s -X POST http://localhost:8080/mcp \
  -H "Content-Type: application/json" \
  -H "Accept: application/json" \
  -H "MCP-Session-Id: $SESSION_ID" \
  -d '{
    "jsonrpc": "2.0",
    "id": 4,
    "method": "tools/call",
    "params": {
      "name": "get_weather",
      "arguments": {
        "latitude": "31.2304",
        "longitude": "121.4737",
        "city": "Shanghai"
      }
    }
  }' | python3 -m json.tool
```

### 5.3 查询东京天气

```bash
curl -s -X POST http://localhost:8080/mcp \
  -H "Content-Type: application/json" \
  -H "Accept: application/json" \
  -H "MCP-Session-Id: $SESSION_ID" \
  -d '{
    "jsonrpc": "2.0",
    "id": 5,
    "method": "tools/call",
    "params": {
      "name": "get_weather",
      "arguments": {
        "latitude": "35.6762",
        "longitude": "139.6503",
        "city": "Tokyo"
      }
    }
  }' | python3 -m json.tool
```

---

## 测试 6：SSE 流式响应（动态切换）

当客户端 Accept 头包含 `text/event-stream` 时，服务器应切换为 SSE 流式响应。

```bash
curl -s -N -X POST http://localhost:8080/mcp \
  -H "Content-Type: application/json" \
  -H "Accept: text/event-stream" \
  -H "MCP-Session-Id: $SESSION_ID" \
  -d '{
    "jsonrpc": "2.0",
    "id": 6,
    "method": "tools/call",
    "params": {
      "name": "get_weather",
      "arguments": {
        "latitude": "51.5074",
        "longitude": "-0.1278",
        "city": "London"
      }
    }
  }'
```

**预期响应格式（SSE 事件流）：**
```
event: message
data: {"jsonrpc":"2.0","id":6,"result":{"content":[{"type":"text","text":"Weather Forecast for London:\n\nToday's Temperature Forecast:\n..."}],"isError":false}}

```

注意：`-N` 参数禁用 curl 的缓冲，实时显示 SSE 事件。

---

## 测试 7：GET SSE 长连接（服务器通知）

建立 SSE 长连接以接收服务器主动推送的通知（如插件热重载时的 `tools/list_changed`）。

```bash
# 终端 1：建立 GET SSE 连接
curl -s -N -X GET http://localhost:8080/mcp \
  -H "Accept: text/event-stream" \
  -H "MCP-Session-Id: $SESSION_ID"
```

**预期输出（等待通知时无输出，有通知时显示）：**
```
: keepalive

event: message
data: {"jsonrpc":"2.0","method":"notifications/tools/list_changed"}

```

**触发通知的方法：** 在另一个终端中替换天气插件的 `.so` 文件，PluginsLoader 检测到变化后会自动发送 `notifications/tools/list_changed` 通知。

---

## 测试 8：Ping 心跳

```bash
curl -s -X POST http://localhost:8080/mcp \
  -H "Content-Type: application/json" \
  -H "Accept: application/json" \
  -H "MCP-Session-Id: $SESSION_ID" \
  -d '{
    "jsonrpc": "2.0",
    "id": 7,
    "method": "ping"
  }' | python3 -m json.tool
```

**预期响应：**
```json
{
    "jsonrpc": "2.0",
    "id": 7,
    "result": {}
}
```

---

## 测试 9：列出提示词（prompts/list）

```bash
curl -s -X POST http://localhost:8080/mcp \
  -H "Content-Type: application/json" \
  -H "Accept: application/json" \
  -H "MCP-Session-Id: $SESSION_ID" \
  -d '{
    "jsonrpc": "2.0",
    "id": 8,
    "method": "prompts/list"
  }' | python3 -m json.tool
```

**预期响应：**
```json
{
    "jsonrpc": "2.0",
    "id": 8,
    "result": {
        "prompts": [
            {
                "name": "code-review",
                "description": "...",
                "arguments": [...]
            }
        ]
    }
}
```

---

## 测试 10：列出资源（resources/list）

```bash
curl -s -X POST http://localhost:8080/mcp \
  -H "Content-Type: application/json" \
  -H "Accept: application/json" \
  -H "MCP-Session-Id: $SESSION_ID" \
  -d '{
    "jsonrpc": "2.0",
    "id": 9,
    "method": "resources/list"
  }' | python3 -m json.tool
```

---

## 测试 11：销毁会话（DELETE /mcp）

```bash
curl -s -D - -X DELETE http://localhost:8080/mcp \
  -H "MCP-Session-Id: $SESSION_ID"
```

**预期响应：**
```
HTTP/1.1 200 OK
Content-Type: application/json

{"status":"session terminated"}
```

**验证会话已销毁：** 再次使用同一 Session-Id 发送请求应返回 404。

```bash
curl -s -D - -X POST http://localhost:8080/mcp \
  -H "Content-Type: application/json" \
  -H "Accept: application/json" \
  -H "MCP-Session-Id: $SESSION_ID" \
  -d '{"jsonrpc":"2.0","id":99,"method":"ping"}'
```

**预期响应：**
```
HTTP/1.1 404 Not Found

{"jsonrpc":"2.0","error":{"code":-32600,"message":"Invalid or missing MCP-Session-Id"},"id":null}
```

---

## 测试 12：错误处理

### 12.1 无效 Content-Type

```bash
curl -s -D - -X POST http://localhost:8080/mcp \
  -H "Content-Type: text/plain" \
  -d 'hello'
```

**预期：** `415 Unsupported Media Type`

### 12.2 空请求体

```bash
curl -s -D - -X POST http://localhost:8080/mcp \
  -H "Content-Type: application/json" \
  -H "Accept: application/json" \
  -d ''
```

**预期：** `400 Bad Request`

### 12.3 无效 JSON

```bash
curl -s -D - -X POST http://localhost:8080/mcp \
  -H "Content-Type: application/json" \
  -H "Accept: application/json" \
  -d '{invalid json'
```

**预期：** `400 Bad Request` + `Parse error`

### 12.4 无会话 ID 请求（initialize 之后）

```bash
# 不携带 MCP-Session-Id
curl -s -D - -X POST http://localhost:8080/mcp \
  -H "Content-Type: application/json" \
  -H "Accept: application/json" \
  -d '{"jsonrpc":"2.0","id":99,"method":"ping"}'
```

**预期：** `404 Not Found` + `Invalid or missing MCP-Session-Id`

### 12.5 未初始化时发送非 initialize 请求

```bash
# 直接发送 tools/list 而不先 initialize（需要重启服务器确保无会话）
curl -s -D - -X POST http://localhost:8080/mcp \
  -H "Content-Type: application/json" \
  -H "Accept: application/json" \
  -d '{"jsonrpc":"2.0","id":99,"method":"tools/list"}'
```

**预期：** `400 Bad Request` + `Session not initialized. Send 'initialize' request first.`

---

## 完整测试脚本

将以下内容保存为 `test_streamable.sh`，一键执行所有测试：

```bash
#!/bin/bash
set -e

MCP_URL="http://localhost:8080/mcp"

echo "=== 测试 1: 健康检查 ==="
curl -s http://localhost:8080/health | python3 -m json.tool
echo ""

echo "=== 测试 2: Initialize 握手 ==="
RESPONSE=$(curl -s -D /tmp/mcp_headers.txt -X POST "$MCP_URL" \
  -H "Content-Type: application/json" \
  -H "Accept: application/json" \
  -d '{
    "jsonrpc": "2.0",
    "id": 1,
    "method": "initialize",
    "params": {
      "protocolVersion": "2025-03-26",
      "capabilities": {},
      "clientInfo": {"name": "test-client", "version": "1.0.0"}
    }
  }')
echo "$RESPONSE" | python3 -m json.tool
SESSION_ID=$(grep -i "MCP-Session-Id" /tmp/mcp_headers.txt | awk '{print $2}' | tr -d '\r')
echo "Session ID: $SESSION_ID"
echo ""

echo "=== 测试 3: initialized 通知 ==="
curl -s -D - -X POST "$MCP_URL" \
  -H "Content-Type: application/json" \
  -H "Accept: application/json" \
  -H "MCP-Session-Id: $SESSION_ID" \
  -d '{"jsonrpc":"2.0","method":"notifications/initialized"}'
echo ""

echo "=== 测试 4: tools/list ==="
curl -s -X POST "$MCP_URL" \
  -H "Content-Type: application/json" \
  -H "Accept: application/json" \
  -H "MCP-Session-Id: $SESSION_ID" \
  -d '{"jsonrpc":"2.0","id":2,"method":"tools/list"}' | python3 -m json.tool
echo ""

echo "=== 测试 5: tools/call (天气查询 - 北京) ==="
curl -s -X POST "$MCP_URL" \
  -H "Content-Type: application/json" \
  -H "Accept: application/json" \
  -H "MCP-Session-Id: $SESSION_ID" \
  -d '{
    "jsonrpc": "2.0",
    "id": 3,
    "method": "tools/call",
    "params": {
      "name": "get_weather",
      "arguments": {"latitude":"39.9042","longitude":"116.4074","city":"Beijing"}
    }
  }' | python3 -m json.tool
echo ""

echo "=== 测试 6: ping ==="
curl -s -X POST "$MCP_URL" \
  -H "Content-Type: application/json" \
  -H "Accept: application/json" \
  -H "MCP-Session-Id: $SESSION_ID" \
  -d '{"jsonrpc":"2.0","id":7,"method":"ping"}' | python3 -m json.tool
echo ""

echo "=== 测试 7: 销毁会话 ==="
curl -s -D - -X DELETE "$MCP_URL" \
  -H "MCP-Session-Id: $SESSION_ID"
echo ""

echo "=== 所有测试完成 ==="
```

```bash
chmod +x test_streamable.sh
./test_streamable.sh