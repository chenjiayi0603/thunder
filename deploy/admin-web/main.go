package main

import (
	"log"
	"net/http"
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

	h := handler.New(s)

	mux := http.NewServeMux()

	// API routes
	mux.HandleFunc("/api/overview", h.Overview)
	mux.HandleFunc("/api/nodes", h.Nodes)
	mux.HandleFunc("/api/canary/", h.Canary)
	mux.HandleFunc("/api/config/", h.Config)
	mux.HandleFunc("/api/lua/", h.Lua)
	mux.HandleFunc("/api/plugins/", h.Plugins)
	mux.HandleFunc("/api/plugins", h.Plugins)
	mux.HandleFunc("/api/audit", h.Audit)

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
