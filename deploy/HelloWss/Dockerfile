FROM ubuntu:26.04
RUN apt-get update && apt-get install -y --no-install-recommends \
    libjemalloc2 libluajit-5.1-2 ca-certificates && \
    rm -rf /var/lib/apt/lists/*
COPY deploy/lib/lib*.so* /usr/lib/x86_64-linux-gnu/
COPY deploy/3lib/*.so* /usr/lib/x86_64-linux-gnu/
RUN ldconfig
ENV NODE_VERSION=v1
COPY deploy/HelloWss/ /app/
WORKDIR /app
COPY deploy/HelloWss/entrypoint.sh /app/entrypoint.sh
RUN chmod +x /app/entrypoint.sh
ENTRYPOINT ["./entrypoint.sh"]
