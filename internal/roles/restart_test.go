package roles

import (
	"bytes"
	"context"
	"encoding/json"
	"errors"
	"path/filepath"
	"strings"
	"testing"
	"time"

	hoststate "meshcore.local/meshcore/internal/state"
)

func TestNativeRebootAdmissionAndRoleClose(t *testing.T) {
	for _, tc := range []struct {
		name  string
		room  bool
		flags byte
	}{
		{"repeater-cli", false, 4},
		{"room-cli", true, 4},
		{"repeater-legacy", false, 0},
	} {
		t.Run(tc.name, func(t *testing.T) {
			cfg := config(t)
			admin, guest := fixedID(2), fixedID(3)
			var service *Service
			admitted := make(chan error, 1)
			closed := make(chan error, 1)
			cfg.RequestRestart = func(ctx context.Context) error {
				if deadline, ok := ctx.Deadline(); !ok || time.Until(deadline) > 5*time.Second {
					err := errors.New("missing bounded restart context")
					admitted <- err
					return err
				}
				if !service.mu.TryRLock() {
					err := errors.New("restart callback held role mutex")
					admitted <- err
					return err
				}
				service.mu.RUnlock()
				raw, err := hoststate.ReadRoleJSON(filepath.Join(cfg.StateDir, "state.json"))
				var disk diskState
				if err == nil {
					err = json.Unmarshal(raw, &disk)
				}
				if err == nil && (disk.Members[admin.Identity.String()] == nil ||
					disk.Members[admin.Identity.String()].LastTimestamp != 2) {
					err = errors.New("restart was requested before command state commit")
				}
				admitted <- err
				if err == nil {
					// Model the parent's role-only lifecycle worker, not a
					// synchronous Close from the admission callback.
					go func() { closed <- service.Close() }()
				}
				return err
			}
			s, r, id := startRole(t, tc.room, cfg)
			service = s
			loggedIn(t, s, r, admin, id, tc.room, 1, "admin")
			loggedIn(t, s, r, guest, id, tc.room, 1, "room")
			r.inject(t, textWire(t, guest, id, 2, 4, "reboot"), false)
			r.inject(t, textWire(t, admin, id, 0, tc.flags, "reboot"), false)
			r.quiet(t)
			if tc.room {
				r.inject(t, textWire(t, guest, id, 3, 0, "reboot"), false)
				if raw := r.next(t); raw[0] != 0x0d {
					t.Fatal("ordinary room text was treated as a restart command")
				}
			}
			select {
			case err := <-admitted:
				t.Fatalf("unauthorized/replayed reboot reached callback: %v", err)
			default:
			}
			r.inject(t, textWire(t, admin, id, 2, tc.flags, "reboot"), false)
			select {
			case err := <-admitted:
				if err != nil {
					t.Fatal(err)
				}
			case <-time.After(time.Second):
				t.Fatal("native reboot admission was delayed or absent")
			}
			select {
			case err := <-closed:
				if err != nil {
					t.Fatal(err)
				}
			case <-time.After(2 * time.Second):
				t.Fatal("asynchronous role Close deadlocked restart admission")
			}
			if tc.flags == 0 {
				raw := r.next(t)
				want := append([]byte{0x0d, 0}, ackProof(append([]byte{2, 0, 0, 0, 0}, []byte("reboot")...), admin.PublicKey())...)
				if !bytes.Equal(raw, want) {
					t.Fatalf("native legacy transport ACK: %x, want %x", raw, want)
				}
				assertWireDelay(t, r, raw, 200*time.Millisecond)
			}
			// CommonCLI::handleCommand calls board.reboot without returning:
			// there is no CLI success response or reply-delay wait.
			r.quiet(t)
		})
	}
}

