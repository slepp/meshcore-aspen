package roles

import (
	"bytes"
	"context"
	"encoding/hex"
	"encoding/json"
	"errors"
	"fmt"
	"os"
	"path/filepath"
	"strings"
	"testing"
	"time"

	hoststate "meshcore.local/meshcore/internal/state"
)

func TestNativeIdentityStagingAndDeferredActivation(t *testing.T) {
	for _, tc := range []struct {
		name  string
		room  bool
		flags byte
	}{
		{"repeater", false, 4}, {"room", true, 4}, {"repeater-legacy", false, 0},
	} {
		t.Run(tc.name, func(t *testing.T) {
			cfg := config(t)
			root := cfg.StateDir
			role := "repeater"
			if tc.room {
				role = "room"
			}
			cfg.StateDir = filepath.Join(root, role)
			if err := os.Mkdir(cfg.StateDir, 0700); err != nil {
				t.Fatal(err)
			}
			active, next := fixedID(1), fixedID(4)
			seed := active.Seed()
			if err := os.WriteFile(filepath.Join(cfg.StateDir, "identity.seed"), seed[:], 0600); err != nil {
				t.Fatal(err)
			}
			key := fixtureExpandedKey(t, next)
			command := "set prv.key " + hex.EncodeToString(key)
			admin, guest := fixedID(2), fixedID(3)
			var service *Service
			var borrowed []byte
			calls := 0
			entered, release := make(chan struct{}), make(chan struct{})
			released := false
			defer func() {
				if !released {
					close(release)
				}
			}()
			restarts := make(chan struct{}, 1)
			cfg.RequestRestart = func(context.Context) error { restarts <- struct{}{}; return nil }
			cfg.StageIdentity = func(ctx context.Context, input []byte) error {
				calls++
				borrowed = input
				if !bytes.Equal(input, key) {
					return errors.New("wrong staging input")
				}
				if deadline, ok := ctx.Deadline(); !ok || time.Until(deadline) > 5*time.Second {
					return errors.New("missing staging deadline")
				}
				if !service.mu.TryRLock() {
					return errors.New("staging callback held role mutex")
				}
				service.mu.RUnlock()
				raw, err := hoststate.ReadRoleJSON(filepath.Join(cfg.StateDir, "state.json"))
				var committed diskState
				if err == nil {
					err = json.Unmarshal(raw, &committed)
				}
				if err != nil {
					return err
				}
				if committed.Members[admin.Identity.String()].LastTimestamp != 10 {
					return errors.New("staging preceded command-state commit")
				}
				close(entered)
				<-release
				return hoststate.StageRoleIdentity(ctx, root, role, input)
			}
			s, r, id := startRole(t, tc.room, cfg)
			service = s
			loggedIn(t, s, r, admin, id, tc.room, 1, "admin")
			loggedIn(t, s, r, guest, id, tc.room, 1, "room")
			r.inject(t, textWire(t, guest, id, 2, 4, command), false)
			r.quiet(t)
			unclamped := bytes.Clone(key)
			unclamped[0] |= 1
			for i, bad := range []string{"00", strings.Repeat("z", 128), strings.Repeat("0", 128), hex.EncodeToString(unclamped), hex.EncodeToString(key) + "00"} {
				r.inject(t, textWire(t, admin, id, uint32(i+2), 4, "set prv.key "+bad), false)
				body := decrypted(t, r.next(t), admin, id)
				if string(cstring(body[5:])) != "Error, bad key" {
					t.Fatal("malformed native key did not receive the exact error")
				}
			}
			if calls != 0 {
				t.Fatal("unauthorized or malformed key reached staging")
			}
			r.inject(t, textWire(t, admin, id, 10, tc.flags, command), false)
			if tc.flags == 0 {
				ack := r.next(t)
				if ack[0] != 0x0d {
					t.Fatal("missing legacy transport ACK")
				}
				assertWireDelay(t, r, ack, 200*time.Millisecond)
			}
			select {
			case <-entered:
			case <-time.After(time.Second):
				t.Fatal("staging callback not reached")
			}
			r.quiet(t)
			close(release)
			released = true
			raw := r.next(t)
			body := decrypted(t, raw, admin, id)
			want := "OK, reboot to apply! New pubkey: " + strings.ToUpper(next.Identity.String())
			if string(cstring(body[5:])) != want || body[4] != 4 {
				t.Fatal("staging did not return the exact native public-key reply")
			}
			delay := 1500 * time.Millisecond
			if tc.room {
				delay = 300 * time.Millisecond
			}
			assertWireDelay(t, r, raw, delay)
			if calls != 1 || !bytes.Equal(borrowed, make([]byte, 64)) {
				t.Fatal("staging callback repeated or borrowed key buffer was retained")
			}
			r.inject(t, textWire(t, admin, id, 10, tc.flags|1, command), false)
			if tc.flags == 0 {
				assertWireDelay(t, r, r.next(t), 200*time.Millisecond)
			}
			r.quiet(t)
			if calls != 1 {
				t.Fatal("replayed command repeated identity staging")
			}
			for i, text := range []string{"set name Retained", "get public.key", "get prv.key", "erase"} {
				r.inject(t, textWire(t, admin, id, uint32(11+i), 4, text), false)
				body := decrypted(t, r.next(t), admin, id)
				reply := string(cstring(body[5:]))
				if i == 0 && reply != "OK" || i == 1 && reply != "> "+strings.ToUpper(active.Identity.String()) ||
					i >= 2 && reply != "Err - unsupported by host role" {
					t.Fatal("active identity changed or remote export/erase became available")
				}
			}
			r.inject(t, textWire(t, admin, id, 15, 4, "advert.zerohop"), false)
			advert := r.next(t)
			if !bytes.Equal(advert[2:34], active.PublicKeyBytes()) {
				t.Fatal("staged identity became active in advertisements")
			}
			r.next(t)
			if tc.room {
				r.inject(t, textWire(t, guest, id, 3, 0, "post after staging"), false)
				r.next(t)
			}
			current, err := hoststate.Identity(root, role)
			if err != nil || current.PublicKey() != active.PublicKey() {
				t.Fatal("staging changed active persisted identity")
			}
			select {
			case <-restarts:
				t.Fatal("staging automatically requested a restart")
			default:
			}
			if err := s.Close(); err != nil {
				t.Fatal(err)
			}
			if err := hoststate.ActivateRoleIdentity(root, role, RebindStateIdentity); err != nil {
				t.Fatal(err)
			}
			activated, err := hoststate.Identity(root, role)
			if err != nil || activated.PublicKey() != next.PublicKey() {
				t.Fatalf("ordinary state saves lost pending identity: %v", err)
			}
			newRadio := newWireRadio()
			var restarted *Service
			if tc.room {
				restarted, err = NewRoom(activated, newRadio, cfg)
			} else {
				restarted, err = NewRepeater(activated, newRadio, cfg)
			}
			if err != nil {
				t.Fatal(err)
			}
			t.Cleanup(func() { _ = restarted.Close() })
			newAdvert := newRadio.next(t)
			if !bytes.Equal(newAdvert[2:34], next.PublicKeyBytes()) {
				t.Fatal("retired role did not activate the staged identity")
			}
			newRadio.inject(t, textWire(t, admin, activated, 16, 4, "get name"), false)
			body = decrypted(t, newRadio.next(t), admin, activated)
			if string(cstring(body[5:])) != "> Retained" {
				t.Fatal("activation lost role state or encrypted administrator access")
			}
			restarted.mu.RLock()
			historyOK := !tc.room || len(restarted.state.History) == 1 && restarted.state.History[0].Text == "post after staging"
			restarted.mu.RUnlock()
			if !historyOK {
				t.Fatal("activation lost a post committed after staging")
			}
		})
	}
}

