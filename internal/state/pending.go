package state

import (
	"bytes"
	"context"
	"encoding/json"
	"errors"
	"fmt"
	"os"
	"path/filepath"

	meshcore "github.com/meshcore-go/meshcore-go"
)

// StageRoleIdentity saves a room/repeater key for the next logical or host
// restart. Neither the active identity nor the selected document changes.
func StageRoleIdentity(ctx context.Context, dir, role string, expanded []byte) error {
	return stageRoleIdentity(ctx, dir, role, expanded, syncDirectory)
}

func stageRoleIdentity(ctx context.Context, dir, role string, expanded []byte, syncDir func(string) error) error {
	if err := roleStateMu.lockContext(ctx); err != nil {
		return err
	}
	defer roleStateMu.Unlock()
	if err := ctx.Err(); err != nil {
		return err
	}
	if role != "room" && role != "repeater" {
		return errors.New("pending identity requires a room or repeater role")
	}
	next, err := decodeExpandedIdentity(expanded)
	if err != nil {
		return err
	}
	if err := checkRoleIdentity(dir, role, next.Identity); err != nil {
		return err
	}
	roleDir := filepath.Join(dir, role)
	envelope, err := readRoleEnvelope(roleDir)
	if errors.Is(err, os.ErrNotExist) {
		key, err := ExportIdentity(dir, role)
		if err != nil {
			return err
		}
		data, err := os.ReadFile(filepath.Join(roleDir, "state.json"))
		if err != nil {
			return err
		}
		envelope = &roleEnvelope{Version: 1, Expanded: key, Document: "state.json", State: data}
	} else if err != nil {
		return err
	}
	body := bytes.TrimSpace(envelope.State)
	if envelope.Document != "state.json" || len(body) == 0 || body[0] != '{' || !json.Valid(body) {
		return errors.New("pending identity requires an existing role document")
	}
	envelope.Pending = bytes.Clone(expanded)
	if err := ctx.Err(); err != nil {
		return err
	}
	return writeRoleEnvelope(roleDir, envelope, syncDir)
}

// ActivateRoleIdentity must only run before service construction, after its old
// service and source have retired. Rebind performs no I/O and must not re-enter
// state helpers. With no pending key this performs no writes, even for a new root.
func ActivateRoleIdentity(dir, role string, rebind func([]byte, meshcore.Identity, meshcore.Identity) ([]byte, error)) error {
	return activateRoleIdentity(dir, role, rebind, syncDirectory)
}

func activateRoleIdentity(dir, role string, rebind func([]byte, meshcore.Identity, meshcore.Identity) ([]byte, error), syncDir func(string) error) error {
	roleStateMu.Lock()
	defer roleStateMu.Unlock()
	if role != "room" && role != "repeater" {
		return errors.New("pending identity activation requires a room or repeater role")
	}
	roleDir := filepath.Join(dir, role)
	envelope, err := readRoleEnvelope(roleDir)
	if errors.Is(err, os.ErrNotExist) {
		return nil
	}
	if err != nil {
		return err
	}
	if envelope.Document != "state.json" {
		return errors.New("pending identity belongs to a different document")
	}
	if envelope.Pending == nil {
		return nil
	}
	next := envelope.pending
	if err := checkRoleIdentity(dir, role, next.Identity); err != nil {
		return err
	}
	document, err := rebind(envelope.State, envelope.identity.Identity, next.Identity)
	if err != nil {
		return err
	}
	if body := bytes.TrimSpace(document); len(body) == 0 || body[0] != '{' || !json.Valid(body) {
		return errors.New("identity activation requires a complete role document")
	}
	envelope.Expanded, envelope.State, envelope.Pending = envelope.Pending, document, nil
	return writeRoleEnvelope(roleDir, envelope, syncDir)
}

// Callers hold roleStateMu across this check and the subsequent commit. Read
// current authority rather than keeping a second in-memory identity registry.
func checkRoleIdentity(dir, target string, candidate meshcore.Identity) error {
	for _, role := range []string{"repeater", "room", "companion", "observer", "bot", "bot_companion"} {
		if role == target {
			continue
		}
		envelope, err := readRoleEnvelope(filepath.Join(dir, role))
		if err == nil {
			if candidate.PublicKey() == envelope.identity.PublicKey() ||
				(envelope.Pending != nil && candidate.PublicKey() == envelope.pending.PublicKey()) {
				return fmt.Errorf("identity is already current or pending for %s", role)
			}
			continue
		}
		if !errors.Is(err, os.ErrNotExist) {
			return fmt.Errorf("%s identity authority: %w", role, err)
		}
		key, err := ExportIdentity(dir, role)
		if errors.Is(err, os.ErrNotExist) {
			continue
		}
		if err != nil {
			return fmt.Errorf("%s identity authority: %w", role, err)
		}
		current, err := decodeExpandedIdentity(key)
		if err != nil {
			return err
		}
		if candidate.PublicKey() == current.PublicKey() {
			return fmt.Errorf("identity is already current for %s", role)
		}
	}
	return nil
}
