package roles

import (
	"bytes"
	"encoding/json"
	"errors"
	"os"
	"path/filepath"
	"strings"
	"testing"
	"time"

	hoststate "meshcore.local/meshcore/internal/state"
)

func assertWirePriority(t *testing.T, r *wireRadio, raw []byte, want uint8) {
	t.Helper()
	r.mu.Lock()
	defer r.mu.Unlock()
	for i := len(r.jobs) - 1; i >= 0; i-- {
		if bytes.Equal(r.jobs[i].wire, raw) {
			if r.jobs[i].priority != want {
				t.Fatalf("priority %d, want %d", r.jobs[i].priority, want)
			}
			return
		}
	}
	t.Fatal("packet bypassed scheduled submission")
}

func TestNativeACLResponseCapacityAndLocalView(t *testing.T) {
	cfg := config(t)
	failures := make(chan error, 1)
	cfg.ErrorHandler = func(err error) { failures <- err }
	s, r, id := startRole(t, false, cfg)
	admin := fixedID(2)
	loggedIn(t, s, r, admin, id, false, 1, "admin")
	want := append([]byte{2, 0, 0, 0}, admin.PublicKeyBytes()[:6]...)
	want = append(want, 3)
	s.mu.Lock()
	s.state.Members[admin.Identity.String()].KnownPath = true
	for seed := byte(3); seed <= 24; seed++ {
		client := fixedID(seed)
		m, err := s.putMember(client.Identity)
		if err != nil {
			s.mu.Unlock()
			t.Fatal(err)
		}
		m.Permissions = 0x81
		want = append(want, client.PublicKeyBytes()[:6]...)
		want = append(want, 0x81)
	}
	s.mu.Unlock()

	// MyMesh::handleRequest allows 23 entries (165 plaintext bytes).
	// Mesh::createDatagram permits this direct reply, but not 24 entries.
	r.inject(t, peerWire(t, 0, admin, id, []byte{2, 0, 0, 0, 5, 0, 0}), false)
	raw := r.next(t)
	if raw[0] != 0x06 || raw[1] != 0 {
		t.Fatalf("ACL response did not use the learned zero-hop path: %x", raw)
	}
	body := decrypted(t, raw, admin, id)
	if len(body) < len(want) || !bytes.Equal(body[:len(want)], want) ||
		!bytes.Equal(body[len(want):], make([]byte, len(body)-len(want))) {
		t.Fatalf("native 23-entry ACL response: %x, want %x padded", body, want)
	}

	s.mu.Lock()
	m, err := s.putMember(fixedID(25).Identity)
	if err == nil {
		m.Permissions = 0x83
	}
	s.mu.Unlock()
	if err != nil {
		t.Fatal(err)
	}
	r.inject(t, peerWire(t, 0, admin, id, []byte{3, 0, 0, 0, 5, 0, 0}), false)
	select {
	case err := <-failures:
		if !strings.Contains(err.Error(), "native plaintext limit") {
			t.Fatal(err)
		}
	case <-time.After(2 * time.Second):
		t.Fatal("oversize native ACL response was not reported")
	}
	r.quiet(t)
	entries := s.AccessList()
	if len(entries) != 24 || entries[0].PublicKey != admin.PublicKey() ||
		entries[23].PublicKey != fixedID(25).PublicKey() || entries[23].Permissions != 0x83 {
		t.Fatalf("local ACL view omitted entries or permission bits: %+v", entries)
	}
	entries[0].Permissions = 0
	if s.AccessList()[0].Permissions != 3 {
		t.Fatal("local ACL view aliases live permissions")
	}
}

