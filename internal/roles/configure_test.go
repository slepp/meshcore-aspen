package roles

import (
	"context"
	"encoding/json"
	"errors"
	"path/filepath"
	"testing"

	hoststate "meshcore.local/meshcore/internal/state"
)

func TestConfigureNameUsesCommandPersistenceAndRoleIsolation(t *testing.T) {
	for _, room := range []bool{false, true} {
		t.Run(map[bool]string{false: "repeater", true: "room"}[room], func(t *testing.T) {
			cfg := config(t)
			s, radio, id := startRole(t, room, cfg)
			ctx := context.Background()
			if reply, err := s.ConfigureName(ctx, "Configured"); err != nil || reply != "OK" {
				t.Fatalf("name command: %q %v", reply, err)
			}
			if s.Name() != "Configured" || len(s.AccessList()) != 0 {
				t.Fatal("name command did not update name or granted RF privileges")
			}
			var saved diskState
			data, err := hoststate.ReadRoleJSON(filepath.Join(cfg.StateDir, "state.json"))
			if err != nil {
				t.Fatal(err)
			}
			if err := json.Unmarshal(data, &saved); err != nil {
				t.Fatal(err)
			}
			if saved.Name != "Configured" || saved.Preferences.LocalAdvertSeconds != 0 ||
				s.policySnapshot.Load().LocalAdvertSeconds != 0 {
				t.Fatal("name command bypassed native preference commit/publication")
			}
			radio.quiet(t)
			guest := fixedID(2)
			radio.inject(t, loginWire(t, room, guest, id, 1, 0, "room", 2, 0, nil), true)
			radio.next(t)
			radio.inject(t, textWire(t, guest, id, 2, 4, "set name Unauthorized"), true)
			radio.quiet(t)
			if s.Name() != "Configured" {
				t.Fatal("host owner name change granted guest RF administration")
			}
			if err := s.Close(); err != nil {
				t.Fatal(err)
			}
			restartedRadio := newWireRadio()
			var restarted *Service
			if room {
				restarted, err = NewRoom(id, restartedRadio, cfg)
			} else {
				restarted, err = NewRepeater(id, restartedRadio, cfg)
			}
			if err != nil {
				t.Fatal(err)
			}
			t.Cleanup(func() {
				if err := restarted.Close(); err != nil {
					t.Error(err)
				}
			})
			if restarted.Name() != "Configured" {
				t.Fatal("name change did not survive role restart")
			}
			if reply, err := s.ConfigureName(ctx, "Closed"); !errors.Is(err, ErrRoleStopped) || reply != "" {
				t.Fatalf("closed role admitted mutation: %q %v", reply, err)
			}
		})
	}
}

func TestConfigureNameValidationAndRegionOwner(t *testing.T) {
	s, _, _ := startRole(t, false, config(t))
	for _, name := range []string{"", "01234567890123456789012345678901é", "New\nreboot", "New\x00", string([]byte{0xff})} {
		if _, err := s.ConfigureName(context.Background(), name); err == nil {
			t.Fatalf("unsafe name admitted: %q", name)
		}
	}
	if reply, err := s.ConfigureName(context.Background(), "Invalid:name"); err != nil ||
		reply != "Error, bad chars or name length" || s.Name() != "Host" {
		t.Fatalf("native name validation changed: %q %v name=%q", reply, err, s.Name())
	}
	s.mu.Lock()
	s.regionLoad = &regionLoad{owner: fixedID(2).Identity.String()}
	s.mu.Unlock()
	if reply, err := s.ConfigureName(context.Background(), "No takeover"); err != nil ||
		reply != "Err - region load owned by another administrator" || s.Name() != "Host" {
		t.Fatalf("local adapter bypassed region owner: %q %v", reply, err)
	}
}

func TestConfigureNameAdmissionAndCancellation(t *testing.T) {
	ctx, cancel := context.WithCancel(context.Background())
	cancel()
	s := &Service{queue: make(chan event, 1), done: make(chan struct{})}
	if _, err := s.ConfigureName(ctx, "Canceled"); !errors.Is(err, context.Canceled) || len(s.queue) != 0 {
		t.Fatalf("canceled call admitted: %v", err)
	}
	s.queue <- event{}
	if _, err := s.ConfigureName(context.Background(), "Full"); !errors.Is(err, ErrQueueFull) {
		t.Fatalf("full queue error: %v", err)
	}
	<-s.queue
	ctx, cancel = context.WithCancel(context.Background())
	result := make(chan error, 1)
	go func() {
		_, err := s.ConfigureName(ctx, "Canceled after admission")
		result <- err
	}()
	e := <-s.queue
	cancel()
	if err := <-result; !errors.Is(err, ErrNameChangeUnknown) || !errors.Is(err, context.Canceled) {
		t.Fatalf("admitted cancellation lost uncertain classification: %v", err)
	}
	s.process(e)
	if completion := <-e.ownerCommand.result; !errors.Is(completion.err, context.Canceled) {
		t.Fatalf("canceled queued operation executed: %+v", completion)
	}
}

func TestConfigureNamePersistenceFailureDoesNotReportSuccess(t *testing.T) {
	s, _, _ := startRole(t, false, config(t))
	failure := errors.New("role persistence aborted")
	s.mu.Lock()
	writer := s.writeJSON
	s.writeJSON = func(string, any) error { return failure }
	s.mu.Unlock()
	reply, err := s.ConfigureName(context.Background(), "Not committed")
	s.mu.Lock()
	s.writeJSON, s.lastErr = writer, nil
	name := s.state.Name
	s.mu.Unlock()
	if !errors.Is(err, failure) || reply != "" || name != "Host" {
		t.Fatalf("persistence failure contract: reply=%q error=%v name=%q", reply, err, name)
	}
	if reply, err := s.ConfigureName(context.Background(), "Recovered"); err != nil || reply != "OK" {
		t.Fatalf("known-aborted failure prevented recovery: %q %v", reply, err)
	}
	s.mu.Lock()
	s.restartPending = true
	s.mu.Unlock()
	if _, err := s.ConfigureName(context.Background(), "Restarting"); !errors.Is(err, ErrRoleStopped) {
		t.Fatalf("restarting role admitted mutation: %v", err)
	}
}

func TestConfigureNameIndeterminateCommitFailsClosed(t *testing.T) {
	s, err := NewRepeater(fixedID(1), newWireRadio(), config(t))
	if err != nil {
		t.Fatal(err)
	}
	t.Cleanup(func() {
		if err := s.Close(); !errors.Is(err, ErrCommitUncertain) {
			t.Errorf("terminal close classification: %v", err)
		}
	})
	writes := 0
	s.mu.Lock()
	s.writeJSON = func(string, any) error {
		writes++
		return hoststate.ErrCommitIndeterminate
	}
	s.mu.Unlock()
	if reply, err := s.ConfigureName(context.Background(), "Uncertain"); reply != "" ||
		!errors.Is(err, ErrCommitUncertain) {
		t.Fatalf("indeterminate name change reported success: %q %v", reply, err)
	}
	if reply, err := s.ConfigureName(context.Background(), "Denied"); reply != "" ||
		!errors.Is(err, ErrCommitUncertain) {
		t.Fatalf("terminal role admitted another name change: %q %v", reply, err)
	}
	s.mu.RLock()
	count := writes
	s.mu.RUnlock()
	if count != 1 || !errors.Is(s.ApplicationError(), ErrCommitUncertain) {
		t.Fatalf("terminal commit retried or lost classification: writes=%d error=%v", count, s.ApplicationError())
	}
}
