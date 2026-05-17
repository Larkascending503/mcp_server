# CPP MCP-SERVER

一个基于 C++ 实现的模型上下文协议（Model Context Protocol）服务器，采用插件架构设计，支持多种传输协议（包括 MCP 2025-03-26 规范的 Streamable HTTP），具备运行时插件热插拔能力。

## 项目架构

```
                          ┌─────────────────────┐
                          │      main.cpp       │
                          │    （启动入口）       │
                          └──────────┬──────────┘
                                     │
                  ┌──────────────────┼──────────────────┐
                  │                  │                  │
          ┌───────▼───────┐  ┌──────▼───────┐   ┌───────▼────────┐
          │    Server     │  │PluginsLoader │   │   Transport    │
          │  （请求调度）   │  │  （插件管理）  │   │   （通信协议）   │
          └───────┬───────┘  └──────┬───────┘   └───────┬────────┘
                  │                 │                   │
         MCP 协议命令路由      动态库加载/（监控）      ┌─────┼──────┐
         回调覆盖机制          副本机制（staging）     │     |      │
         通知队列发送          快照式并发访问        Stdio  SSE  HTTP Stream
                                                   Streamable HTTP ← 新增
                   │                 │            
                  │          ┌──────▼───────┐
                  └─────────►│   Plugins    │
                             │ （动态链接库)  │
                             └──────────────┘
```

系统由四个核心模块组成：

- **Server**：实现 MCP 协议的请求解析、命令路由和响应构造，内置通知队列和异步写入线程
- **PluginsLoader**：负责插件的加载、卸载和热插拔，提供线程安全的快照访问接口
- **Transport**：抽象传输层，定义统一的读写接口，支持 Stdio / SSE / HTTP Stream / Streamable HTTP 四种实现
- **Plugins**：以动态链接库形式存在的功能模块，通过 C 接口与宿主交互

## 代码结构

```
mcp_server/
│
├── CMakeLists.txt                  构建配置（C++20，版本 0.8.0）
├── version.h.in                    版本号模板，CMake 构建时生成 version.h
├── README.md                       本文档
├── Docs/                           技术文档目录
│
├── src/                            源码目录
│   ├── main.cpp                    入口：命令行解析、回调配置、启动流程
│   │
│   ├── server/                     MCP 服务器核心
│   │   ├── Server.h                Server 类声明、线程模型、命令注册表
│   │   └── Server.cpp              协议命令实现、请求分发、通知队列
│   │
│   ├── transport/                  传输层实现
│   │   ├── StdioTransport.h/cpp          标准输入输出传输（默认）
│   │   ├── SSETransport.h/cpp            SSE 传输（长轮询）
│   │   ├── HttpStreamTransport.hpp/cpp   HTTP 流式传输（会话制）
│   │   └── StreamableTransport.h/cpp     Streamable HTTP 传输（MCP 2025-03-26 规范）
│   │
│   ├── loader/                     插件加载与热插拔
│   │   ├── PluginsLoader.h         PluginEntry 生命周期结构体、PluginsLoader 类
│   │   └── PluginsLoader.cpp       staging 加载、后台监控、三阶段扫描
│   │
│   ├── interface/                  接口定义
│   │   ├── ITransport.h            传输层抽象接口
│   │   └── PluginAPI.h             插件 C 接口（CreatePlugin/DestroyPlugin/HandleRequest）
│   │
│   └── utils/                      工具类
│       ├── MCPBuilder.h            MCP 响应和通知消息的 JSON 构造器
│       ├── SessionBuilder.h        会话 ID 生成器（时间戳 + 随机数）
│       └── TSingleton.h            线程安全单例模板
│
├── plugins/                        示例插件（各自编译为独立 .so/.dll）
|   ├── code-review/                代码审核prompt
|   ├── notification/               服务器通知/长任务进度通知测试工具
|   ├── sleep/                      模拟延迟工具
│   └── weather/                    天气查询工具（调用 open-meteo.com API）
│
├── include/                        第三方头文件
│   |── httplib.h                   cpp-httplib HTTP 库
│   |── json.hpp                    nlohmann/json JSON 库
|   ├── base64.hpp                  Base64 编解码
|   └── popl.hpp                    命令行参数解析器
|
└── libs_tier_01/                   第三方依赖库
    └── aixlog-1.5.0/               日志库
                       
```

