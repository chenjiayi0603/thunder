package handler

import (
	"crypto/md5"
	"encoding/hex"
	"encoding/json"
	"fmt"
	"io"
	"net/http"
	"os"
	"path/filepath"
	"sort"
	"strings"
	"time"

	"thunder-admin-web/store"
)

type Handler struct {
	s   *store.Store
	k8s K8sPodClient // nil if not running in K8s (local dev mode)
}

func New(s *store.Store, k8s K8sPodClient) *Handler {
	return &Handler{s: s, k8s: k8s}
}

func writeJSON(w http.ResponseWriter, data interface{}) {
	w.Header().Set("Content-Type", "application/json")
	json.NewEncoder(w).Encode(data)
}

func writeOK(w http.ResponseWriter, data interface{}) {
	writeJSON(w, map[string]interface{}{"ok": true, "data": data})
}

func writeErr(w http.ResponseWriter, msg string) {
	w.Header().Set("Content-Type", "application/json")
	// §7.1: 所有业务错误统一 HTTP 200
	w.WriteHeader(200)
	json.NewEncoder(w).Encode(map[string]interface{}{"ok": false, "error": msg})
}

// Overview returns cluster summary
func (h *Handler) Overview(w http.ResponseWriter, r *http.Request) {
	if r.Method != "GET" { writeErr(w, "method not allowed"); return }

	// Scan etcd registry for node info
	prefix := "/thunder/registry/"
	kvs, err := h.s.EtcdGetPrefix(prefix)
	if err != nil { writeErr(w, "etcd error: "+err.Error()); return }

	type ServiceStat struct {
		NodeType string            `json:"node_type"`
		Count    int               `json:"count"`
		Online   int               `json:"online"`
		Versions map[string]int     `json:"versions"`
	}

	services := make(map[string]*ServiceStat)
	total, nOnline := 0, 0
	nowTs := time.Now().Unix()

	for key, raw := range kvs {
		// Parse key: /thunder/registry/{node_type}/{identify}
		parts := strings.Split(strings.TrimPrefix(key, prefix), "/")
		if len(parts) < 1 { continue }
		nodeType := parts[0]

		var node struct {
			Version      string `json:"node_version"`
			RegisteredAt int64  `json:"registered_at"`
		}
		if err := store.ParseJSON(raw, &node); err != nil { continue }

		online := node.RegisteredAt > 0 && (nowTs-node.RegisteredAt) < 30

		if _, ok := services[nodeType]; !ok {
			services[nodeType] = &ServiceStat{NodeType: nodeType, Versions: make(map[string]int)}
		}
		s := services[nodeType]
		s.Count++
		total++
		if online { s.Online++; nOnline++ }
		v := node.Version
		if v == "" { v = "unknown" }
		s.Versions[v]++
	}

	result := make([]ServiceStat, 0, len(services))
	for _, s := range services { result = append(result, *s) }
	sort.Slice(result, func(i, j int) bool { return result[i].NodeType < result[j].NodeType })

	writeOK(w, map[string]interface{}{
		"etcd_connected": true,
		"total_nodes":    total,
		"online_nodes":   nOnline,
		"services":       result,
	})
}

