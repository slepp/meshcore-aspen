package main

import (
	"os"
	"path/filepath"
	"strings"
	"testing"
)

func testState(t *testing.T, origin string) (*stateStore, *savedState) {
	t.Helper()
	store, state, err := openState(filepath.Join(t.TempDir(), "private"), origin)
	if err != nil {
		t.Fatal(err)
	}
	t.Cleanup(store.close)
	return store, state
}

func TestStatePersistsDeviceDraftAndUncertainPost(t *testing.T) {
	store, saved := testState(t, "https://room.test")
	id, err := newPostID()
	if err != nil || !postIDPattern.MatchString(id) {
		t.Fatalf("UUID: %q %v", id, err)
	}
	saved.Selected, saved.Username, saved.Name = "A", "alice", "Alice"
	saved.Rooms["A"] = savedRoom{Draft: "saved draft", Pending: &pendingPost{id, "uncertain", saved.PublicKey}}
	if err := store.save(saved); err != nil {
		t.Fatal(err)
	}
	loaded, err := store.load()
	if err != nil {
		t.Fatal(err)
	}
	if loaded.Seed != saved.Seed || loaded.PublicKey != saved.PublicKey || loaded.Rooms["A"].Pending.ID != id ||
		loaded.Rooms["A"].Draft != "saved draft" {
		t.Fatal("private identity or unsent work changed")
	}
	info, err := os.Stat(filepath.Join(store.directory, "state.json"))
	if err != nil || info.Mode().Perm() != 0600 {
		t.Fatalf("state permissions: %v %v", info, err)
	}
	if _, _, err := openState(store.directory, saved.Origin); err == nil || !strings.Contains(err.Error(), "already in use") {
		t.Fatalf("second writer was not blocked: %v", err)
	}
}

func TestStateRejectsDamagePermissionsSymlinksAndWrongOrigin(t *testing.T) {
	for _, scenario := range []string{"key", "version", "unknown-field", "permissions", "symlink"} {
		t.Run(scenario, func(t *testing.T) {
			store, saved := testState(t, "https://room.test")
			path := filepath.Join(store.directory, "state.json")
			switch scenario {
			case "key":
				saved.PublicKey = strings.Repeat("00", 32)
				if err := store.save(saved); err == nil {
					t.Fatal("accepted mismatched device key")
				}
				return
			case "version":
				if err := os.WriteFile(path, []byte(`{"version":2}`), 0600); err != nil {
					t.Fatal(err)
				}
			case "unknown-field":
				raw, err := os.ReadFile(path)
				if err != nil {
					t.Fatal(err)
				}
				raw = append([]byte(`{"extra":true,`), raw[1:]...)
				if err := os.WriteFile(path, raw, 0600); err != nil {
					t.Fatal(err)
				}
			case "permissions":
				if err := os.Chmod(path, 0644); err != nil {
					t.Fatal(err)
				}
			case "symlink":
				if err := os.Remove(path); err != nil {
					t.Fatal(err)
				}
				if err := os.Symlink(filepath.Join(t.TempDir(), "outside"), path); err != nil {
					t.Fatal(err)
				}
			}
			if _, err := store.load(); err == nil {
				t.Fatal("unsafe/damaged state was accepted")
			}
		})
	}
	dir := filepath.Join(t.TempDir(), "private")
	store, _, err := openState(dir, "https://first.test")
	if err != nil {
		t.Fatal(err)
	}
	store.close()
	if _, _, err := openState(dir, "https://second.test"); err == nil || !strings.Contains(err.Error(), "another service") {
		t.Fatalf("origin mismatch: %v", err)
	}
}
