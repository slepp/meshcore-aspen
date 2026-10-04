package state

import (
	"crypto/rand"
	"crypto/sha512"
	"encoding/json"
	"errors"
	"fmt"
	"io"
	"os"
	"path/filepath"
	"strings"
	"syscall"

	meshcore "github.com/meshcore-go/meshcore-go"
)

var ErrCommitIndeterminate = errors.New("state replacement is visible but durability is unconfirmed; recovery required")

func Lock(dir string) (*os.File, error) {
	if err := makeStateDirectory(dir); err != nil {
		return nil, err
	}
	file, err := os.OpenFile(filepath.Join(dir, ".lock"), os.O_CREATE|os.O_RDWR, 0600)
	if err != nil {
		return nil, err
	}
	if err := syscall.Flock(int(file.Fd()), syscall.LOCK_EX|syscall.LOCK_NB); err != nil {
		file.Close()
		return nil, fmt.Errorf("another host owns state directory %s: %w", dir, err)
	}
	return file, nil
}

func Identity(dir, role string) (meshcore.LocalIdentity, error) {
	roleStateMu.Lock()
	defer roleStateMu.Unlock()
	var zero meshcore.LocalIdentity
	roleDir, err := identityDir(dir, role)
	if err != nil {
		return zero, err
	}
	if envelope, err := readRoleEnvelope(roleDir); err == nil {
		return envelope.identity, nil
	} else if !errors.Is(err, os.ErrNotExist) {
		return zero, err
	}
	expanded, err := readIdentityFile(filepath.Join(roleDir, "identity.expanded"), 64)
	if err == nil {
		return decodeExpandedIdentity(expanded)
	}
	if !errors.Is(err, os.ErrNotExist) {
		return zero, err
	}
	path := filepath.Join(roleDir, "identity.seed")
	seed, err := readIdentityFile(path, 32)
	if err == nil {
		return meshcore.NewLocalIdentityFromSeed([32]byte(seed)), nil
	}
	if !errors.Is(err, os.ErrNotExist) {
		return zero, err
	}
	if role == "bot" {
		if _, err := os.Lstat(filepath.Join(roleDir, "native")); err == nil {
			return zero, errors.New("native bot data exists but Go identity authority is missing; restore the bot identity before startup")
		} else if !errors.Is(err, os.ErrNotExist) {
			return zero, err
		}
	}
	id, err := generateIdentity()
	if err != nil {
		return zero, err
	}
	if err := makeStateDirectory(filepath.Dir(path)); err != nil {
		return zero, err
	}
	file, err := os.OpenFile(path, os.O_WRONLY|os.O_CREATE|os.O_EXCL, 0600)
	if err != nil {
		return zero, err
	}
	value := id.Seed()
	_, writeErr := file.Write(value[:])
	err = errors.Join(writeErr, file.Sync(), file.Close())
	if err != nil {
		return zero, errors.Join(err, os.Remove(path))
	}
	if err := syncDirectory(filepath.Dir(path)); err != nil {
		return zero, errors.Join(ErrCommitIndeterminate, err)
	}
	return id, nil
}

// ImportIdentity persists a native scalar-prefix key. The caller must hold the
// state lock and quiesce/reload the affected role; this does not rekey a live node.
func ImportIdentity(dir, role string, expanded []byte) (meshcore.LocalIdentity, error) {
	roleStateMu.Lock()
	defer roleStateMu.Unlock()
	var zero meshcore.LocalIdentity
	roleDir, err := identityDir(dir, role)
	if err != nil {
		return zero, err
	}
	if _, err := readRoleEnvelope(roleDir); err == nil {
		return zero, errors.New("role uses an atomic identity/state envelope; use ImportRoleIdentity")
	} else if !errors.Is(err, os.ErrNotExist) {
		return zero, err
	}
	id, err := decodeExpandedIdentity(expanded)
	if err != nil {
		return zero, err
	}
	if err := checkRoleIdentity(dir, role, id.Identity); err != nil {
		return zero, err
	}
	if err := writeAtomic(filepath.Join(roleDir, "identity.expanded"), expanded, syncDirectory); err != nil {
		return zero, err
	}
	return id, nil
}