// Nodes returns node list, optional ?node_type filter
func (h *Handler) Nodes(w http.ResponseWriter, r *http.Request) {
	if r.Method != "GET" { writeErr(w, "method not allowed"); return }

	nodeTypeFilter := r.URL.Query().Get("node_type")
	prefix := "/thunder/registry/"
	if nodeTypeFilter != "" {
		prefix += nodeTypeFilter + "/"
	}

	kvs, err := h.s.EtcdGetPrefix(prefix)
	if err != nil { writeErr(w, "etcd error: "+err.Error()); return }

	type NodeInfoRaw struct {
		NodeType     string `json:"node_type"`
		IP           string `json:"node_ip"`
		Port         int    `json:"node_port"`
		NodeID       int    `json:"node_id"`
		Version      string `json:"node_version"`
		WorkerNum    int    `json:"worker_num"`
		RegisteredAt int64  `json:"registered_at"`
	}

	type NodeInfo struct {
		NodeType   string `json:"node_type"`
		IP         string `json:"ip"`
		Port       int    `json:"port"`
		NodeID     int    `json:"node_id"`
		Version    string `json:"version"`
		WorkerNum  int    `json:"worker_num"`
		Online     bool   `json:"online"`
		ActiveTime string `json:"active_time"`
	}

	now := time.Now().Unix()
	var nodes []NodeInfo
	for key, raw := range kvs {
		parts := strings.Split(strings.TrimPrefix(key, "/thunder/registry/"), "/")
		var nodeType string
		if len(parts) >= 1 { nodeType = parts[0] }

		var nr NodeInfoRaw
		if err := store.ParseJSON(raw, &nr); err != nil { continue }

		// Derive online status: registered within last 30s (lease TTL)
		online := (now - nr.RegisteredAt) < 30
		activeTime := time.Unix(nr.RegisteredAt, 0).UTC().Format(time.RFC3339)
		if nr.RegisteredAt == 0 {
			activeTime = "-"
			online = false
		}

		n := NodeInfo{
			NodeType:   nodeType,
			IP:         nr.IP,
			Port:       nr.Port,
			NodeID:     nr.NodeID,
			Version:    nr.Version,
			WorkerNum:  nr.WorkerNum,
			Online:     online,
			ActiveTime: activeTime,
		}
		nodes = append(nodes, n)
	}

	writeOK(w, map[string]interface{}{
		"node_type": nodeTypeFilter,
		"nodes":     nodes,
	})
}

// Canary reads real weights from etcd: /thunder/canary/{service}/weights
func (h *Handler) Canary(w http.ResponseWriter, r *http.Request) {
	if r.Method != "GET" && r.Method != "POST" {
		writeErr(w, "method not allowed")
		return
	}

	// Extract service from path: /api/canary/{service}/weights
	path := strings.TrimPrefix(r.URL.Path, "/api/canary/")
	path = strings.TrimSuffix(path, "/weights")
	path = strings.TrimSuffix(path, "/")
	svc := strings.ToUpper(path)
	if svc == "" {
		writeErr(w, "service required in path")
		return
	}

	key := "/thunder/canary/" + svc + "/weights"

	if r.Method == "POST" {
		// Apply new weights
		var body struct {
			Weights map[string]int `json:"weights"`
		}
		if err := json.NewDecoder(r.Body).Decode(&body); err != nil {
			writeErr(w, "invalid JSON body")
			return
		}
		// Validate sum = 100
		sum := 0
		for _, v := range body.Weights {
			sum += v
		}
		if sum != 100 {
			writeErr(w, "weights must sum to 100")
			return
		}
		// Read previous weights for diff
		var previous map[string]int
		if oldRaw, _ := h.s.EtcdGet(key); oldRaw != "" {
			json.Unmarshal([]byte(oldRaw), &previous)
		}
		raw, _ := json.Marshal(body.Weights)
		if err := h.s.EtcdPut(key, string(raw)); err != nil {
			writeErr(w, "etcd write error: "+err.Error())
			return
		}
		// §7.2: return previous + current
		writeOK(w, map[string]interface{}{
			"previous": previous,
			"current":  body.Weights,
		})
		return
	}

	// GET: read weights
	raw, err := h.s.EtcdGet(key)
	if err != nil {
		writeOK(w, map[string]interface{}{"service": svc, "active": false, "error": err.Error()})
		return
	}
	if raw == "" {
		writeOK(w, map[string]interface{}{"service": svc, "active": false, "message": "no weights configured"})
		return
	}

	var weights map[string]int
	if err := json.Unmarshal([]byte(raw), &weights); err != nil {
		writeOK(w, map[string]interface{}{"service": svc, "active": false, "error": "invalid weights JSON"})
		return
	}

	total := 0
	for _, v := range weights {
		total += v
	}

	writeOK(w, map[string]interface{}{
		"service": svc,
		"weights": weights,
		"total":   total,
		"active":  true,
	})
}

