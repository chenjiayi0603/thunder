# ===== 构建阶段 =====
FROM ubuntu:22.04 AS builder

LABEL maintainer="Thunder Framework"
ENV DEBIAN_FRONTEND=noninteractive

# 安装构建依赖
RUN apt-get update && apt-get install -y \
    build-essential \
    cmake \
    git \
    libssl-dev \
    libev-dev \
    pkg-config \
    curl \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /build

# 复制源代码
COPY . .

# 创建并进入构建目录
RUN mkdir -p build && cd build && \
    cmake .. \
        -DCMAKE_BUILD_TYPE=Release \
        -DCMAKE_CXX_STANDARD=20 \
        -DCMAKE_CXX_STANDARD_REQUIRED=ON && \
    make -j$(nproc) && \
    make test || true

# ===== 运行阶段 =====
FROM ubuntu:22.04

LABEL maintainer="Thunder Framework"
ENV DEBIAN_FRONTEND=noninteractive

# 安装运行时依赖
RUN apt-get update && apt-get install -y \
    libssl3 \
    libev4 \
    curl \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /app

# 从构建阶段复制二进制文件
COPY --from=builder /build/build/bin/thunder_server /app/thunder_server
COPY --from=builder /build/build/lib/* /app/lib/ 2>/dev/null || true

# 创建日志目录
RUN mkdir -p /app/logs && chmod 777 /app/logs

# 设置库路径
ENV LD_LIBRARY_PATH=/app/lib:$LD_LIBRARY_PATH

# 暴露端口
EXPOSE 8080

# 健康检查
HEALTHCHECK --interval=30s --timeout=10s --start-period=40s --retries=3 \
    CMD curl -f http://localhost:8080/health || exit 1

# 启动应用
CMD ["./thunder_server"]
