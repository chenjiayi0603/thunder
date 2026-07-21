package main

import (
	"archive/tar"
	"bytes"
	"context"
	"fmt"
	"io"
	"os"
	"path/filepath"
	"time"

	corev1 "k8s.io/api/core/v1"
	metav1 "k8s.io/apimachinery/pkg/apis/meta/v1"
	"k8s.io/client-go/kubernetes"
	"k8s.io/client-go/kubernetes/scheme"
	"k8s.io/client-go/rest"
	"k8s.io/client-go/tools/remotecommand"
)

// K8sClient wraps a kubernetes clientset for in-cluster operations.
type K8sClient struct {
	clientset *kubernetes.Clientset
	config    *rest.Config
	namespace string
}

// NewK8sClient creates an in-cluster K8s client.
// Reads namespace from service account mount if not provided.
func NewK8sClient(namespace string) (*K8sClient, error) {
	config, err := rest.InClusterConfig()
	if err != nil {
		return nil, fmt.Errorf("in-cluster config: %w (not running in K8s?)", err)
	}
	config.Timeout = 30 * time.Second

	clientset, err := kubernetes.NewForConfig(config)
	if err != nil {
		return nil, fmt.Errorf("clientset: %w", err)
	}

	if namespace == "" {
		if data, err := os.ReadFile("/var/run/secrets/kubernetes.io/serviceaccount/namespace"); err == nil {
			namespace = string(data)
		}
	}
	if namespace == "" {
		namespace = "thunder"
	}

	return &K8sClient{clientset: clientset, config: config, namespace: namespace}, nil
}

// ListPodsByLabel returns all Running pods matching the given label selector.
func (k *K8sClient) ListPodsByLabel(labelSelector string) ([]corev1.Pod, error) {
	ctx, cancel := context.WithTimeout(context.Background(), 10*time.Second)
	defer cancel()

	pods, err := k.clientset.CoreV1().Pods(k.namespace).List(ctx, metav1.ListOptions{
		LabelSelector: labelSelector,
		FieldSelector: "status.phase=Running",
	})
	if err != nil {
		return nil, fmt.Errorf("list pods (ns=%s, label=%s): %w", k.namespace, labelSelector, err)
	}
	return pods.Items, nil
}

// ListPodNames returns names of Running pods matching the label selector.
func (k *K8sClient) ListPodNames(labelSelector string) ([]string, error) {
	pods, err := k.ListPodsByLabel(labelSelector)
	if err != nil {
		return nil, err
	}
	names := make([]string, len(pods))
	for i, p := range pods {
		names[i] = p.Name
	}
	return names, nil
}

// ListPodNamesByVersion returns names of Running pods whose NODE_VERSION env matches.
// Optional labelSelector narrows the search (e.g., "app=thunder-hello" for node_type).
func (k *K8sClient) ListPodNamesByVersion(version, labelSelector string) ([]string, error) {
	ctx, cancel := context.WithTimeout(context.Background(), 10*time.Second)
	defer cancel()

	opts := metav1.ListOptions{FieldSelector: "status.phase=Running"}
	if labelSelector != "" {
		opts.LabelSelector = labelSelector
	}

	pods, err := k.clientset.CoreV1().Pods(k.namespace).List(ctx, opts)
	if err != nil {
		return nil, fmt.Errorf("list pods: %w", err)
	}

	var names []string
	for _, pod := range pods.Items {
		for _, container := range pod.Spec.Containers {
			for _, env := range container.Env {
				if env.Name == "NODE_VERSION" && env.Value == version {
					names = append(names, pod.Name)
					break
				}
			}
		}
	}
	return names, nil
}

