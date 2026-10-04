package roles

import (
	"bufio"
	"bytes"
	"encoding/hex"
	"encoding/json"
	"fmt"
	"os"
	"testing"

	meshcore "github.com/meshcore-go/meshcore-go"
)

type nativeRoleFixture struct {
	raw                   json.RawMessage
	Scenario              string  `json:"scenario"`
	Event                 string  `json:"event"`
	Bytes                 string  `json:"bytes"`
	Priority              uint8   `json:"priority"`
	BufferSize            int     `json:"buffer_size"`
	InputBytes            int     `json:"input_bytes"`
	RetainedBytes         int     `json:"retained_bytes"`
	InputHex              string  `json:"input_hex"`
	RetainedHex           string  `json:"retained_hex"`
	PlaintextBytes        int     `json:"plaintext_bytes"`
	Accepted              bool    `json:"accepted"`
	EncryptedPayloadBytes int     `json:"encrypted_payload_bytes"`
	MilliVolts            uint16  `json:"millivolts"`
	Celsius               float32 `json:"celsius"`
}

func nativeRoleFixtures(t *testing.T) []nativeRoleFixture {
	t.Helper()
	// The parent differential gate supplies the freshly generated shared corpus.
	path := os.Getenv("MESHCORE_POLICY_ORACLE")
	if path == "" {
		path = "../../testdata/parity/native-events.jsonl"
	}
	file, err := os.Open(path)
	if err != nil {
		t.Fatal(err)
	}
	defer file.Close()
	var fixtures []nativeRoleFixture
	scanner := bufio.NewScanner(file)
	for scanner.Scan() {
		var event nativeRoleFixture
		if err := json.Unmarshal(scanner.Bytes(), &event); err != nil {
			t.Fatal(err)
		}
		event.raw = bytes.Clone(scanner.Bytes())
		fixtures = append(fixtures, event)
	}
	if err := scanner.Err(); err != nil {
		t.Fatal(err)
	}
	return fixtures
}

func TestNativeACLAdmissionFixtures(t *testing.T) {
	type client struct {
		PublicKey    string `json:"public_key"`
		Permissions  byte   `json:"permissions"`
		LastActivity uint32 `json:"last_activity"`
	}
	type admissionCase struct {
		Name       string   `json:"case"`
		Capacity   int      `json:"capacity"`
		Input      []client `json:"input_clients"`
		Admission  *client  `json:"admission"`
		Result     []client `json:"result_clients"`
		Reloaded   []client `json:"reloaded_clients"`
		Comparison string   `json:"comparison"`
	}
	seen := make(map[string]bool)
	passed := false
	for _, event := range nativeRoleFixtures(t) {
		if event.Scenario != "client-acl-vectors" {
			continue
		}
		if event.Event == "passed" {
			passed = true
			continue
		}
		if event.Event != "acl_case" {
			continue
		}
		var tc admissionCase
		if err := json.Unmarshal(event.raw, &tc); err != nil {
			t.Fatal(err)
		}
		seen[tc.Name] = true
		t.Run(tc.Name, func(t *testing.T) {
			cfg := config(t)
			cfg.MaxMembers, cfg.Retention = tc.Capacity, NativeRetention
			s, _, _ := startRole(t, false, cfg)
			put := func(c client) error {
				raw, err := hex.DecodeString(c.PublicKey)
				if err != nil {
					return err
				}
				id, err := meshcore.NewIdentityFromBytes(raw)
				if err != nil {
					return err
				}
				s.mu.Lock()
				defer s.mu.Unlock()
				m, err := s.putMember(id)
				if err == nil {
					m.Permissions, m.LastActivity = c.Permissions, c.LastActivity
				}
				return err
			}
			check := func(service *Service, want []client) {
				t.Helper()
				service.mu.RLock()
				defer service.mu.RUnlock()
				keys := service.memberKeys()
				if len(keys) != len(want) {
					t.Fatalf("membership count %d, native %d", len(keys), len(want))
				}
				for i, expected := range want {
					got := service.state.Members[keys[i]]
					if keys[i] != expected.PublicKey || got.Permissions != expected.Permissions || got.LastActivity != expected.LastActivity {
						t.Fatalf("slot %d: %s/%02x/%d, native %s/%02x/%d", i, keys[i], got.Permissions, got.LastActivity,
							expected.PublicKey, expected.Permissions, expected.LastActivity)
					}
				}
			}
			for _, c := range tc.Input {
				if err := put(c); err != nil {
					t.Fatal(err)
				}
			}
			if tc.Admission != nil {
				err := put(*tc.Admission)
				if tc.Comparison == "adopted-host-rejection-exception" {
					if err == nil {
						t.Fatal("all-admin admission must reject without mutation")
					}
					check(s, tc.Input)
					return
				}
				if err != nil {
					t.Fatal(err)
				}
			}
			check(s, tc.Result)
			s.mu.Lock()
			err := s.save()
			s.mu.Unlock()
			if err != nil {
				t.Fatal(err)
			}
			if err := s.Close(); err != nil {
				t.Fatal(err)
			}
			restarted, _, _ := startRole(t, false, cfg)
			check(restarted, tc.Reloaded)
		})
	}
	for _, name := range []string{"raw-permissions", "least-active", "equal-activity", "all-admin-exception"} {
		if !seen[name] {
			t.Errorf("missing native ACL case %q", name)
		}
	}
	if !passed {
		t.Fatal("native client-acl-vectors scenario did not report completion")
	}
}

