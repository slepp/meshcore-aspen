package policy

import (
	"encoding/json"
	"math"
	"testing"
)

func TestNativeDefaultsAndPresence(t *testing.T) {
	// Native simple_repeater/MyMesh.cpp:890-907, simple_room_server:647-665,
	// companion_radio:878-895 and NodePrefs.h:39.
	for _, tt := range []struct {
		profile      Profile
		repeat       bool
		direct       float32
		local, flood uint32
	}{{Repeater, true, .3, 120, 169200}, {Room, false, .2, 120, 169200}, {Companion, false, .2, 0, 0}} {
		p := Defaults(tt.profile)
		if err := Validate(tt.profile, p); err != nil {
			t.Fatal(err)
		}
		if p.PathHashMode != 0 || p.RXDelay != 0 || p.TXDelay != .5 || p.AirtimeFactor != 1 ||
			p.Repeat != tt.repeat || p.DirectTXDelay != tt.direct ||
			p.LocalAdvertSeconds != tt.local || p.FloodAdvertSeconds != tt.flood {
			t.Fatalf("%s defaults: %+v", tt.profile, p)
		}
	}
	p := Defaults(Repeater)
	p.OwnerInfo = "saved owner"
	var overrides Overrides
	if err := json.Unmarshal([]byte(`{"path_hash_mode":2,"repeat":false,"local_advert_seconds":0}`), &overrides); err != nil {
		t.Fatal(err)
	}
	got, err := ApplyOverrides(Repeater, p, overrides)
	if err != nil {
		t.Fatal(err)
	}
	if got.Repeat || got.LocalAdvertSeconds != 0 || got.PathHashMode != 2 || got.OwnerInfo != "saved owner" || got.FloodAdvertSeconds != 169200 {
		t.Fatalf("presence lost: %+v", got)
	}
}

func TestInvalidPreferences(t *testing.T) {
	for _, change := range []func(*Preferences){
		func(p *Preferences) { p.PathHashMode = 3 },
		func(p *Preferences) { p.RXDelay = float32(math.NaN()) },
		func(p *Preferences) { p.TXDelay = float32(math.Inf(1)) },
		func(p *Preferences) { p.AirtimeFactor = float32(math.NaN()) },
		func(p *Preferences) { p.AirtimeFactor = float32(math.Inf(1)) },
		func(p *Preferences) { p.AirtimeFactor = -1 },
		func(p *Preferences) { p.FloodMaxHops = 65 },
		func(p *Preferences) { p.Version++ },
		func(p *Preferences) {
			p.Regions = []Region{{ID: 1, Parent: 2, Name: "a"}, {ID: 2, Parent: 1, Name: "b"}}
		},
	} {
		p := Defaults(Repeater)
		change(&p)
		if err := Validate(Repeater, p); err == nil {
			t.Fatalf("accepted invalid preferences: %+v", p)
		}
	}
}

func TestRuntimeAirtimeFactorPersistence(t *testing.T) {
	// Native CommonCLI.cpp:450-465 accepts dutycycle 1 (AF99), dutycycle
	// 100 (AF0), and unrestricted set af. Its load-time clamp at line111 is
	// deliberately NOT the durable host preference profile.
	for _, profile := range []Profile{Repeater, Room, Companion} {
		for _, factor := range []float32{0, .1234567, 9, 99, 123.456, math.MaxFloat32} {
			p, err := ApplyOverrides(profile, Defaults(profile), Overrides{AirtimeFactor: &factor})
			if err != nil {
				t.Fatalf("%s runtime factor %g: %v", profile, factor, err)
			}
			data, err := json.Marshal(p)
			if err != nil {
				t.Fatal(err)
			}
			var restored Preferences
			if err := json.Unmarshal(data, &restored); err != nil {
				t.Fatal(err)
			}
			if err := Validate(profile, restored); err != nil {
				t.Fatal(err)
			}
			if restored.AirtimeFactor != factor {
				t.Fatalf("%s factor changed on restart: got%g want%g", profile, restored.AirtimeFactor, factor)
			}
		}
	}
}

func TestPreferenceMergeDoesNotAliasCallerState(t *testing.T) {
	base := Defaults(Repeater)
	base.Regions = []Region{{ID: 1, Name: "$private", Keys: [][16]byte{{1}}}}
	got, err := ApplyOverrides(Repeater, base, Overrides{})
	if err != nil {
		t.Fatal(err)
	}
	base.Regions[0].Keys[0][0] = 2
	if got.Regions[0].Keys[0][0] != 1 {
		t.Fatal("merged preferences retain mutable caller storage")
	}
	zero := []Region{}
	got, err = ApplyOverrides(Repeater, got, Overrides{Regions: &zero})
	if err != nil || len(got.Regions) != 0 {
		t.Fatal("explicit empty region table failed to clear saved entries")
	}
}

func TestOrdinaryPathBoundaries(t *testing.T) {
	// Native Packet.cpp:13-17; count overflow is the approved safety exception.
	for _, tt := range []struct {
		width uint8
		max   int
	}{{1, 63}, {2, 32}, {3, 21}} {
		p, err := NewPath(tt.width, make([]byte, int(tt.width)*tt.max))
		if err != nil || int(p.Count()) != tt.max {
			t.Fatalf("valid boundary: %v %v", p, err)
		}
		if _, err := p.Append(make([]byte, tt.width)); err == nil {
			t.Fatal("accepted overflow")
		}
		empty, err := NewPath(tt.width, nil)
		if err != nil || !empty.Known() || empty.Count() != 0 || empty.Width() != tt.width {
			t.Fatal("lost zero-hop width")
		}
	}
	if UnknownPath().Known() || UnknownPath().Encoded() != 255 {
		t.Fatal("unknown path became direct")
	}
	if _, err := ParsePath(0xc0, nil); err == nil {
		t.Fatal("accepted reserved width")
	}
}