## 技术文档

| 文档 | 说明 |
|------|------|
| [StreamableHTTP技术实现文档.md](Docs/StreamableHTTP技术实现文档.md) | Streamable HTTP 传输层的架构设计与实现细节 |
| [StreamableHTTP测试文档.md](Docs/StreamableHTTP测试文档.md) | 使用 curl 测试 Streamable HTTP 的完整用例 |
| [热插拔技术文档.md](Docs/热插拔技术文档.md) | 插件热插拔机制的设计文档 |

## 核心模块

### Server（请求调度）

Server 维护一个 `method → handler` 的命令路由表，覆盖了 MCP 协议定义的所有命令：

| 类别     | 命令                                  | 说明                 |
| -------- | ------------------------------------- | -------------------- |
| 生命周期 | `initialize`、`ping`                  | 握手和心跳           |
| 工具     | `tools/list`、`tools/call`            | 列出和调用工具插件   |
| 提示词   | `prompts/list`、`prompts/get`         | 列出和获取提示词插件 |
| 资源     | `resources/list`、`resources/read`    | 列出和读取资源插件   |
| 通知     | `notifications/tools/list_changed` 等 | 能力列表变更通知     |

`main.cpp` 通过 `OverrideCallback()` 将 `tools/list`、`tools/call` 等命令的默认实现替换为从 PluginsLoader 获取插件快照并遍历执行的逻辑。

Server 内部有一个独立的 **Writer 线程**，负责从通知队列中取出消息并通过 Transport 发送给客户端，避免通知发送阻塞请求处理。

### Transport

四种传输实现都继承自 `ITransport` 接口：

| 传输协议            | 通信方式                                                    | 端口 | 适用场景                     | MCP 规范版本   |
| ------------------- | ----------------------------------------------------------- | ---- | ---------------------------- | -------------- |
| **Stdio**           | stdin/stdout                                                | 无   | Claude Desktop 等本地客户端  | —              |
| **HTTP Stream**     | POST(Client请求+Server应答) + GET SSE(通知) + 会话管理      | 8080 | REST API 客户端              | 2024-11-05     |
| **SSE**             | POST(Client请求) + GET SSE(Server应答+通知)                 | 8080 | 长轮询客户端                 | —              |
| **Streamable HTTP** | POST(动态JSON/SSE响应) + GET SSE(通知) + DELETE(会话) + 会话管理 | 8080 | MCP 标准客户端               | **2025-03-26** |

> **Streamable HTTP** 是 MCP 2025-03-26 规范推荐的传输方式，支持根据客户端 `Accept` 头动态切换 JSON 响应和 SSE 流式响应。详见 [StreamableHTTP技术实现文档.md](Docs/StreamableHTTP技术实现文档.md)。

通过命令行参数选择：
```bash
./mcp_server                   # 默认 Stdio
./mcp_server -t                # HTTP Stream
./mcp_server -s                # SSE
./mcp_server -m                # Streamable HTTP（推荐）
```

### PluginsLoader（插件管理与热插拔）

插件加载器负责插件的完整生命周期管理，核心流程：

**启动加载**：扫描指定目录 → 复制到 staging → dlopen → CreatePlugin → Initialize → 设置通知系统

**运行时热插拔**：后台线程每 5 秒扫描目录变化，自动处理：
- 新增 `.so` 文件 → 加载新插件
- 文件 mtime/size 变化 → 先加载新版本，成功后替换旧版本
- 文件被删除 → 卸载插件

**并发安全**：请求线程通过 `GetPluginsSnapshot()` 获取插件列表的 `shared_ptr` 快照，在无锁状态下执行插件代码。旧插件通过引用计数延迟释放，直到所有使用它的请求完成。

详细设计参见 [热插拔技术文档.md](Docs/热插拔技术文档.md)。

### 插件接口（PluginAPI）

插件通过 C 接口与宿主交互，每个插件编译为独立的动态链接库，导出两个函数：

