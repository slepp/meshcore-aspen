package roles

import (
	"bytes"
	"encoding/hex"
	"meshcore.local/meshcore/internal/buildinfo"
	"testing"
	"time"

	meshcore "github.com/meshcore-go/meshcore-go"
	"meshcore.local/meshcore/internal/policy"
)

func TestNativeOriginatedPriorityFixture(t *testing.T) {
	s, r, _ := startRole(t, false, config(t))
	count := 0
	passed := false
	for _, event := range nativeRoleFixtures(t) {
		if event.Scenario == "originated-priorities" && event.Event == "passed" {
			passed = true
		}
		if event.Scenario != "originated-priorities" || event.Event != "enqueue" {
			continue
		}
		raw, err := hex.DecodeString(event.Bytes)
		if err != nil {
			t.Fatal(err)
		}
		packet, err := meshcore.PacketFromBytes(raw)
		if err != nil {
			t.Fatal(err)
		}
		s.transmit(one(packet), nil)
		if got := r.next(t); !bytes.Equal(got, raw) {
			t.Fatalf("fixture packet changed: %x, want %x", got, raw)
		}
		r.mu.Lock()
		job := r.jobs[len(r.jobs)-1]
		r.mu.Unlock()
		if job.priority != event.Priority {
			t.Fatalf("actual native fixture %s priority %d, got %d", event.Bytes, event.Priority, job.priority)
		}
		count++
	}
	if count < 16 {
		t.Fatalf("native priority fixture count %d, need at least 16 baseline cases", count)
	}
	if !passed {
		t.Fatal("native originated-priorities scenario did not report completion")
	}
}

func TestNativeRoomDoesNotRepeatAndRepeaterRejectsUnknownScope(t *testing.T) {
	_, roomRadio, _ := startRole(t, true, config(t))
	roomRadio.inject(t, []byte{0x15, 0, 0x22, 0x33, 0x44}, false)
	roomRadio.quiet(t)
	_, repeaterRadio, _ := startRole(t, false, config(t))
	// Transport-flood header, unknown transport codes, zero-hop ordinary path.
	repeaterRadio.inject(t, []byte{0x14, 0x12, 0x34, 0, 0, 0, 0x22, 0x33, 0x44}, false)
	repeaterRadio.quiet(t)
}

func TestLocalReflectionBypassesRXScoreAndHold(t *testing.T) {
	cfg := config(t)
	base := float32(2)
	cfg.Policy.RXDelay = &base
	calls := 0
	cfg.RXScore = func(p *meshcore.Packet, size int, airtime uint32) (float32, error) {
		calls++
		if p.SNR == -32 && p.RSSI == 127 {
			t.Error("local reflection reached physical scoring")
		}
		if size != 8 || airtime != 100 {
			t.Errorf("scoring inputs changed: size=%d airtime=%d", size, airtime)
		}
		return .25, nil
	}
	s, _, _ := startRole(t, false, cfg)
	local := &meshcore.Packet{HasSignalInfo: true, SNR: -32, RSSI: 127}
	if delay := s.rxDelay(local, 8, 100); delay != 0 || calls != 0 {
		t.Fatalf("local reflection received hold=%s score calls=%d", delay, calls)
	}
	physical := &meshcore.Packet{HasSignalInfo: true, SNR: 4.5, RSSI: -73}
	// Actual native-delay-kernels vector: base 2, score .25, airtime 100.
	if delay := s.rxDelay(physical, 8, 100); delay != 51*time.Millisecond || calls != 1 {
		t.Fatalf("physical RX hold=%s score calls=%d, want 51ms/1", delay, calls)
	}
}