func TestNativePasswordByteTruncationSurvivesRestart(t *testing.T) {
	for _, command := range []string{"password ", "set guest.password "} {
		t.Run(strings.TrimSpace(command), func(t *testing.T) {
			cfg := config(t)
			s, r, id := startRole(t, true, cfg)
			admin := fixedID(2)
			loggedIn(t, s, r, admin, id, true, 1, "admin")
			// Native strncpy keeps 15 bytes even when the last UTF-8 code point splits.
			input := strings.Repeat("x", 14) + string([]byte{0xc3, 0xa9})
			r.inject(t, textWire(t, admin, id, 2, 4, command+input), false)
			body := decrypted(t, r.next(t), admin, id)
			wantReply := "OK"
			if command == "password " {
				wantReply = "password now: " + input[:15]
			}
			if len(body) < 5+len(wantReply) || body[4] != 4 || string(body[5:5+len(wantReply)]) != wantReply {
				t.Fatalf("native password-management response: %x", body)
			}
			s.mu.RLock()
			localAdvertSeconds := s.state.Preferences.LocalAdvertSeconds
			s.mu.RUnlock()
			if localAdvertSeconds != 0 {
				t.Errorf("CommonCLI::savePrefs did not disable initial two-minute adverts: %d", localAdvertSeconds)
			}
			if err := s.Close(); err != nil {
				t.Fatal(err)
			}
			restored, radio, _ := startRole(t, true, cfg)
			client := fixedID(3)
			radio.inject(t, loginWire(t, true, client, id, 3, 0, input[:15], 2, 0, nil), false)
			body = decrypted(t, radio.next(t), client, id)
			wantAdmin, wantPermission := byte(0), byte(2)
			if command == "password " {
				wantAdmin, wantPermission = 1, 3
			}
			if len(body) < 13 || body[4] != 0 || body[6] != wantAdmin || body[7] != wantPermission {
				t.Fatalf("byte-preserving password login after restart: %x", body)
			}
			radio.inject(t, textWire(t, admin, id, 4, 4, command+"replacement"), false)
			body = decrypted(t, radio.next(t), admin, id)
			if len(body) < 7 || body[4] != 4 || (string(body[5:7]) != "OK" && !bytes.HasPrefix(body[5:], []byte("password now: replacement"))) {
				t.Fatalf("password replacement after binary-state restore: %x", body)
			}
			if err := restored.Close(); err != nil {
				t.Fatal(err)
			}
			_, finalRadio, _ := startRole(t, true, cfg)
			client = fixedID(4)
			finalRadio.inject(t, loginWire(t, true, client, id, 5, 0, "replacement", 2, 0, nil), false)
			body = decrypted(t, finalRadio.next(t), client, id)
			if len(body) < 13 || body[4] != 0 || body[6] != wantAdmin || body[7] != wantPermission {
				t.Fatalf("replacement did not supersede binary password state: %x", body)
			}
		})
	}
}

// Source oracles: ClientACL.cpp::putClient/save and the room/repeater
// MyMesh.cpp login handlers. These are source-derived expectations, not an
// executable-native oracle claim.
func TestNativeACLPermissionBitsAndBlankLogin(t *testing.T) {
	cfg := config(t)
	cfg.Retention = NativeRetention
	s, r, id := startRole(t, true, cfg)
	client := fixedID(2)
	loggedIn(t, s, r, client, id, true, 20, "room")
	s.mu.Lock()
	m := s.state.Members[client.Identity.String()]
	m.Permissions, m.SyncSince = 0x82, 15
	s.mu.Unlock()
	r.inject(t, loginWire(t, true, client, id, 21, 7, "admin", 2, 0, nil), false)
	body := decrypted(t, r.next(t), client, id)
	if !bytes.Equal(body[4:8], []byte{0, 0, 1, 0x83}) {
		t.Fatalf("password login erased permission bits: %x", body)
	}
	r.inject(t, loginWire(t, true, client, id, 0, 999, "", 2, 0, nil), false)
	body = decrypted(t, r.next(t), client, id)
	if body[7] != 0x83 {
		t.Fatalf("blank ACL login failed: %x", body)
	}
	s.mu.Lock()
	if m.LastTimestamp != 21 || m.SyncSince != 7 {
		t.Errorf("blank ACL login mutated replay/cursor: %d/%d", m.LastTimestamp, m.SyncSince)
	}
	m.Permissions = 0x81
	s.mu.Unlock()
	r.inject(t, textWire(t, client, id, 22, 0, "not allowed"), false)
	r.quiet(t)
	s.mu.RLock()
	defer s.mu.RUnlock()
	if len(s.state.History) != 0 {
		t.Fatal("READ_ONLY member posted")
	}
}