// Config stub — P4 (§7.2: GET /api/config/{module}?type=xxx, PUT /api/config/{module})
func (h *Handler) Config(w http.ResponseWriter, r *http.Request) {
	path := strings.TrimPrefix(r.URL.Path, "/api/config/")
	path = strings.TrimSuffix(path, "/")
	module := strings.ToUpper(path)

	// GET: read config  → ?type=Logic.json
	if r.Method == "GET" {
		cfgType := r.URL.Query().Get("type")
		if cfgType == "" {
			writeErr(w, "?type= query parameter required")
			return
		}
		writeOK(w, map[string]interface{}{
			"module":   module,
			"type":     cfgType,
			"content":  map[string]interface{}{},
			"revision": 0,
			"message":  "config management coming in P4",
		})
		return
	}

	// PUT: update config
	if r.Method == "PUT" {
		var body struct {
			Type    string                 `json:"type"`
			Content map[string]interface{} `json:"content"`
		}
		if err := json.NewDecoder(r.Body).Decode(&body); err != nil {
			writeErr(w, "invalid JSON body")
			return
		}
		writeOK(w, map[string]interface{}{
			"module":   module,
			"type":     body.Type,
			"revision": 1,
			"message":  "config management coming in P4",
		})
		return
	}

	writeErr(w, "method not allowed")
}

// SO plugin management — upload to local artifact store, list from NFS (deployed)
// Artifact store (upload goes here): /app/data/artifacts/{Type}/
// NFS runtime dir (deploy goes here):  /data/thunder/plugins/{Type}/
// GET  /api/plugins/{Type}      → list files in artifact store
// PUT  /api/plugins/{Type}/{file} → upload .so to artifact store
func (h *Handler) Plugins(w http.ResponseWriter, r *http.Request) {
	path := strings.TrimPrefix(r.URL.Path, "/api/plugins")
	path = strings.TrimPrefix(path, "/")
	path = strings.TrimSuffix(path, "/")

	// POST deploy: /api/plugins/{Type}/deploy
	if r.Method == "POST" && strings.HasSuffix(path, "/deploy") {
		h.deploySO(w, r, strings.TrimSuffix(path, "/deploy"))
		return
	}
	// GET deployed: /api/plugins/{Type}/deployed
	if r.Method == "GET" && strings.HasSuffix(path, "/deployed") {
		h.listDeployed(w, r, strings.TrimSuffix(path, "/deployed"))
		return
	}

	// PUT: upload to local artifact store (NOT NFS — deploy is a separate step)
	if r.Method == "PUT" {
		idx := strings.Index(path, "/")
		if idx < 0 {
			writeErr(w, "path required: /api/plugins/{Type}/{filename}")
			return
		}
		typeDir := path[:idx]
		filename := path[idx+1:]
		if typeDir == "" || filename == "" {
			writeErr(w, "type and filename required")
			return
		}
		if !strings.HasSuffix(filename, ".so") {
			writeErr(w, "only .so files allowed")
			return
		}
		artifactDir := filepath.Join("/app/data/artifacts", typeDir)
		if err := os.MkdirAll(artifactDir, 0755); err != nil {
			writeErr(w, "mkdir: "+err.Error())
			return
		}
		fpath := filepath.Join(artifactDir, filename)
		f, err := os.Create(fpath)
		if err != nil {
			writeErr(w, "create file: "+err.Error())
			return
		}
		defer f.Close()
		written, err := io.Copy(f, r.Body)
		if err != nil {
			writeErr(w, "write: "+err.Error())
			return
		}
		writeOK(w, map[string]interface{}{
			"type": typeDir, "filename": filename, "path": fpath, "size": written,
		})
		return
	}

	// GET: list files from artifact store
	if r.Method == "GET" {
		typeDir := path
		baseDir := "/app/data/artifacts"
		if typeDir == "" {
			entries, err := os.ReadDir(baseDir)
			if err != nil {
				writeErr(w, "readdir: "+err.Error())
				return
			}
			var types []map[string]interface{}
			for _, e := range entries {
				if e.IsDir() && !strings.HasPrefix(e.Name(), ".") {
					soCount := countSoFiles(filepath.Join(baseDir, e.Name()))
					types = append(types, map[string]interface{}{
						"type": e.Name(), "so_count": soCount,
					})
				}
			}
			writeOK(w, map[string]interface{}{"types": types})
			return
		}
		dir := filepath.Join(baseDir, typeDir)
		entries, err := os.ReadDir(dir)
		if err != nil {
			writeErr(w, "readdir: "+err.Error())
			return
		}
		type FileInfo struct {
			Name string `json:"filename"`
			Size int64  `json:"size"`
			Mtime string `json:"mod_time"`
		}
		var files []FileInfo
		for _, e := range entries {
			if e.IsDir() || !strings.HasSuffix(e.Name(), ".so") {
				continue
			}
			info, err := e.Info()
			if err != nil { continue }
			files = append(files, FileInfo{
				Name: e.Name(), Size: info.Size(), Mtime: info.ModTime().Format(time.RFC3339),
			})
		}
		sort.Slice(files, func(i, j int) bool { return files[i].Mtime > files[j].Mtime })
		writeOK(w, map[string]interface{}{"type": typeDir, "files": files})
		return
	}

	writeErr(w, "method not allowed, use GET or PUT")
}