// These are actual native StrHelper, Mesh and CayenneLPP outputs, not execution
// of MyMesh's application handlers.
func TestNativeApplicationHelperFixtures(t *testing.T) {
	s, _, _ := startRole(t, true, config(t))
	counts := make(map[string]int)
	passed := make(map[string]bool)
	for i, event := range nativeRoleFixtures(t) {
		if event.Event == "passed" {
			passed[event.Scenario] = true
			continue
		}
		switch event.Scenario {
		case "room-post-copy-boundary", "response-datagram-capacity", "cayenne-role-telemetry":
		default:
			continue
		}
		t.Run(fmt.Sprintf("%s/%s/%d", event.Scenario, event.Event, i), func(t *testing.T) {
			decode := func(value string) []byte {
				t.Helper()
				raw, err := hex.DecodeString(value)
				if err != nil {
					t.Fatal(err)
				}
				return raw
			}
			switch event.Event {
			case "post_copy", "post_copy_utf8":
				if event.BufferSize != 151 {
					t.Fatalf("unexpected native post buffer: %d", event.BufferSize)
				}
				input := bytes.Repeat([]byte{'x'}, event.InputBytes)
				want := bytes.Repeat([]byte{'x'}, event.RetainedBytes)
				if event.Event == "post_copy_utf8" {
					input, want = decode(event.InputHex), decode(event.RetainedHex)
				}
				s.mu.Lock()
				s.addPost(fixedID(2).PublicKey(), string(input))
				got := s.state.History[len(s.state.History)-1].text()
				s.mu.Unlock()
				if !bytes.Equal([]byte(got), want) {
					t.Fatalf("stored post differs from native copy: %x, want %x", got, want)
				}
			case "response_capacity":
				s.mu.Lock()
				packet, err := s.datagram(&member{Key: fixedID(2).PublicKey()}, meshcore.PayloadTypeResponse,
					make([]byte, event.PlaintextBytes))
				s.mu.Unlock()
				if event.Accepted {
					if err != nil || packet == nil {
						t.Fatalf("native accepted %d bytes, host rejected: %v", event.PlaintextBytes, err)
					}
					if len(packet.Payload) != event.EncryptedPayloadBytes {
						t.Fatalf("encrypted payload length %d, native %d", len(packet.Payload), event.EncryptedPayloadBytes)
					}
				} else if err == nil || packet != nil {
					t.Fatalf("host accepted %d bytes rejected by native", event.PlaintextBytes)
				}
			case "voltage", "temperature":
				telemetry := Telemetry{HasBattery: event.Event == "voltage", BatteryMilliVolts: event.MilliVolts,
					HasMCUTemperature: event.Event == "temperature", MCUTemperatureC: event.Celsius}
				want := decode(event.Bytes)
				if len(want) != 4 {
					t.Fatalf("invalid native scalar telemetry fixture: %x", want)
				}
				// The helper vector uses channel 2 for temperature; MyMesh uses
				// TELEM_CHANNEL_SELF (1) for both native role measurements.
				want[0] = 1
				got, err := telemetryPayload(telemetry)
				if err != nil || !bytes.Equal(got, want) {
					t.Fatalf("role telemetry %x (%v), native scalar encoding %x", got, err, want)
				}
			default:
				t.Fatalf("unhandled native helper event %q", event.Event)
			}
			counts[event.Event]++
		})
	}
	for event, minimum := range map[string]int{"post_copy": 4, "post_copy_utf8": 1, "response_capacity": 4, "voltage": 2, "temperature": 1} {
		if counts[event] < minimum {
			t.Errorf("native %s selected %d vectors, require at least %d", event, counts[event], minimum)
		}
	}
	for _, scenario := range []string{"room-post-copy-boundary", "response-datagram-capacity", "cayenne-role-telemetry"} {
		if !passed[scenario] {
			t.Errorf("native scenario %q did not report completion", scenario)
		}
	}
}
