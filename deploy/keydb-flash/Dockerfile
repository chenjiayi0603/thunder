# KeyDB-Flash 构建镜像
#
# 源码来自子模块: code/3party/keydb (git submodule)
# 条件编译: ENABLE_FLASH=yes → 启用 RocksDB Flash 存储
#          不加此参数 → 标准 KeyDB (纯内存), 不依赖 RocksDB
#
# 构建:
#   # 先初始化子模块
#   git submodule update --init code/3party/keydb
#   # 构建 Flash 版
#   docker build -f deploy/keydb-flash/Dockerfile -t thunder-keydb-flash:latest .
#   # 或构建标准版 (不含 Flash)
#   docker build -f deploy/keydb-flash/Dockerfile --build-arg ENABLE_FLASH=no -t thunder-keydb:latest .
#
# 无需网络访问 (源码来自本地子模块, 系统包来自 apt)

FROM ubuntu:26.04 AS builder

ARG MAKE_JOBS=4
ARG ENABLE_FLASH=yes

SHELL ["/bin/bash", "-c"]

# 系统依赖: 编译工具链 + RocksDB(Flash模式) + jemalloc + hiredis
RUN apt-get update && DEBIAN_FRONTEND=noninteractive apt-get install -y --no-install-recommends \
    build-essential pkg-config libssl-dev libjemalloc-dev libcurl4-openssl-dev \
    libbz2-dev libzstd-dev liblz4-dev libsnappy-dev uuid-dev tcl-dev \
    librocksdb-dev libhiredis-dev \
    && rm -rf /var/lib/apt/lists/*

# 从子模块复制源码 (无需 git clone)
COPY code/3party/keydb /tmp/KeyDB
WORKDIR /tmp/KeyDB

# Patch: RocksDB 9.x API 兼容 (ConfigOptions)
RUN sed -i 's/GetDBOptionsFromString(options, options_string, \&options)/GetDBOptionsFromString(rocksdb::ConfigOptions(), options, options_string, \&options)/' \
    src/storage/rocksdbfactory.cpp

# Patch: CXXFLAGS 追加 ENABLE_ROCKSDB (Makefile 第150行会覆盖第79行的 +=)
RUN sed -i '/USE_SYSTEM_ROCKSDB),yes/a CXXFLAGS+= -DENABLE_ROCKSDB' src/Makefile

# 编译 (条件编译: ENABLE_FLASH=yes → RocksDB Flash / =no → 纯内存 KeyDB)
RUN if [ "${ENABLE_FLASH}" = "yes" ]; then \
        make -j${MAKE_JOBS} BUILD_TLS=yes ENABLE_FLASH=yes \
            USE_SYSTEM_ROCKSDB=yes USE_SYSTEM_JEMALLOC=yes USE_SYSTEM_HIREDIS=yes; \
    else \
        make -j${MAKE_JOBS} BUILD_TLS=yes; \
    fi

RUN cd src && strip keydb-server keydb-cli keydb-benchmark \
    keydb-check-rdb keydb-check-aof keydb-sentinel

# ============================================================
# 运行阶段
FROM ubuntu:26.04

RUN apt-get update && DEBIAN_FRONTEND=noninteractive apt-get install -y --no-install-recommends \
    librocksdb9.11 libssl3t64 libjemalloc2 libcurl4t64 libbz2-1.0 libzstd1 liblz4-1 \
    libsnappy1v5 libhiredis1.1.0 ca-certificates \
    && rm -rf /var/lib/apt/lists/*

COPY --from=builder /tmp/KeyDB/src/keydb-server /usr/local/bin/
COPY --from=builder /tmp/KeyDB/src/keydb-cli /usr/local/bin/
COPY --from=builder /tmp/KeyDB/src/keydb-benchmark /usr/local/bin/
COPY --from=builder /tmp/KeyDB/src/keydb-check-rdb /usr/local/bin/
COPY --from=builder /tmp/KeyDB/src/keydb-check-aof /usr/local/bin/
COPY --from=builder /tmp/KeyDB/src/keydb-sentinel /usr/local/bin/
COPY --from=builder /tmp/KeyDB/keydb.conf /etc/keydb/keydb.conf

RUN mkdir -p /data/flash /etc/keydb \
    && ln -s /usr/local/bin/keydb-cli /usr/local/bin/redis-cli \
    && ln -s /etc/keydb/keydb.conf /etc/keydb/redis.conf

EXPOSE 6379 6380
VOLUME /data

ENTRYPOINT ["keydb-server"]
CMD ["/etc/keydb/keydb.conf", "--port", "6380"]
