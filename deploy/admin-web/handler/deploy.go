package handler

import (
	"crypto/md5"
	"encoding/hex"
	"encoding/json"
	"fmt"
	"os"
	"path/filepath"
	"strings"
	"time"
)

// K8sPodClient is the interface for K8s pod operations needed by deploySO.
// The concrete implementation (in main package) uses client-go.
type K8sPodClient interface {
	// ListPodNames returns names of Running pods matching a label selector.
	ListPodNames(labelSelector string) ([]string, error)
	// CopyFileToPod copies a local file into a container via exec+tar.
	CopyFileToPod(podName, containerName, srcPath, destDir string) (int64, error)
	// VerifyFileInPod checks file exists in a container and matches expected size.
	VerifyFileInPod(podName, containerName, filePath string, expectedSize int64) error
	// ExecPodCmd runs a command in a container and returns stdout.
	ExecPodCmd(podName, containerName string, cmd []string) (string, error)
}

// typeDir → K8s label selector mapping.
// Used to find target pods for SO deployment.
// Container name is always "app" (standard across all thunder Deployments).
var typeDirLabelMap = map[string]string{
	"HelloHttp":  "app=thunder-hello",
	"HelloHttps": "app=thunder-hello-https",
	"HelloWs":    "app=thunder-hello-ws",
	"HelloWss":   "app=thunder-hello-wss",
	"Interface":  "app=thunder-interface",
	"Logic":      "app=thunder-logic,version=v1",
	"Logic-v2":   "app=thunder-logic,version=v2",
}

const containerName = "app"
const destPluginsDir = "/app/plugins/"
const maxRetries = 3

// PodDeployResult records the deployment outcome for a single pod.
type PodDeployResult struct {
	PodName   string `json:"pod"`
	Success   bool   `json:"success"`
	Size      int64  `json:"size,omitempty"`
	Error     string `json:"error,omitempty"`
	Retries   int    `json:"retries,omitempty"`
}

// DeployResult is the overall result of a deploy operation.
type DeployResult struct {
	TypeDir    string            `json:"type_dir"`
	Filename   string            `json:"filename"`
	NodeType   string            `json:"node_type"`
	TotalPods  int               `json:"total_pods"`
	Succeeded  int               `json:"succeeded"`
	Failed     int               `json:"failed"`
	Pods       []PodDeployResult `json:"pods"`
	EtcdBumped bool              `json:"etcd_bumped"`
}

// getLabelForTypeDir returns the K8s label selector for a typeDir, with fuzzy matching.
func getLabelForTypeDir(typeDir string) (string, bool) {
	// Exact match first
	if label, ok := typeDirLabelMap[typeDir]; ok {
		return label, true
	}
	// Fuzzy match: normalize and search
	normalized := strings.ToUpper(strings.ReplaceAll(typeDir, "_", ""))
	for k, v := range typeDirLabelMap {
		kNorm := strings.ToUpper(strings.ReplaceAll(k, "_", ""))
		if kNorm == normalized {
			return v, true
		}
	}
	return "", false
}

// DeploySOToPod copies a SO file from the artifact store to a single pod.
func (h *Handler) DeploySOToPod(podName, srcPath string) (int64, error) {
	return h.k8s.CopyFileToPod(podName, containerName, srcPath, destPluginsDir)
}

// VerifySOInPod checks that a deployed SO file exists with correct size.
func (h *Handler) VerifySOInPod(podName, filename string, expectedSize int64) error {
	filePath := filepath.Join(destPluginsDir, filename)
	return h.k8s.VerifyFileInPod(podName, containerName, filePath, expectedSize)
}

