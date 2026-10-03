package state

import (
	"bytes"
	"errors"
	"os"
	"path/filepath"
	"testing"

	meshcore "github.com/meshcore-go/meshcore-go"
)

func TestBotIdentityStagingCancelApplyAndIsolation(t *testing.T) {
	dir := t.TempDir()
	old, err := Identity(dir, "bot")
	if err != nil {
		t.Fatal(err)
	}
	other, err := Identity(dir, "room")
	if err != nil {
		t.Fatal(err)
	}
	next := meshcore.NewLocalIdentityFromSeed([32]byte{91})
	key := expandSeed(next.Seed())
	for _, invalid := range [][]byte{nil, key[:32], make([]byte, 64)} {
		if _, err := BotIdentityChange(dir, "stage", invalid); err == nil {
			t.Fatal("invalid bot key staged")
		}
	}
	if _, err := BotIdentityChange(dir, "stage", key); err != nil {
		t.Fatal(err)
	}
	active, _ := Identity(dir, "bot")
	if active.PublicKey() != old.PublicKey() {
		t.Fatal("stage changed active key")
	}
	pending, err := PendingBotIdentity(dir)
	if err != nil || !bytes.Equal(key, pending) {
		t.Fatalf("pending mismatch: %v", err)
	}
	if _, err := BotIdentityChange(dir, "stage", expandSeed(other.Seed())); err == nil {
		t.Fatal("conflicting pending replaced")
	}
	if _, err := BotIdentityChange(dir, "cancel", nil); err != nil {
		t.Fatal(err)
	}
	if _, err := PendingBotIdentity(dir); err == nil {
		t.Fatal("cancel left pending key")
	}
	if _, err := BotIdentityChange(dir, "stage", expandSeed(other.Seed())); err == nil {
		t.Fatal("another role's key staged")
	}
	if _, err := BotIdentityChange(dir, "stage", key); err != nil {
		t.Fatal(err)
	}
	if _, err := BotIdentityChange(dir, "apply", key); err != nil {
		t.Fatal(err)
	}
	active, _ = Identity(dir, "bot")
	unchanged, _ := Identity(dir, "room")
	exported, _ := ExportIdentity(dir, "bot")
	if active.PublicKey() != next.PublicKey() || unchanged.PublicKey() != other.PublicKey() || !bytes.Equal(exported, key) {
		t.Fatal("apply/reload changed wrong identity authority")
	}
	if info, err := os.Stat(filepath.Join(dir, "bot", roleEnvelopeName)); err != nil || info.Mode().Perm() != 0600 {
		t.Fatal("bot key envelope not private")
	}
}

func TestBotApplyFailureAtomicity(t *testing.T) {
	dir := t.TempDir()
	old, err := Identity(dir, "bot")
	if err != nil {
		t.Fatal(err)
	}

	next := meshcore.NewLocalIdentityFromSeed([32]byte{92})
	key := expandSeed(next.Seed())
	if _, err := BotIdentityChange(dir, "stage", key); err != nil {
		t.Fatal(err)
	}
	failedSync := func(string) error { return errors.New("injected directory sync failure") }
	if _, err := botIdentityChange(dir, "apply", key, failedSync); !errors.Is(err, ErrCommitIndeterminate) {
		t.Fatalf("visible commit failure reported as rollback: %v", err)
	}
	active, err := Identity(dir, "bot")
	if err != nil || active.PublicKey() != next.PublicKey() {
		t.Fatal("visible rename not authoritative")
	}
	if _, err := BotIdentityChange(dir, "apply", key); err != nil {
		t.Fatalf("saved authority could not be confirmed on retry: %v", err)
	}
	// A failed stage validation never changes the committed key.
	if _, err := BotIdentityChange(dir, "stage", expandSeed(old.Seed())[:32]); err == nil {
		t.Fatal("invalid stage admitted")
	}
	active, _ = Identity(dir, "bot")
	if active.PublicKey() != next.PublicKey() {
		t.Fatal("failed stage changed active key")
	}
}

func TestNativeBotMissingAuthorityDoesNotGenerateKey(t *testing.T) {
	dir := t.TempDir()
	if err := os.MkdirAll(filepath.Join(dir, "bot", "native"), 0700); err != nil {
		t.Fatal(err)
	}
	if _, err := Identity(dir, "bot"); err == nil {
		t.Fatal("native bot with missing Go authority generated a replacement identity")
	}
	if _, err := os.Stat(filepath.Join(dir, "bot", "identity.seed")); !errors.Is(err, os.ErrNotExist) {
		t.Fatal("missing bot authority check created a seed")
	}
}
