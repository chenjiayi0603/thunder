# picohttpparser 优化分析

> 数据来源: `docs/performance/10-vs-nginx-benchmark-20260610.md`
> 单次替换收益: **+49% RPS**（旧版 http_parser → picohttpparser，其余条件不变）

---

## 1. 数据

| 端点 | RPS | vs Nginx(212k) |
|------|-----|----------------|
| ModuleRaw（Fast Path） | 236k | +11% |
| lua_echo | 188k | −11% |
| ModuleHello（完整 pb+JSON） | 162k | −24% |

优化路线历史收益（相对值）：

| 优化项 | 收益 |
|--------|------|
| picohttpparser 替换 http_parser | **+49%** 🏆 |
| SendToClient Fast Path | +8% |
| Encode 响应头模板（vsnprintf ×5→×1） | +5% |
| codec 指针缓存 + memchr | +2% |

picohttpparser 是所有优化中**单项收益最大的**。

---

## 2. 为什么快：三个原因

### 2.1 SSE4.2 SIMD — 一次比较 16 字节

picohttpparser 核心是 `findchar_fast()`，用 `_mm_cmpestri` 指令在一个时钟周期内并行扫描 16 个字节，找到 HTTP header 分隔符（`\r\n`、`:`、空格等）：

```c
// picohttpparser.c — findchar_fast()
#if __SSE4_2__
    __m128i ranges16 = _mm_loadu_si128((const __m128i *)ranges);  // 字符范围装入寄存器
    do {
        __m128i b16 = _mm_loadu_si128((const __m128i *)buf);      // 取 16 字节
        int r = _mm_cmpestri(ranges16, ranges_size, b16, 16,      // 并行范围匹配
                    _SIDD_LEAST_SIGNIFICANT | _SIDD_CMP_RANGES | _SIDD_UBYTE_OPS);
        if (unlikely(r != 16)) { buf += r; break; }               // 命中 → 找到分隔符
        buf += 16;                                                  // 未命中 → 跳 16 字节
    } while (likely(left != 0));
#endif
```

旧版 http_parser 逐字节扫描，picohttpparser 每次跳 16 字节 → **理论扫描速度 16×**（实际受内存带宽限制，但对典型 HTTP header 仍显著）。

无 SSE4.2 时自动退回逐字节慢路径（`#else` 分支），向下兼容。

### 2.2 零分配 — 结果直接指向原始 buffer

```c
// phr_parse_request 返回结果：const char* 指针 + size_t 长度
// 直接指向入参 buf，不 malloc、不 strcpy
headers[i].name  = buf + offset;   // 指针指向原始数据
headers[i].name_len = len;
```

旧版 http_parser 通过**回调函数**逐段通知：

```
http_parser_execute()
  → on_url(parser, at, length)       // 回调 1
  → on_header_field(parser, at, len) // 回调 N
  → on_header_value(parser, at, len) // 回调 N
  → on_headers_complete(parser)      // 回调 N+1
```

每次回调都是一次函数调用，N 个 header = 2N+2 次回调 + 业务层拼接字符串（通常需要 copy）。

picohttpparser 一次调用返回所有结果，**零回调、零拷贝**。

### 2.3 无状态机开销

http_parser 是**增量状态机**，维护 `parser->state`、`parser->flags`、`parser->index` 等十几个字段，每字节都要查状态转移表。

picohttpparser 是**一遍扫描**：buffer 完整时直接跑，解析完成即返回，无状态维护，cache 友好。

---

## 3. 对比总结

| 维度 | http_parser（旧）| picohttpparser（新）|
|------|----------------|-------------------|
| 扫描方式 | 逐字节 | SSE4.2 SIMD，每次 16 字节 |
| 内存分配 | 业务层通常需 copy | 零分配，指针指向原始 buffer |
| 回调模型 | 状态机回调（2N+2 次/请求）| 单次调用，直接返回结构体数组 |
| 状态维护 | 有（10+ 字段）| 无 |
| 增量解析 | ✅ 支持（跨包重入）| ✅ 支持（返回 -2 表示不完整）|
| 实测收益 | 基准 | **+49% RPS** |

---

## 4. 局限性

- **HTTPS Fast Path 无收益**：SSL 解密是瓶颈，HTTP 解析节省可忽略（实测 ±0%）
- **大 header 场景**：SSE4.2 优势更大（header 越长，SIMD 跳跃次数越多）；小 header（如 64B body）header 本身短，SIMD 收益已经在 +49% 里
- **无 SSE4.2 平台**：退回逐字节，与旧版持平，无负收益
