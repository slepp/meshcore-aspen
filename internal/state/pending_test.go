package state

import (
	"bytes"
	"context"
	"encoding/json"
	"errors"
	"fmt"
	"os"
	"path/filepath"
	"strings"
	"sync"
	"testing"
	"time"

	meshcore "github.com/meshcore-go/meshcore-go"
)

type pendingDocument struct {
	Identity string
	Count    int
}

func pendingFixture(t *testing.T) (string, map[string]meshcore.LocalIdentity) {
	t.Helper()
	dir := t.TempDir()
	ids := make(map[string]meshcore.LocalIdentity)
	for _, role := range []string{"room", "repeater", "companion", "observer", "bot"} {
		id, err := Identity(dir, role)
		if err != nil {
			t.Fatal(err)
		}
		ids[role] = id
		name := "state.json"
		if role == "companion" {
			name = "companion.json"
		}
		if err := WriteRoleJSON(filepath.Join(dir, role, name), pendingDocument{id.String(), 7}); err != nil {
			t.Fatal(err)
		}
	}
	return dir, ids
}

func rebindPendingDocument(data []byte, previous, next meshcore.Identity) ([]byte, error) {
	var doc pendingDocument
	if err := json.Unmarshal(data, &doc); err != nil {
		return nil, err
	}
	if doc.Identity != previous.String() {
		return nil, errors.New("wrong previous identity")
	}
	doc.Identity = next.String()
	return json.Marshal(doc)
}

func TestAbsentPendingIdentityDoesNotCreateAnyAuthority(t *testing.T) {
	dir := t.TempDir()
	if err := ActivateRoleIdentity(dir, "room", rebindPendingDocument); err != nil {
		t.Fatal(err)
	}
	entries, err := os.ReadDir(dir)
	if err != nil || len(entries) != 0 {
		t.Fatalf("no-pending activation created files: %v", err)
	}
}

func TestPendingIdentityCancellationBoundsAdmissionNotCommittedSuccess(t *testing.T) {
	dir, _ := pendingFixture(t)
	key := expandSeed([32]byte{1})
	ctx, cancel := context.WithCancel(context.Background())
	roleStateMu.Lock()
	result := make(chan error, 1)
	go func() { result <- StageRoleIdentity(ctx, dir, "room", key) }()
	cancel()
	select {
	case err := <-result:
		roleStateMu.Unlock()
		if !errors.Is(err, context.Canceled) {
			t.Fatalf("cancelled admission result: %v", err)
		}
	case <-time.After(time.Second):
		roleStateMu.Unlock()
		t.Fatal("cancelled admission waited for another role's transaction")
	}
	if _, err := os.Stat(filepath.Join(dir, "room", roleEnvelopeName)); !errors.Is(err, os.ErrNotExist) {
		t.Fatal("cancelled admission wrote pending authority")
	}
	ctx, cancel = context.WithCancel(context.Background())
	defer cancel()
	err := stageRoleIdentity(ctx, dir, "room", key, func(path string) error {
		cancel()
		return syncDirectory(path)
	})
	if err != nil {
		t.Fatalf("late cancellation hid a committed pending key: %v", err)
	}
	envelope, err := readRoleEnvelope(filepath.Join(dir, "room"))
	if err != nil || !bytes.Equal(envelope.Pending, key) {
		t.Fatalf("confirmed commit lost pending key after cancellation: %v", err)
	}
}