// ExportIdentity returns native scalar-prefix bytes, never Go's seed-public-key
// representation. Export does not create an identity when one is missing.
func ExportIdentity(dir, role string) ([]byte, error) {
	roleDir, err := identityDir(dir, role)
	if err != nil {
		return nil, err
	}
	if envelope, err := readRoleEnvelope(roleDir); err == nil {
		return envelope.Expanded, nil
	} else if !errors.Is(err, os.ErrNotExist) {
		return nil, err
	}
	expanded, err := readIdentityFile(filepath.Join(roleDir, "identity.expanded"), 64)
	if err == nil {
		if _, err := decodeExpandedIdentity(expanded); err != nil {
			return nil, err
		}
		return expanded, nil
	}
	if !errors.Is(err, os.ErrNotExist) {
		return nil, err
	}
	seed, err := readIdentityFile(filepath.Join(roleDir, "identity.seed"), 32)
	if err != nil {
		return nil, err
	}
	return expandSeed([32]byte(seed)), nil
}

func expandSeed(seed [32]byte) []byte {
	hash := sha512.Sum512(seed[:])
	hash[0] &= 248
	hash[31] &= 63
	hash[31] |= 64
	return hash[:]
}

func generateIdentity() (meshcore.LocalIdentity, error) {
	for {
		id, err := meshcore.GenerateLocalIdentity(rand.Reader)
		if err != nil {
			return meshcore.LocalIdentity{}, err
		}
		if key := id.PublicKey(); key[0] != 0 && key[0] != 255 {
			return id, nil
		}
	}
}

func decodeExpandedIdentity(expanded []byte) (meshcore.LocalIdentity, error) {
	var zero meshcore.LocalIdentity
	if len(expanded) != 64 {
		return zero, errors.New("native identity must contain 64 scalar-prefix bytes")
	}
	// The SDK clamps again; reject malformed native scalars instead of silently
	// deriving a different identity from the supplied bytes.
	if expanded[0]&7 != 0 || expanded[31]&0xc0 != 0x40 {
		return zero, errors.New("native identity scalar is not clamped")
	}
	id, err := meshcore.NewLocalIdentityFromExpandedKey(expanded)
	if err != nil {
		return zero, err
	}
	if public := id.PublicKey(); public[0] == 0 || public[0] == 255 {
		return zero, errors.New("native identity has a reserved public-key prefix")
	}
	return id, nil
}

func identityDir(dir, role string) (string, error) {
	if role == "" || strings.ContainsAny(role, `/\.`) {
		return "", fmt.Errorf("invalid role name %q", role)
	}
	return filepath.Join(dir, role), nil
}

func readIdentityFile(path string, size int) ([]byte, error) {
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
		return nil, fmt.Errorf("identity %s must be a private regular file (0600)", path)
	}
	if info.Size() != int64(size) {
		return nil, fmt.Errorf("invalid identity length in %s; refusing to replace identity", path)
	}
	data, err := io.ReadAll(io.LimitReader(file, int64(size+1)))
	if err != nil {
		return nil, err
	}
	if len(data) != size {
		return nil, fmt.Errorf("identity length changed while reading %s", path)
	}
	return data, nil
}

func WriteJSON(path string, value any) error {
	data, err := json.MarshalIndent(value, "", "  ")
	if err != nil {
		return err
	}
	return writeAtomic(path, append(data, '\n'), syncDirectory)
}

func writeAtomic(path string, data []byte, syncDir func(string) error) error {
	if err := makeStateDirectory(filepath.Dir(path)); err != nil {
		return err
	}
	file, err := os.CreateTemp(filepath.Dir(path), ".state-*")
	if err != nil {
		return err
	}
	defer os.Remove(file.Name())
	_, writeErr := file.Write(data)
	err = errors.Join(writeErr, file.Sync(), file.Close())
	if err != nil {
		return err
	}
	if err := os.Rename(file.Name(), path); err != nil {
		return err
	}
	if err := syncDir(filepath.Dir(path)); err != nil {
		return errors.Join(ErrCommitIndeterminate, err)
	}
	return nil
}

func syncDirectory(path string) error {
	dir, err := os.Open(path)
	if err != nil {
		return err
	}
	return errors.Join(dir.Sync(), dir.Close())
}

func makeStateDirectory(path string) error {
	if path == "" {
		return errors.New("state directory must not be empty")
	}
	err := os.Mkdir(path, 0700)
	if errors.Is(err, os.ErrExist) {
		info, statErr := os.Stat(path)
		if statErr != nil {
			return statErr
		}
		if !info.IsDir() {
			return fmt.Errorf("state path %s is not a directory", path)
		}
		return nil
	}
	if errors.Is(err, os.ErrNotExist) {
		if err := makeStateDirectory(filepath.Dir(path)); err != nil {
			return err
		}
		return makeStateDirectory(path)
	}
	if err != nil {
		return err
	}
	return syncDirectory(filepath.Dir(path))
}
