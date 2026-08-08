# Streamable HTTP 传输层技术实现

## 1. 实现目标

本实现使用 Boost.Asio + Boost.Beast，支持两类 MCP Streamable HTTP 协议：

| Profile | 协议版本 | 行为 |
|---|---|---|
| Modern | `2026-07-28` | 无状态、仅 POST、请求级 JSON/SSE、`subscriptions/listen` |
| Legacy | `2025-11-25`、`2025-06-18`、`2025-03-26` | initialize、Session、GET SSE、DELETE |

默认监听 `127.0.0.1:8080`。使用 `-a 0.0.0.0` 可以显式监听局域网，但部署方必须同时配置鉴权、TLS 和可信 Origin。

## 2. 架构

```text
Beast Listener
  └─ HttpSession（每条连接绑定 Asio strand）
       ├─ HTTP 解析、Origin/Header 校验
       ├─ 普通 JSON 响应
       └─ SSE chunked 写队列
             ↑
       ResponseQueue（每个 HTTP exchange 独立）
             ↑
       ITransport::WriteMessage(exchange_id)
             ↑
       Server / Plugin
```

传输层为每个请求生成内部 `exchange_id`。它与 JSON-RPC `id` 无关，因此多个客户端可以并发复用相同 JSON-RPC ID，而不会串响应。

`ResponseQueue` 持有消息，不持有 socket 或裸 `DataSink*`。业务线程只投递消息，所有网络写入都在连接所属的 Asio executor 上执行。

## 3. Modern Profile

### 3.1 请求校验

每个 POST 必须满足：

- `Content-Type: application/json`
- `Accept` 同时包含 `application/json` 和 `text/event-stream`
- `MCP-Protocol-Version: 2026-07-28`
- `Mcp-Method` 与 JSON-RPC `method` 一致
- `tools/call`、`prompts/get`、`resources/read` 必须提供匹配的 `Mcp-Name`
- `params._meta.io.modelcontextprotocol/protocolVersion` 与 Header 一致
- `_meta` 包含 clientInfo 和 clientCapabilities

无法表示为普通 ASCII 的 `Mcp-Name` 支持 `=?base64?...?=` 解码后比较。

### 3.2 响应策略

- `server/discover`、list 等普通请求返回 `application/json`
- `tools/call` 返回请求级 SSE，可在最终响应前发送 progress/message 通知
- 最终响应发送后关闭 SSE stream
- `subscriptions/listen` 返回长连接 SSE，并首先发送 acknowledged 通知
- `subscriptions/listen` 严格按 tools/prompts/resources 过滤条件投递；每条后续通知带原始 JSON-RPC ID 对应的 `subscriptionId`
- Modern Profile 不提供 GET/DELETE，也不生成 `Mcp-Session-Id`

Modern 成功响应会补齐 `result.resultType = "complete"`，并回传 `MCP-Protocol-Version` Header。

## 4. Legacy Profile

缺少版本 Header 时按 `2025-03-26` 兼容模式处理。

1. `initialize` 创建加密随机 Session ID。
2. 后续 POST、GET、DELETE 必须携带 `Mcp-Session-Id`。
3. 每个 Session 保存自己的 GET SSE 通知流。
4. 不同 Session 和不同 HTTP exchange 完全隔离。
5. DELETE 关闭通知流并销毁 Session。

## 5. 并发与生命周期

- Beast 网络线程数：至少 2，通常等于硬件并发数。
- Server 业务并发度由 Transport 声明；stdio 仍保持单线程。
- HTTP 请求通过有界 `std::async` worker 并发执行。
- 每个 ResponseQueue 共享一个 cancellation flag。
- 连接关闭、超时、Transport Stop 都会关闭 ResponseQueue。
- SSE 单队列限制为 256 条消息或 1 MiB，慢客户端超过限制会被关闭。
- 普通 pending exchange 和 subscription 分别限制为 1024，超限返回 HTTP 503。
- 请求体上限为 1 MiB。

## 6. 安全措施

- 默认仅绑定 localhost。
- 对带 `Origin` 的请求执行 DNS rebinding 防护；非法 Origin 返回 403。
- CORS 不再使用 `Access-Control-Allow-Origin: *`，只回显已验证 Origin。
- Header 名按 HTTP 规则大小写不敏感处理。
- SSE 返回 `X-Accel-Buffering: no`，避免 nginx 等代理缓存流式响应。
- SSE 使用 chunked encoding，并发送 keepalive comment。

## 7. 主要文件

- `src/interface/ITransport.h`：context-aware TransportMessage/TransportWrite。
- `src/server/Server.cpp`：exchange 上下文、并发 worker、request-scoped notification。
- `src/transport/StreamableTransport.h`：稳定的 PImpl 公共接口。
- `src/transport/StreamableTransport.cpp`：Beast 网络层、协议 profile、SSE 和 session。
- `tests/streamable_http_integration.sh`：端到端协议测试。

## 8. 构建

```bash
brew install cmake ninja boost
cmake -S . -B build -G Ninja
cmake --build build
ctest --test-dir build --output-on-failure
```