func TestPolicyManagementPersistsOriginWidthAndKeepsReplyWidth(t *testing.T) {
	cfg := config(t)
	cfg.AdvertInterval = 0
	s, r, id := startRole(t, false, cfg)
	admin := fixedID(2)
	loggedIn(t, s, r, admin, id, false, 1, "admin")
	r.inject(t, textWire(t, admin, id, 2, 4, "set path.hash.mode 2"), false)
	reply := r.next(t)
	body := decrypted(t, reply, admin, id)
	if reply[1] != 0 || string(cstring(body[5:])) != "OK" {
		t.Fatalf("management reply must retain incoming one-byte width: %x / %x", reply, body)
	}
	assertWireDelay(t, r, reply, 1500*time.Millisecond)
	r.inject(t, textWire(t, admin, id, 3, 4, "advert"), false)
	advert := r.next(t)
	if advert[0] != 0x11 || advert[1] != 0x80 {
		t.Fatalf("originated advert must use configured three-byte width: %x", advert)
	}
	assertWireDelay(t, r, advert, 1500*time.Millisecond)
	r.mu.Lock()
	var job wireJob
	for _, recorded := range r.jobs {
		if bytes.Equal(recorded.wire, advert) {
			job = recorded
		}
	}
	r.mu.Unlock()
	if !bytes.Equal(job.wire, advert) || job.priority != 3 {
		t.Fatalf("native originated flood advert priority: %+v", job)
	}
	r.next(t)
	if err := s.Close(); err != nil {
		t.Fatal(err)
	}
	restarted, _, _ := startRole(t, false, cfg)
	restarted.mu.RLock()
	mode := restarted.state.Preferences.PathHashMode
	restarted.mu.RUnlock()
	if mode != 2 {
		t.Fatalf("saved management preference overwritten at restart: %d", mode)
	}
}

func TestExplicitPolicyZeroAndNativeSchedulingPriorities(t *testing.T) {
	cfg := config(t)
	cfg.AdvertInterval = 0
	zero := uint32(0)
	cfg.Policy = policy.Overrides{LocalAdvertSeconds: &zero, FloodAdvertSeconds: &zero}
	s, r, id := startRole(t, true, cfg)
	s.mu.RLock()
	disabled := s.adverts.NextFlood.IsZero() && s.adverts.NextLocal.IsZero()
	s.mu.RUnlock()
	if !disabled {
		t.Fatal("explicit zero advert override did not disable both timers")
	}
	r.mu.Lock()
	startup := r.jobs[0]
	r.mu.Unlock()
	if startup.priority != 0 || startup.delay != 16*time.Second {
		t.Fatalf("native startup zero-hop advert timing: %+v", startup)
	}
	client := fixedID(2)
	r.inject(t, loginWire(t, true, client, id, 1, 0, "room", 1, 0x80, nil), false)
	path := r.next(t)
	if path[0] != 0x21 || path[1] != 0x80 {
		t.Fatalf("three-byte PATH reply: %x", path)
	}
	r.mu.Lock()
	job := r.jobs[len(r.jobs)-1]
	r.mu.Unlock()
	if job.priority != 2 || job.delay != 300*time.Millisecond {
		t.Fatalf("native PATH response eligibility: %+v", job)
	}
}

func TestNativeReadOnlyAdmissionAndExtraDirectACK(t *testing.T) {
	s, r, id := startRole(t, true, config(t))
	admin, client := fixedID(2), fixedID(3)
	loggedIn(t, s, r, admin, id, true, 1, "admin")
	r.inject(t, textWire(t, admin, id, 2, 4, "set allow.read.only on"), false)
	r.next(t)
	r.inject(t, loginWire(t, true, client, id, 1, 0, "wrong", 2, 0, nil), false)
	body := decrypted(t, r.next(t), client, id)
	if !bytes.Equal(body[4:8], []byte{0, 0, 2, 0}) {
		t.Fatalf("native public guest login: %x", body)
	}
	r.inject(t, textWire(t, client, id, 2, 0, "denied"), false)
	r.quiet(t)
	r.inject(t, textWire(t, admin, id, 3, 4, "set multi.acks 1"), false)
	r.next(t)
	r.inject(t, loginWire(t, true, client, id, 3, 0, "room", 2, 0, nil), false)
	r.next(t)
	learnPath(t, s, r, client, id, 0x80, nil)
	r.inject(t, textWire(t, client, id, 4, 0, "two ACKs"), false)
	multi, plain := r.next(t), r.next(t)
	want := ackProof(append([]byte{4, 0, 0, 0, 0}, []byte("two ACKs")...), client.PublicKey())
	if !bytes.Equal(multi, append([]byte{0x2a, 0x80, 0x13}, want...)) ||
		!bytes.Equal(plain, append([]byte{0x0e, 0x80}, want...)) {
		t.Fatalf("native multipart/plain ACK pair: %x / %x", multi, plain)
	}
	assertWireDelay(t, r, multi, 200*time.Millisecond)
	assertWireDelay(t, r, plain, 500*time.Millisecond)
}

