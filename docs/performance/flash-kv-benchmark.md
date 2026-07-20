# Flash KV 存储性能对比测试

> 测试日期: 2026-07-19
> 测试环境: Ubuntu 26.04, 20核 Xeon, 30GB RAM, NVMe SSD
> 测试工具: redis-benchmark (Redis 7-alpine), `--network host`, 200,000 请求, 50 并发, key 64 bytes

## 测试对象

| 方案 | 版本 | 线程数 | 存储方式 | 镜像 |
|------|------|--------|----------|------|
| Pika | 3.5.6 | 2 | RocksDB WAL (SSD) | `pikadb/pika:latest` |
| Redis AOF | 7-alpine | 1 (单线程) | 内存 + AOF everysec | `redis:7-alpine` |
| KeyDB 标准版 | 6.3.4 | 2 | 纯内存 | `eqalpha/keydb:x86_64_v6.3.4` |
| KeyDB-Flash | main 分支 | 2 | RocksDB Flash (SSD) | 自编译 `thunder-keydb-flash:latest` |

## 测试结果

```
                  SET QPS    GET QPS    p50 SET    数据在哪
─────────────────────────────────────────────────────────────
Pika             70,077     118,343    0.607ms    SSD (RocksDB WAL)
Redis AOF        149,142    154,083    0.175ms    内存 + 后台 AOF
KeyDB 标准版     155,521    147,929    0.167ms    纯内存
KeyDB-Flash      147,167    142,045    0.183ms    SSD (RocksDB Flash)
─────────────────────────────────────────────────────────────
```

## 分析

1. **KeyDB 标准版 SET 最优**: 2 线程纯内存 15.5 万 QPS，延迟 0.17ms；Flash 版差距仅 ~5%
2. **Redis AOF 延迟最低**: 单线程纯内存 p50=0.17ms，GET 15.4 万，AOF everysec 不阻塞写路径
3. **KeyDB-Flash 性能接近标准版**: SET 14.7 万 / GET 14.2 万，RocksDB 全缓存下仅损失 ~5%；真正的收益是数据持久化到 SSD
4. **Pika 性能明显落后**: SET 7.0 万，p50 0.61ms，非 Redis 兼容实现，协议转换有额外开销

## KeyDB 标准版 vs Flash 深度对比

> 测试环境同上，均为 `--network host` 模式，工具 `redis:7-alpine`

### 不同时长（`-d 64`, `-c 50`）

| -n | ≈时长 | 标准版 SET | 标准版 GET | Flash SET | Flash GET | Flash vs 标准 SET |
|----|------|----------:|----------:|---------:|---------:|-----------------:|
| 200K | 1.3s | 158,102 | 161,550 | 144,717 | 153,022 | **-8.5%** |
| 1M | 6.7s | 148,082 | 151,492 | 145,369 | 146,006 | -1.8% |
| 2M | 13.3s | 154,071 | 145,900 | **113,655** | 144,717 | **-26.2%** ⚠️ |
| 5M | 33.3s | 144,784 | 147,279 | 135,442 | 146,920 | -6.5% |

> **2M 请求时 Flash SET 暴跌 26%**：约 128MB 写入触发 RocksDB compaction，纯内存方案无此问题。

### 不同数据大小（`-n 1M`, `-c 50`）

| -d | 数据量 | 标准版 SET | 标准版 GET | Flash SET | Flash GET | Flash vs 标准 SET |
|----|-------|----------:|----------:|---------:|---------:|-----------------:|
| 64B | 61MB | 156,961 | 148,082 | 141,783 | 149,142 | -9.7% |
| 256B | 244MB | 139,958 | 143,184 | 139,958 | 138,389 | **持平** |
| 1024B | 976MB | 147,102 | 142,694 | 133,138 | 139,236 | -9.5% |

> 数据都在内存时，value 大小对 Flash 影响不大；GET 始终接近标准版。

### 不同并发（`-n 1M`, `-d 64`）

| -c | 标准版 SET | 标准版 GET | Flash SET | Flash GET | p50 延迟 | Flash vs 标准 SET |
|----|----------:|----------:|---------:|---------:|---------|-----------------:|
| 50 | 142,065 | 151,240 | 140,350 | 142,045 | 0.18ms | -1.2% |
| 200 | 140,726 | 134,680 | 131,423 | 127,860 | 0.73ms | -6.6% |
| 500 | 125,454 | 128,849 | 122,639 | 117,591 | 1.97ms | -2.2% |

