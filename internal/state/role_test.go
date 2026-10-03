package state

import (
	"bytes"
	"encoding/json"
	"errors"
	"os"
	"path/filepath"
	"testing"

	meshcore "github.com/meshcore-go/meshcore-go"
)

func TestRoleIdentityAndDocumentShareOneAuthority(t *testing.T) {
	dir := t.TempDir()
	original, err := Identity(dir, "companion")
	if err != nil {
		t.Fatal(err)
	}
	sibling, err := Identity(dir, "room")
	if err != nil {
		t.Fatal(err)
	}
	path := filepath.Join(dir, "companion", "companion.json")
	type document struct {
		PublicKey [32]byte
		Cursor    int
		Messages  []string
	}
	old := document{original.PublicKey(), 6, []string{"saved message"}}
	if err := WriteRoleJSON(path, old); err != nil {
		t.Fatal(err)
	}
	legacy, err := os.ReadFile(path)
	if err != nil {
		t.Fatal(err)
	}
	key := expandSeed([32]byte{1})
	nextID := meshcore.NewLocalIdentityFromSeed([32]byte{1})
	next := document{nextID.PublicKey(), 7, old.Messages}
	raw, err := json.Marshal(next)
	if err != nil {
		t.Fatal(err)
	}
	if _, err := ImportRoleIdentity(path, key, raw); err != nil {
		t.Fatal(err)
	}
	check := func(wantID meshcore.LocalIdentity, wantCursor int) {
		t.Helper()
		gotID, err := Identity(dir, "companion")
		if err != nil || gotID.PublicKey() != wantID.PublicKey() {
			t.Fatalf("identity authority disagrees: %v", err)
		}
		data, err := ReadRoleJSON(path)
		if err != nil {
			t.Fatal(err)
		}
		var got document
		if err := json.Unmarshal(data, &got); err != nil {
			t.Fatal(err)
		}
		if got.PublicKey != gotID.PublicKey() || got.Cursor != wantCursor {
			t.Fatal("key and role state selected different authorities")
		}
	}
	check(nextID, 7)
	next.Cursor = 8
	if err := WriteRoleJSON(path, next); err != nil {
		t.Fatal(err)
	}
	check(nextID, 8)
	exported, err := ExportIdentity(dir, "companion")
	if err != nil || !bytes.Equal(exported, key) {
		t.Fatal("state save discarded or changed the imported key")
	}
	preserved, err := os.ReadFile(path)
	if err != nil || !bytes.Equal(preserved, legacy) {
		t.Fatal("legacy rollback document changed after migration")
	}
	if _, err := ImportRoleIdentity(path, expandSeed([32]byte{3}), json.RawMessage("{")); err == nil {
		t.Fatal("malformed next state was committed")
	}
	check(nextID, 8)
	if _, err := ImportIdentity(dir, "companion", expandSeed([32]byte{3})); err == nil {
		t.Fatal("key-only import bypassed the combined authority")
	}
	resetID, err := ResetRole(path)
	if err != nil {
		t.Fatal(err)
	}
	if _, err := ReadRoleJSON(path); !errors.Is(err, os.ErrNotExist) {
		t.Fatalf("reset must expose absent state, not the legacy document: %v", err)
	}
	reloaded, err := Identity(dir, "companion")
	if err != nil || reloaded.PublicKey() != resetID.PublicKey() ||
		reloaded.PublicKey() == nextID.PublicKey() || reloaded.PublicKey() == original.PublicKey() {
		t.Fatal("reset did not durably replace the selected identity")
	}
	if err := WriteRoleJSON(path, document{PublicKey: resetID.PublicKey()}); err != nil {
		t.Fatal(err)
	}
	check(resetID, 0)
	room, err := Identity(dir, "room")
	if err != nil || room.PublicKey() != sibling.PublicKey() {
		t.Fatal("companion import/reset changed a sibling identity")
	}
}

func TestMalformedRoleEnvelopeNeverReactivatesLegacyState(t *testing.T) {
	dir := t.TempDir()
	if _, err := Identity(dir, "companion"); err != nil {
		t.Fatal(err)
	}
	path := filepath.Join(dir, "companion", "companion.json")
	if err := WriteRoleJSON(path, map[string]int{"cursor": 3}); err != nil {
		t.Fatal(err)
	}
	// This is not an explicit reset: its state field is absent.
	if err := WriteJSON(filepath.Join(filepath.Dir(path), roleEnvelopeName), map[string]any{
		"version": 1, "identity": expandSeed([32]byte{1}), "document": "companion.json",
	}); err != nil {
		t.Fatal(err)
	}
	if _, err := ReadRoleJSON(path); err == nil || errors.Is(err, os.ErrNotExist) {
		t.Fatalf("missing state field became a reset or legacy fallback: %v", err)
	}
	if _, err := Identity(dir, "companion"); err == nil {
		t.Fatal("identity ignored the invalid envelope")
	}
	if _, err := ExportIdentity(dir, "companion"); err == nil {
		t.Fatal("export ignored the invalid envelope")
	}
	if err := WriteRoleJSON(path, map[string]int{"cursor": 4}); err == nil {
		t.Fatal("ordinary save silently overwrote an invalid authority")
	}
}
