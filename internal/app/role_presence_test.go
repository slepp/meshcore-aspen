package app

import (
	"bytes"
	"context"
	"encoding/json"
	"io"
	"log/slog"
	"net"
	"os"
	"path/filepath"
	"slices"
	"strings"
	"sync/atomic"
	"testing"
	"time"

	meshcore "github.com/meshcore-go/meshcore-go"
	"github.com/meshcore-go/meshcore-go/hardware"
	"github.com/meshcore-go/meshcore-go/node"
	"meshcore.local/meshcore/internal/radio"
	"meshcore.local/meshcore/internal/state"
)

func claimTestConfig(t *testing.T) (Config, *authorityMast) {
	t.Helper()
	cfg := DefaultConfig()
	cfg.PHYAuthority = "modem"
	cfg.StateDir = t.TempDir()
	mast := newAuthorityMast(t, cfg)
	cfg.RadioAddress = mast.listener.Addr().String()
	return cfg, mast
}

func claimIdentity(t *testing.T, cfg Config, role string) meshcore.LocalIdentity {
	t.Helper()
	identity, err := state.Identity(cfg.StateDir, role)
	if err != nil {
		t.Fatal(err)
	}
	return identity
}

func testClaimLink(t *testing.T, cfg Config, role string, identity meshcore.LocalIdentity) *radio.Link {
	t.Helper()
	link, err := cfg.openRoleSource(context.Background(), role, identity, slog.New(slog.NewTextHandler(io.Discard, nil)), nil)
	if err != nil {
		t.Fatal(err)
	}
	t.Cleanup(func() { _ = link.Close() })
	return link
}

func TestRolePresenceIDsAndStateKeys(t *testing.T) {
	identity := meshcore.NewLocalIdentityFromSeed([32]byte{1})
	for _, tc := range []struct {
		role string
		id   radio.MastRole
	}{
		{"repeater", radio.RepeaterRole},
		{"room", radio.RoomRole},
		{"companion", radio.CompanionRole},
		{"observer", radio.ObserverRole},
	} {
		claim := roleAnnouncement(tc.role, identity)
		if claim == nil || claim.Role != tc.id || !bytes.Equal(claim.PublicKey[:], identity.PublicKeyBytes()) {
			t.Fatalf("%s claim does not use its actual public identity: %+v", tc.role, claim)
		}
	}
	for _, role := range []string{"controller", "bot_companion", "bot"} {
		if claim := roleAnnouncement(role, identity); claim != nil {
			t.Fatalf("%s impersonates a mast role: %+v", role, claim)
		}
	}
}

func TestRolePresenceUsesStateIdentityBeforeStartupAndEveryReconnect(t *testing.T) {
	cfg, mast := claimTestConfig(t)
	mast.localSlots, mast.preClaimRX = 4, true
	id, err := state.Identity(cfg.StateDir, "repeater")
	if err != nil {
		t.Fatal(err)
	}
	cfg.StateDir = filepath.Join(t.TempDir(), "must-not-be-read")
	link := testClaimLink(t, cfg, "repeater", id)
	first := <-mast.clients
	var rx atomic.Int32
	link.SetDataHandler(func([]byte, float32, int8, bool) { rx.Add(1) })
	first.Close()
	waitAuthority(t, func() bool {
		mast.mu.Lock()
		defer mast.mu.Unlock()
		return len(mast.claims) >= 2 && link.Online()
	})
	var key [32]byte
	copy(key[:], id.PublicKeyBytes())
	mast.mu.Lock()
	claims := append([]radio.RoleAnnouncement(nil), mast.claims...)
	gens := append([]uint32(nil), mast.claimGenerations...)
	sets := mast.sets
	mast.mu.Unlock()
	if len(claims) != 2 || claims[0] != (radio.RoleAnnouncement{Role: radio.RepeaterRole, PublicKey: key}) ||
		claims[1] != claims[0] || gens[0] == gens[1] || sets != 0 || rx.Load() != 0 {
		t.Fatalf("claim ordering/key/reconnect: claims=%+v generations=%v CONFIG SET=%d early RX=%d",
			claims, gens, sets, rx.Load())
	}
	if _, err := os.Stat(cfg.StateDir); !os.IsNotExist(err) {
		t.Fatalf("source reopened the state directory instead of using the worker identity: %v", err)
	}
}

