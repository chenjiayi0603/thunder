package store

import (
	"context"
	"fmt"
	"io"
	"os"
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
	client *minio.Client
	bucket string
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
		return &MinIOClient{client: nil, bucket: bucket}, nil
	}

	mc := &MinIOClient{client: client, bucket: bucket}

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
func (m *MinIOClient) GetObjectURL(key string) string {
	if m.IsAvailable() {
		return fmt.Sprintf("http://%s/%s/%s", defaultMinIOEndpoint, m.bucket, key)
	}
	// #159: Fallback — admin-web serves artifacts directly via HTTP
	// Manager GET: http://thunder-admin-web.thunder:8090/api/artifacts/{typeDir}/{filename}
	return fmt.Sprintf("http://thunder-admin-web.thunder:8090/api/artifacts/%s", key)
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