func countSoFiles(dir string) int {
	entries, err := os.ReadDir(dir)
	if err != nil { return 0 }
	n := 0
	for _, e := range entries {
		if !e.IsDir() && strings.HasSuffix(e.Name(), ".so") { n++ }
	}
	return n
}

// Lua script management — reads/writes /thunder/config/module/{node_type}
// JSON format: {"module":[{"url_path":"...","script_content":"...","version":N},...]}
// C++ DoPollConfig reads this key every 5s; CmdReloadLua hot-reloads on version change.
func (h *Handler) Lua(w http.ResponseWriter, r *http.Request) {
	path := strings.TrimPrefix(r.URL.Path, "/api/lua/")
	path = strings.TrimSuffix(path, "/")
	nodeType := strings.ToUpper(path)

	if nodeType == "" {
		writeErr(w, "node_type required: /api/lua/{node_type}")
		return
	}

	cfgKey := "/thunder/config/module/" + nodeType

	// Helpers for the module array format
	readModuleConfig := func() ([]map[string]interface{}, error) {
		raw, err := h.s.EtcdGet(cfgKey)
		if err != nil {
			return nil, err
		}
		if raw == "" {
			return []map[string]interface{}{}, nil
		}
		var cfg struct {
			Module []map[string]interface{} `json:"module"`
		}
		if err := json.Unmarshal([]byte(raw), &cfg); err != nil {
			return nil, fmt.Errorf("invalid module config JSON")
		}
		return cfg.Module, nil
	}

	writeModuleConfig := func(modules []map[string]interface{}) error {
		raw, err := json.Marshal(map[string]interface{}{"module": modules})
		if err != nil {
			return err
		}
		return h.s.EtcdPut(cfgKey, string(raw))
	}

	if r.Method == "GET" {
		modules, err := readModuleConfig()
		if err != nil {
			writeErr(w, "etcd error: "+err.Error())
			return
		}
		// Flatten: extract script_name from url_path for frontend display
		type ScriptInfo struct {
			Name          string `json:"script_name"`
			URLPath       string `json:"url_path"`
			Version       int    `json:"version"`
			ScriptContent string `json:"script_content"`
			NodeType      string `json:"node_type"`
		}
		var scripts []ScriptInfo
		for _, m := range modules {
			urlPath, _ := m["url_path"].(string)
			if _, ok := m["script_content"]; !ok { continue } // skip non-Lua entries
			name := urlPath
			if idx := strings.LastIndex(urlPath, "/"); idx >= 0 {
				name = urlPath[idx+1:] + ".lua"
			}
			ver := 0
			if v, ok := m["version"].(float64); ok {
				ver = int(v)
			}
			content, _ := m["script_content"].(string)
			scripts = append(scripts, ScriptInfo{
				Name:          name,
				URLPath:       urlPath,
				Version:       ver,
				ScriptContent: content,
				NodeType:      nodeType,
			})
		}
		writeOK(w, map[string]interface{}{
			"node_type": nodeType,
			"scripts":   scripts,
		})
		return
	}

	if r.Method == "POST" {
		// POST body: {"url_path":"/hello/lua_echo","script_content":"...","version":99}
		var body struct {
			URLPath       string `json:"url_path"`
			ScriptContent string `json:"script_content"`
			Version       int    `json:"version"`
		}
		if err := json.NewDecoder(r.Body).Decode(&body); err != nil {
			writeErr(w, "invalid JSON body")
			return
		}
		if body.URLPath == "" {
			writeErr(w, "url_path required")
			return
		}

		modules, err := readModuleConfig()
		if err != nil {
			writeErr(w, "etcd error: "+err.Error())
			return
		}

		// Find and update matching module
		var previous map[string]interface{}
		found := false
		for i, m := range modules {
			if up, _ := m["url_path"].(string); up == body.URLPath {
				previous = m
				if body.Version == 0 {
					if v, ok := m["version"].(float64); ok {
						body.Version = int(v) + 1
					} else {
						body.Version = 1
					}
				}
				// Merge: preserve existing module fields (so_path, entrance_symbol, load etc.)
				// If missing from etcd (e.g. key deleted by Manager lease), restore defaults
				existing := modules[i]
				existing["script_content"] = body.ScriptContent
				existing["version"] = float64(body.Version)
				if _, ok := existing["so_path"]; !ok {
					existing["so_path"] = "plugins/HelloHttp_ModuleLua.so"
				}
				if _, ok := existing["entrance_symbol"]; !ok {
					existing["entrance_symbol"] = "create"
				}
				if _, ok := existing["load"]; !ok {
					existing["load"] = true
				}
				modules[i] = existing
				found = true
				break
			}
		}
		if !found {
			if body.Version == 0 {
				body.Version = 1
			}
			modules = append(modules, map[string]interface{}{
				"url_path":         body.URLPath,
				"script_content":   body.ScriptContent,
				"version":          body.Version,
			})
		}

		if err := writeModuleConfig(modules); err != nil {
			writeErr(w, "etcd write error: "+err.Error())
			return
		}

		// Also persist Lua script to NFS (mirrors old Python _lua_push)
		scriptsDir := filepath.Join("/data/thunder/plugins", nodeType, "scripts")
		os.MkdirAll(scriptsDir, 0755)
		// Derive file name from url_path: /hello/lua_echo → lua_echo.lua
		name := body.URLPath
		if idx := strings.LastIndex(name, "/"); idx >= 0 {
			name = name[idx+1:]
		}
		fpath := filepath.Join(scriptsDir, name+".lua")
		os.WriteFile(fpath, []byte(body.ScriptContent), 0644)

		writeOK(w, map[string]interface{}{
			"node_type": nodeType,
			"url_path":  body.URLPath,
			"version":   body.Version,
			"previous":  previous,
		})
		return
	}

	writeErr(w, "method not allowed")
}