func TestWorkersOpenWithTheirActualIdentityAfterLifecycleChanges(t *testing.T) {
	t.Run("room-rekey", func(t *testing.T) {
		cfg, ids := lifecycleConfig(t)
		opened := make(chan meshcore.LocalIdentity, 2)
		worker := routingStartWithIdentity(t, context.Background(), cfg, "room",
			func(_ context.Context, identity meshcore.LocalIdentity) (sourceLink, error) {
				opened <- identity
				return newRoutingLink(), nil
			})
		if got := <-opened; got.PublicKey() != ids["room"].PublicKey() ||
			worker.status().PublicKey != got.String() {
			t.Fatal("room startup opened with a different identity from its service")
		}
		next, key := candidateIdentity(t, cfg.StateDir)
		if err := state.StageRoleIdentity(context.Background(), cfg.StateDir, "room", key); err != nil {
			t.Fatal(err)
		}
		if err := worker.request(context.Background()); err != nil {
			t.Fatal(err)
		}
		select {
		case got := <-opened:
			lifecycleWait(t, func() bool {
				return worker.status().State == "running" && worker.status().PublicKey == next.String()
			})
			if got.PublicKey() != next.PublicKey() {
				t.Fatal("room logical restart announced the retired identity")
			}
		case <-time.After(5 * time.Second):
			t.Fatal("room did not reopen after rekey")
		}
	})
	t.Run("companion-reset", func(t *testing.T) {
		cfg, ids := lifecycleConfig(t)
		opened := make(chan meshcore.LocalIdentity, 2)
		worker := lifecycleStartWithIdentity(t, context.Background(), cfg, ids,
			func(_ context.Context, identity meshcore.LocalIdentity) (sourceLink, error) {
				opened <- identity
				return &lifecycleLink{}, nil
			})
		if got := <-opened; got.PublicKey() != ids["companion"].PublicKey() ||
			worker.status().PublicKey != got.String() {
			t.Fatal("companion startup opened with a different identity from its service")
		}
		if err := worker.request(context.Background(), companionReset); err != nil {
			t.Fatal(err)
		}
		select {
		case got := <-opened:
			lifecycleWait(t, func() bool {
				return worker.status().State == "running" && worker.status().PublicKey == got.String()
			})
			if got.PublicKey() == ids["companion"].PublicKey() {
				t.Fatal("companion reset announced its retired identity")
			}
		case <-time.After(5 * time.Second):
			t.Fatal("companion did not reopen after reset")
		}
	})
}

func TestMalformedOrStaleRoleAnnouncementRepliesAreOverlapUnknown(t *testing.T) {
	for _, tc := range []struct {
		name  string
		setup func(*authorityMast, [32]byte)
		want  string
	}{
		{"malformed", func(m *authorityMast, _ [32]byte) { m.claimMalformed = true }, "invalid ROLE_PRESENCE"},
		{"stale-generation", func(m *authorityMast, _ [32]byte) { m.claimStale = true }, "stale ROLE_PRESENCE"},
		{"invalid-flags", func(m *authorityMast, _ [32]byte) { m.replyFlags = 0x80 }, "warning flags 0x80"},
		{"invalid-reason", func(m *authorityMast, _ [32]byte) { m.claimError = 1 }, "reason 1"},
		{"unexpected-busy", func(m *authorityMast, _ [32]byte) { m.claimError = 5 }, "reason 5"},
	} {
		t.Run(tc.name, func(t *testing.T) {
			cfg, mast := claimTestConfig(t)
			mast.localSlots = 4
			id, err := state.Identity(cfg.StateDir, "room")
			if err != nil {
				t.Fatal(err)
			}
			var key [32]byte
			copy(key[:], id.PublicKeyBytes())
			tc.setup(mast, key)
			var logs bytes.Buffer
			link, err := cfg.openRoleSource(context.Background(), "room", id, slog.New(slog.NewTextHandler(&logs, nil)), nil)
			if err != nil {
				t.Fatalf("advisory presence denied admission: %v", err)
			}
			defer link.Close()
			presence := link.RolePresenceStatus()
			if presence == nil || presence.State != "unknown" || !presence.Online ||
				!strings.Contains(presence.Warning, "Overlap unknown") || !strings.Contains(presence.Warning, tc.want) ||
				!strings.Contains(logs.String(), "role presence unknown") {
				t.Fatalf("presence=%+v log=%s, want unknown %q", presence, logs.String(), tc.want)
			}
			entry := roleStatus{}
			entry.setRolePresence(presence)
			if entry.Warning != presence.Warning {
				t.Fatalf("overlap-unknown warning omitted from status: %+v", entry)
			}
		})
	}
}