func TestIdentityStagingFailuresNeverExposePrivateInput(t *testing.T) {
	for _, outcome := range []string{"disabled", "rejected", "indeterminate", "commit-before", "commit-after"} {
		t.Run(outcome, func(t *testing.T) {
			cfg := config(t)
			key := fixtureExpandedKey(t, fixedID(4))
			command := "set prv.key " + hex.EncodeToString(key)
			failures := make(chan error, 8)
			cfg.ErrorHandler = func(err error) { failures <- err }
			var service *Service
			calls := 0
			if outcome != "disabled" {
				cfg.StageIdentity = func(context.Context, []byte) error {
					calls++
					switch outcome {
					case "indeterminate":
						return errors.Join(hoststate.ErrCommitIndeterminate, fmt.Errorf("private input: %s", command))
					case "commit-after":
						service.mu.Lock()
						service.writeJSON = func(string, any) error { return errors.New("injected reply commit failure") }
						service.mu.Unlock()
						return nil
					default:
						return fmt.Errorf("private input: %s", command)
					}
				}
			}
			r := newWireRadio()
			id, admin := fixedID(1), fixedID(2)
			s, err := NewRepeater(id, r, cfg)
			if err != nil {
				t.Fatal(err)
			}
			service = s
			t.Cleanup(func() { _ = s.Close() })
			r.next(t)
			loggedIn(t, s, r, admin, id, false, 1, "admin")
			if outcome == "commit-before" {
				s.mu.Lock()
				s.writeJSON = func(string, any) error { return errors.New("injected command commit failure") }
				s.mu.Unlock()
			}
			r.inject(t, textWire(t, admin, id, 2, 4, command), false)
			if outcome == "disabled" || outcome == "rejected" {
				body := decrypted(t, r.next(t), admin, id)
				want := "Error, key import disabled"
				if outcome == "rejected" {
					want = "Error, key import failed"
				}
				if string(cstring(body[5:])) != want {
					t.Fatal("failed staging returned success or echoed private input")
				}
			} else {
				r.quiet(t)
			}
			if outcome != "disabled" {
				select {
				case err := <-failures:
					if strings.Contains(err.Error(), hex.EncodeToString(key)) || strings.Contains(err.Error(), "set prv.key") {
						t.Fatal("private input leaked into error reporting")
					}
					if outcome == "indeterminate" && !errors.Is(err, hoststate.ErrCommitIndeterminate) {
						t.Fatal("indeterminate staging lost its classification")
					}
				case <-time.After(time.Second):
					t.Fatal("staging failure was not reported")
				}
			}
			if outcome == "commit-before" || outcome == "disabled" {
				if calls != 0 {
					t.Fatal("staging ran without a committed enabled command")
				}
			} else if calls != 1 {
				t.Fatal("staging callback count differs")
			}
			if outcome == "indeterminate" {
				r.inject(t, textWire(t, admin, id, 3, 4, "get name"), false)
				r.quiet(t)
				if err := s.Close(); !errors.Is(err, hoststate.ErrCommitIndeterminate) {
					t.Fatal("indeterminate staging did not fail closed")
				}
			}
		})
	}
}
