package policy

import (
	"bytes"
	"testing"
	"time"
)

func TestNativeDelayBoundaries(t *testing.T) {
	// Source-derived exact cases, not an executable-native oracle:
	// simple_repeater/MyMesh.cpp:539-554, simple_room_server:278-297,
	// companion_radio:268-280, Utils.cpp:18-22.
	p := Defaults(Repeater)
	p.RXDelay = 10
	for _, tt := range []struct {
		score float32
		air   uint32
		want  int32
	}{{-.15, 123, 1107}, {.85, 1000, 0}, {1.85, 1000, -899}} {
		got, err := RXDelayMillis(p, tt.score, tt.air)
		if err != nil || got != tt.want {
			t.Errorf("RX score %g air %d: %d, %v; want %d", tt.score, tt.air, got, err, tt.want)
		}
	}
	p.RXDelay = 0
	if got, err := RXDelayMillis(p, 0, 1000); err != nil || got != 0 {
		t.Fatalf("disabled RX: %d %v", got, err)
	}
	for _, tt := range []struct {
		profile Profile
		direct  bool
		air     uint32
		max     uint32
	}{
		{Repeater, false, 101, 251}, {Repeater, true, 101, 151},
		{Room, false, 101, 251}, {Room, true, 101, 101},
		{Companion, false, 101, 251}, {Companion, true, 101, 101},
		{Repeater, true, 3, 1}, {Repeater, false, 16777219, 41943051},
	} {
		for _, high := range []bool{false, true} {
			called := false
			got, err := RetransmitDelayMillis(tt.profile, Defaults(tt.profile), tt.direct, tt.air,
				func(min, max uint32) uint32 {
					called = true
					if min != 0 || max != tt.max {
						t.Errorf("RNG interval [%d,%d), want [0,%d)", min, max, tt.max)
					}
					if high {
						return max - 1
					}
					return 0
				})
			want := uint32(0)
			if high {
				want = tt.max - 1
			}
			if err != nil || got != want || !called {
				t.Errorf("%+v high=%v: %d %v called=%v", tt, high, got, err, called)
			}
		}
	}
	if _, err := RetransmitDelayMillis(Repeater, p, false, 100, func(_, max uint32) uint32 { return max }); err == nil {
		t.Fatal("accepted exclusive upper RNG endpoint")
	}
}

func TestNativeOriginAndRelayPriorities(t *testing.T) {
	// Actual native source: Mesh.cpp:637-738 and :344-355.
	for _, tt := range []struct {
		route Route
		typ   uint8
		zero  bool
		want  uint8
	}{
		{Flood, 8, false, 2}, {TransportFlood, 4, false, 3}, {Flood, 2, false, 1},
		{Direct, 8, false, 1}, {Direct, 4, false, 0}, {TransportDirect, 2, false, 0},
		{Direct, 9, false, 5}, {Direct, 8, true, 0},
	} {
		got, err := OriginatedPriority(tt.route, tt.typ, tt.zero)
		if err != nil || got != tt.want {
			t.Errorf("%+v: %d %v", tt, got, err)
		}
	}
	if _, err := OriginatedPriority(Flood, 9, false); err == nil {
		t.Fatal("accepted flooded TRACE")
	}
	path, err := NewPath(3, []byte{1, 2, 3, 4, 5, 6})
	if err != nil {
		t.Fatal(err)
	}
	for _, tt := range []struct {
		route     Route
		typ, want uint8
	}{{Flood, 4, 2}, {Direct, 8, 0}, {Direct, 9, 5}} {
		got, err := RelayedPriority(tt.route, tt.typ, path)
		if err != nil || got != tt.want {
			t.Errorf("relay %+v: %d %v", tt, got, err)
		}
	}
}