func TestOverlappingReconnectDoesNotReplayUnknownTX(t *testing.T) {
	cfg, mast := claimTestConfig(t)
	link := testClaimLink(t, cfg, "repeater", claimIdentity(t, cfg, "repeater"))
	first := <-mast.clients
	sent := make(chan error, 1)
	go func() { sent <- link.SendData([]byte{0x15, 0, 1, 2}) }()
	waitAuthority(t, func() bool {
		mast.mu.Lock()
		defer mast.mu.Unlock()
		return mast.submissions == 1
	})
	mast.mu.Lock()
	mast.nativeMask = 1 << radio.RepeaterRole
	mast.mu.Unlock()
	first.Close()
	select {
	case err := <-sent:
		if err == nil || !strings.Contains(err.Error(), "not replaying") {
			t.Fatalf("unknown in-flight TX reported success: %v", err)
		}
	case <-time.After(5 * time.Second):
		t.Fatal("unknown TX did not resolve on disconnect")
	}
	waitAuthority(t, func() bool {
		mast.mu.Lock()
		defer mast.mu.Unlock()
		return len(mast.claims) >= 2 && link.Online()
	})
	mast.mu.Lock()
	submissions := mast.submissions
	mast.mu.Unlock()
	status := link.RolePresenceStatus()
	if submissions != 1 || !link.Online() || status == nil || status.State != "overlap" ||
		!status.Online || status.Warning == "" || status.WarningFlags&0x01 == 0 {
		t.Fatalf("overlapping reconnect replayed TX or went offline: submissions=%d status=%+v", submissions, status)
	}
	var restored net.Conn
	select {
	case restored = <-mast.clients:
	case <-time.After(3 * time.Second):
		t.Fatal("overlapping reconnect did not reopen the source")
	}
	received := make(chan []byte, 1)
	link.SetDataHandler(func(data []byte, _ float32, _ int8, _ bool) { received <- data })
	if _, err := restored.Write(append(hardware.EncodeDataFrame([]byte{0x15, 0, 9, 8}),
		hardware.EncodeHardwareFrame(0, hardware.HW_RESP_RX_META, []byte{0x14, 0xc4})...)); err != nil {
		t.Fatal(err)
	}
	select {
	case data := <-received:
		if !bytes.Equal(data, []byte{0x15, 0, 9, 8}) {
			t.Fatalf("overlap source RX corrupted: %x", data)
		}
	case <-time.After(3 * time.Second):
		t.Fatal("overlap source did not receive radio frames")
	}
	roleRadio, stop := link.Radio()
	defer stop()
	if !roleRadio.(node.TxRadio).Enqueue([]byte{0x15, 0, 7, 6}, 0, 0) {
		t.Fatal("overlap source cannot transmit")
	}
	waitAuthority(t, func() bool {
		mast.mu.Lock()
		defer mast.mu.Unlock()
		return mast.submissions == 2
	})
}

