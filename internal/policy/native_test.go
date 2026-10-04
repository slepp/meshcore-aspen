package policy

import (
	"bufio"
	"bytes"
	"encoding/hex"
	"encoding/json"
	"math"
	"os"
	"testing"

	"github.com/meshcore-go/meshcore-go/hardware"
)

// The checked-in corpus comes from the actual pinned native runner. Set
// MESHCORE_POLICY_ORACLE to freshly generated events.jsonl for the live native
// gate; ordinary unit runs check the provenance-backed snapshot without needing
// a C++ compiler or firmware dependencies.
func TestNativeDifferential(t *testing.T) {
	path := os.Getenv("MESHCORE_POLICY_ORACLE")
	if path == "" {
		path = "../../testdata/parity/native-events.jsonl"
	}
	f, err := os.Open(path)
	if err != nil {
		t.Fatal(err)
	}
	defer f.Close()
	counts := make(map[string]int)
	passed := make(map[string]bool)
	var nativeTrace TracePath
	traceWidths := make(map[uint8]bool)
	scanner := bufio.NewScanner(f)
	for line := 1; scanner.Scan(); line++ {
		var e struct {
			Scenario           string  `json:"scenario"`
			Event              string  `json:"event"`
			Encoded            uint8   `json:"encoded"`
			Valid              bool    `json:"valid"`
			Bytes              string  `json:"bytes"`
			Priority           uint8   `json:"priority"`
			AirtimeMS          uint32  `json:"airtime_ms"`
			RNG                uint32  `json:"rng_u32"`
			RNGMin             *uint32 `json:"rng_min"`
			FloodMax           *uint32 `json:"flood_max_exclusive"`
			RepeaterDirectMax  *uint32 `json:"repeater_direct_max_exclusive"`
			RoomDirectMax      *uint32 `json:"room_direct_max_exclusive"`
			CompanionDirectMax *uint32 `json:"companion_direct_max_exclusive"`
			FloodMS            uint32  `json:"flood_ms"`
			RepeaterDirectMS   uint32  `json:"repeater_direct_ms"`
			RoomDirectMS       uint32  `json:"room_direct_ms"`
			CompanionDirectMS  uint32  `json:"companion_direct_ms"`
			Base               float32 `json:"base"`
			Score              float32 `json:"score"`
			SNR                float32 `json:"snr"`
			SF                 uint8   `json:"sf"`
			Length             int     `json:"length"`
			DelayMS            int32   `json:"delay_ms"`
			Route              uint8   `json:"route"`
			Scope              uint8   `json:"scope"`
			Hops               uint8   `json:"hops"`
			Denied             bool    `json:"denied"`
			RequestFlags       uint8   `json:"request_flags"`
			Name               string  `json:"name"`
			Key                string  `json:"key"`
			TransportCode      uint16  `json:"transport_code"`
			PayloadType        uint8   `json:"payload_type"`
			Payload            string  `json:"payload"`
			Code               uint16  `json:"code"`
			Mode               uint8   `json:"mode"`
			Width              uint8   `json:"width"`
			Path               string  `json:"path"`
			SelfPublicKey      string  `json:"self_public_key"`
			Looped             bool    `json:"looped"`
			AirtimeFactor      float32 `json:"airtime_factor"`
			Float32Bits        uint32  `json:"float32_bits"`
			Flags              uint8   `json:"flags"`
			HashWidth          uint8   `json:"hash_width"`
		}
		if err := json.Unmarshal(scanner.Bytes(), &e); err != nil {
			t.Fatalf("native event line %d: %v", line, err)
		}
		if e.Event == "passed" {
			passed[e.Scenario] = true
		}
		switch {
		case e.Scenario == "ordinary-path-encoding" && e.Event == "path_valid":
			counts["paths"]++
			size := int((e.Encoded>>6)+1) * int(e.Encoded&63)
			_, err := ParsePath(e.Encoded, make([]byte, size))
			if (err == nil) != e.Valid {
				t.Errorf("native path encoding %02x: valid=%v, Go error=%v", e.Encoded, e.Valid, err)
			}
		case e.Scenario == "originated-priorities" && e.Event == "enqueue":
			counts["priorities"]++
			raw, err := hex.DecodeString(e.Bytes)
			if err != nil || len(raw) < 2 {
				t.Fatalf("invalid native packet bytes line %d", line)
			}
			route, typ := Route(raw[0]&3), (raw[0]>>2)&15
			zeroHop := !route.IsFlood() && raw[1] == 0
			got, err := OriginatedPriority(route, typ, zeroHop)
			if err != nil || got != e.Priority {
				t.Errorf("native originated priority %s: %d %v, want %d", e.Bytes, got, err, e.Priority)
			}
		case e.Scenario == "trace-origin" && e.Event == "enqueue":
			counts["trace-origin"]++
			raw, err := hex.DecodeString(e.Bytes)
			if err != nil || len(raw) < 12 || raw[1] != 0 {
				t.Fatalf("invalid native TRACE bytes line %d", line)
			}
			route, typ := Route(raw[0]&3), (raw[0]>>2)&15
			if typ != payloadTrace {
				t.Fatal("native TRACE fixture has the wrong payload type")
			}
			// TRACE stores hashes in its payload: ordinary path_len=0 does
			// not imply the sendZeroHop API or its priority zero.
			got, err := OriginatedPriority(route, typ, false)
			if err != nil || got != e.Priority {
				t.Errorf("native TRACE priority: got%d %v want%d", got, err, e.Priority)
			}
			nativeTrace, err = ParseTracePath(raw[10], raw[11:])
			if err != nil {
				t.Fatal(err)
			}
		case e.Scenario == "trace-origin" && e.Event == "trace_inputs":
			counts["trace-inputs"]++
			hashes, err := hex.DecodeString(e.Path)
			if err != nil || nativeTrace.Flags != e.Flags || nativeTrace.Width() != e.HashWidth ||
				!bytes.Equal(nativeTrace.Hashes, hashes) {
				t.Fatalf("native TRACE flags/hash payload differs line %d", line)
			}
			path, err := NewTracePath(e.HashWidth, hashes)
			if err != nil || path.Flags != nativeTrace.Flags {
				t.Fatalf("native TRACE width %d cannot be requested explicitly line %d: %v", e.HashWidth, line, err)
			}
			traceWidths[e.HashWidth] = true
		case e.Scenario == "native-delay-kernels" && e.Event == "delay":
			counts["tx-delay"]++
			for _, tt := range []struct {
				profile Profile
				direct  bool
				want    uint32
				max     *uint32
			}{
				{Repeater, false, e.FloodMS, e.FloodMax}, {Repeater, true, e.RepeaterDirectMS, e.RepeaterDirectMax},
				{Room, true, e.RoomDirectMS, e.RoomDirectMax}, {Companion, true, e.CompanionDirectMS, e.CompanionDirectMax},
			} {
				if e.RNGMin == nil || tt.max == nil {
					t.Fatal("native delay corpus lacks observed RNG bounds; regenerate with current oracle")
				}
				got, err := RetransmitDelayMillis(tt.profile, Defaults(tt.profile), tt.direct, e.AirtimeMS,
					func(min, max uint32) uint32 {
						if min != *e.RNGMin || max != *tt.max {
							t.Errorf("native RNG bounds %s direct=%v air=%d: got[%d,%d) want[%d,%d)",
								tt.profile, tt.direct, e.AirtimeMS, min, max, *e.RNGMin, *tt.max)
						}
						return e.RNG%(max-min) + min
					})
				if err != nil || got != tt.want {
					t.Errorf("native delay %s direct=%v air=%d random=%d: got%d %v want%d",
						tt.profile, tt.direct, e.AirtimeMS, e.RNG, got, err, tt.want)
				}
			}
		case e.Scenario == "native-delay-kernels" && e.Event == "rx_delay":
			counts["rx-delay"]++
			p := Defaults(Repeater)
			p.RXDelay = e.Base
			got, err := RXDelayMillis(p, e.Score, e.AirtimeMS)
			if err != nil || got != e.DelayMS {
				t.Errorf("native RX base=%g score=%g air=%d: got%d %v want%d", e.Base, e.Score, e.AirtimeMS, got, err, e.DelayMS)
			}
		case e.Scenario == "snr-score-kernel" && e.Event == "score":
			counts["score"]++
			got := float32(hardware.PacketScore(float64(e.SNR), e.SF, e.Length))
			// Native std::to_string emits six fractional digits.
			if math.Abs(float64(got-e.Score)) > 0.0000006 {
				t.Errorf("native score SNR%g SF%d length%d: got%g want%g", e.SNR, e.SF, e.Length, got, e.Score)
			}
		case e.Scenario == "repeater-loop-kernel" && e.Event == "loop":
			counts["loops"]++
			pathBytes, err := hex.DecodeString(e.Path)
			if err != nil {
				t.Fatal(err)
			}
			path, err := ParsePath(e.Encoded, pathBytes)
			if err != nil || path.Width() != e.Width || e.Mode < 1 || e.Mode > 3 {
				t.Fatalf("invalid native loop fixture line %d", line)
			}
			keyBytes, err := hex.DecodeString(e.SelfPublicKey)
			if err != nil || len(keyBytes) != 32 {
				t.Fatalf("invalid native loop public key line %d", line)
			}
			var key [32]byte
			copy(key[:], keyBytes)
			p := Defaults(Repeater)
			p.Loop = LoopPolicy(e.Mode)
			ctx := ReceiveContext{Route: Flood, Path: path, Unscoped: true}
			if allowed := AllowForward(Repeater, p, ctx, 2, key); allowed == e.Looped {
				t.Errorf("native loop mode%d width%d path%s: looped%v, Go allowed%v", e.Mode, e.Width, e.Path, e.Looped, allowed)
			}
		case e.Scenario == "native-runtime-airtime-factor" && e.Event == "runtime_af":
			counts["runtime-af"]++
			for _, profile := range []Profile{Repeater, Room} {
				p, err := ApplyOverrides(profile, Defaults(profile), Overrides{AirtimeFactor: &e.AirtimeFactor})
				if err != nil {
					t.Fatalf("native runtime AF%g rejected: %v", e.AirtimeFactor, err)
				}
				data, err := json.Marshal(p)
				if err != nil {
					t.Fatal(err)
				}
				var restored Preferences
				if err := json.Unmarshal(data, &restored); err != nil {
					t.Fatal(err)
				}
				if math.Float32bits(restored.AirtimeFactor) != e.Float32Bits {
					t.Errorf("native AF%g bits%08x became%08x in durable host preferences",
						e.AirtimeFactor, e.Float32Bits, math.Float32bits(restored.AirtimeFactor))
				}
			}
		case e.Scenario == "routing-policy-helpers" && e.Event == "hop_admission":
			counts["hops"]++
			p := Defaults(Room)
			p.Repeat, p.FloodMaxHops, p.UnscopedMaxHops, p.AdvertMaxHops = true, 8, 3, 2
			path, err := NewPath(3, make([]byte, int(e.Hops)*3))
			if err != nil {
				t.Fatal(err)
			}
			ctx := ReceiveContext{Route: Route(e.Route), Path: path, ScopeKnown: true, Unscoped: true}
			if got := AllowForward(Room, p, ctx, 1, [32]byte{}); got == e.Denied {
				t.Errorf("native hop admission route%d hops%d denied%v: Go allowed%v", e.Route, e.Hops, e.Denied, got)
			}
		case e.Scenario == "routing-policy-helpers" && e.Event == "reply":
			counts["replies"]++
			gotRoute := ChooseReplyRoute(e.RequestFlags&1 != 0, e.RequestFlags&2 != 0, e.RequestFlags&4 != 0)
			if uint8(gotRoute) != e.Route {
				t.Errorf("native reply route flags%d: got%d want%d", e.RequestFlags, gotRoute, e.Route)
			}
			request, fallback := Scope{Key: [16]byte{1}}, Scope{}
			if e.RequestFlags&4 != 0 {
				fallback.Key[0] = 2
			}
			got := ChooseReplyScope(ReceiveContext{ScopeKnown: e.RequestFlags&1 != 0, Unscoped: e.RequestFlags&2 != 0, Scope: request}, fallback)
			want := Scope{}
			if e.Scope == 0 {
				want = request
			} else if e.Scope == 1 {
				want = fallback
			}
			if got != want {
				t.Errorf("native reply scope flags%d: got%v want%v", e.RequestFlags, got, want)
			}
		case e.Scenario == "region-keys-storage" && e.Event == "region":
			counts["regions"]++
			scope := AutoScope(e.Name)
			if hex.EncodeToString(scope.Key[:]) != e.Key || TransportCode(scope, 1, []byte("hello")) != e.TransportCode {
				t.Errorf("native public scope %s: derived key or HMAC transport code differs", e.Name)
			}
		case e.Scenario == "region-keys-storage" && e.Event == "transport_code":
			counts["transport-code"]++
			payload, err := hex.DecodeString(e.Payload)
			if err != nil {
				t.Fatal(err)
			}
			scope := AutoScope(e.Name)
			if hex.EncodeToString(scope.Key[:]) != e.Key || TransportCode(scope, e.PayloadType, payload) != e.Code {
				t.Errorf("native transport code %s type%d payload%s differs", e.Name, e.PayloadType, e.Payload)
			}
		}
	}
	if err := scanner.Err(); err != nil {
		t.Fatal(err)
	}
	for scenario, min := range map[string]int{"paths": 256, "priorities": 16, "trace-origin": 1, "trace-inputs": 1, "tx-delay": 7, "rx-delay": 16, "score": 192, "loops": 27, "runtime-af": 5, "hops": 8, "replies": 8, "regions": 1, "transport-code": 1} {
		if counts[scenario] < min {
			t.Errorf("native %s selected %d vectors, require at least %d", scenario, counts[scenario], min)
		}
		if os.Getenv("MESHCORE_POLICY_ORACLE") != "" {
			for _, width := range []uint8{1, 2, 4, 8} {
				if !traceWidths[width] {
					t.Errorf("fresh native oracle omitted TRACE width %d", width)
				}
			}
		}
	}
	for _, scenario := range []string{"crypto-known-answer", "ordinary-path-encoding", "originated-priorities", "trace-origin", "native-delay-kernels", "native-runtime-airtime-factor", "snr-score-kernel", "repeater-loop-kernel", "routing-policy-helpers", "region-keys-storage"} {
		if !passed[scenario] {
			t.Errorf("native scenario %q did not report completion", scenario)
		}
	}
}
