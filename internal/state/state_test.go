package state

import (
	"bytes"
	"crypto/ed25519"
	"crypto/sha512"
	"encoding/hex"
	"errors"
	"os"
	"path/filepath"
	"testing"
)

func TestNativeIdentityImportExport(t *testing.T) {
	// RFC 8032 test 1 is public test material, not an operational identity.
	seed, err := hex.DecodeString("9d61b19deffd5a60ba844af492ec2cc44449c5697b326919703bac031cae7f60")
	if err != nil {
		t.Fatal(err)
	}
	dir := t.TempDir()
	roleDir := filepath.Join(dir, "companion")
	if err := os.Mkdir(roleDir, 0700); err != nil {
		t.Fatal(err)
	}
	seedPath := filepath.Join(roleDir, "identity.seed")
	if err := os.WriteFile(seedPath, seed, 0600); err != nil {
		t.Fatal(err)
	}
	native, err := ExportIdentity(dir, "companion")
	if err != nil {
		t.Fatal(err)
	}
	if len(native) != 64 || bytes.Equal(native, ed25519.NewKeyFromSeed(seed)) {
		t.Fatal("export must be scalar-prefix, not seed-public-key")
	}
	backup := bytes.Repeat([]byte{7}, 32)
	if err := os.WriteFile(seedPath, backup, 0600); err != nil {
		t.Fatal(err)
	}
	other, err := Identity(dir, "room")
	if err != nil {
		t.Fatal(err)
	}
	imported, err := ImportIdentity(dir, "companion", native)
	if err != nil {
		t.Fatal(err)
	}
	reloaded, err := Identity(dir, "companion")
	if err != nil {
		t.Fatal(err)
	}
	if imported.PublicKey() != reloaded.PublicKey() ||
		reloaded.String() != "d75a980182b10ab7d54bfed3c964073a0ee172f3daa62325af021a68f707511a" {
		t.Fatal("native identity did not survive reload")
	}
	wantSignature := "e5564300c360ac729086e2cc806e828a84877f1eb8e5d974d873e065224901555f" +
		"b8821590a33bacc61e39701cf9b46bd25bf5f0595bbe24655141438e7a100b"
	if hex.EncodeToString(reloaded.Sign(nil)) != wantSignature {
		t.Fatal("imported native identity failed RFC 8032 signing vector")
	}
	exported, err := ExportIdentity(dir, "companion")
	if err != nil || !bytes.Equal(exported, native) {
		t.Fatal("native key was not preserved exactly")
	}
	preserved, err := os.ReadFile(seedPath)
	if err != nil || !bytes.Equal(preserved, backup) {
		t.Fatal("legacy seed was not preserved for migration rollback")
	}
	unchanged, err := Identity(dir, "room")
	if err != nil || unchanged.PublicKey() != other.PublicKey() {
		t.Fatal("import changed another role")
	}
	secret, err := reloaded.SharedSecret(other.Identity)
	if err != nil {
		t.Fatal(err)
	}
	peerSecret, err := other.SharedSecret(reloaded.Identity)
	if err != nil || !bytes.Equal(secret, peerSecret) {
		t.Fatal("imported identity cannot exchange keys with a seed identity")
	}
	info, err := os.Stat(filepath.Join(roleDir, "identity.expanded"))
	if err != nil || info.Mode().Perm() != 0600 {
		t.Fatal("imported key file is not private")
	}
}

func TestIdentityRejectsCorruptionWithoutFallback(t *testing.T) {
	dir := t.TempDir()
	if _, err := Identity(dir, "companion"); err != nil {
		t.Fatal(err)
	}
	for _, role := range []string{"", "..", "room/child", `room\child`} {
		if _, err := ImportIdentity(dir, role, make([]byte, 64)); err == nil {
			t.Fatalf("accepted invalid role %q", role)
		}
	}
	if _, err := ImportIdentity(dir, "companion", make([]byte, 32)); err == nil {
		t.Fatal("accepted a seed as a native expanded key")
	}
	path := filepath.Join(dir, "companion", "identity.expanded")
	if err := os.WriteFile(path, []byte("broken"), 0600); err != nil {
		t.Fatal(err)
	}
	if _, err := Identity(dir, "companion"); err == nil {
		t.Fatal("fell back to legacy seed after imported key corruption")
	}
	if _, err := ExportIdentity(dir, "companion"); err == nil {
		t.Fatal("export fell back to a different identity")
	}
	data, err := os.ReadFile(path)
	if err != nil || string(data) != "broken" {
		t.Fatal("corrupt imported key was changed")
	}
	if _, err := ExportIdentity(dir, "missing"); !errors.Is(err, os.ErrNotExist) {
		t.Fatalf("missing export should fail without creating a key: %v", err)
	}
	if _, err := os.Stat(filepath.Join(dir, "missing")); !errors.Is(err, os.ErrNotExist) {
		t.Fatal("export created identity state")
	}
}