// CopyFileToPod copies a local file to a container in a pod via exec+tar.
// srcPath is the local file path; destDir is the target directory inside the container.
// Returns the size written on success.
func (k *K8sClient) CopyFileToPod(podName, containerName, srcPath, destDir string) (int64, error) {
	srcFile, err := os.Open(srcPath)
	if err != nil {
		return 0, fmt.Errorf("open src: %w", err)
	}
	defer srcFile.Close()

	stat, err := srcFile.Stat()
	if err != nil {
		return 0, fmt.Errorf("stat src: %w", err)
	}
	fileSize := stat.Size()
	fileName := filepath.Base(srcPath)

	// Build tar archive with single file
	var tarBuf bytes.Buffer
	tw := tar.NewWriter(&tarBuf)
	hdr := &tar.Header{
		Name: fileName,
		Mode: 0644,
		Size: fileSize,
	}
	if err := tw.WriteHeader(hdr); err != nil {
		return 0, fmt.Errorf("tar header: %w", err)
	}
	if _, err := io.Copy(tw, srcFile); err != nil {
		return 0, fmt.Errorf("tar copy: %w", err)
	}
	if err := tw.Close(); err != nil {
		return 0, fmt.Errorf("tar close: %w", err)
	}

	// Exec: tar xmf - -C <destDir> --no-same-owner
	cmd := []string{"tar", "xmf", "-", "-C", destDir, "--no-same-owner"}

	req := k.clientset.CoreV1().RESTClient().Post().
		Resource("pods").
		Name(podName).
		Namespace(k.namespace).
		SubResource("exec").
		VersionedParams(&corev1.PodExecOptions{
			Container: containerName,
			Command:   cmd,
			Stdin:     true,
			Stdout:    true,
			Stderr:    true,
			TTY:       false,
		}, scheme.ParameterCodec)

	exec, err := remotecommand.NewSPDYExecutor(k.config, "POST", req.URL())
	if err != nil {
		return 0, fmt.Errorf("executor: %w", err)
	}

	var stdout, stderr bytes.Buffer
	err = exec.StreamWithContext(context.Background(), remotecommand.StreamOptions{
		Stdin:  &tarBuf,
		Stdout: &stdout,
		Stderr: &stderr,
	})
	if err != nil {
		return 0, fmt.Errorf("exec stream: %w (stderr: %s)", err, stderr.String())
	}

	return fileSize, nil
}

// VerifyFileInPod checks file existence and size in a container via stat.
func (k *K8sClient) VerifyFileInPod(podName, containerName, filePath string, expectedSize int64) error {
	cmd := []string{"stat", "-c%s", filePath}

	req := k.clientset.CoreV1().RESTClient().Post().
		Resource("pods").
		Name(podName).
		Namespace(k.namespace).
		SubResource("exec").
		VersionedParams(&corev1.PodExecOptions{
			Container: containerName,
			Command:   cmd,
			Stdin:     false,
			Stdout:    true,
			Stderr:    true,
			TTY:       false,
		}, scheme.ParameterCodec)

	exec, err := remotecommand.NewSPDYExecutor(k.config, "POST", req.URL())
	if err != nil {
		return fmt.Errorf("verify executor: %w", err)
	}

	var stdout, stderr bytes.Buffer
	if err := exec.StreamWithContext(context.Background(), remotecommand.StreamOptions{
		Stdout: &stdout,
		Stderr: &stderr,
	}); err != nil {
		return fmt.Errorf("verify exec: %w (stderr: %s)", err, stderr.String())
	}

	var actualSize int64
	if _, err := fmt.Sscanf(stdout.String(), "%d", &actualSize); err != nil {
		return fmt.Errorf("parse stat output: %w (got: %q)", err, stdout.String())
	}
	if actualSize != expectedSize {
		return fmt.Errorf("size mismatch: expected %d, got %d", expectedSize, actualSize)
	}
	return nil
}

// ExecPodCmd runs a command in a container and returns combined stdout+stderr.
func (k *K8sClient) ExecPodCmd(podName, containerName string, cmd []string) (string, error) {
	req := k.clientset.CoreV1().RESTClient().Post().
		Resource("pods").
		Name(podName).
		Namespace(k.namespace).
		SubResource("exec").
		VersionedParams(&corev1.PodExecOptions{
			Container: containerName,
			Command:   cmd,
			Stdin:     false,
			Stdout:    true,
			Stderr:    true,
			TTY:       false,
		}, scheme.ParameterCodec)

	exec, err := remotecommand.NewSPDYExecutor(k.config, "POST", req.URL())
	if err != nil {
		return "", fmt.Errorf("executor: %w", err)
	}

	var stdout, stderr bytes.Buffer
	if err := exec.StreamWithContext(context.Background(), remotecommand.StreamOptions{
		Stdout: &stdout,
		Stderr: &stderr,
	}); err != nil {
		return "", fmt.Errorf("exec: %w (stderr: %s)", err, stderr.String())
	}

	if stdout.Len() > 0 {
		return stdout.String(), nil
	}
	return stderr.String(), nil
}