func TestPendingRoleIdentityPreservesAuthorityAndNormalSaves(t *testing.T) {
	dir, ids := pendingFixture(t)
	path := filepath.Join(dir, "room", "state.json")
	key := expandSeed([32]byte{1})
	next, err := decodeExpandedIdentity(key)
	if err != nil {
		t.Fatal(err)
	}
	if err := StageRoleIdentity(context.Background(), dir, "room", key); err != nil {
		t.Fatal(err)
	}
	current, err := Identity(dir, "room")
	if err != nil || current.PublicKey() != ids["room"].PublicKey() {
		t.Fatalf("stage prematurely activated identity: %v", err)
	}
	exported, err := ExportIdentity(dir, "room")
	if err != nil || bytes.Equal(exported, key) {
		t.Fatalf("export selected pending instead of active identity: %v", err)
	}
	for count := 8; count < 12; count++ {
		if err := WriteRoleJSON(path, pendingDocument{current.String(), count}); err != nil {
			t.Fatal(err)
		}
	}
	envelope, err := readRoleEnvelope(filepath.Dir(path))
	if err != nil || !bytes.Equal(envelope.Pending, key) || envelope.identity.PublicKey() != current.PublicKey() {
		t.Fatalf("ordinary role save lost pending/current authority: %v", err)
	}
	var doc pendingDocument
	if err := json.Unmarshal(envelope.State, &doc); err != nil || doc.Count != 11 || doc.Identity != current.String() {
		t.Fatalf("stage changed document or later saves were lost: %+v %v", doc, err)
	}
	if err := ActivateRoleIdentity(dir, "room", rebindPendingDocument); err != nil {
		t.Fatal(err)
	}
	active, err := Identity(dir, "room")
	if err != nil || active.PublicKey() != next.PublicKey() {
		t.Fatalf("activation: %v", err)
	}
	envelope, err = readRoleEnvelope(filepath.Dir(path))
	if err != nil || envelope.Pending != nil || envelope.identity.PublicKey() != next.PublicKey() {
		t.Fatalf("activation did not atomically clear pending: %v", err)
	}
	if err := json.Unmarshal(envelope.State, &doc); err != nil || doc.Count != 11 || doc.Identity != next.String() {
		t.Fatalf("activation lost data or coherent document identity: %+v %v", doc, err)
	}
	before, err := os.ReadFile(filepath.Join(filepath.Dir(path), roleEnvelopeName))
	if err != nil {
		t.Fatal(err)
	}
	if err := ActivateRoleIdentity(dir, "room", func([]byte, meshcore.Identity, meshcore.Identity) ([]byte, error) {
		t.Fatal("no pending identity still called rebind")
		return nil, nil
	}); err != nil {
		t.Fatal(err)
	}
	after, err := os.ReadFile(filepath.Join(filepath.Dir(path), roleEnvelopeName))
	if err != nil || !bytes.Equal(before, after) {
		t.Fatal("no-op activation rewrote authority")
	}
	for role, id := range ids {
		if role == "room" {
			continue
		}
		got, err := Identity(dir, role)
		if err != nil || got.PublicKey() != id.PublicKey() {
			t.Fatalf("activation changed %s identity: %v", role, err)
		}
	}
}

