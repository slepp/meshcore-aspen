package nativebot

import (
	"context"
	"encoding/hex"
	"errors"
	"fmt"
	"log/slog"
	"net"
	"os"
	"path/filepath"
	"strings"
	"sync"
	"time"

	meshcore "github.com/meshcore-go/meshcore-go"
	"meshcore.local/meshcore/internal/state"
)

// Service retains the shared source and owner socket across a bot-only reload.
type Service struct {
	ctx         context.Context
	cfg         Config
	root        string
	logger      *slog.Logger
	fatal       chan error
	adminSocket net.Listener
	adminPath   string
	operation   sync.Mutex
	mu          sync.RWMutex
	worker      *Worker
	key         meshcore.LocalIdentity
	fault       string
}

func Start(ctx context.Context, cfg Config) (*Service, error) {
	worker, err := startWorker(ctx, cfg)
	if err != nil {
		return nil, err
	}
	identity := cfg.Identity
	cfg.Expanded = nil
	cfg.Identity = meshcore.LocalIdentity{}
	s := &Service{ctx: ctx, cfg: cfg, root: filepath.Dir(filepath.Dir(cfg.StateDir)),
		logger: cfg.Logger, fatal: make(chan error, 1), worker: worker, key: identity}
	if err := s.listenAdmin(ctx, filepath.Join(cfg.StateDir, "admin.sock")); err != nil {
		worker.Close()
		return nil, err
	}
	s.monitor(worker)
	return s, nil
}

func (s *Service) abort(err error) {
	select {
	case s.fatal <- err:
	default:
	}
}

func (s *Service) monitor(w *Worker) {
	go func() {
		var err error
		select {
		case err = <-w.Fatal():
		case <-w.ctx.Done():
			select {
			case err = <-w.Fatal():
			default:
			}
		}
		s.mu.RLock()
		current := s.worker == w
		s.mu.RUnlock()
		if current && err != nil {
			s.abort(err)
		}
	}()
}

func (s *Service) Fatal() <-chan error { return s.fatal }

func (s *Service) PublicKey() string {
	s.mu.RLock()
	defer s.mu.RUnlock()
	return s.key.String()
}

func (s *Service) Error() string {
	s.mu.RLock()
	defer s.mu.RUnlock()
	return s.fault
}

func (s *Service) Snapshot() Status {
	s.mu.RLock()
	defer s.mu.RUnlock()
	if s.worker == nil {
		return Status{Fault: s.fault != ""}
	}
	return s.worker.Snapshot()
}

func (s *Service) Close() error {
	if s.adminSocket != nil {
		_ = s.adminSocket.Close()
	}
	s.operation.Lock()
	defer s.operation.Unlock()
	s.mu.Lock()
	w := s.worker
	s.worker = nil
	s.mu.Unlock()
	if w != nil {
		_ = w.Close()
	}
	if s.adminPath != "" {
		if err := os.Remove(s.adminPath); err != nil && !errors.Is(err, os.ErrNotExist) {
			return err
		}
	}
	return nil
}

func (s *Service) Admin(ctx context.Context, command string) (string, error) {
	s.operation.Lock()
	defer s.operation.Unlock()
	if err := ctx.Err(); err != nil {
		return "", err
	}
	words := strings.Split(command, " ")
	if len(words) >= 3 && words[0] == "role" && words[1] == "key" {
		words = append([]string{"key"}, words[2:]...)
	}
	if len(words) >= 1 && (words[0] == "key" || words[0] == "identity") {
		reply, err := s.identityCommand(ctx, words)
		if err != nil {
			return "Error: " + err.Error(), err
		}
		return reply, nil
	}
	s.mu.RLock()
	w := s.worker
	s.mu.RUnlock()
	if w == nil {
		return "", errors.New("bot worker unavailable; inspect key bot and retry key bot apply")
	}
	return w.Admin(ctx, command)
}

func (s *Service) identityCommand(ctx context.Context, words []string) (string, error) {
	if len(words) < 2 || words[0] != "key" || words[1] != "bot" {
		return "", errors.New("use key bot [pending|cancel|apply]; stage with key-import bot")
	}
	if len(words) == 2 {
		return "KEY " + s.PublicKey(), nil
	}
	if len(words) != 3 {
		return "", errors.New("key bot requires one operation")
	}
	switch words[2] {
	case "pending":
		id, err := state.BotIdentityChange(s.root, "pending", nil)
		if err != nil {
			return "", err
		}
		return "KEY " + id.String() + " pending; apply required", nil
	case "cancel":
		id, err := state.BotIdentityChange(s.root, "cancel", nil)
		if err != nil {
			return "", err
		}
		return "KEY " + id.String() + " pending cancelled", nil
	case "apply":
		return s.apply(ctx)
	default:
		expanded, err := hex.DecodeString(words[2])
		if err != nil || len(expanded) != 64 {
			return "", errors.New("key bot import requires exactly 128 hex digits of a native expanded key")
		}
		defer clear(expanded)
		id, err := state.BotIdentityChange(s.root, "stage", expanded)
		if errors.Is(err, state.ErrBotIdentityActive) {
			return "KEY " + id.String() + "; already active; no apply required", nil
		}
		if err != nil {
			return "", err
		}
		return "KEY " + id.String() + " pending; apply required", nil
	}
}

