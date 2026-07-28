package store

import "database/sql"

func initDB(db *sql.DB) error {
	_, err := db.Exec(`
		CREATE TABLE IF NOT EXISTS config_history (
			id INTEGER PRIMARY KEY AUTOINCREMENT,
			config_key TEXT NOT NULL,
			revision INTEGER NOT NULL,
			content TEXT NOT NULL,
			created_at DATETIME DEFAULT CURRENT_TIMESTAMP
		)
	`)
	if err != nil { return err }

	_, err = db.Exec(`
		CREATE TABLE IF NOT EXISTS audit_log (
			id INTEGER PRIMARY KEY AUTOINCREMENT,
			action TEXT NOT NULL,
			target TEXT NOT NULL,
			before_value TEXT,
			after_value TEXT,
			client_ip TEXT,
			created_at DATETIME DEFAULT CURRENT_TIMESTAMP
		)
	`)
	return err
}

// ConfigHistorySave saves old config value before overwrite
func (s *Store) ConfigHistorySave(key, oldValue string) error {
	if oldValue == "" { return nil }
	var maxRev int
	s.db.QueryRow(
		"SELECT COALESCE(MAX(revision),0) FROM config_history WHERE config_key=?", key,
	).Scan(&maxRev)
	_, err := s.db.Exec(
		"INSERT INTO config_history (config_key, revision, content) VALUES (?, ?, ?)",
		key, maxRev+1, oldValue,
	)
	return err
}

// ConfigHistoryList returns history entries for a config key
func (s *Store) ConfigHistoryList(key string) ([]ConfigHistoryEntry, error) {
	rows, err := s.db.Query(
		"SELECT id, revision, content, created_at FROM config_history WHERE config_key=? ORDER BY revision DESC LIMIT 50", key,
	)
	if err != nil { return nil, err }
	defer rows.Close()
	var result []ConfigHistoryEntry
	for rows.Next() {
		var e ConfigHistoryEntry
		if err := rows.Scan(&e.ID, &e.Revision, &e.Content, &e.CreatedAt); err != nil { continue }
		result = append(result, e)
	}
	return result, nil
}

type ConfigHistoryEntry struct {
	ID        int    `json:"id"`
	Revision  int    `json:"revision"`
	Content   string `json:"content"`
	CreatedAt string `json:"created_at"`
}

// AuditQuery returns audit entries filtered by target (type prefix match)
func (s *Store) AuditQuery(targetFilter string) ([]AuditEntry, error) {
	query := "SELECT id, action, target, before_value, after_value, client_ip, created_at FROM audit_log"
	var args []interface{}
	if targetFilter != "" {
		query += " WHERE target LIKE ?"
		args = append(args, targetFilter+"%")
	}
	query += " ORDER BY id DESC LIMIT 100"
	rows, err := s.db.Query(query, args...)
	if err != nil { return nil, err }
	defer rows.Close()
	var result []AuditEntry
	for rows.Next() {
		var e AuditEntry
		if err := rows.Scan(&e.ID, &e.Action, &e.Target, &e.Before, &e.After, &e.ClientIP, &e.CreatedAt); err != nil { continue }
		result = append(result, e)
	}
	return result, nil
}

// AuditLog writes an audit entry
func (s *Store) AuditLog(action, target, before, after, ip string) error {
	_, err := s.db.Exec(
		"INSERT INTO audit_log (action, target, before_value, after_value, client_ip) VALUES (?, ?, ?, ?, ?)",
		action, target, before, after, ip,
	)
	return err
}

// AuditLogList returns recent audit entries
func (s *Store) AuditLogList(actionFilter string, limit int) ([]AuditEntry, error) {
	if limit <= 0 { limit = 50 }
	query := "SELECT id, action, target, before_value, after_value, client_ip, created_at FROM audit_log"
	var args []interface{}
	if actionFilter != "" {
		query += " WHERE action = ?"
		args = append(args, actionFilter)
	}
	query += " ORDER BY id DESC LIMIT ?"
	args = append(args, limit)
	rows, err := s.db.Query(query, args...)
	if err != nil { return nil, err }
	defer rows.Close()
	var result []AuditEntry
	for rows.Next() {
		var e AuditEntry
		if err := rows.Scan(&e.ID, &e.Action, &e.Target, &e.Before, &e.After, &e.ClientIP, &e.CreatedAt); err != nil { continue }
		result = append(result, e)
	}
	return result, nil
}

type AuditEntry struct {
	ID        int    `json:"id"`
	Action    string `json:"action"`
	Target    string `json:"target"`
	Before    string `json:"before_value"`
	After     string `json:"after_value"`
	ClientIP  string `json:"client_ip"`
	CreatedAt string `json:"created_at"`
}
