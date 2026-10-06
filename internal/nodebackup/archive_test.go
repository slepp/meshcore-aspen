// SPDX-License-Identifier: Apache-2.0
package nodebackup

import (
	"context"
	"encoding/hex"
	"errors"
	"io"
	"os"
	"os/exec"
	"path/filepath"
	"strings"
	"sync/atomic"
	"testing"
	"time"

	meshcore "github.com/meshcore-go/meshcore-go"
)

func fixture() (map[string][]byte, [32]byte) {
	var seed [32]byte
	for i := range seed {
		seed[i] = byte(i)
	}
	id := meshcore.NewLocalIdentityFromSeed(seed)
	return map[string][]byte{
		"manifest.json":                 []byte(`{"schema_version":1,"format":"meshcore-node-backup","product":"birch"}`),
		"files/bot/native/source.lua":   []byte("function hello() reply('hello') end\n"),
		"identities/companion.expanded": make([]byte, 2048),
	}, id.PublicKey()
}

func TestArchiveInteroperatesWithOperatorDecoder(t *testing.T) {
	records, key := fixture()
	raw, err := Encode(records, key)
	if err != nil {
		t.Fatal(err)
	}
	if len(raw) > 1024 {
		t.Fatalf("logical zero-filled archive did not compress: %d", len(raw))
	}
	path := filepath.Join(t.TempDir(), "node.mcb")
	if err := os.WriteFile(path, raw, 0600); err != nil {
		t.Fatal(err)
	}
	command := exec.Command("python3", "-B", "-c", `
import sys
from tools.node_backup import decrypt,entries
raw=open(sys.argv[1],'rb').read()
manifest,files=entries(decrypt(raw,bytes(range(32))))
assert manifest['product']=='birch'
assert files['files/bot/native/source.lua']==b"function hello() reply('hello') end\n"
assert files['identities/companion.expanded']==bytes(2048)
`, path)
	command.Dir = "../.."
	if output, err := command.CombinedOutput(); err != nil {
		t.Fatalf("operator decoder: %v\n%s", err, output)
	}
	for _, name := range []string{"files/../secret", "files//data", "/identities/key", "files/a\\b", "logs/a"} {
		records[name] = []byte("x")
		if _, err := Encode(records, key); err == nil {
			t.Errorf("unsafe name accepted: %q", name)
		}
		delete(records, name)
	}
	records["files/huge"] = make([]byte, RawLimit)
	if _, err := Encode(records, key); err == nil {
		t.Fatal("decoded limit accepted an oversized archive")
	}
}

func await(t *testing.T, service *Service) string {
	t.Helper()
	deadline := time.Now().Add(3 * time.Second)
	for time.Now().Before(deadline) {
		status := service.Command("backup status", false)
		if !strings.HasPrefix(status, "PREPARING") {
			return status
		}
		time.Sleep(time.Millisecond)
	}
	t.Fatal("backup did not finish")
	return ""
}

