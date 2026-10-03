package roles

import (
	"context"
	"encoding/json"
	"errors"
	"os"
	"path/filepath"
	"strings"
	"testing"
	"time"
)

type sourceRadio struct {
	*wireRadio
	apply func(context.Context, float64) error
}

func (r *sourceRadio) SetSourceAirtimeFactor(ctx context.Context, factor float64) error {
	return r.apply(ctx, factor)
}

func TestRuntimeAirtimeDomainAndPreferenceReloadProfiles(t *testing.T) {
	for _, tc := range []struct {
		name       string
		profile    PreferenceProfile
		command    string
		reply      string
		runtime    float64
		reloaded   float64
		legacy     bool
		override99 bool
	}{
		{"native-af99", NativePreferences, "set dutycycle 1", "OK - 1.0%", 99, 9, false, false},
		{"durable-af99", DurableHostPreferences, "set af 99", "OK", 99, 99, false, false},
		{"native-zero", NativePreferences, "set af 0", "OK", 0, 0, false, false},
		{"durable-zero", DurableHostPreferences, "set dutycycle 100", "OK - 100.0%", 0, 0, false, false},
		{"legacy-host", "", "set dutycycle 1", "OK - 1.0%", 99, 99, true, false},
		{"explicit-override", NativePreferences, "set dutycycle 1", "OK - 1.0%", 99, 99, false, true},
	} {
		t.Run(tc.name, func(t *testing.T) {
			cfg := config(t)
			cfg.PreferenceProfile = tc.profile
			applied := make(chan float64, 8)
			nextFactor := func() float64 {
				t.Helper()
				select {
				case factor := <-applied:
					return factor
				case <-time.After(time.Second):
					t.Fatal("source policy was not applied")
					return 0
				}
			}
			id, admin := fixedID(1), fixedID(2)
			open := func() (*Service, *sourceRadio) {
				t.Helper()
				r := &sourceRadio{wireRadio: newWireRadio()}
				r.apply = func(ctx context.Context, factor float64) error {
					if _, ok := ctx.Deadline(); !ok {
						return errors.New("source policy has no deadline")
					}
					raw, err := os.ReadFile(filepath.Join(cfg.StateDir, "state.json"))
					if err != nil {
						return err
					}
					var disk diskState
					if err := json.Unmarshal(raw, &disk); err != nil {
						return err
					}
					if float64(disk.Preferences.AirtimeFactor) != factor {
						return errors.New("persisted source factor differs from device request")
					}
					applied <- factor
					return nil
				}
				s, err := NewRepeater(id, r, cfg)
				if err != nil {
					t.Fatal(err)
				}
				t.Cleanup(func() {
					if err := s.Close(); err != nil {
						t.Error(err)
					}
				})
				r.next(t)
				return s, r
			}
			s, r := open()
			if factor := nextFactor(); factor != 1 {
				t.Fatalf("startup source factor %g, want 1", factor)
			}
			loggedIn(t, s, r.wireRadio, admin, id, false, 1, "admin")
			r.inject(t, textWire(t, admin, id, 2, 4, tc.command), false)
			body := decrypted(t, r.next(t), admin, id)
			if got := string(cstring(body[5:])); got != tc.reply {
				t.Fatalf("runtime command reply %q, want %q", got, tc.reply)
			}
			if factor := nextFactor(); factor != tc.runtime {
				t.Fatalf("runtime source factor %g, want %g", factor, tc.runtime)
			}
			for i, invalid := range []string{"-1", "NaN", "+Inf", "-Inf"} {
				r.inject(t, textWire(t, admin, id, uint32(i+3), 4, "set af "+invalid), false)
				body := decrypted(t, r.next(t), admin, id)
				if got := string(cstring(body[5:])); got != "Error, invalid airtime value" {
					t.Fatalf("invalid factor %s got reply %q", invalid, got)
				}
				select {
				case factor := <-applied:
					t.Fatalf("invalid input changed device factor to %g", factor)
				default:
				}
			}
			if err := s.Close(); err != nil {
				t.Fatal(err)
			}
			path := filepath.Join(cfg.StateDir, "state.json")
			raw, err := os.ReadFile(path)
			if err != nil {
				t.Fatal(err)
			}
			var disk diskState
			if err := json.Unmarshal(raw, &disk); err != nil || float64(disk.Preferences.AirtimeFactor) != tc.runtime {
				t.Fatalf("runtime value was truncated at persistence: %g (%v)", disk.Preferences.AirtimeFactor, err)
			}
			if tc.legacy {
				disk.PreferenceProfile = ""
				raw, err = json.Marshal(disk)
				if err != nil {
					t.Fatal(err)
				}
				if err := os.WriteFile(path, raw, 0600); err != nil {
					t.Fatal(err)
				}
			}
			if tc.override99 {
				factor := float32(99)
				cfg.Policy.AirtimeFactor = &factor
			}
			restored, _ := open()
			if factor := nextFactor(); factor != tc.reloaded {
				t.Fatalf("reloaded source factor %g, want %g", factor, tc.reloaded)
			}
			restored.mu.RLock()
			profile := restored.state.PreferenceProfile
			restored.mu.RUnlock()
			if tc.legacy && profile != DurableHostPreferences {
				t.Fatal("legacy host preferences were not assigned the durable extension")
			}
		})
	}
}

func TestSourcePolicyCommitsBeforeDeviceApplicationAndFailsClosed(t *testing.T) {
	cfg := config(t)
	errs := make(chan error, 8)
	cfg.ErrorHandler = func(err error) { errs <- err }
	r := &sourceRadio{wireRadio: newWireRadio()}
	r.apply = func(ctx context.Context, factor float64) error {
		if _, ok := ctx.Deadline(); !ok {
			return errors.New("missing source-policy deadline")
		}
		raw, err := os.ReadFile(filepath.Join(cfg.StateDir, "state.json"))
		if err != nil {
			return err
		}
		var disk diskState
		if err := json.Unmarshal(raw, &disk); err != nil {
			return err
		}
		if float64(disk.Preferences.AirtimeFactor) != factor {
			return errors.New("device policy changed before durable state")
		}
		if factor == 3 {
			return errors.New("injected source-policy transport failure")
		}
		return nil
	}
	id, admin := fixedID(1), fixedID(2)
	s, err := NewRepeater(id, r, cfg)
	if err != nil {
		t.Fatal(err)
	}
	t.Cleanup(func() { _ = s.Close() })
	r.next(t)
	loggedIn(t, s, r.wireRadio, admin, id, false, 1, "admin")
	r.inject(t, textWire(t, admin, id, 2, 4, "set af 2"), false)
	body := decrypted(t, r.next(t), admin, id)
	if string(cstring(body[5:])) != "OK" {
		t.Fatalf("source-policy response %x", body)
	}
	r.inject(t, textWire(t, admin, id, 3, 4, "set af 3"), false)
	select {
	case err := <-errs:
		if !errors.Is(err, ErrCommitUncertain) || !strings.Contains(err.Error(), "source-policy transport") {
			t.Fatal(err)
		}
	case <-time.After(time.Second):
		t.Fatal("device policy failure not surfaced")
	}
	r.quiet(t)
	s.mu.RLock()
	failed, retained := s.commitUncertain, s.state.Preferences.AirtimeFactor
	s.mu.RUnlock()
	if !failed || retained != 3 {
		t.Fatal("device policy failure pretended to roll back committed state")
	}
	if err := s.Close(); !errors.Is(err, ErrCommitUncertain) {
		t.Fatalf("Close lost device-policy failure: %v", err)
	}
}
