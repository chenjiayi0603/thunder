package handler

import (
	"encoding/json"
	"net/http"
	"strings"

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

func writeErr(w http.ResponseWriter, msg string, code int) {
	w.Header().Set("Content-Type", "application/json")
	w.WriteHeader(code)
	json.NewEncoder(w).Encode(map[string]interface{}{"ok": false, "error": msg})
}

// Overview returns cluster summary
func (h *Handler) Overview(w http.ResponseWriter, r *http.Request) {
	if r.Method != "GET" { writeErr(w, "method not allowed", 405); return }

	// Scan etcd registry for node info
	prefix := "/thunder/registry/"
	kvs, err := h.s.EtcdGetPrefix(prefix)
	if err != nil { writeErr(w, "etcd error: "+err.Error(), 500); return }

	type ServiceStat struct {
		NodeType string            `json:"node_type"`
		Count    int               `json:"count"`
		Online   int               `json:"online"`
		Versions map[string]int     `json:"versions"`
	}

	services := make(map[string]*ServiceStat)
	total, online := 0, 0

	for key, raw := range kvs {
		// Parse key: /thunder/registry/{node_type}/{identify}
		parts := strings.Split(strings.TrimPrefix(key, prefix), "/")
		if len(parts) < 1 { continue }
		nodeType := parts[0]

		var node struct {
			Version string `json:"version"`
			Online  bool   `json:"online"`
		}
		if err := store.ParseJSON(raw, &node); err != nil { continue }

		if _, ok := services[nodeType]; !ok {
			services[nodeType] = &ServiceStat{NodeType: nodeType, Versions: make(map[string]int)}
		}
		s := services[nodeType]
		s.Count++
		total++
		if node.Online { s.Online++; online++ }
		v := node.Version
		if v == "" { v = "unknown" }
		s.Versions[v]++
	}

	result := make([]ServiceStat, 0, len(services))
	for _, s := range services { result = append(result, *s) }

	writeOK(w, map[string]interface{}{
		"etcd_connected": true,
		"total_nodes":    total,
		"online_nodes":   online,
		"services":       result,
	})
}

// Nodes returns node list, optional ?node_type filter
func (h *Handler) Nodes(w http.ResponseWriter, r *http.Request) {
	if r.Method != "GET" { writeErr(w, "method not allowed", 405); return }

	nodeTypeFilter := r.URL.Query().Get("node_type")
	prefix := "/thunder/registry/"
	if nodeTypeFilter != "" {
		prefix += nodeTypeFilter + "/"
	}

	kvs, err := h.s.EtcdGetPrefix(prefix)
	if err != nil { writeErr(w, "etcd error: "+err.Error(), 500); return }

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

	var nodes []NodeInfo
	for key, raw := range kvs {
		parts := strings.Split(strings.TrimPrefix(key, "/thunder/registry/"), "/")
		var nodeType string
		if len(parts) >= 1 { nodeType = parts[0] }

		var n NodeInfo
		if err := store.ParseJSON(raw, &n); err != nil { continue }
		n.NodeType = nodeType
		nodes = append(nodes, n)
	}

	writeOK(w, map[string]interface{}{
		"node_type": nodeTypeFilter,
		"nodes":     nodes,
	})
}

// Canary stub — depends on #134
func (h *Handler) Canary(w http.ResponseWriter, r *http.Request) {
	writeOK(w, map[string]interface{}{"message": "canary depends on #134 weighted routing", "active": false})
}

// Config stub — P4
func (h *Handler) Config(w http.ResponseWriter, r *http.Request) {
	writeOK(w, map[string]interface{}{"message": "config management coming in P4"})
}

// Plugins stub — P4
func (h *Handler) Plugins(w http.ResponseWriter, r *http.Request) {
	writeOK(w, map[string]interface{}{"plugins": []interface{}{}})
}

// Audit stub — P5
func (h *Handler) Audit(w http.ResponseWriter, r *http.Request) {
	writeOK(w, map[string]interface{}{"entries": []interface{}{}, "message": "audit log coming in P5"})
}