func TestRoleAdmissionAndLoopThresholds(t *testing.T) {
	// Native room deliberately omits repeater region and loop gates.
	// Repeater MyMesh.cpp:396-455; room MyMesh.cpp:300-322; companion:485-487.
	var key [32]byte
	key[0], key[1], key[2] = 7, 8, 9
	empty, _ := NewPath(1, nil)
	ctx := ReceiveContext{Route: TransportFlood, Path: empty}
	for _, profile := range []Profile{Repeater, Room, Companion} {
		p := Defaults(profile)
		if AllowForward(profile, p, ctx, 2, key) {
			t.Fatalf("%s forwarded by default without scope", profile)
		}
		p.Repeat = true
		want := profile != Repeater
		if got := AllowForward(profile, p, ctx, 2, key); got != want {
			t.Fatalf("%s scope admission=%v", profile, got)
		}
		ctx.Unscoped = true
		if !AllowForward(profile, p, ctx, 2, key) {
			t.Fatalf("%s rejected permitted flood", profile)
		}
		ctx.Unscoped = false
	}
	for _, tt := range []struct {
		loop  LoopPolicy
		width uint8
		limit int
	}{
		{LoopMinimal, 1, 4}, {LoopMinimal, 2, 2}, {LoopMinimal, 3, 1},
		{LoopModerate, 1, 2}, {LoopModerate, 2, 1}, {LoopModerate, 3, 1},
		{LoopStrict, 1, 1}, {LoopStrict, 2, 1}, {LoopStrict, 3, 1},
	} {
		p := Defaults(Repeater)
		p.Loop = tt.loop
		for _, count := range []int{tt.limit - 1, tt.limit} {
			path, _ := NewPath(tt.width, bytes.Repeat(key[:tt.width], count))
			ctx := ReceiveContext{Route: Flood, Path: path, Unscoped: true}
			if got := AllowForward(Repeater, p, ctx, 2, key); got != (count < tt.limit) {
				t.Errorf("loop %+v count%d: %v", tt, count, got)
			}
		}
	}
	p := Defaults(Repeater)
	for _, tt := range []struct {
		route Route
		typ   uint8
		limit uint8
	}{
		{Flood, 2, 3}, {TransportFlood, 2, 3}, {Flood, 4, 2},
	} {
		p.FloodMaxHops, p.UnscopedMaxHops, p.AdvertMaxHops = 3, 3, 2
		for _, count := range []uint8{tt.limit - 1, tt.limit} {
			path, _ := NewPath(2, make([]byte, int(count)*2))
			ctx := ReceiveContext{Route: tt.route, Path: path, ScopeKnown: true, Unscoped: true}
			if got := AllowForward(Repeater, p, ctx, tt.typ, key); got != (count < tt.limit) {
				t.Errorf("hop limit %+v count%d: %v", tt, count, got)
			}
		}
	}
	for _, width := range []uint8{1, 2, 3} {
		max := 64 / int(width)
		if max > 63 {
			max = 63
		}
		path, _ := NewPath(width, make([]byte, max*int(width)))
		ctx := ReceiveContext{Route: Flood, Path: path, Unscoped: true}
		p := Defaults(Companion)
		p.Repeat = true
		if AllowForward(Companion, p, ctx, 2, key) {
			t.Fatalf("overflow forwarded width%d", width)
		}
	}
	p = Defaults(Repeater)
	p.UnscopedMaxHops = 0
	unscoped := ReceiveContext{Route: Flood, Path: empty, Unscoped: true}
	scoped := ReceiveContext{Route: TransportFlood, Path: empty, ScopeKnown: true}
	if AllowForward(Repeater, p, unscoped, 2, key) || !AllowForward(Repeater, p, scoped, 2, key) {
		t.Fatal("unscoped limit applied to scoped traffic or ignored zero")
	}
	p.AdvertMaxHops = 0
	if AllowForward(Repeater, p, scoped, 4, key) || !AllowForward(Repeater, p, scoped, 2, key) {
		t.Fatal("advert limit applied to ordinary traffic or ignored zero")
	}
	p = Defaults(Room)
	p.Repeat, p.Loop = true, LoopStrict
	selfPath, _ := NewPath(3, key[:3])
	if !AllowForward(Room, p, ReceiveContext{Route: TransportFlood, Path: selfPath}, 2, key) {
		t.Fatal("repeater loop/region gates leaked into room profile")
	}
}

func TestNativeRegionAndReplyScope(t *testing.T) {
	p := Defaults(Repeater)
	first, second := Scope{Name: "$private", Key: [16]byte{1}}, Scope{Name: "$private", Key: [16]byte{2}}
	p.DefaultScope = Scope{Name: "fallback", Key: [16]byte{3}}
	p.Regions = []Region{
		{ID: 1, Name: "$denied", Flags: RegionDenyFlood, Keys: [][16]byte{second.Key}},
		{ID: 2, Parent: 1, Name: "$private", Keys: [][16]byte{first.Key, second.Key}},
	}
	path, _ := NewPath(2, nil)
	payload := []byte{1, 2, 3}
	ctx := ResolveReceiveContext(p, TransportFlood, path, 2, payload, TransportCode(second, 2, payload))
	if !ctx.ScopeKnown || ctx.RegionID != 2 || ChooseReplyScope(ctx, p.DefaultScope) != first {
		t.Fatalf("private match/reply key: %+v", ctx)
	}
	p.Regions[1].Keys[0][0] = 99
	if ctx.Scope != first {
		t.Fatal("queued receive context aliased preferences")
	}
	for _, tt := range []struct {
		route  Route
		denied bool
		want   Scope
	}{
		{Flood, false, Scope{}}, {Flood, true, p.DefaultScope}, {Direct, false, p.DefaultScope}, {TransportFlood, false, p.DefaultScope},
	} {
		p.WildcardFlags = 0
		if tt.denied {
			p.WildcardFlags = RegionDenyFlood
		}
		ctx := ResolveReceiveContext(p, tt.route, path, 2, payload, 0)
		if got := ChooseReplyScope(ctx, p.DefaultScope); got != tt.want {
			t.Errorf("scope %+v: %+v", tt, got)
		}
	}
	if AutoScope("test").Key != AutoScope("#test").Key || !AutoScope("$test").IsNull() {
		t.Fatal("incorrect public/private derivation")
	}
	a, b := SendScope{Override: first}, SendScope{Unscoped: true}
	if a.Resolve(p.DefaultScope) != first || !b.Resolve(p.DefaultScope).IsNull() {
		t.Fatal("session scopes interfered")
	}
	for _, tt := range []struct {
		flood, supplied, learned bool
		want                     ReplyRoute
	}{
		{true, true, true, ReplyPathReturn}, {false, true, true, ReplyDirectSupplied}, {false, false, true, ReplyDirectLearned}, {false, false, false, ReplyFlood},
	} {
		if got := ChooseReplyRoute(tt.flood, tt.supplied, tt.learned); got != tt.want {
			t.Errorf("reply route %+v: %d", tt, got)
		}
	}
}

