package main

import (
	"bytes"
	"crypto/ed25519"
	"crypto/rand"
	"crypto/sha256"
	"encoding/hex"
	"encoding/json"
	"errors"
	"fmt"
	"io"
	"net/http"
	"os"
	"path/filepath"
	"regexp"
	"syscall"

	"golang.org/x/sys/unix"
)

var aliasPattern = regexp.MustCompile(`^[a-zA-Z0-9_-]{1,64}$`)
var publicKeyPattern = regexp.MustCompile(`^[a-f0-9]{64}$`)
var postIDPattern = regexp.MustCompile(`^[a-f0-9]{8}-[a-f0-9]{4}-4[a-f0-9]{3}-[89ab][a-f0-9]{3}-[a-f0-9]{12}$`)

type pendingPost struct {
	ID     string `json:"id"`
	Text   string `json:"text"`
	Author string `json:"author"`
}

type savedRoom struct {
	Draft   string       `json:"draft"`
	Pending *pendingPost `json:"pending,omitempty"`
}

type savedState struct {
	Version        int                  `json:"version"`
	Origin         string               `json:"origin"`
	Seed           string               `json:"seed"`
	PublicKey      string               `json:"publicKey"`
	LegacyIdentity string               `json:"legacyIdentity"`
	Username       string               `json:"username"`
	Name           string               `json:"name"`
	Selected       string               `json:"selected"`
	Rooms          map[string]savedRoom `json:"rooms"`
	Cookies        []*http.Cookie       `json:"cookies"`
}

func (s *savedState) privateKey() ed25519.PrivateKey {
	seed, _ := hex.DecodeString(s.Seed) // Validated before the state enters the model.
	defer clear(seed)
	return ed25519.NewKeyFromSeed(seed)
}

type stateStore struct {
	directory string
	lock      *os.File
}

func privateInfo(file *os.File, directory bool) error {
	info, err := file.Stat()
	if err != nil {
		return err
	}
	stat, ok := info.Sys().(*syscall.Stat_t)
	if !ok || int(stat.Uid) != os.Getuid() || info.IsDir() != directory ||
		(!directory && !info.Mode().IsRegular()) || info.Mode().Perm()&0077 != 0 {
		return errors.New("state requires an owner-only directory (0700) and regular files (0600)")
	}
	return nil
}

func openPrivate(path string, flags int, directory bool) (*os.File, error) {
	file, err := os.OpenFile(path, flags|unix.O_NOFOLLOW, 0600)
	if err != nil {
		return nil, err
	}
	if err := privateInfo(file, directory); err != nil {
		file.Close()
		return nil, err
	}
	return file, nil
}

func openState(directory, origin string) (*stateStore, *savedState, error) {
	if directory == "" {
		base, err := os.UserConfigDir()
		if err != nil {
			return nil, nil, err
		}
		hash := sha256.Sum256([]byte(origin))
		directory = filepath.Join(base, "aspen-rooms", hex.EncodeToString(hash[:8]))
	}
	directory, err := filepath.Abs(directory)
	if err != nil {
		return nil, nil, err
	}
	if err := os.MkdirAll(directory, 0700); err != nil {
		return nil, nil, fmt.Errorf("create private state directory: %w", err)
	}
	dir, err := openPrivate(directory, os.O_RDONLY|unix.O_DIRECTORY, true)
	if err != nil {
		return nil, nil, fmt.Errorf("open state directory: %w", err)
	}
	dir.Close()
	lock, err := openPrivate(filepath.Join(directory, "session.lock"), os.O_CREATE|os.O_RDWR, false)
	if err != nil {
		return nil, nil, fmt.Errorf("open state lock: %w", err)
	}
	if err := unix.Flock(int(lock.Fd()), unix.LOCK_EX|unix.LOCK_NB); err != nil {
		lock.Close()
		return nil, nil, errors.New("this state directory is already in use by another room client")
	}
	store := &stateStore{directory: directory, lock: lock}
	saved, err := store.load()
	if errors.Is(err, os.ErrNotExist) {
		seed, identity := make([]byte, 32), make([]byte, 32)
		if _, err = rand.Read(seed); err == nil {
			_, err = rand.Read(identity)
		}
		if err == nil {
			key := ed25519.NewKeyFromSeed(seed)
			saved = &savedState{Version: 1, Origin: origin, Seed: hex.EncodeToString(seed),
				PublicKey: hex.EncodeToString(key.Public().(ed25519.PublicKey)), LegacyIdentity: hex.EncodeToString(identity),
				Rooms: make(map[string]savedRoom)}
			clear(key)
			err = store.save(saved)
		}
		clear(seed)
		clear(identity)
	}
	if err == nil && saved.Origin != origin {
		err = errors.New("state directory belongs to another service; use a separate --state-dir")
	}
	if err != nil {
		store.close()
		return nil, nil, fmt.Errorf("load private state: %w", err)
	}
	return store, saved, nil
}

