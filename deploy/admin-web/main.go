package main

import (
	"log"
	"net/http"
	"net/http/httputil"
	"net/url"
	"os"
	"path/filepath"

	"thunder-admin-web/handler"
	"thunder-admin-web/store"
)

func main() {
	etcdEP := os.Getenv("ETCD_ENDPOINTS")
	if etcdEP == "" {
		etcdEP = "http://127.0.0.1:12379"
	}

	// Init stores
	// admin.db + artifacts 存 PVC subPath ".admin-web"
	// Pod 重启不丢，包含审计数据
	dataDir := "/app/data"
	if err := os.MkdirAll(dataDir, 0755); err != nil {
		log.Fatalf("init data dir: %v", err)
	}
	s, err := store.New(etcdEP, filepath.Join(dataDir, "admin.db"))
	if err != nil {
		log.Fatalf("init store: %v", err)
	}
	defer s.Close()

	// Init K8s client (nil if not in-cluster — local dev mode)
	var k8sClient handler.K8sPodClient
	kc, err := NewK8sClient("")
	if err != nil {
		log.Printf("WARNING: K8s client unavailable (%v) — running in local dev mode, deploySO will use NFS fallback", err)
	} else {
		k8sClient = kc
		log.Printf("K8s client initialized (namespace=%s)", kc.namespace)
	}

	h := handler.New(s, k8sClient)

	mux := http.NewServeMux()

	// API routes
	mux.HandleFunc("/api/overview", h.Overview)
	mux.HandleFunc("/api/storage/stats", h.StorageOverview)
	mux.HandleFunc("/api/nodes", h.Nodes)
	mux.HandleFunc("/api/canary/", h.Canary)
	mux.HandleFunc("/api/config/", h.Config)
	mux.HandleFunc("/api/lua/", h.Lua)
	mux.HandleFunc("/api/plugins/", h.Plugins)
	mux.HandleFunc("/api/plugins", h.Plugins)
	mux.HandleFunc("/api/audit", h.Audit)
	mux.HandleFunc("/api/etcd/keys", h.EtcdBrowser)

	// #159: Artifact file serving (Manager Pull — HTTP GET .so / .lua)
	mux.Handle("/api/artifacts/", http.StripPrefix("/api/artifacts/",
		http.FileServer(http.Dir("/app/data/artifacts"))))

	// #160: MinIO Console 反向代理 — 解决远程浏览器无法直连 NodePort 30091
	// MinIO Console 是 React SPA，JS 会发绝对路径请求 (/api/v1/..., /ws, /static/...)，
	// 必须把所有 MinIO Console 使用的路径前缀都代理过去，iframe 才能正常工作。
	minioTarget := os.Getenv("MINIO_CONSOLE_URL")
	if minioTarget == "" {
		minioTarget = "http://thunder-minio.thunder:9001" // 集群内 DNS
	}
	minioURL, err := url.Parse(minioTarget)
	if err != nil {
		log.Fatalf("MINIO_CONSOLE_URL 解析失败: %v", err)
	}
	log.Printf("MinIO Console 代理 → %s", minioTarget)

	minioProxy := httputil.NewSingleHostReverseProxy(minioURL)
	origErrHandler := minioProxy.ErrorHandler
	minioProxy.ErrorHandler = func(w http.ResponseWriter, r *http.Request, err error) {
		log.Printf("MinIO proxy error: %v (url=%s)", err, r.URL.String())
		if origErrHandler != nil {
			origErrHandler(w, r, err)
		} else {
			http.Error(w, "minio proxy error: "+err.Error(), 502)
		}
	}

	// #160: MinIO Console 返回 X-Frame-Options: DENY 导致浏览器拒绝 iframe 嵌入
	// 必须剥离此头，并在 CSP 中追加 frame-ancestors 允许 admin-web 自身嵌入
	minioProxy.ModifyResponse = func(r *http.Response) error {
		r.Header.Del("X-Frame-Options")
		csp := r.Header.Get("Content-Security-Policy")
		if csp != "" {
			r.Header.Set("Content-Security-Policy", csp+"; frame-ancestors 'self'")
		}
		return nil
	}

	// 代理 /api/minio/ → MinIO Console 入口 (strip 前缀，因为 MinIO 不认这个路径)
	mux.Handle("/api/minio/", http.StripPrefix("/api/minio", minioProxy))
	// 代理 /static/ → MinIO Console 静态资源 (JS/CSS，不 strip)
	mux.Handle("/static/", minioProxy)
	// 代理 /api/v1/ → MinIO Console JS 发出的管理 API 请求
	mux.Handle("/api/v1/", minioProxy)
	// 代理 /ws → MinIO Console WebSocket 实时通知
	mux.Handle("/ws", minioProxy)
	mux.Handle("/ws/", minioProxy)

	// Static files — serve from static/ (Nacos-style SPA)
	fs := http.FileServer(http.Dir("static"))
	mux.Handle("/", fs)

	port := os.Getenv("PORT")
	if port == "" {
		port = "8090"
	}
	log.Printf("Thunder Admin listening on :%s", port)
	log.Fatal(http.ListenAndServe(":"+port, corsMiddleware(mux)))
}

func corsMiddleware(next http.Handler) http.Handler {
	return http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		w.Header().Set("Access-Control-Allow-Origin", "*")
		w.Header().Set("Access-Control-Allow-Methods", "GET,POST,PUT,DELETE,OPTIONS")
		w.Header().Set("Access-Control-Allow-Headers", "Content-Type")
		if r.Method == "OPTIONS" { w.WriteHeader(204); return }
		next.ServeHTTP(w, r)
	})
}
