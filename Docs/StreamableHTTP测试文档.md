# Streamable HTTP 传输层测试文档

本文档提供完整的 curl 命令，用于测试 Streamable HTTP 传输层的各项功能。所有测试可通过一个 shell 脚本 `test_streamable.sh` 一键执行。

---

## 目录

- [前置条件](#前置条件)
- [测试用例](#测试用例)
  - [测试 1：健康检查](#测试-1健康检查)
  - [测试 2：Initialize 握手](#测试-2initialize-握手)
  - [测试 3：initialized 通知](#测试-3initialized-通知)
  - [测试 4：列出可用工具](#测试-4列出可用工具toolslist)
  - [测试 5：调用天气插件](#测试-5调用天气插件toolscall)
  - [测试 6：Ping 心跳](#测试-6ping-心跳)
  - [测试 7：POST SSE 流式响应](#测试-7post-sse-流式响应动态切换)
  - [测试 8：GET SSE 通知长连接](#测试-8get-sse-通知长连接)
  - [测试 9：触发服务端主动推送](#测试-9触发服务端主动推送)
  - [测试 10：校验 SSE 接收数据](#测试-10校验-sse-接收数据)
  - [测试 11：销毁会话](#测试-11销毁会话delete-mcp)
  - [测试 12：错误处理](#测试-12错误处理)
- [完整测试脚本](#完整测试脚本)
- [Shell 语法速查](#shell-语法速查附录)

---

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

## 测试用例

### 测试 1：健康检查

验证服务器是否正常运行。

```bash
curl -s http://localhost:8080/health | python3 -m json.tool
```

| 参数 | 含义 |
|------|------|
| `-s` | silent 模式，不显示进度条 |
| `\| python3 -m json.tool` | 将 JSON 输出格式化为易读形式 |

**预期响应：**
```json
{
    "status": "ok",
    "transport": "streamable-http"
}
```

---

### 测试 2：Initialize 握手

建立会话，获取 `MCP-Session-Id`。这是所有后续请求的前提。

**Step 1：发送 initialize 请求，同时将响应头保存到文件**

```bash
curl -s -D /tmp/mcp_headers.txt -X POST http://localhost:8080/mcp \
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

| 参数 | 含义 |
|------|------|
| `-D /tmp/mcp_headers.txt` | 将 HTTP 响应**头**保存到文件（而不是打印到终端） |
| `-X POST` | 指定 HTTP 方法为 POST |
| `-H "Key: Value"` | 设置请求头 |
| `-d '{...}'` | 设置请求体（JSON 数据） |

**Step 2：从保存的响应头文件中提取 Session ID**

```bash
SESSION_ID=$(grep -i "^MCP-Session-Id:" /tmp/mcp_headers.txt | awk '{print $2}' | tr -d '\r')
echo "Session ID: $SESSION_ID"
```

| 命令 | 含义 |
|------|------|
| `$( ... )` | 命令替换，将括号内命令的输出赋值给变量 |
| `grep -i "^MCP-Session-Id:"` | 在文件中查找以 `MCP-Session-Id:` 开头的行（`-i` 忽略大小写） |
| `awk '{print $2}'` | 取该行的第 2 个字段（冒号后面的值） |
| `tr -d '\r'` | 删除 Windows 风格的回车符 `\r`（有些 HTTP 头会带） |

**预期响应头：**
```
HTTP/1.1 200 OK
Content-Type: application/json
MCP-Session-Id: 651fd572ad393-8632bc7
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

> **为什么用 `-D` 写文件而不是 `-D -` 打印到终端？**
> 用 `-D -` 会把响应头和响应体混在一起打印，很难用 `grep` 准确提取。
> 写到文件后，`/tmp/mcp_headers.txt` 只包含响应头，提取更可靠。

---

### 测试 3：initialized 通知

握手完成后发送通知（无 `id` 字段），服务器应返回 `202 Accepted`。

```bash
curl -s -D - -X POST "$MCP_URL" \
  -H "Content-Type: application/json" \
  -H "Accept: application/json" \
  -H "MCP-Session-Id: $SESSION_ID" \
  -d '{"jsonrpc":"2.0","method":"notifications/initialized"}'
```

| 语法 | 含义 |
|------|------|
| `"$MCP_URL"` | 引用之前定义的变量，注意双引号不能省略 |
| `$SESSION_ID` | 引用测试 2 中提取的会话 ID |
| `-D -` | 将响应头打印到标准输出（`-` 表示 stdout） |

**预期响应：**
```
HTTP/1.1 202 Accepted
```

---

### 测试 4：列出可用工具（tools/list）

获取所有已加载插件提供的工具列表。

```bash
curl -s -X POST "$MCP_URL" \
  -H "Content-Type: application/json" \
  -H "Accept: application/json" \
  -H "MCP-Session-Id: $SESSION_ID" \
  -d '{"jsonrpc":"2.0","id":2,"method":"tools/list"}' | python3 -m json.tool
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

### 测试 5：调用天气插件（tools/call）

查询北京天气（纬度 39.9，经度 116.4）。

```bash
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
                "text": "Weather Forecast for Beijing:\n\nToday's Temperature Forecast:\n..."
            }
        ],
        "isError": false
    }
}
```

---

### 测试 6：Ping 心跳

```bash
curl -s -X POST "$MCP_URL" \
  -H "Content-Type: application/json" \
  -H "Accept: application/json" \
  -H "MCP-Session-Id: $SESSION_ID" \
  -d '{"jsonrpc":"2.0","id":4,"method":"ping"}' | python3 -m json.tool
```

**预期响应：**
```json
{
    "jsonrpc": "2.0",
    "id": 4,
    "result": {}
}
```

---

### 测试 7：POST SSE 流式响应（动态切换）

当客户端 Accept 头包含 `text/event-stream` 时，服务器应切换为 SSE 流式响应。

```bash
curl -s -N -X POST "$MCP_URL" \
  -H "Content-Type: application/json" \
  -H "Accept: text/event-stream" \
  -H "MCP-Session-Id: $SESSION_ID" \
  -d '{"jsonrpc":"2.0","id":5,"method":"ping"}'
```

| 参数 | 含义 |
|------|------|
| `-N` | 禁用 curl 的输出缓冲，**实时**显示收到的数据（SSE 流式必须加） |

**预期响应格式（SSE 事件流）：**
```
event: message
data: {"jsonrpc":"2.0","id":5,"result":{}}

```

> **与测试 6 的区别**：同一个 `ping` 请求，测试 6 用 `Accept: application/json` 得到普通 JSON，测试 7 用 `Accept: text/event-stream` 得到 SSE 事件流。这体现了 Streamable HTTP 的**动态响应切换**特性。

---

### 测试 8：GET SSE 通知长连接

建立 SSE 长连接以接收服务器主动推送的通知。这个测试涉及**后台进程管理**，是脚本中最复杂的部分。

```bash
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
```

**逐行解释：**

| 代码 | 含义 |
|------|------|
| `> /tmp/sse_stream.log` | 用 `>` 重定向（无命令）**清空**文件内容。如果文件不存在则创建 |
| `curl ... > /tmp/sse_stream.log &` | 将 curl 的输出重定向到日志文件，末尾的 `&` 让命令在**后台**运行 |
| `SSE_PID=$!` | `$!` 是一个特殊变量，保存**最近一个后台进程的 PID**（进程号） |
| `trap '...' EXIT` | 注册一个**退出陷阱**：当脚本结束时（无论成功或失败），执行引号中的命令 |
| `kill $SSE_PID 2>/dev/null \|\| true` | 发送 SIGTERM 给后台 curl 进程。`2>/dev/null` 隐藏错误信息，`\|\| true` 确保即使 kill 失败也不影响退出码 |
| `sleep 1` | 等待 1 秒，让后台 curl 有时间建立 SSE 连接 |

> **为什么需要 `trap`？**
> 如果脚本中途出错或用户按 Ctrl+C 中断，后台的 curl 进程不会自动退出，会一直占用端口。
> `trap ... EXIT` 确保**无论如何退出**都会清理后台进程。

---

### 测试 9：触发服务端主动推送

调用 `logging_test` 工具，该工具会触发服务端向 GET SSE 长连接推送通知。

```bash
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
```

**预期响应：** `logging_test` 工具返回的结果。

> **此时**，后台的 GET SSE 连接（测试 8）应该已经收到了服务端推送的通知。

---

### 测试 10：校验 SSE 接收数据

等待 1 秒后，查看后台 SSE 连接接收到的数据。

```bash
sleep 1

echo "--------------- SSE 数据流起始 ---------------"
cat /tmp/sse_stream.log
echo "--------------- SSE 数据流结束 ---------------"
```

| 命令 | 含义 |
|------|------|
| `cat /tmp/sse_stream.log` | 打印日志文件的全部内容 |

**预期输出（示例）：**
```
--------------- SSE 数据流起始 ---------------
event: message
data: {"jsonrpc":"2.0","method":"notifications/message","params":{"level":"info","data":"..."}}

--------------- SSE 数据流结束 ---------------
```

---

### 测试 11：销毁会话（DELETE /mcp）

```bash
curl -s -D - -X DELETE "$MCP_URL" \
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
curl -s -D - -X POST "$MCP_URL" \
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

### 测试 12：错误处理

#### 12.1 无效 Content-Type

```bash
curl -s -D - -X POST "$MCP_URL" \
  -H "Content-Type: text/plain" \
  -d 'hello'
```

**预期：** `415 Unsupported Media Type`

#### 12.2 空请求体

```bash
curl -s -D - -X POST "$MCP_URL" \
  -H "Content-Type: application/json" \
  -H "Accept: application/json" \
  -d ''
```

**预期：** `400 Bad Request`

#### 12.3 无效 JSON

```bash
curl -s -D - -X POST "$MCP_URL" \
  -H "Content-Type: application/json" \
  -H "Accept: application/json" \
  -d '{invalid json'
```

**预期：** `400 Bad Request` + `Parse error`

#### 12.4 无会话 ID 请求

```bash
# 不携带 MCP-Session-Id
curl -s -D - -X POST "$MCP_URL" \
  -H "Content-Type: application/json" \
  -H "Accept: application/json" \
  -d '{"jsonrpc":"2.0","id":99,"method":"ping"}'
```

**预期：** `404 Not Found` + `Invalid or missing MCP-Session-Id`

#### 12.5 未初始化时发送非 initialize 请求

```bash
# 直接发送 tools/list 而不先 initialize（需要重启服务器确保无会话）
curl -s -D - -X POST "$MCP_URL" \
  -H "Content-Type: application/json" \
  -H "Accept: application/json" \
  -d '{"jsonrpc":"2.0","id":99,"method":"tools/list"}'
```

**预期：** `400 Bad Request` + `Session not initialized. Send 'initialize' request first.`

---

## 完整测试脚本

将以下内容保存为项目根目录下的 `test_streamable.sh`，一键执行所有测试。

### 使用方法

```bash
chmod +x test_streamable.sh    # 赋予执行权限（只需执行一次）
./test_streamable.sh           # 运行测试
```

### 脚本内容

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

# ----------------- SSE 测试部分 -----------------

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
```

---

## Shell 语法速查（附录）

以下是本脚本中用到的 Shell 语法，按出现顺序解释。适合没有 Shell 经验的读者。

### Shebang 行

```bash
#!/bin/bash
```

文件的**第一行**，告诉操作系统用哪个解释器来运行这个脚本。`/bin/bash` 是 Linux 最常用的 Shell。`#!` 被称为 shebang。

### `set -e`

```bash
set -e
```

**遇错即停**：如果任何一条命令的退出码非零（表示失败），脚本会立即停止，不再执行后面的命令。这能避免在第一个错误后继续执行导致的连锁失败。

### 变量定义和引用

```bash
MCP_URL="http://localhost:8080/mcp"   # 定义变量（等号两边不能有空格！）
echo "$MCP_URL"                        # 引用变量，用 $前缀
echo "${MCP_URL}/extra"                # 花括号用于避免歧义
```

> **注意**：`MCP_URL = "..."` 是**错误的**！等号两边不能有空格。

### 命令替换 `$( )`

```bash
SESSION_ID=$(grep "MCP-Session-Id" file.txt | awk '{print $2}')
```

`$( )` 会执行括号内的命令，并将**输出结果**赋值给变量。这里等价于"从文件中提取 Session-Id 的值"。

### 管道 `|`

```bash
cat file.txt | grep "error" | wc -l
```

将前一个命令的**输出**作为后一个命令的**输入**。上面的意思是："读取文件 → 过滤含 error 的行 → 统计行数"。

### 输出重定向 `>` 和 `>>`

```bash
> /tmp/sse_stream.log          # 清空文件（如果文件不存在则创建）
echo "hello" > file.txt        # 将 hello 写入文件（覆盖）
echo "world" >> file.txt       # 将 world 追加到文件末尾（不覆盖）
```

### 后台运行 `&`

```bash
curl -s -N http://localhost:8080/mcp > /tmp/log.txt &
SSE_PID=$!
```

- 命令末尾加 `&`，让该命令在**后台**运行，脚本不会等待它完成
- `$!` 保存最近一个后台进程的 **PID**（进程标识符，类似进程的身份证号）

### `trap` 退出陷阱

```bash
trap 'kill $SSE_PID 2>/dev/null || true' EXIT
```

注册一个**回调函数**：当脚本收到 `EXIT` 信号（即脚本结束时）自动执行引号中的命令。

| 部分 | 含义 |
|------|------|
| `kill $SSE_PID` | 向后台 curl 进程发送终止信号 |
| `2>/dev/null` | 将标准错误（stderr）重定向到"黑洞"，隐藏报错信息 |
| `\|\| true` | 即使 `kill` 失败（进程已不存在），也返回成功，避免 `set -e` 中断脚本 |
| `EXIT` | 特殊信号，脚本正常退出、出错退出、被 Ctrl+C 中断时都会触发 |

### `sleep N`

```bash
sleep 1       # 等待 1 秒
sleep 0.5     # 等待 0.5 秒
```

### 文本处理三件套

#### `grep` — 过滤行

```bash
grep "error" file.txt              # 输出包含 "error" 的行
grep -i "MCP-Session-Id" file.txt  # -i 忽略大小写
grep "^MCP-Session-Id:" file.txt   # ^ 表示行首匹配
```

#### `awk` — 按列提取

```bash
echo "MCP-Session-Id: abc123" | awk '{print $2}'
# 输出: abc123
```

`awk '{print $2}'` 表示取每一行的**第 2 个字段**（以空格分隔）。

#### `tr` — 字符替换/删除

```bash
echo "hello\r" | tr -d '\r'    # 删除回车符，输出: hello
```

`tr -d '\r'` 删除所有 `\r`（回车符）。HTTP 响应头通常以 `\r\n` 结尾，需要去掉 `\r` 才能正确匹配。

### curl 常用参数

| 参数 | 含义 | 示例 |
|------|------|------|
| `-s` | silent，不显示进度条 | `curl -s http://...` |
| `-S` | 即使 `-s` 也显示错误 | `curl -sS http://...` |
| `-N` | 禁用输出缓冲（实时显示） | SSE 流式必须加 |
| `-D -` | 打印响应头到 stdout | 调试 HTTP 响应 |
| `-D file` | 保存响应头到文件 | 提取 Session-Id |
| `-X POST` | 指定 HTTP 方法 | POST/DELETE 等 |
| `-H "K: V"` | 设置请求头 | Content-Type 等 |
| `-d '...'` | 设置请求体 | JSON 数据 |
| `&` | 命令末尾加 `&` = 后台运行 | GET SSE 长连接 |