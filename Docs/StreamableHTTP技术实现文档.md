# Streamable HTTP 传输层技术实现文档

## 1. 概述

本文档详细解释 MCP Server 中 **Streamable HTTP 传输层**的实现，基于 [MCP 2025-03-26 规范](https://modelcontextprotocol.io/specification/2025-03-26/basic/transports) 的 Streamable HTTP 传输协议。

### 1.1 什么是 Streamable HTTP？

Streamable HTTP 是 MCP 协议定义的一种 HTTP 传输方式。与传统的请求-响应模式不同，它允许服务器：

- 对**快速请求**直接返回 JSON 响应
- 对**耗时请求**自动切换为 SSE（Server-Sent Events）流式响应
- 通过独立的 GET SSE 连接主动推送**通知消息**

### 1.2 核心设计原则

| 原则 | 说明 |
|------|------|
| **单一端点** | 所有请求通过同一个 `/mcp` 路径访问 |
| **动态响应** | 根据客户端 `Accept` 头自动选择 JSON 或 SSE 响应 |
| **会话管理** | 通过 `MCP-Session-Id` HTTP Header 管理客户端会话 |
| **线程安全** | 核心数据结构均使用互斥锁保护 |

---

## 2. 架构设计

### 2.1 类结构

```
ITransport (抽象接口)
    │
    ├── StdioTransport        (标准输入输出)
    ├── SSE                   (Server-Sent Events)
    ├── HttpStream            (HTTP 流式)
    └── StreamableTransport   (Streamable HTTP) ← 本次新增
```

`StreamableTransport` 继承自 `ITransport` 接口，实现以下纯虚方法：

```cpp
class ITransport {
    virtual bool Start() = 0;                                    // 启动服务
    virtual void Stop() = 0;                                     // 停止服务
    virtual std::pair<size_t, std::string> Read() = 0;           // 读取客户端消息
    virtual void Write(const std::string& json_data) = 0;        // 写回响应/通知
    // ...
};
```

### 2.2 数据流

```
客户端                                    服务器
  │                                         │
  │  ── POST /mcp (JSON-RPC) ──────────►    │
  │                                         │  ── 入队 incoming_queue_
  │                                         │  ── Server::Read() 取出
  │                                         │  ── Server::HandleRequest()
  │                                         │  ── Server::Write() 回调
  │  ◄── JSON 响应 或 SSE 事件流 ────────     │
  │                                         │
  │  ── GET /mcp (SSE 长连接) ─────────►     │
  │  ◄── 服务器（主动）通知 ───────────────    │
  │                                         │
  │  ── DELETE /mcp （销毁会话） ───────►     │
  │  ◄── 200 OK ──────────────────────      │
```

### 2.3 线程模型

```
HTTP 监听线程     cpp-httplib 内部管理，处理 HTTP 请求
Server 读取线程   从 incoming_queue_ 取消息，调用 HandleRequest
Server 写入线程   从 notification_queue_ 取通知，调用 Write()
```

---

## 3. 路由设计

### 3.1 HTTP 路由表

| 方法   | 路径    | 功能                        | 认证要求           |
|--------|---------|-----------------------------|--------------------|
| POST   | `/mcp`  | 接收 JSON-RPC 消息          | `initialize` 后需 Session-Id |
| GET    | `/mcp`  | SSE 长连接（服务器通知）     | 需 `MCP-Session-Id`        |
| DELETE | `/mcp`  | 销毁会话                    | 需 `MCP-Session-Id`        |
| OPTIONS| `/*`    | CORS 预检                   | 无                          |
| GET    | `/health` | 健康检查                  | 无                          |

### 3.2 POST /mcp 请求处理流程

这是最核心的路由，处理流程如下：

```
POST /mcp 收到请求
    │
    ├─ 1. 验证 Content-Type = application/json
    │     └─ 不匹配 → 415 Unsupported Media Type
    │
    ├─ 2. 验证 Accept 头
    │     └─ 不含 application/json 或 text/event-stream → 406 Not Acceptable
    │
    ├─ 3. 解析 JSON-RPC 消息
    │     └─ 解析失败 → 400 Bad Request (Parse error)
    │
    ├─ 4. 会话管理
    │     ├─ 是 initialize 请求 → 创建新会话，生成 MCP-Session-Id
    │     ├─ 其他请求且会话已初始化 → 验证 MCP-Session-Id 头
    │     └─ 其他请求且会话未初始化 → 400 + "Session not initialized"
    │
    ├─ 5. 判断消息类型
    │     ├─ 通知消息（无 id）→ 入队 + 返回 202 Accepted
    │     └─ 请求消息（有 id）→ 进入响应策略选择 ↓
    │
    └─ 6. 动态响应策略
          ├─ Accept 含 text/event-stream → SSE 流式模式
          │     └─ 返回 200 + Content-Type: text/event-stream
          │        Server 通过 DataSink 推送 SSE 事件
          │
          └─ 否则 → JSON 模式
                └─ 阻塞等待 Server 处理
                   返回 200 + Content-Type: application/json
```

---

## 4. 关键实现细节

### 4.1 动态响应切换

这是 Streamable HTTP 与现有 HttpStreamTransport 的**核心区别**。

**JSON 模式**（默认）：
```cpp
// 通过 promise/future 同步等待
auto pending = std::make_shared<PendingRequest>();
pending->mode = ResponseMode::JSON;
std::future<std::string> response_future = pending->json_promise.get_future();

// 入队等待 Server 处理
incoming_queue_.push(message);

// 阻塞等待（超时 30 秒）
response_future.wait_for(std::chrono::seconds(30));

// Server::Write() 中通过 promise.set_value() 回传结果
// POST handler 收到结果后直接返回 JSON
res.set_content(response_data, "application/json");
```

**SSE 流式模式**（当客户端 Accept 头包含 `text/event-stream`）：
```cpp
pending->mode = ResponseMode::SSE;
pending->stream_active.store(true);

// 使用 httplib 的 content_provider 持续推送
res.set_content_provider("text/event-stream",
    [this, id_str, pending_weak](size_t offset, httplib::DataSink& sink) -> bool {
        // 存储 sink 指针供 Write() 使用
        pending->sse_sink = &sink;

        // 定义清理函数，防止悬空指针和内存泄漏
        auto cleanup = [&]() {
            pending->sse_sink = nullptr;        // 防止悬空指针
            pending->stream_active.store(false);
            std::lock_guard<std::mutex> lock(pending_mutex_);
            pending_requests_.erase(id_str);    // 防止内存泄漏
        };

        // 保持连接直到流结束
        while (pending->stream_active.load() && running_.load()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
        }
        cleanup();  // 确保退出前清理
        return false;
    }
);

// Server::Write() 中通过 sink 直接推送 SSE 事件
std::string sse_event = "event: message\ndata: " + json_data + "\n\n";
pending->sse_sink->write(sse_event.c_str(), sse_event.length());
```

> **安全性说明**：content_provider 在客户端断开或超时时退出，必须在退出前执行 `cleanup()` 以：
> 1. 将 `sse_sink` 置为 `nullptr`，防止 `Write()` 通过悬空指针调用已销毁的 `DataSink`（段错误）
> 2. 从 `pending_requests_` 中移除条目，防止内存泄漏

### 4.2 Write() 路由逻辑

`Write()` 方法是 Server 层向客户端发送数据的统一入口，内部根据数据类型路由到不同通道：

```cpp
void StreamableTransport::Write(const std::string& json_data) {
    auto parsed = nlohmann::json::parse(json_data);

    // 判断：是请求响应还是服务器通知？
    if (parsed.contains("id") && (parsed.contains("result") || parsed.contains("error"))) {
        // 是响应 → 路由到对应的 PendingRequest
        auto it = pending_requests_.find(id_str);
        if (it != pending_requests_.end()) {
            if (pending->mode == ResponseMode::JSON) {
                pending->json_promise.set_value(json_data);    // JSON 模式
            } else {
                pending->sse_sink->write(sse_event);           // SSE 模式
            }
        }
    } else {
        // 是通知 → 推送到 GET SSE 流
        sse_notifications_.push(json_data);
    }
}
```

### 4.3 会话管理

会话通过 `MCP-Session-Id` HTTP Header 管理：

```
1. 客户端发送 initialize 请求（无需 Session-Id）
2. 服务器创建会话，生成唯一 ID
3. 响应头中包含 MCP-Session-Id
4. 后续所有请求必须携带此 Header
5. 客户端发送 DELETE /mcp 销毁会话
```

会话 ID 生成使用 `SessionBuilder::GenerateUniqueSessionID()`，格式为 `<时间戳十六进制>-<随机数十六进制>`。

### 4.4 SSE 事件格式

严格遵循 MCP 规范：

```
event: message
data: {"jsonrpc":"2.0","id":1,"result":{"content":[{"type":"text","text":"Hello"}]}}

event: message
data: {"jsonrpc":"2.0","method":"notifications/tools/list_changed"}

```

每个事件由三部分组成：
- `event: message` — 事件类型（固定为 `message`）
- `data: {json}` — JSON-RPC 消息体
- 空行 `\n` — 事件分隔符

### 4.5 线程安全机制

| 数据结构 | 保护方式 | 说明 |
|----------|----------|------|
| `incoming_queue_` | `incoming_mutex_` + `incoming_cv_` | 生产者-消费者模式 |
| `pending_requests_` | `pending_mutex_` | 读写互斥 |
| `sse_notifications_` | `sse_mutex_` + `sse_cv_` | 生产者-消费者模式 |
| `sse_stream_active_` | `std::atomic<bool>` | 原子操作 |
| `client_connected_` | `std::atomic<bool>` | 原子操作 |
| `pending->sse_sink` | `pending->sink_mutex` | 保护 SSE sink 并发访问 |

### 4.6 关键安全修复

#### Stop() 关机顺序

`server_->stop()` 会阻塞直到所有 HTTP handler 线程退出。在 JSON 模式下，POST handler 会阻塞在 `response_future.wait_for(30s)` 上等待 `promise` 被设置值。如果先调用 `server_->stop()` 再清理 `pending_requests_`，会导致死锁（最长 30 秒）。

**修复**：必须先清理 `pending_requests_`（设置 `stream_active = false` + promise 设值），再调用 `server_->stop()`。

```
Stop() 流程（修复后）:
  1. 设置 running_ = false，通知条件变量
  2. 清理 pending_requests_（解除所有 handler 阻塞）
  3. server_->stop()（安全退出，无死锁）
  4. server_thread_.join()
```

#### SSE 悬空指针与内存泄漏

当客户端异常断开时，`content_provider` 退出，httplib 销毁 `DataSink` 对象。但 `PendingRequest::sse_sink` 仍指向已销毁的对象。如果随后 `Write()` 被调用，会通过悬空指针触发段错误。

**修复**：在 `content_provider` 的所有退出路径上执行清理：置空 `sse_sink`、设 `stream_active = false`、从 `pending_requests_` 中移除条目。

#### 未初始化请求拦截

如果客户端发送的第一个请求不是 `initialize`，代码必须拒绝而非放行。

**修复**：在会话管理逻辑中添加 `else` 分支，返回 `400 + -32600 "Session not initialized"`。

---

## 5. 与现有 HttpStreamTransport 的对比

| 特性 | HttpStreamTransport | StreamableTransport |
|------|--------------------|--------------------|
| POST 响应方式 | 固定同步 JSON | **动态 JSON / SSE 切换** |
| 响应模式判断 | 无 | **根据 Accept 头自动切换** |
| 会话 Header 名 | `Mcp-Session-Id` | `MCP-Session-Id`（规范大小写） |
| 协议版本 | `2024-11-05` | `2025-03-26` |
| SSE 事件格式 | `event: message\ndata: {json}` | `event: message\ndata: {json}`（相同） |
| CORS 支持 | ✅ | ✅ |
| 健康检查端点 | `/health` | `/health` |

---

## 6. 文件清单

### 新增文件

| 文件 | 说明 |
|------|------|
| `src/transport/StreamableTransport.h` | 类声明、成员变量、内部结构体 |
| `src/transport/StreamableTransport.cpp` | 完整实现（约 450 行） |

### 修改文件

| 文件 | 修改内容 |
|------|----------|
| `src/main.cpp` | 添加 `-m/--streamable` 命令行选项和 `StreamableTransport` 初始化 |
| `CMakeLists.txt` | 将 `StreamableTransport.cpp` 加入编译源文件列表 |
| `src/server/Server.cpp` | 协议版本从 `2024-11-05` 更新为 `2025-03-26` |

---

## 7. 依赖关系

```
StreamableTransport
    ├── ITransport.h          (传输接口抽象)
    ├── httplib.h             (cpp-httplib HTTP 服务器)
    ├── json.hpp              (nlohmann/json JSON 解析)
    ├── aixlog.hpp            (日志系统)
    └── SessionBuilder.h      (会话 ID 生成)
```

所有依赖均为项目已有的 header-only 库，无需额外引入。