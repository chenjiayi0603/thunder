FROM ubuntu:26.04
RUN apt-get update && apt-get install -y --no-install-recommends ca-certificates && rm -rf /var/lib/apt/lists/*
WORKDIR /app
COPY admin-web /app/admin-web
COPY static/ /app/static/
COPY lua_scripts/ /app/lua_scripts/
EXPOSE 8090
CMD ["/app/admin-web"]