func TestRolePresenceWarnsForOverlappingHostsAndNativeRoles(t *testing.T) {
	cfg, mast := claimTestConfig(t)
	mast.localSlots = 4
	repeater := claimIdentity(t, cfg, "repeater")
	first := testClaimLink(t, cfg, "repeater", repeater)
	if !first.Online() {
		t.Fatal("first claim offline")
	}
	second := testClaimLink(t, cfg, "repeater", repeater)
	if status := second.RolePresenceStatus(); status == nil || status.State != "overlap" ||
		!status.Online || status.Role != "repeater" || status.PublicKey != repeater.String() ||
		status.WarningFlags != 0x0a ||
		!strings.Contains(status.Warning, "another connected host announced the same role") ||
		!strings.Contains(status.Warning, "same public key") ||
		!strings.Contains(status.Warning, repeater.String()) {
		t.Fatalf("same role/key overlap did not warn while online: %+v", status)
	}
	room, err := state.Identity(cfg.StateDir, "room")
	if err != nil {
		t.Fatal(err)
	}
	key := roleAnnouncement("room", room)
	key.PublicKey = roleAnnouncement("repeater", repeater).PublicKey
	link, err := radio.Open(context.Background(), radio.Config{
		Address: cfg.RadioAddress, Radio: cfg.Radio, TxPower: cfg.TxPower, RequireParity: true,
		RoleAnnouncement: key,
	})
	if err != nil {
		t.Fatalf("other role with same key blocked: %v", err)
	}
	defer link.Close()
	if status := link.RolePresenceStatus(); status == nil || status.State != "overlap" ||
		status.WarningFlags != 0x08 || !strings.Contains(status.Warning, "same public key") {
		t.Fatalf("duplicate key overlap not reported: %+v", status)
	}
	mast.mu.Lock()
	mast.nativeMask = 1 << radio.RoomRole
	mast.nativeKeys = append(mast.nativeKeys, room.PublicKey())
	mast.mu.Unlock()
	var logs bytes.Buffer
	roomLink, err := cfg.openRoleSource(context.Background(), "room", room,
		slog.New(slog.NewTextHandler(&logs, nil)), nil)
	if err != nil {
		t.Fatal(err)
	}
	defer roomLink.Close()
	status := roomLink.RolePresenceStatus()
	if status == nil || status.State != "overlap" || status.NativeMask != 1<<radio.RoomRole ||
		status.WarningFlags&0x05 != 0x05 ||
		status.NativeRoleRunning == nil || !*status.NativeRoleRunning ||
		!slices.Equal(status.AppliedNativeRoles, []string{"room"}) ||
		!status.Online || status.Role != "room" || status.PublicKey != room.String() ||
		!strings.Contains(status.Warning, "same role also running on mast") ||
		!strings.Contains(status.Warning, "mast also reports the same public key") ||
		!strings.Contains(status.Warning, room.String()) {
		t.Fatalf("native overlap not visible while online: %+v", status)
	}
	if !strings.Contains(logs.String(), "role presence overlap") ||
		!strings.Contains(logs.String(), "role=room") ||
		!strings.Contains(logs.String(), room.String()) {
		t.Fatalf("operator log omitted role identity or overlap: %s", logs.String())
	}
	handler := newStatusHandler(context.Background(), func() map[string]roleStatus {
		entries := onlineHealthSnapshot()
		entry := entries["room"]
		online := roomLink.Online()
		entry.RadioConnected = &online
		if !online {
			entry.Telemetry = nil
		}
		entry.PublicKey = room.String()
		entry.setRolePresence(roomLink.RolePresenceStatus())
		entries["room"] = entry
		return entries
	}, slog.Default(), cfg)
	for i := 0; i < 2; i++ {
		roomStatus := requestStatus(t, handler)["room"]
		if roomStatus.State != "running" || roomStatus.PublicKey != room.String() ||
			roomStatus.Warning != status.Warning || roomStatus.RolePresence == nil ||
			roomStatus.RolePresence.Warning != status.Warning ||
			!slices.Equal(roomStatus.RolePresence.AppliedNativeRoles, []string{"room"}) ||
			roomStatus.RolePresence.NativeRoleRunning == nil || !*roomStatus.RolePresence.NativeRoleRunning {
			t.Fatalf("existing /status role entry lost persistent overlap: %+v", roomStatus)
		}
		requestReadiness(t, handler, 200, "", "")
	}
	if err := roomLink.Close(); err != nil {
		t.Fatal(err)
	}
	offline := requestStatus(t, handler)["room"]
	if offline.Warning != "" || offline.RolePresence == nil ||
		offline.RolePresence.Online || offline.RolePresence.Warning != status.Warning {
		t.Fatalf("disconnected status presented a stale overlap as current: %+v", offline)
	}
	requestReadiness(t, handler, 503, "room", "radio_disconnected")
}

