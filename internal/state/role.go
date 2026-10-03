package state

import (
	"bytes"
	"context"
	"encoding/json"
	"errors"
	"fmt"
	"io"
	"os"
	"path/filepath"

	meshcore "github.com/meshcore-go/meshcore-go"
)

const roleEnvelopeName = "identity-state.json"
const maxRoleEnvelopeBytes = 16 * 1024 * 1024

// The host holds Lock for its root directory. Within that process, this lock
// serializes identity decisions and envelope read/modify/write transactions.
var roleStateMu = make(roleStateLock, 1)

type roleStateLock chan struct{}

func (m roleStateLock) Lock()   { m <- struct{}{} }
func (m roleStateLock) Unlock() { <-m }
func (m roleStateLock) lockContext(ctx context.Context) error {
	select {
	case m <- struct{}{}:
		return nil
	case <-ctx.Done():
		return ctx.Err()
	}
}

type roleEnvelope struct {
	Version  int             `json:"version"`
	Expanded []byte          `json:"identity"`
	Document string          `json:"document"`
	State    json.RawMessage `json:"state"`
	Pending  []byte          `json:"pending_identity,omitempty"`
	identity meshcore.LocalIdentity
	pending  meshcore.LocalIdentity
}

func readRoleEnvelope(dir string) (*roleEnvelope, error) {
	path := filepath.Join(dir, roleEnvelopeName)
	file, err := os.Open(path)
	if err != nil {
		return nil, err
	}
	defer file.Close()
	info, err := file.Stat()
	if err != nil {
		return nil, err
	}
	if !info.Mode().IsRegular() || info.Mode().Perm()&0077 != 0 {
		return nil, fmt.Errorf("role envelope %s must be a private regular file", path)
	}
	data, err := io.ReadAll(io.LimitReader(file, maxRoleEnvelopeBytes+1))
	if err != nil {
		return nil, err
	}
	if len(data) > maxRoleEnvelopeBytes {
		return nil, errors.New("role envelope exceeds 16 MiB")
	}
	var envelope roleEnvelope
	if err := json.Unmarshal(data, &envelope); err != nil {
		return nil, fmt.Errorf("invalid role envelope: %w", err)
	}
	if envelope.Version != 1 || envelope.Document == "" ||
		envelope.Document == "." || envelope.Document == ".." ||
		filepath.Base(envelope.Document) != envelope.Document {
		return nil, errors.New("invalid role envelope version or document name")
	}
	body := bytes.TrimSpace(envelope.State)
	if len(body) == 0 || (body[0] != '{' && !bytes.Equal(body, []byte("null"))) {
		return nil, errors.New("role envelope must contain an object or explicit null document")
	}
	envelope.identity, err = decodeExpandedIdentity(envelope.Expanded)
	if err != nil {
		return nil, err
	}
	if envelope.Pending != nil {
		envelope.pending, err = decodeExpandedIdentity(envelope.Pending)
		if err != nil {
			return nil, fmt.Errorf("invalid pending role identity: %w", err)
		}
	}
	return &envelope, nil
}

// ReadRoleJSON selects the same authority as Identity. Explicit null marks a
// reset role; it must not reactivate the retained pre-import document.
func ReadRoleJSON(path string) ([]byte, error) {
	envelope, err := readRoleEnvelope(filepath.Dir(path))
	if errors.Is(err, os.ErrNotExist) {
		return os.ReadFile(path)
	}
	if err != nil {
		return nil, err
	}
	if envelope.Document != filepath.Base(path) {
		return nil, errors.New("role envelope belongs to a different document")
	}
	if bytes.Equal(bytes.TrimSpace(envelope.State), []byte("null")) {
		return nil, &os.PathError{Op: "read", Path: path, Err: os.ErrNotExist}
	}
	return envelope.State, nil
}

// WriteRoleJSON requires the role's existing application serialization lock.
// Its envelope transaction also preserves any concurrently staged pending key.
func WriteRoleJSON(path string, value any) error {
	roleStateMu.Lock()
	defer roleStateMu.Unlock()
	envelope, err := readRoleEnvelope(filepath.Dir(path))
	if errors.Is(err, os.ErrNotExist) {
		return WriteJSON(path, value)
	}
	if err != nil {
		return err
	}
	if envelope.Document != filepath.Base(path) {
		return errors.New("role envelope belongs to a different document")
	}
	envelope.State, err = json.Marshal(value)
	if err != nil {
		return err
	}
	return writeRoleEnvelope(filepath.Dir(path), envelope, syncDirectory)
}

// ImportRoleIdentity commits the native key and complete next role document
// together, clearing pending identity. The caller owns application serialization
// and activation; cross-role current/pending uniqueness is checked at commit.
func ImportRoleIdentity(path string, expanded []byte, next json.RawMessage) (meshcore.LocalIdentity, error) {
	return ImportRoleIdentityContext(context.Background(), path, expanded, next)
}

// ImportRoleIdentityContext checks admission cancellation before the coordinated
// commit, but never substitutes a late cancellation for a durable commit result.
func ImportRoleIdentityContext(ctx context.Context, path string, expanded []byte, next json.RawMessage) (meshcore.LocalIdentity, error) {
	if err := roleStateMu.lockContext(ctx); err != nil {
		return meshcore.LocalIdentity{}, err
	}
	defer roleStateMu.Unlock()
	if err := ctx.Err(); err != nil {
		return meshcore.LocalIdentity{}, err
	}
	id, err := decodeExpandedIdentity(expanded)
	if err != nil {
		return meshcore.LocalIdentity{}, err
	}
	if body := bytes.TrimSpace(next); len(body) == 0 || body[0] != '{' {
		return meshcore.LocalIdentity{}, errors.New("identity import requires a complete role object")
	}
	dir := filepath.Dir(path)
	if err := checkRoleIdentity(filepath.Dir(dir), filepath.Base(dir), id.Identity); err != nil {
		return meshcore.LocalIdentity{}, err
	}
	if err := ctx.Err(); err != nil {
		return meshcore.LocalIdentity{}, err
	}
	if err := writeRoleEnvelope(filepath.Dir(path), &roleEnvelope{
		Version: 1, Expanded: expanded, Document: filepath.Base(path), State: next,
	}, syncDirectory); err != nil {
		return meshcore.LocalIdentity{}, err
	}
	return id, nil
}

// ResetRole must run after the old role has closed and finished its final save.
// One replacement keeps legacy files from becoming authoritative after a crash.
func ResetRole(path string) (meshcore.LocalIdentity, error) {
	roleStateMu.Lock()
	defer roleStateMu.Unlock()
	id, err := generateIdentity()
	if err != nil {
		return meshcore.LocalIdentity{}, err
	}
	if err := writeRoleEnvelope(filepath.Dir(path), &roleEnvelope{
		Version: 1, Expanded: expandSeed(id.Seed()), Document: filepath.Base(path),
		State: json.RawMessage("null"),
	}, syncDirectory); err != nil {
		return meshcore.LocalIdentity{}, err
	}
	return id, nil
}

func writeRoleEnvelope(dir string, envelope *roleEnvelope, syncDir func(string) error) error {
	data, err := json.MarshalIndent(envelope, "", "  ")
	if err != nil {
		return err
	}
	if len(data)+1 > maxRoleEnvelopeBytes {
		return errors.New("role envelope exceeds 16 MiB")
	}
	return writeAtomic(filepath.Join(dir, roleEnvelopeName), append(data, '\n'), syncDir)
}
