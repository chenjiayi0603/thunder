# 运行环境：与卷挂载的 deploy/、3lib、code/3party 中 .so 的 glibc 一致。
# 若 3lib 含需 GLIBC_2.38+ 的 Abseil 等，须用 24.04+（22.04 为 glibc 2.35）。
FROM ubuntu:24.04

# apt/dpkg 非交互：构建时无人回答配置提问，避免安装脚本卡住或失败。
ENV DEBIAN_FRONTEND=noninteractive

RUN apt-get update \
    && apt-get install -y --no-install-recommends \
        ca-certificates \
        bash \
        net-tools \
        procps \
        iproute2 \
        libjemalloc2 \
        libbrotli1 \
        libsnappy1v5 \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /thunder