func TestMastAndHostRoomOverlapDoesNotImplySharedIdentity(t *testing.T) {
	cfg, mast := claimTestConfig(t)
	mast.nativeMask = 1 << radio.RoomRole
	mast.nativeKeys = append(mast.nativeKeys, meshcore.NewLocalIdentityFromSeed([32]byte{2}).PublicKey())
	identity := claimIdentity(t, cfg, "room")
	var logs bytes.Buffer
	logger := slog.New(slog.NewTextHandler(&logs, nil))
	link, err := cfg.openRoleSource(context.Background(), "room", identity, logger, nil)
	if err != nil {
		t.Fatal(err)
	}
	defer link.Close()
	status := link.RolePresenceStatus()
	if !link.Online() || status == nil || !status.Online || status.State != "overlap" ||
		status.Role != "room" || status.PublicKey != identity.String() || status.WarningFlags != 0x01 ||
		!strings.Contains(status.Warning, "same role also running on mast") ||
		!strings.Contains(status.Warning, "concurrent instances may be intentional") ||
		strings.Contains(status.Warning, "same public key") {
		t.Fatalf("distinct mast/host room identities misreported: %+v", status)
	}
	if !strings.Contains(logs.String(), "same role also running on mast") ||
		strings.Contains(logs.String(), "same public key") {
		t.Fatalf("operator warnings did not distinguish roles from identities: %s", logs.String())
	}
	mast.mu.Lock()
	claims := append([]radio.RoleAnnouncement(nil), mast.claims...)
	mast.mu.Unlock()
	if len(claims) != 1 || claims[0].Role != radio.RoomRole ||
		!bytes.Equal(claims[0].PublicKey[:], identity.PublicKeyBytes()) {
		t.Fatalf("host room announcement lost its own identity: %+v", claims)
	}
}

func TestExistingWorkerStatusEntriesKeepPresenceWarnings(t *testing.T) {
	cfg, identities := lifecycleConfig(t)
	roomPresence := &radio.RolePresenceStatus{
		State: "overlap", Role: "room", PublicKey: identities["room"].String(),
		Warning: "room identity is also present on the mast", Online: true,
	}
	roomLink := newRoutingLink()
	roomLink.presence = roomPresence
	room := routingStart(t, context.Background(), cfg, "room", func(context.Context) (sourceLink, error) {
		return roomLink, nil
	})
	basePresence := &radio.RolePresenceStatus{
		State: "overlap", Role: "companion", PublicKey: identities["companion"].String(),
		Warning: "another host announced this companion identity", Online: true,
	}
	baseLink := &lifecycleLink{presence: basePresence}
	base := lifecycleStart(t, context.Background(), cfg, identities, func(context.Context) (sourceLink, error) {
		return baseLink, nil
	})
	handler := newStatusHandler(context.Background(), func() map[string]roleStatus {
		entries := onlineHealthSnapshot()
		entries["room"], entries["companion"] = room.status(), base.status()
		return entries
	}, slog.Default(), cfg)
	for _, tc := range []struct {
		role string
		want *radio.RolePresenceStatus
	}{
		{"room", roomPresence},
		{"companion", basePresence},
	} {
		entry := requestStatus(t, handler)[tc.role]
		if entry.PublicKey != identities[tc.role].String() ||
			entry.State != "running" || entry.Warning != tc.want.Warning ||
			entry.RolePresence == nil || entry.RolePresence.Warning != tc.want.Warning {
			t.Fatalf("%s warning omitted from existing /status entry: %+v", tc.role, entry)
		}
	}
	requestReadiness(t, handler, 200, "", "")
}

func TestSelectedButInactiveNativeRoleIsNotReportedAsRunning(t *testing.T) {
	cfg, mast := claimTestConfig(t)
	mast.replyMask = 1 << radio.RoomRole
	link := testClaimLink(t, cfg, "room", claimIdentity(t, cfg, "room"))
	status := link.RolePresenceStatus()
	if status == nil || status.State != "announced" ||
		status.NativeMask != 1<<radio.RoomRole || status.WarningFlags != 0 || status.Warning != "" ||
		status.NativeRoleRunning == nil || *status.NativeRoleRunning ||
		!slices.Equal(status.AppliedNativeRoles, []string{"room"}) {
		t.Fatalf("boot selection was mistaken for an active overlap: %+v", status)
	}
	entry := roleStatus{}
	entry.setRolePresence(status)
	raw, err := json.Marshal(entry)
	if err != nil || bytes.Contains(raw, []byte(`"warning":`)) ||
		!bytes.Contains(raw, []byte(`"applied_native_roles":["room"]`)) ||
		!bytes.Contains(raw, []byte(`"native_role_running":false`)) {
		t.Fatalf("inactive mast role status was misreported: %s, %v", raw, err)
	}
}