// deploySO: Phase 1 — kubectl cp SO to each Running pod, then bump global etcd version.
// Falls back to NFS if K8s client is not available (local dev mode).
func (h *Handler) deploySO(w http.ResponseWriter, r *http.Request, typeDir string) {
	var body struct{ Filename string `json:"filename"` }
	if err := json.NewDecoder(r.Body).Decode(&body); err != nil || body.Filename == "" {
		writeErr(w, "filename required"); return
	}
	src := filepath.Join("/app/data/artifacts", typeDir, body.Filename)

	// Check artifact exists
	if _, err := os.Stat(src); err != nil {
		writeErr(w, "artifact not found: "+err.Error()); return
	}

	if h.k8s == nil {
		// Fallback: local dev mode — write to local NFS path
		h.deploySOFallbackNFS(w, r, typeDir, body.Filename)
		return
	}

	// Phase 1: client-go exec+tar to each pod
	result, err := h.deploySOToAllPods(typeDir, src, body.Filename)
	if err != nil {
		writeErr(w, "deploy: "+err.Error())
		return
	}

	// Audit log per-pod results
	for _, pr := range result.Pods {
		status := "ok"
		if !pr.Success {
			status = "fail: " + pr.Error
		}
		h.s.AuditLog("deploy", typeDir+"/"+body.Filename, pr.PodName,
			fmt.Sprintf("size=%d %s", pr.Size, status), r.RemoteAddr)
	}

	writeOK(w, map[string]interface{}{
		"type":        result.TypeDir,
		"filename":    result.Filename,
		"node_type":   result.NodeType,
		"total_pods":  result.TotalPods,
		"succeeded":   result.Succeeded,
		"failed":      result.Failed,
		"pods":        result.Pods,
		"etcd_bumped": result.EtcdBumped,
		"deployed":    result.Failed == 0,
	})
}

