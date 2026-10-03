package roles

import (
	"bytes"
	"encoding/json"
	"errors"
	"os"
	"path/filepath"
	"testing"
	"time"

	meshcore "github.com/meshcore-go/meshcore-go"
	hoststate "meshcore.local/meshcore/internal/state"
)

func fixtureExpandedKey(t *testing.T, id meshcore.LocalIdentity) []byte {
	t.Helper()
	root := t.TempDir()
	dir := filepath.Join(root, "fixture")
	if err := os.Mkdir(dir, 0700); err != nil {
		t.Fatal(err)
	}
	seed := id.Seed()
	if err := os.WriteFile(filepath.Join(dir, "identity.seed"), seed[:], 0600); err != nil {
		t.Fatal(err)
	}
	key, err := hoststate.ExportIdentity(root, "fixture")
	if err != nil {
		t.Fatal(err)
	}
	return key
}

func TestRoleEnvelopeAuthorityMigrationAndReset(t *testing.T) {
	cfg := config(t)
	s, r, oldID := startRole(t, true, cfg)
	client := fixedID(3)
	loggedIn(t, s, r, client, oldID, true, 1, "room")
	r.inject(t, textWire(t, client, oldID, 2, 0, "before import"), false)
	r.next(t)
	if err := s.Close(); err != nil {
		t.Fatal(err)
	}
	path := filepath.Join(cfg.StateDir, "state.json")
	legacy, err := os.ReadFile(path)
	if err != nil {
		t.Fatal(err)
	}
	var document diskState
	if err := json.Unmarshal(legacy, &document); err != nil {
		t.Fatal(err)
	}
	document.Version, document.Retention, document.PreferenceProfile = 1, "", ""
	raw, err := json.Marshal(document)
	if err != nil {
		t.Fatal(err)
	}
	nextID := fixedID(2)
	rebound, err := RebindStateIdentity(raw, oldID.Identity, nextID.Identity)
	if err != nil {
		t.Fatal(err)
	}
	key := fixtureExpandedKey(t, nextID)
	if _, err := hoststate.ImportRoleIdentity(path, key, rebound); err != nil {
		t.Fatal(err)
	}
	authority, err := hoststate.ReadRoleJSON(path)
	if err != nil {
		t.Fatal(err)
	}
	radio := newWireRadio()
	current, err := NewRoom(nextID, radio, cfg)
	if err != nil {
		t.Fatal(err)
	}
	t.Cleanup(func() { _ = current.Close() })
	radio.next(t)
	backup, err := os.ReadFile(filepath.Join(cfg.StateDir, "state.v1.json"))
	if err != nil || !bytes.Equal(backup, authority) {
		t.Fatal("migration backed up stale legacy file instead of authoritative document")
	}
	radio.inject(t, textWire(t, client, nextID, 3, 0, "after import"), false)
	wantACK := append([]byte{0x0d, 0}, ackProof(append([]byte{3, 0, 0, 0, 0}, []byte("after import")...), client.PublicKey())...)
	if got := radio.next(t); !bytes.Equal(got, wantACK) {
		t.Fatalf("imported role did not acknowledge encrypted post: %x", got)
	}
	if err := current.Close(); err != nil {
		t.Fatal(err)
	}
	raw, err = hoststate.ReadRoleJSON(path)
	if err != nil {
		t.Fatal(err)
	}
	if err := json.Unmarshal(raw, &document); err != nil {
		t.Fatal(err)
	}
	if document.Version != 2 || document.Identity != nextID.Identity.String() || document.Retention != DurableReplay ||
		len(document.History) != 2 || document.History[0].Text != "before import" || document.History[1].Text != "after import" {
		t.Fatal("envelope save lost migrated identity, retention or room history")
	}
	exported, err := hoststate.ExportIdentity(filepath.Dir(cfg.StateDir), filepath.Base(cfg.StateDir))
	if err != nil || !bytes.Equal(exported, key) {
		t.Fatal("role save changed the authoritative expanded identity")
	}
	unchanged, err := os.ReadFile(path)
	if err != nil || !bytes.Equal(unchanged, legacy) {
		t.Fatal("role wrote the stale pre-envelope document")
	}
	resetID, err := hoststate.ResetRole(path)
	if err != nil {
		t.Fatal(err)
	}
	radio = newWireRadio()
	reset, err := NewRoom(resetID, radio, cfg)
	if err != nil {
		t.Fatal(err)
	}
	t.Cleanup(func() { _ = reset.Close() })
	radio.next(t)
	reset.mu.RLock()
	empty := len(reset.state.History) == 0 && len(reset.state.Members) == 0
	reset.mu.RUnlock()
	if !empty {
		t.Fatal("explicit envelope reset resurrected legacy history or membership")
	}
	if err := reset.Close(); err != nil {
		t.Fatal(err)
	}
	// A malformed selected envelope must never fall back to the valid old file.
	if err := os.WriteFile(filepath.Join(cfg.StateDir, "identity-state.json"), []byte("{broken"), 0600); err != nil {
		t.Fatal(err)
	}
	if stale, err := NewRoom(oldID, newWireRadio(), cfg); err == nil {
		_ = stale.Close()
		t.Fatal("corrupt envelope reactivated the legacy role")
	}
}