func TestDefaultNativeBootRolesReportedAsAppliedAndRunning(t *testing.T) {
	cfg, mast := claimTestConfig(t)
	mast.nativeMask = 0x0f
	link := testClaimLink(t, cfg, "repeater", claimIdentity(t, cfg, "repeater"))
	status := link.RolePresenceStatus()
	if status == nil || status.State != "overlap" ||
		status.NativeRoleRunning == nil || !*status.NativeRoleRunning ||
		!slices.Equal(status.AppliedNativeRoles, []string{"repeater", "room", "companion", "observer"}) {
		t.Fatalf("boot-applied native defaults misreported: %+v", status)
	}
}

func TestRoleAnnouncementRadioOnlyFallbackAndModemAuthority(t *testing.T) {
	for _, tc := range []struct {
		name, authority string
		local           byte
		capacity        bool
		explicitLimit   bool
	}{
		{"radio-only-capacity-host", "host", 0, true, false},
		{"radio-only-capacity-modem", "modem", 0, true, false},
		{"combined-host", "host", 4, true, false},
		{"combined-modem", "modem", 4, true, false},
		{"legacy-host-unverifiable-combined", "host", 4, false, false},
		{"unverifiable-modem", "modem", 0, false, true},
	} {
		t.Run(tc.name, func(t *testing.T) {
			cfg, mast := claimTestConfig(t)
			cfg.PHYAuthority = PHYAuthority(tc.authority)
			if tc.explicitLimit {
				limit := 4
				cfg.RadioClientCapacity = &limit
			}
			mast.localSlots, mast.claimSupported, mast.capacitySupported = tc.local, false, tc.capacity
			var logs bytes.Buffer
			link, err := cfg.openRoleSource(context.Background(), "companion",
				claimIdentity(t, cfg, "companion"),
				slog.New(slog.NewTextHandler(&logs, nil)), nil)
			if err != nil || link == nil {
				t.Fatalf("unsupported advisory claim blocked radio: link=%v error=%v", link, err)
			}
			defer link.Close()
			warning := "mast reports zero local sources"
			if tc.local != 0 && tc.capacity {
				warning = "mast reports 4 local sources"
			}
			if !tc.capacity {
				warning = "presence and capacity queries are unsupported"
			}
			status := link.RolePresenceStatus()
			if status == nil || status.State != "unsupported" || !status.Online ||
				status.Role != "companion" || status.PublicKey != claimIdentity(t, cfg, "companion").String() ||
				status.AppliedNativeRoles != nil || status.NativeRoleRunning != nil ||
				!strings.Contains(status.Warning, warning) ||
				!strings.Contains(status.Warning, status.PublicKey) ||
				!strings.Contains(logs.String(), "role presence unavailable") {
				t.Fatalf("missing bounded advisory warning %q: status=%+v log=%s", warning, status, logs.String())
			}
			mast.mu.Lock()
			sets := mast.sets
			mast.mu.Unlock()
			if sets != 0 {
				t.Fatalf("modem authority/follower sent CONFIG SET %d times", sets)
			}
		})
	}
}

func TestUnsupportedClaimWithMalformedCapacityWarnsWithoutDenial(t *testing.T) {
	cfg, mast := claimTestConfig(t)
	cfg.PHYAuthority = "host"
	mast.claimSupported, mast.malformedCapacity = false, true
	link, err := cfg.openRoleSource(context.Background(), "room", claimIdentity(t, cfg, "room"), slog.Default(), nil)
	if err != nil {
		t.Fatalf("optional discovery denied admission: %v", err)
	}
	defer link.Close()
	presence := link.RolePresenceStatus()
	if presence == nil || presence.State != "unsupported" ||
		!strings.Contains(presence.Warning, "CAPACITY failed") || strings.Contains(presence.Warning, "local sources") {
		t.Fatalf("malformed fallback CAPACITY was presented as radio-only: %+v", presence)
	}
}