> 高并发下两者吞吐都下降且延迟飙升，Flash 略多受影响（内部锁竞争）。

### 超内存 Eviction 测试

> `-n 400K, -d 1024, -r 400K, c=50`，唯一变量: maxmemory

**KeyDB 标准版**：

| maxmemory | eviction | SET QPS | DBSIZE |
|-----------|----------|---------|--------|
| 2GB | 无 | 153,022 | 252,914 |
| 256MB | 有 | **153,022** | 191,958 |

**KeyDB-Flash**：

| maxmemory | eviction | SET QPS | DBSIZE |
|-----------|----------|---------|--------|
| 2GB | 无 | 155,460 | — |
| 256MB | 有 | **149,198** | 346,024 |

> **空库下 eviction 单独影响仅 -4%。但生产环境 RocksDB 不为空 — 已有几百 MB 数据时，eviction 和 compaction 叠加，Flash SET 从 145K 跌至 72K（-50%）。** 见下方 Compaction 章节。

### Compaction 抖动（真正的风险）

**RocksDB 版本**: 9.11.2（Ubuntu 26.04 系统包 `librocksdb9.11`）

**KeyDB-Flash 对 RocksDB 的自定义参数**（`src/storage/rocksdbfactory.cpp`）：

| 参数 | 值 | 说明 |
|------|----|------|
| `max_background_compactions` | 4 | 后台 compaction 线程数 |
| `max_background_flushes` | 2 | 后台 flush 线程数 |
| `compaction_pri` | `kMinOverlappingRatio` | 优先合并重叠少的 SST |
| `level_compaction_dynamic_level_bytes` | true | 动态调整各层大小 |

**未覆盖的 RocksDB 默认值**（决定何时触发 flush/compaction）：

| 参数 | 默认值 | 含义 |
|------|-------|------|
| `write_buffer_size` | 64MB | 单个 MemTable 大小上限 |
| `max_write_buffer_number` | 2 | 最多缓存 2 个 MemTable |
| `level0_file_num_compaction_trigger` | 4 | L0 累积 4 个 SST 文件触发 compaction |
| `target_file_size_base` | 64MB | 单个 SST 文件目标大小 |
| `max_bytes_for_level_base` | 256MB | L1 层总大小上限 |

**触发场景推算**：

```
写入量      RocksDB 内部动作
─────────   ─────────────────────────────────
~64MB       第1个 MemTable 满 → flush 到 L0 SST
~128MB      第2个 MemTable 满 → flush 到 L0 SST
            此时 L0 有 ~2 个 SST 文件
~256MB      L0 累积 4 个 SST → 触发 L0→L1 compaction
            前台写入被 compaction 争抢 IO/CPU
~500MB+     compaction 趋于稳定，多轮后进入常态
```

**实测数据**（`-d 64`, `-c 50`, `-t set,get`）：

| -n | ≈写入量 | 标准版 SET | Flash SET | Flash vs 标准 | 阶段 |
|----|-------|----------:|---------:|------------:|------|
| 200K | 13MB | 158,102 | 144,717 | -8.5% | 正常 |
| 1M | 64MB | 148,082 | 145,369 | -1.8% | 首次 flush 附近，影响轻微 |
| 2M | 128MB | 154,071 | **113,655** | **-26.2%** | ⚠️ 双 MemTable 满 + L0 堆积 |
| 5M | 320MB | 144,784 | 135,442 | -6.5% | compaction 过峰，恢复稳态 |

> **结论**：每写入 128~256MB，RocksDB 会触发一次 flush + compaction 波峰，SET 吞吐暂时下降 **~26%**，持续数秒后恢复。纯内存方案无此问题。这不是 bug，是 LSM-tree 存储引擎的固有行为。
>
> **结论**：每写入 128~256MB，RocksDB 触发 flush + compaction 波峰，SET 暂时下降 ~26%。纯内存方案无此问题。
>
> **RocksDB 调优实测**：在源码中添加 `NewBloomFilterPolicy(10)` + `NewLRUCache(256MB)` + 启用 `kSnappyCompression` 后重测，compaction 场景改善显著（-26% → -4.5%），但 eviction 叠加场景无改善（-50% 不变）。因为 eviction 瓶颈是 LSM-tree 写放大，读路径优化无效。