func TestPersistentSnapshotPacingLeasesAndFailureRetention(t *testing.T) {
	var fail atomic.Bool
	_, key := fixture()
	root := filepath.Join(t.TempDir(), "backup")
	snapshot := func(context.Context) (map[string][]byte, error) {
		if fail.Load() {
			return nil, errors.New("source changed")
		}
		fresh, _ := fixture()
		return fresh, nil
	}
	service, err := New(context.Background(), root, snapshot)
	if err != nil {
		t.Fatal(err)
	}
	start := "backup start " + hex.EncodeToString(key[:])
	if reply := service.Command(start, false); !strings.HasPrefix(reply, "PREPARING") {
		t.Fatal(reply)
	}
	status := await(t, service)
	if !strings.HasPrefix(status, "READY ") {
		t.Fatal(status)
	}
	id := strings.Fields(status)[1]
	file, size, err := service.Open(id)
	if err != nil {
		t.Fatal(err)
	}
	raw, err := io.ReadAll(file)
	if err != nil || len(raw) != size || len(raw) < 120 {
		t.Fatalf("snapshot read %d/%d: %v", len(raw), size, err)
	}
	for _, command := range []string{start, "backup load", "backup clear"} {
		if reply := service.Command(command, false); !strings.HasPrefix(reply, "Error:") {
			t.Fatalf("lease did not block %s: %s", command, reply)
		}
	}
	if err := file.Close(); err != nil {
		t.Fatal(err)
	}
	if reply := service.Command("backup read "+id+" 0", true); !strings.HasPrefix(reply, "CHUNK "+id+" 0 ") {
		t.Fatal(reply)
	}
	if reply := service.Command("backup read "+id+" 48", true); !strings.HasPrefix(reply, "WAIT ms=") {
		t.Fatal(reply)
	}
	restarted, err := New(context.Background(), root, snapshot)
	if err != nil || restarted.Command("backup status", false) != status {
		t.Fatalf("saved snapshot changed after restart: %v", err)
	}
	fail.Store(true)
	service.Command(start, false)
	if reply := await(t, service); !strings.Contains(reply, "source changed") {
		t.Fatal(reply)
	}
	if reply := service.Command("backup load", false); strings.HasPrefix(reply, "Error:") {
		t.Fatal(reply)
	}
	if service.Command("backup status", false) != status {
		t.Fatal("failed replacement lost the previous snapshot")
	}
	info, err := os.Stat(service.path(service.saved.Slot))
	if err != nil || info.Mode().Perm() != 0600 {
		t.Fatalf("snapshot is not private: %v", err)
	}
	raw[len(raw)-1] ^= 1
	if err := os.WriteFile(service.path(service.saved.Slot), raw, 0600); err != nil {
		t.Fatal(err)
	}
	if reply := service.Command("backup load", false); !strings.Contains(reply, "checksum failed") {
		t.Fatal(reply)
	}
	if reply := service.Command("backup status", false); !strings.Contains(reply, "checksum failed") {
		t.Fatalf("failed load left a stale ready snapshot: %s", reply)
	}
	if _, _, err := service.Open(id); err == nil {
		t.Fatal("failed load left the corrupt snapshot downloadable")
	}
}

func TestCloseCancelsAndWaitsForSnapshot(t *testing.T) {
	entered, cancelled, release := make(chan struct{}), make(chan struct{}), make(chan struct{})
	service, err := New(context.Background(), filepath.Join(t.TempDir(), "backup"),
		func(ctx context.Context) (map[string][]byte, error) {
			close(entered)
			<-ctx.Done()
			close(cancelled)
			<-release
			return nil, ctx.Err()
		})
	if err != nil {
		t.Fatal(err)
	}
	defer close(release)
	_, key := fixture()
	if reply := service.Command("backup start "+hex.EncodeToString(key[:]), false); !strings.HasPrefix(reply, "PREPARING") {
		t.Fatal(reply)
	}
	select {
	case <-entered:
	case <-time.After(3 * time.Second):
		t.Fatal("snapshot did not start")
	}
	closed := make(chan error, 1)
	go func() { closed <- service.Close() }()
	select {
	case <-cancelled:
	case <-time.After(3 * time.Second):
		t.Fatal("close did not cancel snapshot")
	}
	select {
	case err := <-closed:
		t.Fatalf("close returned before snapshot exited: %v", err)
	default:
	}
	if reply := service.Command("backup status", false); !strings.Contains(reply, "stopping") {
		t.Fatal(reply)
	}
	release <- struct{}{}
	select {
	case err := <-closed:
		if err != nil {
			t.Fatal(err)
		}
	case <-time.After(3 * time.Second):
		t.Fatal("close did not finish after snapshot exited")
	}
	if _, err := os.Stat(filepath.Join(service.root, "active.json")); !errors.Is(err, os.ErrNotExist) {
		t.Fatalf("cancelled snapshot published a pointer: %v", err)
	}
}
