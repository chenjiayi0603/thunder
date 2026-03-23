# HTTP Body 与 HttpCodec / gumbo-parser 说明

本文档记录 HTTP 请求/响应 body 的流向、HttpCodec 与 gumbo-parser 的职责区别，以及 Thunder 管理页的收发示例。

---

## 1. HttpCodec 与 gumbo-parser 的区别

| 维度 | HttpCodec | gumbo-parser |
|------|-----------|--------------|
| **层次** | 传输/协议层 | 应用/文档层 |
| **输入** | TCP 缓冲区的原始字节 | UTF-8 HTML 字符串 |
| **输出** | `HttpMsg`（请求行、头、body 等） | `GumboOutput`（只读 DOM 树） |
| **职责** | HTTP 报文成帧、解析请求行/头/body 边界、编码响应 | HTML5 文档解析为树形结构 |
| **本项目用途** | 连接级：浏览器 ↔ Center 的 HTTP 通信 | 无：管理页不解析 HTML body，只需 JSON API |
| **可否替换** | 否：Gumbo 不解析 HTTP 协议 | 否：HttpCodec 不解析 HTML 文档 |

**结论**：管理页场景只需 **HttpCodec**；gumbo-parser 与 HTTP body 收发无关（除非服务端需解析/校验 HTML 正文）。

---

## 2. HTTP body 由谁定义

body 是 HTTP 消息中的 **不透明载荷**，由收发双方各自定义格式：

| 方向 | 定义方 | 典型内容 |
|------|--------|----------|
| **响应**（服务端 → 浏览器） | 服务端 / Module | HTML 字符串、JSON 字符串 |
| **请求**（浏览器 → 服务端） | 浏览器 / 前端 JS | JSON、`application/x-www-form-urlencoded` 等 |

对 HttpCodec 而言，body 只是字节序列；协议层不关心内容语义。

---

## 3. 响应 body：服务端发出去的 HTML 示例

服务端把 HTML 字符串放入 `HttpMsg.body()`，经 HttpCodec 编码为 HTTP 响应。

### 代码示例（ModuleAdmin）

GET `/admin` 时由 `SendUnifiedAdminPage` 从 **`{工作目录}/conf/admin/AdminPage.html`** 读入整页 HTML，再 `set_body` 发出（构建时拷贝到 `deploy/Center/conf/admin/AdminPage.html`）。

```cpp
void ModuleAdmin::SendUnifiedAdminPage(const net::tagMsgShell& stMsgShell, const HttpMsg& oInHttpMsg)
{
    const std::string path = GetLabor()->GetWorkPath() + "/conf/admin/AdminPage.html";
    std::string html;
    ReadWholeFile(path, html);  // 失败则返回 500
    oHttpMsg.set_body(html);    // ← body 即文件中的 HTML 字符串
    // ... Content-Type: text/html; charset=utf-8，SendTo
}
```

### 实际发出的 HTTP 响应（示意）

```
HTTP/1.1 200 OK\r\n
Content-Type: text/html; charset=utf-8\r\n
Content-Length: 89\r\n
\r\n
<!DOCTYPE html>
<html lang="zh-CN">
<head><meta charset="UTF-8"/><title>Center ModuleAdmin</title></head>
<body><h1>管理</h1></body>
</html>
```

空行之后到报文结束的字节即为 **body**。浏览器收到后，用自带的 HTML 解析器解析 body，构建 DOM 并渲染。

### 数据流

```
服务端：HTML 字符串 → HttpMsg.body() → HttpCodec.Encode() → HTTP 响应字节
浏览器：收到字节 → HTTP 解析 → body 是 HTML → 浏览器解析 HTML → 渲染页面
```

---

## 4. 请求 body：服务端收到的示例

浏览器通过 `fetch` 发 POST 时，body 由前端定义（如 JSON）。

### 前端发送

```javascript
fetch('/admin', {
  method: 'POST',
  headers: {'Content-Type': 'application/json'},
  body: '{"cmd":"show","args":["nodes"]}'
});
```

### 实际到达服务端的字节流（示意）

```
POST /admin HTTP/1.1\r\n
Host: 172.24.177.85:26000\r\n
Content-Type: application/json\r\n
Content-Length: 32\r\n
\r\n
{"cmd":"show","args":["nodes"]}
```

空行之后即 **body**，这里是客户端发来的 JSON 字符串。

### 服务端读取 body

HttpCodec 解码后，body 进入 `HttpMsg.body()`，Module 直接使用：

```cpp
// ModuleAdmin::AnyMessage 中
if (!oCmdJson.Parse(oInHttpMsg.body()))   // body 即 {"cmd":"show","args":["nodes"]}
{
    oResponseData.Add("code", net::ERR_BODY_JSON);
    oResponseData.Add("msg", "error json format!");
    // ...
}
```

---

## 5. 小结

| 问题 | 答案 |
|------|------|
| body 是自己定义的吗？ | 是。响应 body 由服务端定义，请求 body 由客户端定义。 |
| 发出去的 body 示例？ | `<!DOCTYPE html><html>...</html>` 等 HTML 字符串。 |
| 收到的 body 示例？ | `{"cmd":"show","args":["nodes"]}` 等 JSON 字符串。 |
| 只有 HttpCodec 能处理 HTML 页面吗？ | 能。Html 作为 body 透传；解析发生在浏览器。 |
| 需要 gumbo-parser 吗？ | 管理页场景不需要；Gumbo 用于服务端解析 HTML，非 HTTP 收发。 |