func TestPendingIdentityUniquenessIncludesAllCurrentAndPendingRoles(t *testing.T) {
	dir, ids := pendingFixture(t)
	botCompanion, err := Identity(dir, "bot_companion")
	if err != nil {
		t.Fatal(err)
	}
	for _, role := range []string{"repeater", "companion", "observer", "bot", "bot_companion"} {
		key, err := ExportIdentity(dir, role)
		if err != nil {
			t.Fatal(err)
		}
		if err := StageRoleIdentity(context.Background(), dir, "room", key); err == nil {
			t.Fatalf("staging accepted %s current identity", role)
		}
	}
	roomKey, err := ExportIdentity(dir, "room")
	if err != nil {
		t.Fatal(err)
	}
	if err := StageRoleIdentity(context.Background(), dir, "room", roomKey); err != nil {
		t.Fatalf("same-role reimport rejected: %v", err)
	}
	key := expandSeed([32]byte{1})
	if err := StageRoleIdentity(context.Background(), dir, "room", key); err != nil {
		t.Fatal(err)
	}
	if err := StageRoleIdentity(context.Background(), dir, "repeater", key); err == nil {
		t.Fatal("another role's pending key was accepted")
	}
	path := filepath.Join(dir, "companion", "companion.json")
	if _, err := ImportRoleIdentity(path, key, json.RawMessage(`{"state":"replacement"}`)); err == nil {
		t.Fatal("active companion import stole another role's pending key")
	}
	if _, err := ImportRoleIdentity(filepath.Join(dir, "bot_companion", "companion.json"), key,
		json.RawMessage(`{"state":"replacement"}`)); err == nil {
		t.Fatal("bot companion import stole another role's pending key")
	}
	if err := ActivateRoleIdentity(dir, "room", rebindPendingDocument); err != nil {
		t.Fatal(err)
	}
	if _, err := ImportRoleIdentity(path, key, json.RawMessage(`{"state":"replacement"}`)); err == nil {
		t.Fatal("active import checked stale identities after room rekey")
	}
	if _, err := ImportRoleIdentity(path, roomKey, json.RawMessage(`{"state":"replacement"}`)); err != nil {
		t.Fatalf("retired room identity remained incorrectly reserved: %v", err)
	}
	if got, err := Identity(dir, "companion"); err != nil || got.PublicKey() != ids["room"].PublicKey() {
		t.Fatalf("companion import did not select current canonical authority: %v", err)
	}
	if got, err := Identity(dir, "bot_companion"); err != nil || got.PublicKey() != botCompanion.PublicKey() {
		t.Fatalf("sibling import changed bot companion identity: %v", err)
	}
}

func TestConcurrentBotCompanionAndSiblingImportsShareUniquenessLock(t *testing.T) {
	for round := range 4 {
		t.Run(fmt.Sprint(round), func(t *testing.T) {
			dir, ids := pendingFixture(t)
			botCompanion, err := Identity(dir, "bot_companion")
			if err != nil {
				t.Fatal(err)
			}
			candidate, err := Identity(dir, "candidate")
			if err != nil {
				t.Fatal(err)
			}
			key, err := ExportIdentity(dir, "candidate")
			if err != nil {
				t.Fatal(err)
			}
			start := make(chan struct{})
			results := make(chan error, 3)
			var writers sync.WaitGroup
			writers.Go(func() {
				<-start
				results <- StageRoleIdentity(context.Background(), dir, "room", key)
			})
			for _, role := range []string{"companion", "bot_companion"} {
				writers.Go(func() {
					<-start
					_, err := ImportRoleIdentity(filepath.Join(dir, role, "companion.json"), key,
						json.RawMessage(`{"ok":true}`))
					results <- err
				})
			}
			close(start)
			writers.Wait()
			close(results)
			successes := 0
			for err := range results {
				if err == nil {
					successes++
				} else if !strings.Contains(err.Error(), "identity is already current or pending") &&
					!strings.Contains(err.Error(), "identity is already current for") {
					t.Fatalf("transaction failed for a reason other than identity collision: %v", err)
				}
			}
			if successes != 1 {
				t.Fatalf("concurrent role imports/stage committed %d copies of the same identity", successes)
			}
			holders := 0
			for _, role := range []string{"companion", "bot_companion"} {
				got, err := Identity(dir, role)
				if err != nil {
					t.Fatal(err)
				}
				if got.PublicKey() == candidate.PublicKey() {
					holders++
				} else if (role == "companion" && got.PublicKey() != ids["companion"].PublicKey()) ||
					(role == "bot_companion" && got.PublicKey() != botCompanion.PublicKey()) {
					t.Fatalf("%s authority changed to an unrelated identity", role)
				}
			}
			envelope, err := readRoleEnvelope(filepath.Join(dir, "room"))
			if err != nil && !errors.Is(err, os.ErrNotExist) {
				t.Fatal(err)
			}
			if err == nil && envelope.Pending != nil {
				next, err := decodeExpandedIdentity(envelope.Pending)
				if err != nil || next.PublicKey() != candidate.PublicKey() {
					t.Fatalf("room staged a different identity: %v", err)
				}
				holders++
			}
			if holders != 1 {
				t.Fatalf("expected one current or pending authority, found %d", holders)
			}
		})
	}
}