func (s *stateStore) close() { s.lock.Close() }

func (s *stateStore) load() (*savedState, error) {
	file, err := openPrivate(filepath.Join(s.directory, "state.json"), os.O_RDONLY, false)
	if err != nil {
		return nil, err
	}
	defer file.Close()
	reader := io.LimitReader(file, 2*1024*1024+1)
	data, err := io.ReadAll(reader)
	if err != nil {
		return nil, err
	}
	defer clear(data)
	if len(data) > 2*1024*1024 {
		return nil, errors.New("saved state exceeds 2 MiB")
	}
	var saved savedState
	decoder := json.NewDecoder(bytes.NewReader(data))
	decoder.DisallowUnknownFields()
	if err := decoder.Decode(&saved); err != nil {
		return nil, errors.New("saved state is invalid JSON; restore the state file before starting")
	}
	if err := decoder.Decode(new(any)); err != io.EOF {
		return nil, errors.New("saved state has trailing data")
	}
	if err := validateState(&saved); err != nil {
		return nil, err
	}
	return &saved, nil
}

func validateState(saved *savedState) error {
	if saved.Version != 1 || !publicKeyPattern.MatchString(saved.Seed) || !publicKeyPattern.MatchString(saved.PublicKey) ||
		!publicKeyPattern.MatchString(saved.LegacyIdentity) || saved.Rooms == nil || len(saved.Rooms) > 256 ||
		(saved.Selected != "" && !aliasPattern.MatchString(saved.Selected)) || len(saved.Name) > 24 ||
		(saved.Username != "" && !usernamePattern.MatchString(saved.Username)) {
		return errors.New("saved state schema or device key is invalid; restore the state file")
	}
	key := saved.privateKey()
	matches := hex.EncodeToString(key.Public().(ed25519.PublicKey)) == saved.PublicKey
	clear(key)
	if !matches {
		return errors.New("saved device keys do not match; restore the state file")
	}
	for alias, room := range saved.Rooms {
		if !aliasPattern.MatchString(alias) || len(room.Draft) > 8192 ||
			(room.Pending != nil && (!postIDPattern.MatchString(room.Pending.ID) ||
				!publicKeyPattern.MatchString(room.Pending.Author) || len(room.Pending.Text) > 151 || room.Pending.Text == "")) {
			return errors.New("saved room draft or pending post is invalid; restore the state file")
		}
	}
	return nil
}

func (s *stateStore) save(saved *savedState) error {
	if err := validateState(saved); err != nil {
		return err
	}
	data, err := json.MarshalIndent(saved, "", "  ")
	if err != nil {
		return err
	}
	defer clear(data)
	if len(data) > 2*1024*1024 {
		return errors.New("state exceeds 2 MiB; no state file was replaced")
	}
	file, err := os.CreateTemp(s.directory, ".state-")
	if err != nil {
		return err
	}
	temporary := file.Name()
	defer os.Remove(temporary)
	if err := file.Chmod(0600); err != nil {
		file.Close()
		return err
	}
	_, writeErr := file.Write(data)
	syncErr := file.Sync()
	closeErr := file.Close()
	if err := errors.Join(writeErr, syncErr, closeErr); err != nil {
		return err
	}
	target := filepath.Join(s.directory, "state.json")
	if info, err := os.Lstat(target); err == nil {
		if !info.Mode().IsRegular() || info.Mode().Perm()&0077 != 0 {
			return errors.New("refusing to replace an unsafe state file")
		}
	} else if !errors.Is(err, os.ErrNotExist) {
		return err
	}
	if err := os.Rename(temporary, target); err != nil {
		return err
	}
	dir, err := openPrivate(s.directory, os.O_RDONLY|unix.O_DIRECTORY, true)
	if err != nil {
		return err
	}
	syncErr = dir.Sync()
	closeErr = dir.Close()
	if err := errors.Join(syncErr, closeErr); err != nil {
		return err
	}
	checked, err := s.load()
	if err != nil {
		return err
	}
	roundTrip, err := json.MarshalIndent(checked, "", "  ")
	defer clear(roundTrip)
	if err != nil || !bytes.Equal(data, roundTrip) {
		return errors.New("state readback did not match; no send was started")
	}
	return nil
}

func newPostID() (string, error) {
	b := make([]byte, 16)
	if _, err := rand.Read(b); err != nil {
		return "", err
	}
	b[6] = (b[6] & 15) | 64
	b[8] = (b[8] & 63) | 128
	h := hex.EncodeToString(b)
	return h[:8] + "-" + h[8:12] + "-" + h[12:16] + "-" + h[16:20] + "-" + h[20:], nil
}