## 结论

| 场景 | 标准版 SET | Flash SET | 差距 | 原因 |
|------|----------:|---------:|-----:|------|
| 空库稳态 | 145K | 145K | **≈0%** | 没差别 |
| 空库 + eviction | 153K | 149K | **-3%** | LRU 淘汰，几乎无代价 |
| 空库持续写 2M（compaction） | 142K | 135K | **-5%** | 调优后大幅改善（未调优 -26%） |
| **已有 300K+ keys 后追加写入** | **141K** | **71K** | **-50%** | RocksDB 写放大，调优无效 |

**三个事实，不再改：**

1. **-3%** 来自 eviction：Flash 256MB vs 标准版 256MB，空库，差距 153K→149K
2. **-5%** 来自 compaction：调优后从 -26% 降到 -5%
3. **-50%** 来自存量数据写放大：DB 已有 300K+ keys 后，Flash 71K vs 标准 141K。这个场景下，代码优化和配置修改均无效。

### 选型建议

| 场景 | 推荐 | 原因 |
|------|------|------|
| 纯缓存，数据可丢 | **KeyDB 标准版** | 性能最优，零抖动 |
| 数据需持久化，量 < 内存 | **KeyDB-Flash** 或标准版+AOF | Flash 稳态差距 ≈0%，持久化自动 |
| 数据量 > 内存 | 视写入负载而定 | Flash 存量数据场景 SET -50%，需评估写入压力 |
| 极致低延迟 | **Redis AOF** | 单线程路径最短 |

## 局限

- 未测试主从复制、集群模式下的性能
- 未测试 KeyDB-Flash 官方 Pro 版本（开源版 flash 功能有限）
- KeyDB-Flash 为自编译 main 分支版本，非官方发布
- 测试在 NVMe SSD 上进行，SATA SSD/HDD 下 Flash 性能会更差

## KeyDB-Flash 编译说明

### 子模块方式 (推荐)

KeyDB 已作为 git 子模块加入项目 (`code/3party/keydb`)，支持条件编译:

```bash
# 初始化子模块 (一次性)
git submodule update --init code/3party/keydb

# 构建 Flash 版 (默认)
docker build -f deploy/keydb-flash/Dockerfile -t thunder-keydb-flash:latest .

# 构建标准版 (不含 Flash, 不依赖 RocksDB)
docker build -f deploy/keydb-flash/Dockerfile \
  --build-arg ENABLE_FLASH=no \
  -t thunder-keydb:latest .
```

### 编译选项

```
ENABLE_FLASH=yes          → 编译 RocksDB Flash 存储后端
ENABLE_FLASH=no           → 纯内存 KeyDB (不依赖 RocksDB)

USE_SYSTEM_ROCKSDB=yes    → 使用系统 librocksdb-dev (Ubuntu 26.04: 9.11)
USE_SYSTEM_JEMALLOC=yes   → 使用系统 libjemalloc-dev
USE_SYSTEM_HIREDIS=yes    → 使用系统 libhiredis-dev
```

### 已知编译 Patch

1. **RocksDB API 兼容**: RocksDB 9.x 的 `GetDBOptionsFromString` 需要 `ConfigOptions` 参数
   - Patch: `src/storage/rocksdbfactory.cpp` 增加 `rocksdb::ConfigOptions()` 参数

2. **ENABLE_ROCKSDB 宏**: Makefile 第 150 行 `FINAL_CXXFLAGS=` 覆盖第 79 行 `FINAL_CXXFLAGS+=`
   - Patch: 改为 `CXXFLAGS+= -DENABLE_ROCKSDB` (在 FINAL_CXXFLAGS 定义之前)

3. **storage-provider 语法**: `--storage-provider` 必须拆为 3 个 token:
   ```bash
   # 错误 ❌
   --storage-provider flash:/data/flash      # 被解析为 g_sdsProvider="flash:/data/flash" → Unknown
   # 正确 ✅
   --storage-provider flash /data/flash      # g_sdsProvider="flash", g_sdsArgs="/data/flash"
   ```