func TestImportRejectsNonNativeScalarsWithoutReplacingIdentity(t *testing.T) {
	dir := t.TempDir()
	original, err := Identity(dir, "companion")
	if err != nil {
		t.Fatal(err)
	}
	key, err := ExportIdentity(dir, "companion")
	if err != nil {
		t.Fatal(err)
	}
	for _, tc := range []struct {
		name   string
		mutate func([]byte)
	}{
		{"low-scalar-bit", func(key []byte) { key[0] |= 1 }},
		{"missing-high-bit", func(key []byte) { key[31] &^= 64 }},
		{"top-bit", func(key []byte) { key[31] |= 128 }},
	} {
		t.Run(tc.name, func(t *testing.T) {
			invalid := bytes.Clone(key)
			tc.mutate(invalid)
			if _, err := ImportIdentity(dir, "companion", invalid); err == nil {
				t.Fatal("native-invalid scalar was silently clamped and imported")
			}
			current, err := Identity(dir, "companion")
			if err != nil || current.PublicKey() != original.PublicKey() {
				t.Fatal("failed import changed the existing identity")
			}
		})
	}
	key[0] |= 1
	if err := os.WriteFile(filepath.Join(dir, "companion", "identity.expanded"), key, 0600); err != nil {
		t.Fatal(err)
	}
	if _, err := Identity(dir, "companion"); err == nil {
		t.Fatal("malformed persisted scalar was normalized or fell back to the legacy seed")
	}
	if _, err := ExportIdentity(dir, "companion"); err == nil {
		t.Fatal("malformed persisted scalar was exported")
	}
}

func TestImportRejectsNativeReservedPublicPrefixes(t *testing.T) {
	dir := t.TempDir()
	found := map[byte]bool{}
	for i := 0; i < 65536 && len(found) < 2; i++ {
		seed := [32]byte{byte(i), byte(i >> 8)}
		key := ed25519.NewKeyFromSeed(seed[:])
		prefix := key[32]
		if prefix != 0 && prefix != 255 || found[prefix] {
			continue
		}
		found[prefix] = true
		expanded := sha512.Sum512(seed[:])
		expanded[0] &= 248
		expanded[31] &= 63
		expanded[31] |= 64
		if _, err := ImportIdentity(dir, "companion", expanded[:]); err == nil {
			t.Fatalf("accepted native-reserved public prefix %02x", prefix)
		}
	}
	if len(found) != 2 {
		t.Fatal("deterministic fixtures did not reach both reserved prefixes")
	}
	if _, err := os.Stat(filepath.Join(dir, "companion")); !errors.Is(err, os.ErrNotExist) {
		t.Fatal("rejected import created identity state")
	}
}

func TestAtomicCommitOutcomes(t *testing.T) {
	dir := t.TempDir()
	path := filepath.Join(dir, "state.json")
	if err := WriteJSON(path, map[string]int{"cursor": 1}); err != nil {
		t.Fatal(err)
	}
	before, err := os.ReadFile(path)
	if err != nil {
		t.Fatal(err)
	}
	if err := WriteJSON(path, make(chan int)); err == nil || errors.Is(err, ErrCommitIndeterminate) {
		t.Fatalf("encoding failure must be definite: %v", err)
	}
	after, err := os.ReadFile(path)
	if err != nil || !bytes.Equal(before, after) {
		t.Fatal("encoding failure changed committed data")
	}
	err = writeAtomic(path, []byte(`{"cursor":2}`), func(string) error {
		return os.ErrPermission
	})
	if !errors.Is(err, ErrCommitIndeterminate) || !errors.Is(err, os.ErrPermission) {
		t.Fatalf("post-rename failure must expose indeterminate commit and cause: %v", err)
	}
	after, err = os.ReadFile(path)
	if err != nil || string(after) != `{"cursor":2}` {
		t.Fatal("test did not reproduce visible state after a failed commit")
	}
	target := filepath.Join(dir, "not-a-file")
	if err := os.Mkdir(target, 0700); err != nil {
		t.Fatal(err)
	}
	err = writeAtomic(target, []byte("replacement"), func(string) error {
		t.Fatal("directory sync ran after failed rename")
		return nil
	})
	if err == nil || errors.Is(err, ErrCommitIndeterminate) {
		t.Fatalf("rename failure must be definite: %v", err)
	}
	leftovers, err := filepath.Glob(filepath.Join(dir, ".state-*"))
	if err != nil || len(leftovers) != 0 {
		t.Fatalf("temporary state files were not removed: %v, %v", leftovers, err)
	}
}

func TestIdentityIsolationPersistenceAndCorruption(t *testing.T) {
	if lock, err := Lock(""); err == nil {
		lock.Close()
		t.Fatal("accepted an empty state directory")
	}
	dir := filepath.Join(t.TempDir(), "nested", "state")
	lock, err := Lock(dir)
	if err != nil {
		t.Fatal(err)
	}
	defer lock.Close()
	if other, err := Lock(dir); err == nil {
		other.Close()
		t.Fatal("two processes can own the same identities")
	}
	seen := map[string]bool{}
	for _, role := range []string{"repeater", "room", "companion", "observer", "bot"} {
		first, err := Identity(dir, role)
		if err != nil {
			t.Fatal(err)
		}
		second, err := Identity(dir, role)
		if err != nil {
			t.Fatal(err)
		}
		if first.PublicKey() != second.PublicKey() || seen[first.String()] {
			t.Fatal("identity was replaced or shared across roles")
		}
		seen[first.String()] = true
	}
	path := filepath.Join(dir, "room", "identity.seed")
	if err := os.WriteFile(path, []byte("broken"), 0600); err != nil {
		t.Fatal(err)
	}
	if _, err := Identity(dir, "room"); err == nil {
		t.Fatal("corrupt persistent identity was silently replaced")
	}
	data, err := os.ReadFile(path)
	if err != nil || string(data) != "broken" {
		t.Fatal("corrupt identity was modified")
	}
}