func TestRebootUnavailableAndRejectedAdmission(t *testing.T) {
	for _, room := range []bool{false, true} {
		for _, rejected := range []bool{false, true} {
			name := "repeater"
			if room {
				name = "room"
			}
			if rejected {
				name += "-rejected"
			} else {
				name += "-unavailable"
			}
			t.Run(name, func(t *testing.T) {
				cfg := config(t)
				failures := make(chan error, 1)
				cfg.ErrorHandler = func(err error) { failures <- err }
				calls := 0
				if rejected {
					cfg.RequestRestart = func(context.Context) error {
						calls++
						return ErrUnsupported
					}
				}
				s, r, id := startRole(t, room, cfg)
				admin := fixedID(2)
				loggedIn(t, s, r, admin, id, room, 1, "admin")
				r.inject(t, textWire(t, admin, id, 2, 4, "reboot"), false)
				raw := r.next(t)
				body := decrypted(t, raw, admin, id)
				want := "Error, role restart unavailable"
				if rejected {
					want = "Error, restart request rejected"
				}
				if string(cstring(body[5:])) != want {
					t.Fatalf("restart falsely succeeded: %x", body)
				}
				delay := 1500 * time.Millisecond
				if room {
					delay = 300 * time.Millisecond
				}
				assertWireDelay(t, r, raw, delay)
				r.inject(t, textWire(t, admin, id, 2, 5, "reboot"), false)
				r.quiet(t)
				r.inject(t, textWire(t, admin, id, 3, 4, "get name"), false)
				body = decrypted(t, r.next(t), admin, id)
				if string(cstring(body[5:])) != "> Host" {
					t.Fatal("rejected restart stopped the role")
				}
				if !room && !rejected {
					r.inject(t, textWire(t, admin, id, 4, 0, "reboot"), false)
					ack := r.next(t)
					if ack[0] != 0x0d {
						t.Fatal("missing native legacy transport ACK")
					}
					assertWireDelay(t, r, ack, 200*time.Millisecond)
					raw := r.next(t)
					body := decrypted(t, raw, admin, id)
					if string(cstring(body[5:])) != "Error, role restart unavailable" {
						t.Fatal("unsupported legacy restart falsely succeeded")
					}
					assertWireDelay(t, r, raw, 1500*time.Millisecond)
				}
				if rejected {
					if calls != 1 {
						t.Fatalf("retry repeated restart admission: %d", calls)
					}
					select {
					case err := <-failures:
						if !errors.Is(err, ErrUnsupported) {
							t.Fatal(err)
						}
					default:
						t.Fatal("restart admission failure was not reported")
					}
				}
			})
		}
	}
}

func TestRebootWaitsForDurableCommit(t *testing.T) {
	cfg := config(t)
	calls := make(chan struct{}, 1)
	cfg.RequestRestart = func(context.Context) error { calls <- struct{}{}; return nil }
	failures := make(chan error, 1)
	cfg.ErrorHandler = func(err error) { failures <- err }
	r := newWireRadio()
	id, admin := fixedID(1), fixedID(2)
	s, err := NewRepeater(id, r, cfg)
	if err != nil {
		t.Fatal(err)
	}
	t.Cleanup(func() { _ = s.Close() })
	r.next(t)
	loggedIn(t, s, r, admin, id, false, 1, "admin")
	s.mu.Lock()
	s.writeJSON = func(string, any) error { return errors.New("injected role commit failure") }
	s.mu.Unlock()
	r.inject(t, textWire(t, admin, id, 2, 4, "reboot"), false)
	select {
	case err := <-failures:
		if !strings.Contains(err.Error(), "role commit failure") {
			t.Fatal(err)
		}
	case <-time.After(time.Second):
		t.Fatal("commit failure not reported")
	}
	r.quiet(t)
	select {
	case <-calls:
		t.Fatal("failed commit admitted a restart")
	default:
	}
}

func TestSuccessfulCommitClearsPreviousAbortedWriteBeforeRoleRestart(t *testing.T) {
	cfg := config(t)
	admitted := make(chan struct{}, 1)
	cfg.RequestRestart = func(context.Context) error {
		admitted <- struct{}{}
		return nil
	}
	failures := make(chan error, 1)
	cfg.ErrorHandler = func(err error) { failures <- err }
	s, r, id := startRole(t, false, cfg)
	admin := fixedID(2)
	loggedIn(t, s, r, admin, id, false, 1, "admin")
	s.mu.Lock()
	write := s.writeJSON
	s.writeJSON = func(string, any) error { return errors.New("injected aborted write") }
	s.mu.Unlock()
	r.inject(t, textWire(t, admin, id, 2, 4, "get name"), false)
	select {
	case err := <-failures:
		if !strings.Contains(err.Error(), "injected aborted write") {
			t.Fatal(err)
		}
	case <-time.After(time.Second):
		t.Fatal("aborted write not reported")
	}
	r.quiet(t)
	s.mu.Lock()
	s.writeJSON = write
	s.mu.Unlock()
	r.inject(t, textWire(t, admin, id, 2, 4, "reboot"), false)
	select {
	case <-admitted:
	case <-time.After(time.Second):
		t.Fatal("recovered role did not admit restart")
	}
	if err := s.Close(); err != nil {
		t.Fatalf("successful reboot commit retained a rolled-back write failure: %v", err)
	}
}