func TestNativeACLAdmissionAndRoleFilters(t *testing.T) {
	for _, room := range []bool{false, true} {
		name := "repeater"
		if room {
			name = "room"
		}
		t.Run(name, func(t *testing.T) {
			cfg := config(t)
			cfg.Retention, cfg.MaxMembers = NativeRetention, 3
			s, r, id := startRole(t, room, cfg)
			admin, old, recent, incoming := fixedID(2), fixedID(3), fixedID(4), fixedID(5)
			loggedIn(t, s, r, admin, id, room, 1, "admin")
			s.mu.Lock()
			a, err := s.putMember(old.Identity)
			if err != nil {
				s.mu.Unlock()
				t.Fatal(err)
			}
			a.Permissions, a.LastActivity = 0x81, 10
			b, err := s.putMember(recent.Identity)
			if err != nil {
				s.mu.Unlock()
				t.Fatal(err)
			}
			b.Permissions, b.LastActivity = 0x82, 11
			c, err := s.putMember(incoming.Identity)
			if err != nil {
				s.mu.Unlock()
				t.Fatal(err)
			}
			c.Permissions = 0x81
			if s.state.Members[old.Identity.String()] != nil || s.state.Members[admin.Identity.String()] == nil {
				s.mu.Unlock()
				t.Fatal("eviction did not select least-active non-admin")
			}
			if err := s.save(); err != nil {
				s.mu.Unlock()
				t.Fatal(err)
			}
			s.mu.Unlock()
			r.inject(t, peerWire(t, 0, admin, id, []byte{2, 0, 0, 0, 5, 0, 0}), false)
			body := decrypted(t, r.next(t), admin, id)
			want := []byte{2, 0, 0, 0}
			want = append(want, admin.PublicKeyBytes()[:6]...)
			want = append(want, 3)
			if !room {
				want = append(want, incoming.PublicKeyBytes()[:6]...)
				want = append(want, 0x81)
				want = append(want, recent.PublicKeyBytes()[:6]...)
				want = append(want, 0x82)
			}
			if !bytes.Equal(body[:len(want)], want) || !bytes.Equal(body[len(want):], make([]byte, len(body)-len(want))) {
				t.Fatalf("ACL wire filter: %x, want %x padded", body, want)
			}
			if err := s.Close(); err != nil {
				t.Fatal(err)
			}
			restarted, _, _ := startRole(t, room, cfg)
			restarted.mu.RLock()
			defer restarted.mu.RUnlock()
			wantMembers := 3
			if room {
				wantMembers = 1
			}
			if len(restarted.state.Members) != wantMembers || restarted.state.Members[admin.Identity.String()].LastTimestamp != 0 {
				t.Fatal("native persistence filter or transient replay timestamp differs")
			}
		})
	}
}