func TestRepeaterDiscoveryAndOwnerWire(t *testing.T) {
	s, r, id := startRole(t, false, config(t))
	r.inject(t, []byte{0x2e, 0, 0x81, 4, 0x11, 0x22, 0x33, 0x44}, false)
	reply := r.next(t)
	want := append([]byte{0x2e, 0, 0x92, 18, 0x11, 0x22, 0x33, 0x44}, id.PublicKeyBytes()[:8]...)
	if !bytes.Equal(reply, want) {
		t.Fatalf("native prefix discovery response: %x want %x", reply, want)
	}

	admin := fixedID(2)
	loggedIn(t, s, r, admin, id, false, 1, "admin")
	r.inject(t, textWire(t, admin, id, 2, 4, "set owner.info First|Second"), false)
	r.next(t)
	r.inject(t, peerWire(t, 0, admin, id, []byte{3, 0, 0, 0, 7}), false)
	body := decrypted(t, r.next(t), admin, id)
	if string(cstring(body[4:])) != buildinfo.HostVersion+"\nHost\nFirst\nSecond" {
		t.Fatalf("owner-info level-2 body: %x", body)
	}
}

func TestRepeaterDiscoverCLIArguments(t *testing.T) {
	s, r, id := startRole(t, false, config(t))
	admin := fixedID(2)
	loggedIn(t, s, r, admin, id, false, 1, "admin")

	r.inject(t, textWire(t, admin, id, 2, 4, "discover.neighbors now"), false)
	body := decrypted(t, r.next(t), admin, id)
	if got := string(cstring(body[5:])); got != "Err - discover.neighbors has no options" {
		t.Fatalf("invalid discovery reply %q", got)
	}
	r.quiet(t)

	r.inject(t, textWire(t, admin, id, 3, 4, "discover.neighbors   "), false)
	var replied, discovered bool
	for range 2 {
		wire := r.next(t)
		packet, err := meshcore.PacketFromBytes(wire)
		if err != nil {
			t.Fatal(err)
		}
		if packet.PayloadType() == meshcore.PayloadTypeControl {
			if len(packet.Payload) < 2 || !bytes.Equal(packet.Payload[:2], []byte{0x80, 4}) {
				t.Fatalf("discovery packet %x", wire)
			}
			discovered = true
			continue
		}
		body := decrypted(t, wire, admin, id)
		if got := string(cstring(body[5:])); got != "OK - Discover sent" {
			t.Fatalf("discovery reply %q", got)
		}
		replied = true
	}
	if !discovered || !replied {
		t.Fatalf("discovery packet=%v reply=%v", discovered, replied)
	}
	r.quiet(t)
}

func TestRegionManagementLoadAndAtomicScopeSelection(t *testing.T) {
	s, r, id := startRole(t, false, config(t))
	admin := fixedID(2)
	loggedIn(t, s, r, admin, id, false, 1, "admin")
	for i, command := range []string{"region put world", "region put local world", "region default local", "region denyf *", "region list allowed"} {
		r.inject(t, textWire(t, admin, id, uint32(i+2), 4, command), false)
		raw := r.next(t)
		body := decrypted(t, raw, admin, id)
		if i == 4 && string(cstring(body[5:])) != "world,local" {
			t.Fatalf("region list response %x", body)
		}
	}
	s.mu.RLock()
	if s.state.Preferences.DefaultScope.Name != "local" || s.state.Preferences.WildcardFlags != 1 {
		s.mu.RUnlock()
		t.Fatal("region scope management not applied")
	}
	s.mu.RUnlock()
	r.inject(t, textWire(t, admin, id, 8, 4, "region load"), false)
	r.quiet(t)
	r.inject(t, textWire(t, admin, id, 9, 4, " replacement F"), false)
	r.quiet(t)
	r.inject(t, textWire(t, admin, id, 10, 4, "  child F"), false)
	r.quiet(t)
	r.inject(t, textWire(t, admin, id, 11, 4, ""), false)
	body := decrypted(t, r.next(t), admin, id)
	if string(cstring(body[5:])) != "OK - loaded 2 regions" {
		t.Fatalf("region load termination %x", body)
	}
	s.mu.RLock()
	defer s.mu.RUnlock()
	regions := s.state.Preferences.Regions
	if len(regions) != 2 || regions[0].Name != "replacement" || regions[1].Parent != regions[0].ID {
		t.Fatalf("region load tree %+v", regions)
	}
}