func TestConcurrentStageImportAndNormalSaveUseOneAuthority(t *testing.T) {
	dir, ids := pendingFixture(t)
	key := expandSeed([32]byte{1})
	start := make(chan struct{})
	results := make(chan error, 3)
	var writers sync.WaitGroup
	writers.Go(func() { <-start; results <- StageRoleIdentity(context.Background(), dir, "room", key) })
	writers.Go(func() { <-start; results <- StageRoleIdentity(context.Background(), dir, "repeater", key) })
	writers.Go(func() {
		<-start
		_, err := ImportRoleIdentity(filepath.Join(dir, "companion", "companion.json"), key, json.RawMessage(`{"ok":true}`))
		results <- err
	})
	close(start)
	writers.Wait()
	close(results)
	successes := 0
	for err := range results {
		if err == nil {
			successes++
		}
	}
	if successes != 1 {
		t.Fatalf("concurrent duplicate identity requests committed %d times", successes)
	}
	// Independent stage/save transactions must not overwrite each other's fields.
	own, err := ExportIdentity(dir, "room")
	if err != nil {
		t.Fatal(err)
	}
	start = make(chan struct{})
	saveResults := make(chan error, 2)
	writers.Go(func() { <-start; saveResults <- StageRoleIdentity(context.Background(), dir, "room", own) })
	writers.Go(func() {
		<-start
		saveResults <- WriteRoleJSON(filepath.Join(dir, "room", "state.json"), pendingDocument{ids["room"].String(), 42})
	})
	close(start)
	writers.Wait()
	for range 2 {
		if err := <-saveResults; err != nil {
			t.Fatal(err)
		}
	}
	envelope, err := readRoleEnvelope(filepath.Join(dir, "room"))
	if err != nil || !bytes.Equal(envelope.Pending, own) {
		t.Fatalf("concurrent save lost pending key: %v", err)
	}
	var doc pendingDocument
	if err := json.Unmarshal(envelope.State, &doc); err != nil || doc.Count != 42 {
		t.Fatalf("concurrent stage lost role save: %+v %v", doc, err)
	}
}

func TestPendingIdentityValidationAndFailedRebindPreserveAuthority(t *testing.T) {
	dir, _ := pendingFixture(t)
	valid := expandSeed([32]byte{1})
	unclamped := bytes.Clone(valid)
	unclamped[0] |= 1
	for _, bad := range [][]byte{nil, valid[:63], append(bytes.Clone(valid), 0), make([]byte, 64), unclamped} {
		if err := StageRoleIdentity(context.Background(), dir, "room", bad); err == nil {
			t.Fatal("malformed native expanded key accepted")
		}
	}
	if err := StageRoleIdentity(context.Background(), dir, "companion", valid); err == nil {
		t.Fatal("companion acquired pending-key semantics")
	}
	if err := StageRoleIdentity(context.Background(), dir, "room", valid); err != nil {
		t.Fatal(err)
	}
	path := filepath.Join(dir, "room", roleEnvelopeName)
	before, err := os.ReadFile(path)
	if err != nil {
		t.Fatal(err)
	}
	want := errors.New("rebind rejected state")
	if err := ActivateRoleIdentity(dir, "room", func([]byte, meshcore.Identity, meshcore.Identity) ([]byte, error) {
		return nil, want
	}); !errors.Is(err, want) {
		t.Fatalf("rebind error lost: %v", err)
	}
	after, err := os.ReadFile(path)
	if err != nil || !bytes.Equal(before, after) {
		t.Fatal("failed rebind modified durable authority")
	}
}