func TestIndependentAdvertisementSchedules(t *testing.T) {
	// simple_room_server/MyMesh.cpp:810-820,1041-1052; strict comparison is
	// Dispatcher.cpp:382-386. No catch-up burst after a suspended caller.
	start := time.Date(2026, 1, 1, 0, 0, 0, 0, time.UTC)
	p := Defaults(Room)
	s := NewAdvertisementSchedule(start, p)
	next, kind := s.Poll(start.Add(120*time.Second), p)
	if kind != AdvertNone || next != s {
		t.Fatal("advert ran at equality")
	}
	now := start.Add(120*time.Second + time.Millisecond)
	s, kind = s.Poll(now, p)
	if kind != AdvertLocal || !s.NextLocal.Equal(now.Add(120*time.Second)) ||
		!s.NextFlood.Equal(start.Add(169200*time.Second)) {
		t.Fatal("local advert reset flood")
	}
	now = start.Add(169200*time.Second + time.Millisecond)
	s, kind = s.Poll(now, p)
	if kind != AdvertFlood || !s.NextLocal.Equal(now.Add(120*time.Second)) ||
		!s.NextFlood.Equal(now.Add(169200*time.Second)) {
		t.Fatal("flood did not win overlap or reset local")
	}
	if _, kind := s.Poll(now, p); kind != AdvertNone {
		t.Fatal("catch-up burst")
	}
	previousFlood := s.NextFlood
	p.LocalAdvertSeconds = 0
	s = s.RescheduleLocal(now, p)
	if !s.NextLocal.IsZero() || s.NextFlood != previousFlood {
		t.Fatal("local disable disturbed flood")
	}
	p.FloodAdvertSeconds = 0
	s, kind = s.Poll(now.Add(400000*time.Second), p)
	if kind != AdvertNone || !s.NextFlood.IsZero() {
		t.Fatal("disabled flood still ran")
	}
}

func TestTraceEncodingIsIndependent(t *testing.T) {
	for _, tt := range []struct {
		flags uint8
		size  int
	}{{0, 64}, {1, 128}, {2, 176}, {3, 176}} {
		p, err := ParseTracePath(tt.flags, make([]byte, tt.size))
		if err != nil || p.Width() != 1<<tt.flags {
			t.Fatalf("valid trace %+v: %v", tt, err)
		}
	}
	for _, tt := range []struct {
		flags uint8
		size  int
	}{{0, 65}, {1, 3}, {2, 179}, {3, 180}, {0, 0}} {
		if _, err := ParseTracePath(tt.flags, make([]byte, tt.size)); err == nil {
			t.Fatalf("invalid trace accepted %+v", tt)
		}
	}
	ordinary, err := NewPath(3, make([]byte, 6))
	if err != nil || ordinary.Width() != 3 || ordinary.Encoded()>>6 != 2 {
		t.Fatalf("ordinary three-byte path: %+v, %v", ordinary, err)
	}
	for _, tt := range []struct {
		width uint8
		flags uint8
	}{{1, 0}, {2, 1}, {4, 2}, {8, 3}} {
		path, err := NewTracePath(tt.width, make([]byte, 2*int(tt.width)))
		if err != nil || path.Flags != tt.flags || path.Width() != tt.width || path.Count() != 2 {
			t.Fatalf("native TRACE width %d: %+v, %v", tt.width, path, err)
		}
	}
	for _, width := range []uint8{0, 3, 5, 16} {
		if path, err := NewTracePath(width, make([]byte, int(width))); err == nil {
			t.Fatalf("unsupported TRACE width %d silently became %+v", width, path)
		}
	}
	withNativeFlags, err := ParseTracePath(0x81, make([]byte, 4))
	if err != nil || withNativeFlags.Flags != 0x81 || withNativeFlags.Width() != 2 {
		t.Fatalf("native TRACE flags were rewritten: %+v, %v", withNativeFlags, err)
	}
}