// deploySOFallbackNFS is the legacy NFS path for local dev without K8s.
func (h *Handler) deploySOFallbackNFS(w http.ResponseWriter, r *http.Request, typeDir, filename string) {
	src := filepath.Join("/app/data/artifacts", typeDir, filename)
	dst := filepath.Join("/data/thunder/plugins", typeDir, filename)
	if err := os.MkdirAll(filepath.Dir(dst), 0755); err != nil {
		writeErr(w, "mkdir NFS: "+err.Error()); return
	}
	srcF, err := os.Open(src)
	if err != nil { writeErr(w, "open artifact: "+err.Error()); return }
	defer srcF.Close()
	dstF, err := os.Create(dst)
	if err != nil { writeErr(w, "create NFS: "+err.Error()); return }
	defer dstF.Close()
	written, err := io.Copy(dstF, srcF)
	if err != nil { writeErr(w, "copy: "+err.Error()); return }

	nodeType := resolveNodeType(h, typeDir)

	// Compute md5 for NFS fallback
	srcData, _ := os.ReadFile(src)
	srcMd5 := md5Sum(srcData)

	if err := h.bumpEtcdModuleVersion(nodeType, filename, written, srcMd5); err != nil {
		writeErr(w, "etcd bump: "+err.Error()); return
	}

	h.s.AuditLog("deploy", typeDir+"/"+filename, "", fmt.Sprintf("size=%d (NFS fallback)", written), r.RemoteAddr)
	writeOK(w, map[string]interface{}{"type": typeDir, "filename": filename, "size": written, "deployed": true})
}

// listDeployed: list all .so files on target pods, merged with etcd metadata (version/size/md5).
// Image-bundled SO files that have never been hot-deployed show version as "镜像".
func (h *Handler) listDeployed(w http.ResponseWriter, r *http.Request, typeDir string) {
	nodeType := resolveNodeType(h, typeDir)

	type FileInfo struct {
		Name    string  `json:"filename"`
		Version string  `json:"version"` // "1.0" from etcd, or "镜像" for image-bundled
		Load    bool    `json:"load"`
		Size    int64   `json:"size"`
		Md5     string  `json:"md5"`
	}

	// 1. Get actual SO files from target pods (via kubectl exec ls)
	actualFiles := make(map[string]int64) // filename → size
	if h.k8s != nil {
		podNames, err := h.k8s.ListPodNames(getLabelForTypeDir2(typeDir))
		if err == nil && len(podNames) > 0 {
			// Use first Running pod to list SO files
			for _, podName := range podNames {
				files, err := h.listPodPlugins(podName)
				if err == nil && len(files) > 0 {
					for name, sz := range files {
						actualFiles[name] = sz
					}
					break
				}
			}
		}
	}

	// 2. Get etcd metadata (version, md5, load)
	etcdMeta := make(map[string]FileInfo) // filename → metadata
	etcdKey := "/thunder/config/module/" + nodeType
	raw, _ := h.s.EtcdGet(etcdKey)
	if raw != "" {
		var cfg struct {
			Module []map[string]interface{} `json:"module"`
		}
		if json.Unmarshal([]byte(raw), &cfg) == nil {
			for _, m := range cfg.Module {
				sp, _ := m["so_path"].(string)
				if sp == "" {
					continue
				}
				name := filepath.Base(sp)
				ver, _ := m["version"].(float64)
				load, _ := m["load"].(bool)
				sz, _ := m["size"].(float64)
				md5str, _ := m["md5"].(string)
				etcdMeta[name] = FileInfo{
					Name: name, Load: load,
					Version: fmt.Sprintf("%.0f", ver),
					Size:    int64(sz),
					Md5:     md5str,
				}
			}
		}
	}

	// 3. Merge: actual pod files → add etcd metadata, fill gaps from pod
	var files []FileInfo
	for name, sz := range actualFiles {
		if meta, ok := etcdMeta[name]; ok {
			if meta.Size == 0 {
				meta.Size = sz
			}
			files = append(files, meta)
		} else {
			files = append(files, FileInfo{
				Name: name, Size: sz, Load: true,
				Version: "镜像",
			})
		}
	}
	// Also include etcd-only entries (deployed but file not on pod? rare)
	for name, meta := range etcdMeta {
		if _, ok := actualFiles[name]; !ok {
			files = append(files, meta)
		}
	}

	sort.Slice(files, func(i, j int) bool { return files[i].Name < files[j].Name })
	writeOK(w, map[string]interface{}{"type": typeDir, "source": "pod+etcd", "files": files})
}