```c
PluginAPI* CreatePlugin();      // 创建插件实例
void DestroyPlugin(PluginAPI*); // 销毁插件实例
```

`PluginAPI` 结构体包含一组函数指针：

| 函数指针                                | 说明                              |
| --------------------------------------- | --------------------------------- |
| `GetName()` / `GetVersion()`            | 插件名称和版本                    |
| `GetType()`                             | 类型：TOOLS / PROMPTS / RESOURCES |
| `Initialize()` / `Shutdown()`           | 生命周期管理                      |
| `HandleRequest(json)`                   | 处理请求，返回 JSON 响应          |
| `GetToolCount()` / `GetTool(i)`         | 工具注册（TOOLS 类型）            |
| `GetPromptCount()` / `GetPrompt(i)`     | 提示词注册（PROMPTS 类型）        |
| `GetResourceCount()` / `GetResource(i)` | 资源注册（RESOURCES 类型）        |

插件还可以通过 `NotificationSystem` 主动向客户端发送通知。

## 支持的平台

| 平台         | 编译器  |
| ------------ | ------- |
| Ubuntu Linux | GCC     |
| Mac OS       | GCC     |

## 如何编译

```bash
git clone <your-repo-url>
cd mcp_server
mkdir build && cd build
cmake ..
make -j$(nproc)
```

编译产物：
- `build/mcp_server` — 服务器可执行文件
- `build/plugins/` — 各插件的 `.so` / `.dll` 文件

## 命令行参数

| 参数                   | 说明                             | 默认值       |
| ---------------------- | -------------------------------- | ------------ |
| `-n, --name`           | 服务器名称                       | `mcp-server` |
| `-p, --plugins`        | 插件目录路径                     | `./plugins`  |
| `-l, --logs`           | 日志目录路径（**目录必须存在**） | `./logs`     |
| `-v, --verbose`        | 启用详细日志                     | `false`      |
| `-t, --httpstream`     | 使用 HTTP Stream 传输            | —            |
| `-m, --streamable`     | 使用 Streamable HTTP 传输（MCP 2025-03-26） | —            |

## Cursor 配置 (Streamable HTTP)

```json
{
  "mcpServers": {
    "my_mcp_server": {
      "url": "http://192.168.139.107:8080/mcp",
      "headers": {
        "Accept": "application/json, text/event-stream",
        "Content-Type": "application/json"
      }
    }
  }
}
```

## Cursor 配置 (HttpStream)

```json
{
  "mcpServers": {
    "my_mcp_server": {
      "url": "http://192.168.139.107:8080/mcp",
      "headers": {
        "Accept": "application/json",
        "Content-Type": "application/json"
      }
    }
  }
}
```

## Streamable HTTP 快速测试

启动服务器后，可用以下 curl 命令快速验证：

```bash
# 1. 健康检查
curl -s http://localhost:8080/health

# 2. 建立会话
curl -s -D - -X POST http://localhost:8080/mcp \
  -H "Content-Type: application/json" \
  -H "Accept: application/json" \
  -d '{"jsonrpc":"2.0","id":1,"method":"initialize","params":{"protocolVersion":"2025-03-26","capabilities":{},"clientInfo":{"name":"test","version":"1.0.0"}}}'

# 3. 使用返回的 MCP-Session-Id 查询天气
curl -s -X POST http://localhost:8080/mcp \
  -H "Content-Type: application/json" \
  -H "Accept: application/json" \
  -H "MCP-Session-Id: <your-session-id>" \
  -d '{"jsonrpc":"2.0","id":2,"method":"tools/call","params":{"name":"get_weather","arguments":{"latitude":"39.9042","longitude":"116.4074","city":"Beijing"}}}'
```

完整测试用例参见 [StreamableHTTP测试文档.md](Docs/StreamableHTTP测试文档.md)。

## 线程模型

服务器运行时有以下线程：

```
主线程          请求处理主循环（阻塞在 transport->Read()）
Writer 线程     从通知队列取出消息并发送（condition_variable 等待）
Watcher 线程    每 5 秒扫描插件目录（热插拔启用时）
```

三者通过 `shared_mutex`（插件列表）和 `mutex + condition_variable`（通知队列）协调。

