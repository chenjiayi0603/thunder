package store

import (
	"context"
	"fmt"
	"io"
	"os"
	"syscall"
	"time"

	"github.com/minio/minio-go/v7"
	"github.com/minio/minio-go/v7/pkg/credentials"
)

const (
	defaultMinIOEndpoint  = "thunder-minio.thunder:9000"
	defaultMinIOBucket    = "artifacts"
	minIOConnectTimeout   = 5 * time.Second
)

// MinIOClient wraps minio-go for artifact storage operations.
// If endpoint is empty, client is nil (local dev mode — fallback to PVC).
type MinIOClient struct {
	client   *minio.Client
	bucket   string
	endpoint string // actual endpoint (for constructing download URLs)
}

// NewMinIOClient creates a MinIO client. Returns nil client if endpoint is empty.
func NewMinIOClient(endpoint, bucket string) (*MinIOClient, error) {
	if endpoint == "" {
		endpoint = defaultMinIOEndpoint
	}
	if bucket == "" {
		bucket = defaultMinIOBucket
	}

	// Check if MinIO is available via env var; empty = local dev mode
	if os.Getenv("MINIO_ENDPOINT") == "" && endpoint == defaultMinIOEndpoint {
		// Try to detect if we're running in K8s (MinIO Service DNS should resolve)
		// For now: always try to connect, fall back gracefully
	}

	client, err := minio.New(endpoint, &minio.Options{
		Creds:  credentials.NewStaticV4("minioadmin", "minioadmin", ""),
		Secure: false,
	})
	if err != nil {
		// MinIO unreachable → return client=nil but non-nil MinIOClient for fallback URL
		fmt.Printf("WARNING: MinIO unavailable (%v) — using admin-web self-serve artifacts\n", err)
		return &MinIOClient{client: nil, bucket: bucket, endpoint: endpoint}, nil
	}

	mc := &MinIOClient{client: client, bucket: bucket, endpoint: endpoint}

	// Ensure bucket exists on startup
	ctx, cancel := context.WithTimeout(context.Background(), minIOConnectTimeout)
	defer cancel()
	exists, err := client.BucketExists(ctx, bucket)
	if err != nil {
		fmt.Printf("WARNING: MinIO bucket check failed (%v) — using admin-web self-serve\n", err)
		mc.client = nil
		return mc, nil
	}
	if !exists {
		if err := client.MakeBucket(ctx, bucket, minio.MakeBucketOptions{}); err != nil {
			fmt.Printf("WARNING: MinIO create bucket failed (%v) — using admin-web self-serve\n", err)
			mc.client = nil
		}
	}

	return mc, nil
}

// IsAvailable returns true if the MinIO client is initialized.
func (m *MinIOClient) IsAvailable() bool {
	return m != nil && m.client != nil
}

// PutObject uploads data to MinIO bucket at the given key.
func (m *MinIOClient) PutObject(key string, reader io.Reader, size int64) error {
	if !m.IsAvailable() {
		return fmt.Errorf("minio not available")
	}
	ctx, cancel := context.WithTimeout(context.Background(), 30*time.Second)
	defer cancel()
	_, err := m.client.PutObject(ctx, m.bucket, key, reader, size, minio.PutObjectOptions{})
	return err
}

// GetObjectURL returns the HTTP URL for downloading an artifact.
// Priority: MinIO if available, else admin-web self-serve HTTP FileServer.
// Uses the actual configured endpoint (not hardcoded K8s DNS) so both K8s and
// Docker Compose (127.0.0.1) work correctly.
func (m *MinIOClient) GetObjectURL(key string) string {
	if m.IsAvailable() {
		return fmt.Sprintf("http://%s/%s/%s", m.endpoint, m.bucket, key)
	}
	// #159: Fallback — admin-web serves artifacts directly via HTTP
	// Default uses K8s Service DNS; override via ADMIN_WEB_URL for Docker Compose (127.0.0.1:8090).
	adminURL := os.Getenv("ADMIN_WEB_URL")
	if adminURL == "" {
		adminURL = "thunder-admin-web.thunder:8090"
	}
	return fmt.Sprintf("http://%s/api/artifacts/%s", adminURL, key)
}

// ListObjects returns object keys under a prefix in the bucket.
func (m *MinIOClient) ListObjects(prefix string) ([]minio.ObjectInfo, error) {
	if !m.IsAvailable() {
		return nil, fmt.Errorf("minio not available")
	}
	ctx, cancel := context.WithTimeout(context.Background(), 10*time.Second)
	defer cancel()

	var objects []minio.ObjectInfo
	for obj := range m.client.ListObjects(ctx, m.bucket, minio.ListObjectsOptions{
		Prefix:    prefix,
		Recursive: true,
	}) {
		if obj.Err != nil {
			return nil, obj.Err
		}
		objects = append(objects, obj)
	}
	return objects, nil
}

// StorageStats holds MinIO storage overview metrics for the admin-web dashboard.
//
// Capacity is obtained via syscall.Statfs on the MinIO data directory.
// In single-node mode (Docker Compose), MinIO itself uses statfs internally
// to track disk space — this is the same system call and equally accurate.
// For distributed deployments, use Prometheus metrics or mc admin info.
//
// Health, bucket count, and object stats come from the minio-go S3 API.
type StorageStats struct {
	Connected   bool  `json:"connected"`
	TotalBytes  int64 `json:"total_bytes"`
	FreeBytes   int64 `json:"free_bytes"`
	UsedBytes   int64 `json:"used_bytes"`
	BucketCount int   `json:"bucket_count"`
	ObjectCount int   `json:"object_count"`
	TotalSize   int64 `json:"total_size"`
	Healthy     bool  `json:"healthy"`
}

// StorageStats returns storage overview for the dashboard.
func (m *MinIOClient) StorageStats() *StorageStats {
	stats := &StorageStats{}

	if !m.IsAvailable() {
		return stats
	}
	stats.Connected = true

	// Health check + bucket count (minio-go S3 API)
	ctx, cancel := context.WithTimeout(context.Background(), 3*time.Second)
	defer cancel()

	exists, err := m.client.BucketExists(ctx, m.bucket)
	stats.Healthy = err == nil && exists

	buckets, err := m.client.ListBuckets(ctx)
	if err == nil {
		stats.BucketCount = len(buckets)
	}

	// Object count + total size in artifacts bucket (minio-go)
	ctx2, cancel2 := context.WithTimeout(context.Background(), 5*time.Second)
	defer cancel2()
	var objCount int
	var totalSize int64
	for obj := range m.client.ListObjects(ctx2, m.bucket, minio.ListObjectsOptions{
		Recursive: true,
		MaxKeys:   10000,
	}) {
		if obj.Err != nil {
			break
		}
		objCount++
		totalSize += obj.Size
	}
	stats.ObjectCount = objCount
	stats.TotalSize = totalSize

	// Capacity: statfs on MinIO data directory (same syscall MinIO uses internally)
	// Set MINIO_DATA_DIR to the MinIO data path for accurate per-disk reporting.
	dataDir := os.Getenv("MINIO_DATA_DIR")
	if dataDir == "" {
		dataDir = "/"
	}
	var fs syscall.Statfs_t
	if syscall.Statfs(dataDir, &fs) == nil {
		stats.TotalBytes = int64(fs.Blocks) * int64(fs.Bsize)
		stats.FreeBytes = int64(fs.Bavail) * int64(fs.Bsize)
		stats.UsedBytes = stats.TotalBytes - stats.FreeBytes
		if stats.UsedBytes < 0 {
			stats.UsedBytes = 0
		}
	}

	return stats
}
