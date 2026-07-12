FROM python:3.12-alpine
WORKDIR /app
COPY index.html .
COPY server.py .
RUN mkdir -p plugins
EXPOSE 8090
CMD ["python3", "server.py", "--port", "8090"]