// listPodPlugins returns a map of .so filename → size on a pod's /app/plugins/.
func (h *Handler) listPodPlugins(podName string) (map[string]int64, error) {
	// exec: stat -c"%s %n" /app/plugins/*.so
	output, err := h.execPodCmd(podName, "sh", "-c", "stat -c'%s %n' /app/plugins/*.so 2>/dev/null || true")
	if err != nil {
		return nil, err
	}
	result := make(map[string]int64)
	for _, line := range strings.Split(strings.TrimSpace(output), "\n") {
		line = strings.TrimSpace(line)
		if line == "" {
			continue
		}
		parts := strings.SplitN(line, " ", 2)
		if len(parts) == 2 {
			var sz int64
			fmt.Sscanf(parts[0], "%d", &sz)
			name := filepath.Base(parts[1])
			if strings.HasSuffix(name, ".so") {
				result[name] = sz
			}
		}
	}
	return result, nil
}

// execPodCmd runs a command in a pod container and returns stdout.
func (h *Handler) execPodCmd(podName, cmd string, args ...string) (string, error) {
	// Build the command
	fullCmd := append([]string{cmd}, args...)
	return h.k8s.ExecPodCmd(podName, containerName, fullCmd)
}

// getLabelForTypeDir2 is the same as getLabelForTypeDir but returns the label for pod listing.
func getLabelForTypeDir2(typeDir string) string {
	label, ok := getLabelForTypeDir(typeDir)
	if !ok {
		return ""
	}
	return label
}

// Audit: returns SQLite audit log — GET /api/audit?type=HelloHttp
func (h *Handler) Audit(w http.ResponseWriter, r *http.Request) {
	if r.Method != "GET" { writeErr(w, "method not allowed"); return }
	typeFilter := r.URL.Query().Get("type")
	entries, err := h.s.AuditQuery(typeFilter)
	if err != nil { writeErr(w, "audit query: "+err.Error()); return }
	writeOK(w, map[string]interface{}{"entries": entries})
}

// resolveNodeType finds the C++ node_type for a frontend typeDir (e.g. "HelloHttp" → "HELLO_HTTP").
// It scans etcd registry entries dynamically — no hardcoded service names.
func resolveNodeType(h *Handler, typeDir string) string {
	// Normalize typeDir: uppercase, no underscores
	normalized := strings.ToUpper(strings.ReplaceAll(typeDir, "_", ""))

	// Scan etcd registry for matching node_type
	kvs, err := h.s.EtcdGetPrefix("/thunder/registry/")
	if err == nil {
		for key := range kvs {
			parts := strings.Split(strings.TrimPrefix(key, "/thunder/registry/"), "/")
			if len(parts) >= 1 {
				nt := parts[0]
				ntNorm := strings.ToUpper(strings.ReplaceAll(nt, "_", ""))
				if ntNorm == normalized {
					return nt
				}
			}
		}
	}
	// Fallback: use typeDir as-is (backward compatible)
	return strings.ToUpper(typeDir)
}

// fileMD5 returns the hex-encoded MD5 hash of a file, or empty string on error.
func fileMD5(path string) string {
	data, err := os.ReadFile(path)
	if err != nil {
		return ""
	}
	h := md5.Sum(data)
	return hex.EncodeToString(h[:])
}
