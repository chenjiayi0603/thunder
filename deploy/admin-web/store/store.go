package store

import (
	"database/sql"
	"fmt"
	"os"
	"strings"
	"time"

	clientv3 "go.etcd.io/etcd/client/v3"
	_ "github.com/mattn/go-sqlite3"
)

type Store struct {
	etcd  *clientv3.Client
	db    *sql.DB
	MinIO *MinIOClient
}

func New(etcdEP, dbPath string) (*Store, error) {
	cli, err := clientv3.New(clientv3.Config{
		Endpoints:   strings.Split(etcdEP, ","),
		DialTimeout: 5 * time.Second,
	})
	if err != nil { return nil, fmt.Errorf("etcd: %w", err) }

	db, err := sql.Open("sqlite3", dbPath)
	if err != nil { return nil, fmt.Errorf("sqlite: %w", err) }
	if err := initDB(db); err != nil { return nil, err }

	// #159: MinIO client for artifact storage (graceful fallback to admin-web self-serve)
	minioEP := os.Getenv("MINIO_ENDPOINT")
	minioClient, _ := NewMinIOClient(minioEP, "")

	return &Store{etcd: cli, db: db, MinIO: minioClient}, nil
}

func (s *Store) Close() {
	if s.etcd != nil { s.etcd.Close() }
	if s.db != nil { s.db.Close() }
}