func (s *Service) launch(identity meshcore.LocalIdentity, expanded []byte, dormant bool) (*Worker, error) {
	cfg := s.cfg
	cfg.Identity, cfg.Expanded, cfg.Dormant = identity, expanded, dormant
	return startWorker(s.ctx, cfg)
}

func (s *Service) install(w *Worker, id meshcore.LocalIdentity, fault string) {
	s.mu.Lock()
	s.worker, s.key, s.fault = w, id, fault
	s.mu.Unlock()
	if w != nil {
		s.monitor(w)
	}
}

func (s *Service) apply(ctx context.Context) (string, error) {
	nextKey, err := state.PendingBotIdentity(s.root)
	if err != nil {
		s.mu.RLock()
		stopped := s.worker == nil
		s.mu.RUnlock()
		if !stopped {
			return "", err
		}
		// A durable apply with failed activation is retried using the active
		// authority, never by replaying work from the retired worker.
		nextKey, err = state.ExportIdentity(s.root, "bot")
		if err != nil {
			return "", err
		}
	}
	defer clear(nextKey)
	next, err := meshcore.NewLocalIdentityFromExpandedKey(nextKey)
	if err != nil {
		return "", err
	}
	oldKey, err := state.ExportIdentity(s.root, "bot")
	if err != nil {
		return "", err
	}
	defer clear(oldKey)
	old, err := meshcore.NewLocalIdentityFromExpandedKey(oldKey)
	if err != nil {
		return "", err
	}
	s.mu.Lock()
	retired := s.worker
	s.worker, s.fault = nil, ""
	s.mu.Unlock()
	if retired != nil {
		_ = retired.Close()
	}
	restore := func(cause error) (string, error) {
		if retired == nil {
			s.install(nil, old, cause.Error())
			return "", cause
		}
		w, launchErr := s.launch(old, oldKey, false)
		fault := ""
		if launchErr != nil {
			fault = launchErr.Error()
		}
		s.install(w, old, fault)
		return "", errors.Join(cause, launchErr)
	}
	candidate, err := s.launch(next, nextKey, true)
	if err != nil {
		return restore(fmt.Errorf("bot candidate startup failed; active key unchanged: %w", err))
	}
	wait, cancel := context.WithTimeout(ctx, 35*time.Second)
	defer cancel()
	select {
	case <-candidate.ready:
	case <-candidate.ctx.Done():
		_ = candidate.Close()
		return restore(errors.New("bot candidate stopped before READY; active key unchanged"))
	case <-wait.Done():
		_ = candidate.Close()
		return restore(errors.New("bot candidate not READY; active key unchanged"))
	}
	if err := ctx.Err(); err != nil {
		_ = candidate.Close()
		return restore(err)
	}
	if err := candidate.ctx.Err(); err != nil {
		_ = candidate.Close()
		return restore(errors.New("bot candidate stopped before identity commit; active key unchanged"))
	}
	_, err = state.BotIdentityChange(s.root, "apply", nextKey)
	if err != nil {
		_ = candidate.Close()
		if errors.Is(err, state.ErrCommitIndeterminate) {
			// The rename may be durable: never resume either key on RF
			// until the operator retries from the saved authority.
			saved, readErr := state.ExportIdentity(s.root, "bot")
			id := old
			if readErr == nil {
				id, readErr = meshcore.NewLocalIdentityFromExpandedKey(saved)
			}
			clear(saved)
			s.install(nil, id, "bot identity commit durability unknown")
			return "", errors.Join(err, readErr)
		}
		return restore(fmt.Errorf("bot identity commit failed; active key unchanged: %w", err))
	}
	activation, activationCancel := context.WithTimeout(s.ctx, 5*time.Second)
	defer activationCancel()
	queued, err := candidate.activate(activation)
	if err != nil {
		_ = candidate.Close()
		s.install(nil, next, "saved bot identity could not activate")
		return "", errors.New("bot key saved but activation unavailable; inspect key bot and retry key bot apply; no work replayed")
	}
	s.install(candidate, next, "")
	if queued {
		return "KEY " + next.String() + " applied; zero-hop advert queued", nil
	}
	return "KEY " + next.String() + " applied; zero-hop advert not queued; use advert.zerohop", nil
}

func (w *Worker) activate(ctx context.Context) (bool, error) {
	w.mu.Lock()
	w.activating = true
	w.mu.Unlock()
	if !enqueue(ctx, w.send, framed([]byte{0x06})) {
		return false, errors.New("bot activation input unavailable")
	}
	select {
	case queued := <-w.activated:
		return queued, nil
	case <-ctx.Done():
		return false, ctx.Err()
	case <-w.ctx.Done():
		return false, errors.New("bot activation worker stopped")
	}
}
