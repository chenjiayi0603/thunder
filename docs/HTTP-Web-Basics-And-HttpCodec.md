# HTTP / Web 基础与项目 HttpCodec 说明

本文档整理与 HTTP、HTML/DOM、解析库及本仓库 `HttpCodec` 相关的概念，便于查阅。

---

## 1. 为何 Java / Python 做网站常比 C 省事

- **抽象层次**：高级语言生态里已有成熟的 HTTP 服务框架、路由、JSON、模板等；C 更偏系统层，常需自行拼接或使用嵌入式 HTTP 库。
- **内存与安全**：GC 降低日常业务中的内存心智负担；C 在解析不可信输入时更易踩缓冲区类问题。
- **迭代速度**：动态语言或 JVM 在改接口、试想法时反馈更快。

C 更适合做性能关键模块或嵌入式小服务，而不是默认的「全栈建站首选」。

---

## 2. 仅编解码 / 解析、不涉及网络时的库选型（概念）

| 需求 | 典型库（C/C++） |
|------|------------------|
| 解析 HTTP 报文（请求/响应行、头、body 边界、chunked） | **llhttp**、**picohttpparser**、（本仓库）**http_parser** |
| 解析 HTML → DOM 树 | **gumbo-parser**、myhtml、lexbor |
| JSON | simdjson、RapidJSON、nlohmann/json、cJSON |
| URI | uriparser |
| gzip 等 | zlib、zstd |

**llhttp** 管「HTTP 信封」；**gumbo** 管「body 里的 HTML 如何建成树」。二者层次不同。

---

## 3. HTML、DOM、表单

| 概念 | 说明 |
|------|------|
| **HTML** | 描述页面结构的标记文本（文件或 HTTP body 中的字符串）。 |
| **DOM** | 将 HTML 解析后在程序中的**树状对象模型**；可脚本访问与修改。 |
| **表单（`<form>`）** | HTML/DOM 中用于收集用户输入并按 `action`/`method` 提交的一段结构，只是 DOM 的一部分，不等于 DOM 本身。 |

关系：**HTML 字符串 → 解析 → DOM 树**；提交表单通常会产生新的 **HTTP 请求**（另一协议层）。

---

## 4. 「一整页」与「一条 HTTP 消息」

- **`HttpCodec` 一次成功解码**表示收齐 **一条完整的 HTTP/1.x 消息**（含头与 body；可能经 chunked 拼接；本实现可对 gzip body 解压）。
- 该消息的 **body 不一定是用户眼中的「整页」**：
  - **静态整页**：一个响应 body 可能就是完整 HTML。
  - **SPA + API**：首个响应常为很短 HTML 壳；表格数据在后续 **JSON** 的 API 响应里，需多次 HTTP 与前端渲染。

判断依据：看 **`Content-Type`** 与产品是否拆成多请求，不能默认「所有收到的数据拼起来就是一页 HTML」。

---

## 5. HTTP/1.0、HTTP/1.1 与 HTTP/2

| 版本 | 要点 |
|------|------|
| **HTTP/1.0** | 文本协议；连接常「一请求一关」；与 1.1 相比持久连接与 Host 等规范较弱。 |
| **HTTP/1.1** | 文本协议；默认 **keep-alive**；**`Host`** 几乎必需；**chunked** 等常见。 |
| **HTTP/2** | **二进制帧**、多路复用、HPACK 压缩头部；通常经 TLS + ALPN；与 1.x **报文格式不兼容**，不能用 1.x 文本解析器直接解析 TCP 上的 h2 流。 |

---

## 6. 本仓库 `HttpCodec`（`code/Net/src/codec/HttpCodec.cpp`）支持的协议版本

- 使用 **`http_parser`**（`util/http/http_parser.h`），按 **HTTP/1.x 文本格式**解析与编码（状态行/请求行、`Header: value\r\n`、body、chunked 等）。
- **不支持 HTTP/2**（无 h2 帧层）。
- 编码时若版本 **低于 1.1**（`http_major < 1` 或 `1.0`），会添加 **`Connection: close`**；否则 **`Connection: keep-alive`**（见 `HttpCodec.cpp` 中响应编码分支）。

---

## 7. 参考路径

- HTTP 编解码：`code/Net/src/codec/HttpCodec.cpp`、`code/Net/include/codec/HttpCodec.hpp`
- HTTP 消息结构：`code/Net` 下 `http.pb` 生成的 `HttpMsg` 等

---

*文档为知识整理，与具体业务需求变更无关时请按需更新。*
