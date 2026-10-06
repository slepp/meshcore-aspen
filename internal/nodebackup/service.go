// SPDX-License-Identifier: Apache-2.0
package nodebackup

import (
	"context"
	"crypto/sha256"
	"encoding/base64"
	"encoding/hex"
	"encoding/json"
	"errors"
	"fmt"
	"io"
	"os"
	"path/filepath"
	"strconv"
	"strings"
	"sync"
	"time"

	"meshcore.local/meshcore/internal/state"
)

type record struct {
	Slot   int    `json:"slot"`
	Bytes  int    `json:"bytes"`
	SHA256 string `json:"sha256"`
}

type Service struct {
	mu        sync.Mutex
	ctx       context.Context
	root      string
	snapshot  func(context.Context) (map[string][]byte, error)
	saved     record
	ready     bool
	busy      bool
	readers   int
	cancel    context.CancelFunc
	failure   string
	lastRF    time.Time
	closed    bool
	closeRoot context.CancelFunc
	work      sync.WaitGroup
}

func New(ctx context.Context, root string, snapshot func(context.Context) (map[string][]byte, error)) (*Service, error) {
	if snapshot == nil {
		return nil, errors.New("backup snapshot provider is missing")
	}
	if err := os.Mkdir(root, 0700); err != nil && !errors.Is(err, os.ErrExist) {
		return nil, err
	}
	info, err := os.Lstat(root)
	if err != nil || !info.IsDir() || info.Mode().Perm()&0077 != 0 {
		return nil, errors.New("node backup directory must be a private real directory")
	}
	ctx, cancel := context.WithCancel(ctx)
	return &Service{ctx: ctx, closeRoot: cancel, root: root, snapshot: snapshot}, nil
}

func (s *Service) Close() error {
	s.mu.Lock()
	s.closed = true
	s.closeRoot()
	s.mu.Unlock()
	s.work.Wait()
	return nil
}
func (s *Service) path(slot int) string {
	return filepath.Join(s.root, fmt.Sprintf("node-%d.mcb", slot))
}

func (s *Service) pointer() (record, bool, error) {
	var value record
	raw, err := privateRead(filepath.Join(s.root, "active.json"), 256)
	if errors.Is(err, os.ErrNotExist) {
		return value, false, nil
	}
	if err != nil {
		return value, false, err
	}
	if len(raw) > 256 || json.Unmarshal(raw, &value) != nil || value.Slot < 0 || value.Slot > 1 ||
		value.Bytes < 120 || value.Bytes > FileLimit || len(value.SHA256) != 64 {
		return value, false, errors.New("saved backup pointer is invalid; existing files retained")
	}
	return value, true, nil
}

func (s *Service) load() error {
	value, present, err := s.pointer()
	if err != nil {
		return err
	}
	if !present {
		return errors.New("saved backup missing; use backup start KEY64")
	}
	raw, err := privateRead(s.path(value.Slot), FileLimit)
	if err != nil {
		return err
	}
	if len(raw) != value.Bytes || !strings.EqualFold(fmt.Sprintf("%x", sha256.Sum256(raw)), value.SHA256) ||
		string(raw[:8]) != "MCB\x01\x01\x00\x00\x00" {
		return errors.New("saved backup size or checksum failed")
	}
	s.saved, s.ready, s.lastRF = value, true, time.Time{}
	return nil
}

func privateRead(path string, limit int) ([]byte, error) {
	info, err := os.Lstat(path)
	if err != nil {
		return nil, err
	}
	if !info.Mode().IsRegular() || info.Mode().Perm()&0077 != 0 || info.Size() > int64(limit) {
		return nil, errors.New("backup record must be a private bounded regular file")
	}
	file, err := os.Open(path)
	if err != nil {
		return nil, err
	}
	defer file.Close()
	actual, err := file.Stat()
	if err != nil || !os.SameFile(info, actual) {
		return nil, errors.New("backup record changed while opening")
	}
	raw, err := io.ReadAll(io.LimitReader(file, int64(limit)+1))
	if len(raw) > limit {
		return nil, errors.New("backup record grew beyond its limit")
	}
	return raw, err
}