func TestRegionDefRejectsMalformedNativeJumpTokens(t *testing.T) {
	s, r, id := startRole(t, false, config(t))
	admin := fixedID(2)
	loggedIn(t, s, r, admin, id, false, 1, "admin")
	for i, tc := range []struct {
		command string
		reply   string
	}{
		{"region def east west", "*^ F\n east F\n  west F\n"},
		{"region def |east", "Err - empty name"},
		{"region def leaf|east|west", "Err - unknown jump: east|west"},
		{"region def leaf,east,west", "Err - unknown jump: east,west"},
		{"region def leaf|east", "*^ F\n east F\n  west F\n leaf F\n"},
	} {
		r.inject(t, textWire(t, admin, id, uint32(i+2), 4, tc.command), false)
		body := decrypted(t, r.next(t), admin, id)
		if got := string(cstring(body[5:])); got != tc.reply {
			t.Fatalf("%q reply %q, want %q", tc.command, got, tc.reply)
		}
		s.mu.RLock()
		want := 2
		if i == 4 {
			want = 3
		}
		if len(s.state.Preferences.Regions) != want {
			s.mu.RUnlock()
			t.Fatalf("%q left %d regions, want %d", tc.command, len(s.state.Preferences.Regions), want)
		}
		s.mu.RUnlock()
	}
}

func TestRegionPrefixSelectionRepliesWithResolvedName(t *testing.T) {
	s, r, id := startRole(t, false, config(t))
	admin := fixedID(2)
	loggedIn(t, s, r, admin, id, false, 1, "admin")
	for i, tc := range []struct {
		command string
		reply   string
	}{
		{"region put western", "OK - (flood allowed)"},
		{"region put eastern", "OK - (flood allowed)"},
		{"region home wes", " home is now western"},
		{"region default eas", " default scope is now eastern"},
		{"region home", " home is western"},
		{"region default", " default scope is eastern"},
	} {
		r.inject(t, textWire(t, admin, id, uint32(i+2), 4, tc.command), false)
		body := decrypted(t, r.next(t), admin, id)
		if got := string(cstring(body[5:])); got != tc.reply {
			t.Fatalf("%q reply %q, want %q", tc.command, got, tc.reply)
		}
	}
	s.mu.RLock()
	defer s.mu.RUnlock()
	if s.state.HomeRegion != s.state.Preferences.Regions[0].ID ||
		s.state.Preferences.DefaultScope.Name != "eastern" {
		t.Fatalf("prefix-selected regions not active: home %d scope %+v",
			s.state.HomeRegion, s.state.Preferences.DefaultScope)
	}
}

func TestNativeNeighbourQueryUsesMeasuredZeroHopAdverts(t *testing.T) {
	s, r, id := startRole(t, false, config(t))
	peer := fixedID(3)
	app, err := (&meshcore.AdvertAppData{Type: "REPEATER", Name: "Neighbour"}).ToBytes()
	if err != nil {
		t.Fatal(err)
	}
	advert := &meshcore.Advert{PublicKey: peer.Identity, Timestamp: 123, RawAppData: app}
	advert.SignWith(peer)
	payload, err := advert.ToBytes()
	if err != nil {
		t.Fatal(err)
	}
	r.inject(t, append([]byte{0x12, 0}, payload...), false)
	await(t, func() bool { s.mu.RLock(); defer s.mu.RUnlock(); return len(s.neighbours) == 1 })
	admin := fixedID(2)
	loggedIn(t, s, r, admin, id, false, 1, "admin")
	r.inject(t, peerWire(t, 0, admin, id, []byte{2, 0, 0, 0, 6, 0, 1, 0, 0, 2, 6, 0, 0, 0, 0}), false)
	body := decrypted(t, r.next(t), admin, id)
	if !bytes.Equal(body[:8], []byte{2, 0, 0, 0, 1, 0, 1, 0}) || !bytes.Equal(body[8:14], peer.PublicKeyBytes()[:6]) || body[18] != 18 {
		t.Fatalf("native neighbour v0 response %x", body)
	}
}
