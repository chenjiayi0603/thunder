package store

import (
	"context"
	"encoding/json"
	"time"

	clientv3 "go.etcd.io/etcd/client/v3"
)

// EtcdGet reads a key, returns raw JSON string
func (s *Store) EtcdGet(key string) (string, error) {
	ctx, cancel := context.WithTimeout(context.Background(), 3*time.Second)
	defer cancel()
	resp, err := s.etcd.Get(ctx, key)
	if err != nil { return "", err }
	if len(resp.Kvs) == 0 { return "", nil }
	return string(resp.Kvs[0].Value), nil
}

// EtcdGetPrefix reads all keys under a prefix
func (s *Store) EtcdGetPrefix(prefix string) (map[string]string, error) {
	ctx, cancel := context.WithTimeout(context.Background(), 3*time.Second)
	defer cancel()
	resp, err := s.etcd.Get(ctx, prefix, clientv3.WithPrefix())
	if err != nil { return nil, err }
	result := make(map[string]string, len(resp.Kvs))
	for _, kv := range resp.Kvs {
		result[string(kv.Key)] = string(kv.Value)
	}
	return result, nil
}

// EtcdPut writes a key-value pair
func (s *Store) EtcdPut(key, value string) error {
	ctx, cancel := context.WithTimeout(context.Background(), 3*time.Second)
	defer cancel()
	_, err := s.etcd.Put(ctx, key, value)
	return err
}

// ParseJSON helper
func ParseJSON(raw string, v interface{}) error {
	if raw == "" { return nil }
	return json.Unmarshal([]byte(raw), v)
}