func TestPendingIdentityIndeterminateCommitsNeverRollBack(t *testing.T) {
	for _, activation := range []bool{false, true} {
		t.Run(map[bool]string{false: "stage", true: "activate"}[activation], func(t *testing.T) {
			dir, ids := pendingFixture(t)
			key := expandSeed([32]byte{1})
			syncFailure := func(string) error { return errors.New("injected directory sync failure") }
			var err error
			if activation {
				if err := StageRoleIdentity(context.Background(), dir, "room", key); err != nil {
					t.Fatal(err)
				}
				err = activateRoleIdentity(dir, "room", rebindPendingDocument, syncFailure)
			} else {
				err = stageRoleIdentity(context.Background(), dir, "room", key, syncFailure)
			}
			if !errors.Is(err, ErrCommitIndeterminate) {
				t.Fatalf("visible replacement did not report uncertainty: %v", err)
			}
			envelope, err := readRoleEnvelope(filepath.Join(dir, "room"))
			if err != nil {
				t.Fatal(err)
			}
			if activation {
				if envelope.Pending != nil || !bytes.Equal(envelope.Expanded, key) {
					t.Fatal("uncertain activation restored old key or retained pending")
				}
				if err := ActivateRoleIdentity(dir, "room", func([]byte, meshcore.Identity, meshcore.Identity) ([]byte, error) {
					t.Fatal("recovery repeated an already visible activation")
					return nil, nil
				}); err != nil {
					t.Fatal(err)
				}
			} else if !bytes.Equal(envelope.Pending, key) || envelope.identity.PublicKey() != ids["room"].PublicKey() {
				t.Fatal("uncertain stage activated or rolled back visible pending key")
			}
		})
	}
}

func TestMalformedPendingIdentityNeverFallsBackToLegacyAuthority(t *testing.T) {
	dir, _ := pendingFixture(t)
	if err := StageRoleIdentity(context.Background(), dir, "room", expandSeed([32]byte{1})); err != nil {
		t.Fatal(err)
	}
	path := filepath.Join(dir, "room", roleEnvelopeName)
	raw, err := os.ReadFile(path)
	if err != nil {
		t.Fatal(err)
	}
	var envelope map[string]json.RawMessage
	if err := json.Unmarshal(raw, &envelope); err != nil {
		t.Fatal(err)
	}
	envelope["pending_identity"] = json.RawMessage(`"AQ=="`)
	if err := WriteJSON(path, envelope); err != nil {
		t.Fatal(err)
	}
	if _, err := Identity(dir, "room"); err == nil {
		t.Fatal("malformed pending authority fell back to the retained legacy key")
	}
	if err := WriteRoleJSON(filepath.Join(dir, "room", "state.json"), pendingDocument{Count: 8}); err == nil {
		t.Fatal("normal save silently discarded malformed pending authority")
	}
	if err := StageRoleIdentity(context.Background(), dir, "repeater", expandSeed([32]byte{3})); err == nil {
		t.Fatal("cross-role uniqueness ignored unreadable pending authority")
	}
}

func TestResetAndActiveImportClearPendingIdentity(t *testing.T) {
	for _, reset := range []bool{false, true} {
		t.Run(map[bool]string{false: "active-import", true: "reset"}[reset], func(t *testing.T) {
			dir, _ := pendingFixture(t)
			if err := StageRoleIdentity(context.Background(), dir, "room", expandSeed([32]byte{1})); err != nil {
				t.Fatal(err)
			}
			path := filepath.Join(dir, "room", "state.json")
			var active meshcore.LocalIdentity
			var err error
			if reset {
				active, err = ResetRole(path)
			} else {
				active, err = ImportRoleIdentity(path, expandSeed([32]byte{3}), json.RawMessage(`{"active":true}`))
			}
			if err != nil {
				t.Fatal(err)
			}
			envelope, err := readRoleEnvelope(filepath.Dir(path))
			if err != nil || envelope.Pending != nil {
				t.Fatalf("replacement kept stale pending key: %v", err)
			}
			if err := ActivateRoleIdentity(dir, "room", rebindPendingDocument); err != nil {
				t.Fatal(err)
			}
			got, err := Identity(dir, "room")
			if err != nil || got.PublicKey() != active.PublicKey() {
				t.Fatalf("stale pending key activated after replacement: %v", err)
			}
		})
	}
}