func TestNativeRetentionAndLegacyMigration(t *testing.T) {
	cfg := config(t)
	cfg.Retention = NativeRetention
	s, _, id := startRole(t, true, cfg)
	s.mu.Lock()
	s.addPost(fixedID(2).PublicKey(), "volatile")
	if err := s.save(); err != nil {
		s.mu.Unlock()
		t.Fatal(err)
	}
	legacy := s.state.clone()
	legacy.Version, legacy.Retention = 1, ""
	legacy.PreferenceProfile = ""
	legacy.History[0].Text = strings.Repeat("x", 151)
	s.mu.Unlock()
	if err := s.Close(); err != nil {
		t.Fatal(err)
	}
	data, err := os.ReadFile(filepath.Join(cfg.StateDir, "state.json"))
	if err != nil {
		t.Fatal(err)
	}
	var disk diskState
	if err := json.Unmarshal(data, &disk); err != nil || len(disk.History) != 0 {
		t.Fatalf("native profile persisted volatile history: %s (%v)", data, err)
	}
	data, err = json.Marshal(legacy)
	if err != nil {
		t.Fatal(err)
	}
	if err := os.WriteFile(filepath.Join(cfg.StateDir, "state.json"), data, 0600); err != nil {
		t.Fatal(err)
	}
	cfg.Retention = ""
	migrated, _, _ := startRole(t, true, cfg)
	migrated.mu.RLock()
	if migrated.state.Retention != DurableReplay || migrated.state.PreferenceProfile != DurableHostPreferences || len(migrated.state.History) != 1 ||
		migrated.state.History[0].Text != legacy.History[0].Text {
		migrated.mu.RUnlock()
		t.Fatal("migration discarded history or did not name durable extension")
	}
	migrated.mu.RUnlock()
	backup, err := os.ReadFile(filepath.Join(cfg.StateDir, "state.v1.json"))
	if err != nil || !bytes.Equal(backup, data) {
		t.Fatal("legacy rollback copy differs from original state")
	}
	if err := migrated.Close(); err != nil {
		t.Fatal(err)
	}
	cfg.Retention = NativeRetention
	if other, err := NewRoom(id, newWireRadio(), cfg); err == nil {
		other.Close()
		t.Fatal("explicit native override silently discarded legacy history")
	}
}

func TestAnonymousRateLimitNativeWindow(t *testing.T) {
	var limiter rateLimiter
	for i := range 4 {
		if !limiter.allow(1000+uint32(i), 180, 4) {
			t.Fatal("initial native burst denied")
		}
	}
	for range 100 {
		if limiter.allow(1179, 180, 4) {
			t.Fatal("anonymous request budget exceeded")
		}
	}
	if !limiter.allow(1180, 180, 4) {
		t.Fatal("native fixed window did not reset at boundary")
	}
}

func TestAnonymousRateLimitOnWire(t *testing.T) {
	_, r, id := startRole(t, false, config(t))
	client := fixedID(2)
	for i := uint32(1); i <= 4; i++ {
		r.inject(t, loginWire(t, false, client, id, i, 0, "\x03\x00", 2, 0, nil), false)
		raw := r.next(t)
		body := decrypted(t, raw, client, id)
		if raw[0] != 0x06 || raw[1] != 0 || u32(body) != i || body[8] != 0 {
			t.Fatalf("native anonymous clock response: %x / %x", raw, body)
		}
	}
	r.inject(t, loginWire(t, false, client, id, 5, 0, "\x03\x00", 2, 0, nil), false)
	r.quiet(t)
}

func assertWireDelay(t *testing.T, r *wireRadio, raw []byte, want time.Duration) {
	t.Helper()
	r.mu.Lock()
	defer r.mu.Unlock()
	for i := len(r.jobs) - 1; i >= 0; i-- {
		job := r.jobs[i]
		if bytes.Equal(job.wire, raw) {
			if job.delay != want {
				t.Fatalf("wire eligibility delay %v, want %v", job.delay, want)
			}
			return
		}
	}
	t.Fatal("wire response bypassed priority/delay submission")
}