func TestObserverClaimsButControllerAndBotCompanionDoNot(t *testing.T) {
	cfg, mast := claimTestConfig(t)
	mast.nativeMask = 1 << radio.ObserverRole
	cfg.StateDir = t.TempDir()
	observer, err := state.Identity(cfg.StateDir, "observer")
	if err != nil {
		t.Fatal(err)
	}
	logger := slog.New(slog.NewTextHandler(io.Discard, nil))
	observerConfig := cfg.configurationLink("observer", logger, nil, nil)
	observerConfig.RoleAnnouncement = roleAnnouncement("observer", observer)
	link, err := radio.Open(context.Background(), observerConfig)
	if err != nil {
		t.Fatal(err)
	}
	defer link.Close()
	status := link.RolePresenceStatus()
	entry := roleStatus{}
	entry.setRolePresence(status)
	if !link.Online() || status == nil || status.WarningFlags != 0x01 ||
		!strings.Contains(entry.Warning, "same role also running on mast") ||
		strings.Contains(entry.Warning, "same public key") {
		t.Fatalf("passive observer overlap was treated as mesh ownership: %+v", entry)
	}
	var observerConn net.Conn
	select {
	case observerConn = <-mast.clients:
	case <-time.After(3 * time.Second):
		t.Fatal("observer connection was not accepted")
	}
	observed := make(chan []byte, 1)
	link.SetDataHandler(func(data []byte, _ float32, _ int8, _ bool) { observed <- data })
	if _, err := observerConn.Write(append(hardware.EncodeDataFrame([]byte{0x15, 0, 1, 2}),
		hardware.EncodeHardwareFrame(0, hardware.HW_RESP_RX_META, []byte{0x14, 0xc4})...)); err != nil {
		t.Fatal(err)
	}
	select {
	case data := <-observed:
		if !bytes.Equal(data, []byte{0x15, 0, 1, 2}) {
			t.Fatalf("observer RX changed during overlap: %x", data)
		}
	case <-time.After(3 * time.Second):
		t.Fatal("passive observer did not receive mast traffic")
	}
	controller, err := radio.Open(context.Background(), cfg.configurationLink("controller", logger, nil, nil))
	if err != nil {
		t.Fatal(err)
	}
	defer controller.Close()
	bot := testClaimLink(t, cfg, "bot_companion", claimIdentity(t, cfg, "bot_companion"))
	mast.mu.Lock()
	claims := append([]radio.RoleAnnouncement(nil), mast.claims...)
	mast.mu.Unlock()
	if len(claims) != 1 || claims[0].Role != radio.ObserverRole ||
		!bytes.Equal(claims[0].PublicKey[:], observer.PublicKeyBytes()) || !bot.Online() {
		t.Fatalf("unexpected controller/bot role announcements: %+v", claims)
	}
}

func TestUnannouncedKISSLinkSharesPHYWithoutRoleOwnership(t *testing.T) {
	cfg, mast := claimTestConfig(t)
	mast.nativeMask = 1 << radio.RoomRole
	link, err := radio.Open(context.Background(),
		cfg.configurationLink("controller", slog.New(slog.NewTextHandler(io.Discard, nil)), nil, nil))
	if err != nil {
		t.Fatal(err)
	}
	defer link.Close()
	if !link.Online() || link.RolePresenceStatus() != nil {
		t.Fatal("unannounced KISS client incorrectly required a role")
	}
	var client net.Conn
	select {
	case client = <-mast.clients:
	case <-time.After(3 * time.Second):
		t.Fatal("unannounced KISS client was not accepted")
	}
	received := make(chan []byte, 1)
	link.SetDataHandler(func(data []byte, _ float32, _ int8, _ bool) { received <- data })
	if _, err := client.Write(append(hardware.EncodeDataFrame([]byte{0x15, 0, 9, 8}),
		hardware.EncodeHardwareFrame(0, hardware.HW_RESP_RX_META, []byte{0x14, 0xc4})...)); err != nil {
		t.Fatal(err)
	}
	select {
	case data := <-received:
		if !bytes.Equal(data, []byte{0x15, 0, 9, 8}) {
			t.Fatalf("unannounced KISS RX changed: %x", data)
		}
	case <-time.After(3 * time.Second):
		t.Fatal("unannounced KISS client could not receive traffic")
	}
	tx, stop := link.Radio()
	defer stop()
	if !tx.(node.TxRadio).Enqueue([]byte{0x15, 0, 7, 6}, 0, 0) {
		t.Fatal("unannounced KISS client could not transmit")
	}
	waitAuthority(t, func() bool {
		mast.mu.Lock()
		defer mast.mu.Unlock()
		return mast.submissions == 1
	})
	mast.mu.Lock()
	claims := len(mast.claims)
	mast.mu.Unlock()
	if claims != 0 {
		t.Fatalf("unannounced KISS client sent %d unsolicited role announcements", claims)
	}
}