// deploySOToAllPods is the Phase 1 deployment flow:
// 1. Copy SO to each Running pod via exec+tar
// 2. Verify file integrity on each pod
// 3. Only if all pods succeed → bump global etcd version → all pods ReloadSo
//
// Returns DeployResult with per-pod details.
func (h *Handler) deploySOToAllPods(typeDir, srcPath, filename string) (*DeployResult, error) {
	result := &DeployResult{
		TypeDir:  typeDir,
		Filename: filename,
	}

	// Resolve node type for etcd key
	nodeType := resolveNodeType(h, typeDir)
	result.NodeType = nodeType

	// Get target pods
	label, ok := getLabelForTypeDir(typeDir)
	if !ok {
		return nil, fmt.Errorf("unknown typeDir %q: no K8s label mapping (available: %v)", typeDir, mapKeys(typeDirLabelMap))
	}

	podNames, err := h.k8s.ListPodNames(label)
	if err != nil {
		return nil, fmt.Errorf("list pods: %w", err)
	}
	if len(podNames) == 0 {
		return nil, fmt.Errorf("no Running pods found for label %q", label)
	}

	result.TotalPods = len(podNames)
	srcStat, _ := os.Stat(srcPath)
	var srcSize int64
	if srcStat != nil {
		srcSize = srcStat.Size()
	}

	// Compute MD5 of source file for etcd metadata
	srcData, _ := os.ReadFile(srcPath)
	srcMd5 := md5Sum(srcData)

	// Deploy to each pod with retry
	for _, podName := range podNames {
		podResult := PodDeployResult{PodName: podName}

		var lastErr error
		for attempt := 1; attempt <= maxRetries; attempt++ {
			podResult.Retries = attempt
			written, err := h.DeploySOToPod(podName, srcPath)
			if err != nil {
				lastErr = err
				if attempt < maxRetries {
					time.Sleep(time.Duration(attempt) * 500 * time.Millisecond)
				}
				continue
			}
			podResult.Size = written

			// Verify file integrity
			if err := h.VerifySOInPod(podName, filename, srcSize); err != nil {
				lastErr = fmt.Errorf("verify: %w", err)
				if attempt < maxRetries {
					time.Sleep(time.Duration(attempt) * 500 * time.Millisecond)
				}
				continue
			}

			podResult.Success = true
			lastErr = nil
			break
		}

		if !podResult.Success {
			podResult.Error = lastErr.Error()
			result.Failed++
		} else {
			result.Succeeded++
		}
		result.Pods = append(result.Pods, podResult)
	}

	// Only bump etcd if ALL pods succeeded
	if result.Failed == 0 {
		if err := h.bumpEtcdModuleVersion(nodeType, filename, srcSize, srcMd5); err != nil {
			result.EtcdBumped = false
			return result, fmt.Errorf("all pods deployed but etcd bump failed: %w", err)
		}
		result.EtcdBumped = true
	}

	return result, nil
}

// bumpEtcdModuleVersion increments the version of a SO module in the global config.
// Stores size and md5 in etcd so listDeployed can show them without needing the artifact store.
func (h *Handler) bumpEtcdModuleVersion(nodeType, filename string, size int64, md5 string) error {
	etcdKey := "/thunder/config/module/" + nodeType
	raw, _ := h.s.EtcdGet(etcdKey)
	modules := []map[string]interface{}{}
	if raw != "" {
		var cfg struct {
			Module []map[string]interface{} `json:"module"`
		}
		if err := json.Unmarshal([]byte(raw), &cfg); err == nil {
			modules = cfg.Module
		}
	}
	soPath := "plugins/" + filename
	found := false
	for i, m := range modules {
		if sp, _ := m["so_path"].(string); sp == soPath {
			ver := 0.0
			if v, ok := m["version"].(float64); ok {
				ver = v
			}
			modules[i]["version"] = ver + 1
			modules[i]["size"] = size
			modules[i]["md5"] = md5
			found = true
			break
		}
	}
	if !found {
		modules = append(modules, map[string]interface{}{
			"so_path": soPath,
			"version": 1.0,
			"load":    true,
			"size":    size,
			"md5":     md5,
		})
	}
	rawBytes, _ := json.Marshal(map[string]interface{}{"module": modules})
	newRaw := string(rawBytes)
	return h.s.EtcdPut(etcdKey, newRaw)
}

// md5Sum returns hex MD5 of data, or empty string if data is nil.
func md5Sum(data []byte) string {
	if len(data) == 0 {
		return ""
	}
	h := md5.Sum(data)
	return hex.EncodeToString(h[:])
}

func mapKeys(m map[string]string) []string {
	keys := make([]string, 0, len(m))
	for k := range m {
		keys = append(keys, k)
	}
	return keys
}