func (s *Service) prepare(ctx context.Context, recipient [32]byte, previous record, present bool) {
	defer s.work.Done()
	records, err := s.snapshot(ctx)
	defer func() {
		for _, value := range records {
			clear(value)
		}
	}()
	var raw []byte
	if err == nil {
		raw, err = Encode(records, recipient)
	}
	s.mu.Lock()
	defer s.mu.Unlock()
	defer func() { s.cancel(); s.busy = false; s.cancel = nil }()
	if err == nil {
		err = ctx.Err()
	}
	if err == nil {
		slot := 0
		if present {
			slot = 1 - previous.Slot
		}
		path := s.path(slot)
		err = s.publish(path, raw)
		if err == nil {
			next := record{Slot: slot, Bytes: len(raw), SHA256: fmt.Sprintf("%x", sha256.Sum256(raw))}
			err = state.WriteJSON(filepath.Join(s.root, "active.json"), next)
			if err == nil {
				s.saved, s.ready, s.lastRF = next, true, time.Time{}
			}
		}
	}
	if err != nil {
		s.failure = "Backup preparation failed: " + err.Error() + "; use backup load for the previous snapshot"
	}
}

func (s *Service) publish(path string, raw []byte) error {
	file, err := os.CreateTemp(s.root, ".backup-")
	if err != nil {
		return err
	}
	name := file.Name()
	defer os.Remove(name)
	_, writeErr := file.Write(raw)
	err = errors.Join(writeErr, file.Sync(), file.Close())
	if err != nil {
		return err
	}
	actual, err := privateRead(name, FileLimit)
	if err != nil || sha256.Sum256(actual) != sha256.Sum256(raw) {
		return errors.New("backup output checksum readback failed; previous backup retained")
	}
	return os.Rename(name, path)
}

func (s *Service) Command(command string, radio bool) string {
	return s.CommandBudget(command, radio, 162)
}