func TestNativeResponseAndACKDelayMetadata(t *testing.T) {
	// MyMesh.cpp login/handleRequest and TXT_ACK_DELAY/SERVER_RESPONSE_DELAY.
	s, r, id := startRole(t, true, config(t))
	client := fixedID(2)
	r.inject(t, loginWire(t, true, client, id, 1, 0, "room", 2, 0, nil), false)
	assertWireDelay(t, r, r.next(t), 300*time.Millisecond)
	learnPath(t, s, r, client, id, 0, nil)
	r.inject(t, textWire(t, client, id, 2, 0, "delay"), false)
	assertWireDelay(t, r, r.next(t), 200*time.Millisecond)
	r.inject(t, peerWire(t, 0, client, id, []byte{3, 0, 0, 0, 2, 0, 0, 0, 0}), false)
	assertWireDelay(t, r, r.next(t), 300*time.Millisecond)
	r.inject(t, peerWire(t, 0, client, id, []byte{4, 0, 0, 0, 1}), false)
	assertWireDelay(t, r, r.next(t), 300*time.Millisecond)
}

func TestObservedRadioPreservesScheduling(t *testing.T) {
	r := newWireRadio()
	s := &Service{}
	observed := &observedTxRadio{observedRadio: &observedRadio{Radio: r, service: s}, tx: r}
	if !observed.Enqueue([]byte{0x0e, 0, 1, 2, 3, 4}, 5, 300*time.Millisecond) {
		t.Fatal("enqueue rejected")
	}
	r.mu.Lock()
	defer r.mu.Unlock()
	if len(r.jobs) != 1 || r.jobs[0].priority != 5 || r.jobs[0].delay != 300*time.Millisecond {
		t.Fatalf("wrapper lost priority/delay: %+v", r.jobs)
	}
}

func TestIndeterminateCommitFailsClosedWithoutPretendRollback(t *testing.T) {
	cfg := config(t)
	errs := make(chan error, 8)
	cfg.ErrorHandler = func(err error) { errs <- err }
	r := newWireRadio()
	id, client := fixedID(1), fixedID(2)
	s, err := NewRoom(id, r, cfg)
	if err != nil {
		t.Fatal(err)
	}
	t.Cleanup(func() { _ = s.Close() })
	r.next(t)
	s.mu.Lock()
	s.writeJSON = func(path string, value any) error {
		if err := hoststate.WriteRoleJSON(path, value); err != nil {
			return err
		}
		return errors.Join(hoststate.ErrCommitIndeterminate, errors.New("injected directory sync failure"))
	}
	s.mu.Unlock()
	r.inject(t, loginWire(t, true, client, id, 1, 0, "room", 2, 0, nil), false)
	select {
	case err := <-errs:
		if !errors.Is(err, ErrCommitUncertain) || !errors.Is(err, hoststate.ErrCommitIndeterminate) {
			t.Fatalf("lost indeterminate commit classification: %v", err)
		}
	case <-time.After(time.Second):
		t.Fatal("directory-sync failure not reported")
	}
	r.quiet(t)
	s.mu.RLock()
	closed := s.commitUncertain
	_, retained := s.state.Members[client.Identity.String()]
	s.mu.RUnlock()
	if !closed || !retained {
		t.Fatal("post-rename state was rolled back or processing remained enabled")
	}
	path := filepath.Join(cfg.StateDir, "state.json")
	before, err := os.ReadFile(path)
	if err != nil {
		t.Fatal(err)
	}
	var disk diskState
	if err := json.Unmarshal(before, &disk); err != nil || disk.Members[client.Identity.String()] == nil {
		t.Fatal("test did not exercise a visible post-rename commit")
	}
	r.inject(t, textWire(t, client, id, 2, 0, "must not commit"), false)
	select {
	case err := <-errs:
		if !errors.Is(err, ErrCommitUncertain) {
			t.Fatal(err)
		}
	case <-time.After(time.Second):
		t.Fatal("failed-closed request was not diagnosed")
	}
	r.quiet(t)
	after, err := os.ReadFile(path)
	if err != nil || !bytes.Equal(before, after) {
		t.Fatal("indeterminate service continued committing")
	}
	if err := s.Close(); !errors.Is(err, ErrCommitUncertain) {
		t.Fatalf("Close hid indeterminate commit: %v", err)
	}
}