func TestEnvelopeIndeterminateCommitDoesNotSaveDuringClose(t *testing.T) {
	cfg := config(t)
	failures := make(chan error, 8)
	cfg.ErrorHandler = func(err error) { failures <- err }
	first, _, id := startRole(t, true, cfg)
	if err := first.Close(); err != nil {
		t.Fatal(err)
	}
	path := filepath.Join(cfg.StateDir, "state.json")
	raw, err := hoststate.ReadRoleJSON(path)
	if err != nil {
		t.Fatal(err)
	}
	if _, err := hoststate.ImportRoleIdentity(path, fixtureExpandedKey(t, id), raw); err != nil {
		t.Fatal(err)
	}
	r := newWireRadio()
	s, err := NewRoom(id, r, cfg)
	if err != nil {
		t.Fatal(err)
	}
	t.Cleanup(func() { _ = s.Close() })
	r.next(t)
	writes := 0
	s.mu.Lock()
	s.writeJSON = func(path string, value any) error {
		writes++
		if err := hoststate.WriteRoleJSON(path, value); err != nil {
			return err
		}
		return errors.Join(hoststate.ErrCommitIndeterminate, errors.New("injected post-replacement sync failure"))
	}
	s.mu.Unlock()
	client := fixedID(2)
	r.inject(t, loginWire(t, true, client, id, 1, 0, "room", 2, 0, nil), false)
	select {
	case err := <-failures:
		if !errors.Is(err, hoststate.ErrCommitIndeterminate) {
			t.Fatal(err)
		}
	case <-time.After(time.Second):
		t.Fatal("indeterminate shared-store commit was not surfaced")
	}
	r.quiet(t)
	before, err := hoststate.ReadRoleJSON(path)
	if err != nil {
		t.Fatal(err)
	}
	var committed diskState
	if err := json.Unmarshal(before, &committed); err != nil || committed.Members[client.Identity.String()] == nil {
		t.Fatal("test did not reach a visible envelope replacement")
	}
	r.inject(t, textWire(t, client, id, 2, 0, "must not persist"), false)
	if err := s.Close(); !errors.Is(err, hoststate.ErrCommitIndeterminate) {
		t.Fatalf("Close lost shared-store failure: %v", err)
	}
	after, err := hoststate.ReadRoleJSON(path)
	if err != nil || !bytes.Equal(before, after) || writes != 1 {
		t.Fatalf("failed-closed shutdown rewrote authoritative state (writes=%d, err=%v)", writes, err)
	}
	r.quiet(t)
}
