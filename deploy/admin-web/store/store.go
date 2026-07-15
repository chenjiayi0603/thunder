package store

import (
	"context"
	"database/sql"
	"encoding/json"
	"fmt"
	"strings"
	"time"

	clientv3 "go.etcd.io/etcd/client/v3"
	_ "github.com/mattn/go-sqlite3"
)

type Store struct {
	etcd *clientv3.Client
	db   *sql.DB
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

	return &Store{etcd: cli, db: db}, nil
}

func (s *Store) Close() {
	if s.etcd != nil { s.etcd.Close() }
	if s.db != nil { s.db.Close() }
}
