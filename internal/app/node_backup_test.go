// SPDX-License-Identifier: Apache-2.0
package app

import (
	"context"
	"encoding/json"
	"io"
	"log/slog"
	"net/http"
	"net/http/httptest"
	"os"
	"path/filepath"
	"strings"
	"testing"
	"time"

	meshcore "github.com/meshcore-go/meshcore-go"
	"meshcore.local/meshcore/internal/nodebackup"
)

func TestHostUsefulStateBackupAndAuthenticatedDownload(t *testing.T) {
	cfg := adminTestConfig(t)
	cfg.BotRuntime = ""
	cfg.EnabledRoles = RoleSelection{"repeater"}
	dir := filepath.Join(cfg.StateDir, "repeater")
	if err := os.MkdirAll(dir, 0700); err != nil {
		t.Fatal(err)
	}
	for name, value := range map[string][]byte{
		"identity.seed": make([]byte, 32), "state.json": []byte(`{"name":"Birch","saved":"useful"}`),
		"packets.log": []byte("unneeded log"), "pending.stage": []byte("uncommitted"),
	} {
		if err := os.WriteFile(filepath.Join(dir, name), value, 0600); err != nil {
			t.Fatal(err)
		}
	}
	cfg.AdminPasswordEnv = "MESHCORE_BACKUP_TEST_PASSWORD"
	t.Setenv(cfg.AdminPasswordEnv, "private-credential")
	records, err := hostBackupSnapshot(context.Background(), cfg)
	if err != nil {
		t.Fatal(err)
	}
	for _, name := range []string{"files/repeater/identity.seed", "files/repeater/state.json", "config/host.json", "config/environment.json"} {
		if records[name] == nil {
			t.Fatalf("useful record missing: %s", name)
		}
	}
	for _, name := range []string{"files/repeater/packets.log", "files/repeater/pending.stage"} {
		if records[name] != nil {
			t.Fatalf("excluded record present: %s", name)
		}
	}
	var credentials map[string]string
	if err := json.Unmarshal(records["config/environment.json"], &credentials); err != nil || credentials[cfg.AdminPasswordEnv] != "private-credential" {
		t.Fatal("configured credential missing")
	}
	cfg.nodeBackup, err = nodebackup.New(context.Background(), filepath.Join(cfg.StateDir, "node-backup"),
		func(ctx context.Context) (map[string][]byte, error) { return hostBackupSnapshot(ctx, cfg) })
	if err != nil {
		t.Fatal(err)
	}
	handler := newStatusHandler(context.Background(), onlineHealthSnapshot, slog.Default(), cfg)
	token := os.Getenv(cfg.AdminHTTPTokenEnv)
	id := meshcore.NewLocalIdentityFromSeed([32]byte{1})
	for _, auth := range []string{"", "wrong"} {
		w := adminRequest(handler, "/admin/command", "backup start "+id.String(), auth, "admin-v1", "")
		if w.Code != http.StatusUnauthorized {
			t.Fatalf("unauthenticated backup request admitted: %d", w.Code)
		}
	}
	w := adminRequest(handler, "/admin/command", "backup start "+id.String(), token, "admin-v1", "")
	if w.Code != 200 || !strings.HasPrefix(w.Body.String(), "PREPARING") {
		t.Fatalf("backup start: %d %s", w.Code, w.Body)
	}
	deadline := time.Now().Add(3 * time.Second)
	var status string
	for time.Now().Before(deadline) {
		w = adminRequest(handler, "/admin/command", "backup status", token, "admin-v1", "")
		status = w.Body.String()
		if !strings.HasPrefix(status, "PREPARING") {
			break
		}
		time.Sleep(time.Millisecond)
	}
	if !strings.HasPrefix(status, "READY ") {
		t.Fatal(status)
	}
	backupID := strings.Fields(status)[1]
	for _, auth := range []string{"", "wrong", token} {
		request := httptest.NewRequest("GET", "/admin/backup?id="+backupID, nil)
		request.Header.Set("X-Host-Admin", auth)
		request.Header.Set("X-Host-Intent", "admin-v1")
		response := httptest.NewRecorder()
		handler.ServeHTTP(response, request)
		if auth != token {
			if response.Code != http.StatusUnauthorized {
				t.Fatalf("private download exposed: %d", response.Code)
			}
			continue
		}
		if response.Code != 200 || response.Header().Get("Content-Type") != "application/octet-stream" ||
			response.Header().Get("Cache-Control") != "no-store" || !strings.HasPrefix(response.Body.String(), "MCB\x01") {
			t.Fatalf("encrypted download: %d", response.Code)
		}
		file, _, err := cfg.nodeBackup.Open(backupID)
		if err != nil {
			t.Fatal(err)
		}
		raw, err := io.ReadAll(file)
		file.Close()
		if err != nil || string(raw) != response.Body.String() {
			t.Fatal("HTTP download differs from saved snapshot")
		}
	}
}

func TestHostBackupRejectsUnsafeStateAndMissingCredential(t *testing.T) {
	cfg := adminTestConfig(t)
	cfg.AdminPasswordEnv = "MESHCORE_BACKUP_MISSING"
	t.Setenv(cfg.AdminPasswordEnv, "")
	if err := os.Unsetenv(cfg.AdminPasswordEnv); err != nil {
		t.Fatal(err)
	}
	if _, err := hostBackupSnapshot(context.Background(), cfg); err == nil {
		t.Fatal("missing configured credential accepted")
	}
	cfg.AdminPasswordEnv = ""
	path := filepath.Join(cfg.StateDir, "symlink")
	if err := os.Symlink("/etc/passwd", path); err != nil {
		t.Fatal(err)
	}
	if _, err := hostBackupSnapshot(context.Background(), cfg); err == nil {
		t.Fatal("state symlink accepted")
	}
}