func (s *Service) CommandBudget(command string, radio bool, replyLimit int) string {
	s.mu.Lock()
	defer s.mu.Unlock()
	argument := strings.TrimPrefix(command, "backup")
	argument = strings.TrimSpace(argument)
	fail := func(err error) string { return "Error: " + err.Error() }
	if s.closed {
		return "Error: host backup service is stopping"
	}
	if command != "backup" && !strings.HasPrefix(command, "backup ") {
		return "Error: unknown backup command; use backup help"
	}
	switch {
	case argument == "" || argument == "help":
		return "backup start KEY64; status; load; read64 ID16 OFFSET; read ID16 OFFSET; cancel; clear. Encrypted to KEY64; RF reads paced 5s; retain operator seed."
	case argument == "status":
		if s.busy {
			return "PREPARING; check backup status"
		}
		if s.failure != "" {
			return "Error: " + s.failure
		}
		if !s.ready {
			if err := s.load(); err != nil {
				s.ready, s.failure = false, err.Error()
				return fail(err)
			}
		}
		return fmt.Sprintf("READY %s bytes=%d sha=%s", s.saved.SHA256[:16], s.saved.Bytes, s.saved.SHA256)
	case argument == "load":
		if s.busy || s.readers > 0 {
			return "Error: backup preparation or download busy"
		}
		if err := s.load(); err != nil {
			s.ready, s.failure = false, err.Error()
			return fail(err)
		}
		s.failure = ""
		return "PREPARING saved backup; check backup status"
	case strings.HasPrefix(argument, "start "):
		key := strings.TrimPrefix(argument, "start ")
		bytes, err := hex.DecodeString(key)
		if err != nil || len(bytes) != 32 || strings.ToLower(key) != key || key == strings.Repeat("0", 64) {
			return "Error: backup start requires a lowercase operator KEY64"
		}
		if s.busy || s.readers > 0 {
			return "Error: backup preparation or download busy"
		}
		previous, present, err := s.pointer()
		if err != nil {
			return fail(err)
		}
		ctx, cancel := context.WithCancel(s.ctx)
		s.busy, s.cancel, s.failure = true, cancel, ""
		s.work.Add(1)
		go s.prepare(ctx, [32]byte(bytes), previous, present)
		return "PREPARING encrypted node backup; check backup status"
	case argument == "cancel":
		if !s.busy {
			return "Error: no backup preparation active"
		}
		s.cancel()
		return "Cancellation requested; inspect backup status"
	case argument == "clear":
		if s.busy || s.readers > 0 {
			return "Error: backup preparation or download busy"
		}
		for _, path := range []string{filepath.Join(s.root, "active.json"), s.path(0), s.path(1)} {
			if err := os.Remove(path); err != nil && !errors.Is(err, os.ErrNotExist) {
				return fail(err)
			}
			s.ready = false
		}
		directory, err := os.Open(s.root)
		if err != nil {
			return fail(err)
		}
		err = errors.Join(directory.Sync(), directory.Close())
		if err != nil {
			return fail(err)
		}
		s.ready, s.failure = false, ""
		return "Saved backup removed; host settings unchanged"
	case strings.HasPrefix(argument, "read ") || strings.HasPrefix(argument, "read64 "):
		words := strings.Split(argument, " ")
		if len(words) != 3 || strings.Trim(words[2], "0123456789") != "" || words[2] == "" {
			return "Error: backup read requires ID16 and decimal byte offset"
		}
		offset, err := strconv.ParseUint(words[2], 10, 32)
		if err != nil || !s.ready || s.busy || words[1] != s.saved.SHA256[:16] || offset > uint64(s.saved.Bytes) {
			return "Error: backup ID or byte offset changed; inspect backup status"
		}
		if radio && !s.lastRF.IsZero() && time.Since(s.lastRF) < 5*time.Second {
			return fmt.Sprintf("WAIT ms=%d", max(1, (5*time.Second-time.Since(s.lastRF)).Milliseconds()))
		}
		info, err := os.Lstat(s.path(s.saved.Slot))
		if err != nil || !info.Mode().IsRegular() || info.Mode().Perm()&0077 != 0 || info.Size() != int64(s.saved.Bytes) {
			return "Error: backup file read failed"
		}
		if int(offset) == s.saved.Bytes {
			return "EOF"
		}
		file, err := os.Open(s.path(s.saved.Slot))
		if err != nil {
			return "Error: backup file read failed"
		}
		defer file.Close()
		actual, err := file.Stat()
		if err != nil || !os.SameFile(info, actual) {
			return "Error: backup file changed while opening"
		}
		encoded := words[0] == "read64"
		kind := "CHUNK"
		if encoded {
			kind = "CHUNK64"
		}
		header := fmt.Sprintf("%s %s %d ", kind, words[1], offset)
		budget := min(replyLimit, 162) - len(header)
		limit := 48
		if encoded {
			limit = budget * 3 / 4
		}
		if budget < 2 || (!encoded && budget < 96) {
			return "Error: backup reply capacity is too small"
		}
		count := min(limit, s.saved.Bytes-int(offset))
		raw := make([]byte, count)
		if _, err := file.ReadAt(raw, int64(offset)); err != nil {
			return "Error: backup file read failed"
		}
		if radio {
			s.lastRF = time.Now()
		}
		if encoded {
			return header + base64.RawStdEncoding.EncodeToString(raw)
		}
		return header + hex.EncodeToString(raw)
	default:
		return "Error: unknown backup command; use backup help"
	}
}

type lease struct {
	*os.File
	service *Service
	once    sync.Once
}

func (l *lease) Close() error {
	err := l.File.Close()
	l.once.Do(func() {
		l.service.mu.Lock()
		l.service.readers--
		l.service.mu.Unlock()
	})
	return err
}

func (s *Service) Open(id string) (io.ReadCloser, int, error) {
	s.mu.Lock()
	defer s.mu.Unlock()
	if s.closed || s.busy || !s.ready || id != s.saved.SHA256[:16] {
		return nil, 0, errors.New("backup ID changed or is not ready; inspect backup status")
	}
	info, err := os.Lstat(s.path(s.saved.Slot))
	if err != nil || !info.Mode().IsRegular() || info.Mode().Perm()&0077 != 0 {
		return nil, 0, errors.New("saved backup file is unavailable or not private")
	}
	file, err := os.Open(s.path(s.saved.Slot))
	if err != nil {
		return nil, 0, err
	}
	actual, err := file.Stat()
	if err != nil || !os.SameFile(info, actual) || actual.Size() != int64(s.saved.Bytes) {
		file.Close()
		return nil, 0, errors.New("saved backup file changed")
	}
	s.readers++
	return &lease{File: file, service: s}, s.saved.Bytes, nil
}
