# Thunder Admin Web — 管理后台 + SO 上传服务
FROM python:3.12-alpine

RUN pip install docker -q

WORKDIR /app

COPY index.html .
COPY server.py .
RUN mkdir -p plugins

EXPOSE 8090

CMD ["python3", "server.py", "--port", "8090"]
