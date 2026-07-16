package handler

import (
	"encoding/json"
	"fmt"
	"net/http"
	"os"
	"path/filepath"
	"strings"
	"time"

	"thunder-admin-web/store"
)

type Handler struct {
	s *store.Store
}

func New(s *store.Store) *Handler {
	return &Handler{s: s}
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

// Plugins stub — P4
func (h *Handler) Plugins(w http.ResponseWriter, r *http.Request) {
	writeOK(w, map[string]interface{}{"plugins": []interface{}{}})
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
				"url_path":       body.URLPath,
				"script_content": body.ScriptContent,
				"version":        body.Version,
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

// Audit stub — P5
func (h *Handler) Audit(w http.ResponseWriter, r *http.Request) {
	writeOK(w, map[string]interface{}{"entries": []interface{}{}, "message": "audit log coming in P5"})
}
