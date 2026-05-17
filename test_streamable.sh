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

# 提取 Session ID
SESSION_ID=$(grep -i "^MCP-Session-Id:" /tmp/mcp_headers.txt | awk '{print $2}' | tr -d '\r')
echo "提取到的 Session ID: [$SESSION_ID]"
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

echo "=== 测试 6: ping (标准 JSON 响应) ==="
curl -s -X POST "$MCP_URL" \
  -H "Content-Type: application/json" \
  -H "Accept: application/json" \
  -H "MCP-Session-Id: $SESSION_ID" \
  -d '{"jsonrpc":"2.0","id":4,"method":"ping"}' | python3 -m json.tool
echo ""

# ----------------- 新增的 SSE 测试部分 -----------------

echo "=== 测试 7: POST 流式响应测试 (Accept: text/event-stream) ==="
# 使用 -N 禁用缓冲。期望看到 event: message 和 data: {...}
curl -s -N -X POST "$MCP_URL" \
  -H "Content-Type: application/json" \
  -H "Accept: text/event-stream" \
  -H "MCP-Session-Id: $SESSION_ID" \
  -d '{"jsonrpc":"2.0","id":5,"method":"ping"}'
echo -e "\n"

echo "=== 测试 8: 建立 GET SSE 通知长连接 (后台运行) ==="
# 清空之前的日志文件
> /tmp/sse_stream.log
# 将 GET 请求放在后台运行，持续接收通知
curl -s -N -X GET "$MCP_URL" \
  -H "MCP-Session-Id: $SESSION_ID" > /tmp/sse_stream.log &
SSE_PID=$!

# 设置退出拦截：无论脚本成功还是失败，退出时自动清理后台的 curl 进程
trap 'kill $SSE_PID 2>/dev/null || true' EXIT

echo "后台 SSE 监听进程已启动 (PID: $SSE_PID)... 等待 1 秒建立连接"
sleep 1
echo ""

echo "=== 测试 9: 触发服务端主动推送 (调用 logging_test) ==="
# 此操作会产生服务端通知，该通知会被路由到刚刚建立的 GET 长连接中
curl -s -X POST "$MCP_URL" \
  -H "Content-Type: application/json" \
  -H "Accept: application/json" \
  -H "MCP-Session-Id: $SESSION_ID" \
  -d '{
    "jsonrpc": "2.0",
    "id": 6,
    "method": "tools/call",
    "params": {
      "name": "logging_test",
      "arguments": {}
    }
  }' | python3 -m json.tool
echo ""

echo "等待 1 秒让后台接收 SSE 推送..."
sleep 1

echo "=== 测试 10: 校验后台 SSE 接收到的数据 ==="
echo "--------------- SSE 数据流起始 ---------------"
cat /tmp/sse_stream.log
echo "--------------- SSE 数据流结束 ---------------"
echo ""

# -------------------------------------------------------

echo "=== 测试 11: 销毁会话 ==="
curl -s -D - -X DELETE "$MCP_URL" \
  -H "MCP-Session-Id: $SESSION_ID"
echo ""

echo "=== 所有测试完成 ==="
# trap 会在这里自动结束后台的 SSE curl 进程