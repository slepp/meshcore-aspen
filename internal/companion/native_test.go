package companion

import (
	"bufio"
	"bytes"
	"context"
	"encoding/hex"
	"encoding/json"
	"fmt"
	"math"
	"os"
	"path/filepath"
	"testing"
	"time"

	meshcore "github.com/meshcore-go/meshcore-go"
	protocol "github.com/meshcore-go/meshcore-go/companion"
)

// The snapshot is emitted by the pinned native Mesh with real crypto.
// An override selects fresh events without making package tests build firmware.
func nativeEventsFile(t *testing.T) *os.File {
	t.Helper()
	path := os.Getenv("MESHCORE_COMPANION_ORACLE")
	if path == "" {
		path = "../../testdata/parity/native-events.jsonl"
	}
	file, err := os.Open(path)
	if err != nil {
		t.Fatal(err)
	}
	return file
}

func TestNativeVoltageTelemetryDifferential(t *testing.T) {
	file := nativeEventsFile(t)
	defer file.Close()
	scanner := bufio.NewScanner(file)
	count, passed := 0, false
	for scanner.Scan() {
		var event struct {
			Scenario   string `json:"scenario"`
			Event      string `json:"event"`
			Millivolts uint16 `json:"millivolts"`
			Bytes      string `json:"bytes"`
		}
		if err := json.Unmarshal(scanner.Bytes(), &event); err != nil {
			t.Fatal(err)
		}
		if event.Scenario != "cayenne-role-telemetry" {
			continue
		}
		if event.Event == "passed" {
			passed = true
		}
		if event.Event != "voltage" {
			continue
		}
		count++
		t.Run(fmt.Sprintf("%dmV", event.Millivolts), func(t *testing.T) {
			expected, err := hex.DecodeString(event.Bytes)
			if err != nil || len(expected) != 4 {
				t.Fatalf("invalid native voltage bytes: %v", err)
			}
			s := &Server{ctx: context.Background(), cfg: Config{
				Battery: func(context.Context) (uint16, error) { return event.Millivolts, nil },
			}}
			actual, err := s.telemetryData(1)
			if err != nil || !bytes.Equal(actual, expected) {
				t.Fatalf("voltage telemetry: Go=%x native=%x error=%v", actual, expected, err)
			}
		})
	}
	if err := scanner.Err(); err != nil {
		t.Fatal(err)
	}
	if !passed || count < 2 {
		t.Fatal("native voltage corpus lacks boundary cases or successful execution")
	}
}

func TestNativeEncryptedTextDifferential(t *testing.T) {
	file := nativeEventsFile(t)
	defer file.Close()
	var senderSeed, recipientSeed, secret, plaintext, expected []byte
	passed := false
	scanner := bufio.NewScanner(file)
	for scanner.Scan() {
		var event struct {
			Scenario      string `json:"scenario"`
			Event         string `json:"event"`
			SenderSeed    string `json:"sender_seed"`
			RecipientSeed string `json:"recipient_seed"`
			Secret        string `json:"secret"`
			Plaintext     string `json:"plaintext"`
			Bytes         string `json:"bytes"`
		}
		if err := json.Unmarshal(scanner.Bytes(), &event); err != nil {
			t.Fatal(err)
		}
		if event.Scenario != "encrypted-wire-vectors" {
			continue
		}
		decode := func(value string) []byte {
			data, err := hex.DecodeString(value)
			if err != nil {
				t.Fatal(err)
			}
			return data
		}
		switch event.Event {
		case "datagram_inputs":
			if plaintext != nil {
				t.Fatal("multiple native input vectors need explicit pairing")
			}
			senderSeed, recipientSeed = decode(event.SenderSeed), decode(event.RecipientSeed)
			secret, plaintext = decode(event.Secret), decode(event.Plaintext)
		case "encrypted_packet":
			if expected != nil {
				t.Fatal("multiple native outputs need explicit pairing")
			}
			expected = decode(event.Bytes)
		case "passed":
			passed = true
		}
	}
	if err := scanner.Err(); err != nil {
		t.Fatal(err)
	}
	if !passed || len(senderSeed) != 32 || len(recipientSeed) != 32 || len(secret) != 32 || len(plaintext) < 6 || len(expected) == 0 {
		t.Fatal("native encrypted-wire corpus is missing inputs, output or successful execution")
	}
	sender := meshcore.NewLocalIdentityFromSeed([32]byte(senderSeed))
	recipient := meshcore.NewLocalIdentityFromSeed([32]byte(recipientSeed))
	s, r, _ := startTestServer(t, sender, testConfig(""))
	c := commandSession(t, s)
	gotSecret, err := s.Node.SharedSecret(recipient.Identity)
	if err != nil || !bytes.Equal(gotSecret, secret) {
		t.Fatal("Go/native ECDH mismatch")
	}
	contact := &contact{ContactResponse: protocol.ContactResponse{
		PublicKey: recipient.PublicKey(), Type: meshcore.AdvertTypeChat,
		OutPathLen: 0x82, OutPath: [64]byte{1, 2, 3, 4, 5, 6},
	}}
	s.mu.Lock()
	_, err = s.sendText(c, contact, plaintext[4]>>2, plaintext[4]&3, u32(plaintext[:4]), plaintext[5:])
	s.mu.Unlock()
	if err != nil {
		t.Fatal(err)
	}
	packet := r.packet(t)
	actual, err := packet.ToBytes()
	if err != nil {
		t.Fatal(err)
	}
	if !bytes.Equal(actual, expected) {
		t.Fatalf("native encrypted text mismatch\nGo:     %x\nnative: %x", actual, expected)
	}
	r.mu.Lock()
	job := r.jobs[len(r.jobs)-1]
	r.mu.Unlock()
	if job.Priority != 0 || job.Delay != 0 {
		t.Fatalf("native direct text scheduling mismatch: %+v", job)
	}
}

func TestNativeBaseChatACKDifferential(t *testing.T) {
	file := nativeEventsFile(t)
	defer file.Close()
	type event struct {
		Scenario          string `json:"scenario"`
		Event             string `json:"event"`
		Mode              int    `json:"mode"`
		ClockMS           uint32 `json:"clock_ms"`
		ExtraACKs         byte   `json:"extra_acks"`
		ReturnPathEncoded *byte  `json:"return_path_encoded"`
		ReturnPath        string `json:"return_path"`
		SenderSeed        string `json:"sender_seed"`
		RecipientSeed     string `json:"recipient_seed"`
		SharedSecret      string `json:"shared_secret"`
		NonceIndex        *int   `json:"ack_payload_nonce_index"`
		Bytes             string `json:"bytes"`
		Priority          uint8  `json:"priority"`
		EligibleMS        uint32 `json:"eligible_ms"`
	}
	type scenario struct {
		input   event
		outputs []event
	}
	var cases []scenario
	passed := false
	scanner := bufio.NewScanner(file)
	for scanner.Scan() {
		var e event
		if err := json.Unmarshal(scanner.Bytes(), &e); err != nil {
			t.Fatal(err)
		}
		if e.Scenario != "base-chat-wire-ack-timing" {
			continue
		}
		switch e.Event {
		case "ack_input":
			cases = append(cases, scenario{input: e})
		case "enqueue":
			if len(cases) == 0 {
				t.Fatal("native ACK output is missing its input")
			}
			last := &cases[len(cases)-1]
			last.outputs = append(last.outputs, e)
		case "passed":
			passed = true
		}
	}
	if err := scanner.Err(); err != nil {
		t.Fatal(err)
	}
	if !passed || len(cases) != 4 {
		t.Fatal("native corpus must contain all four executed BaseChatMesh ACK scenarios")
	}
	modes := make(map[int]bool)
	for _, tc := range cases {
		if modes[tc.input.Mode] || tc.input.Mode < 0 || tc.input.Mode > 3 {
			t.Fatal("native ACK modes are duplicated or unknown")
		}
		modes[tc.input.Mode] = true
		t.Run(fmt.Sprintf("mode%d", tc.input.Mode), func(t *testing.T) {
			decode := func(value string) []byte {
				data, err := hex.DecodeString(value)
				if err != nil {
					t.Fatal(err)
				}
				return data
			}
			input := tc.input
			senderSeed, recipientSeed := decode(input.SenderSeed), decode(input.RecipientSeed)
			secret := decode(input.SharedSecret)
			if len(senderSeed) != 32 || len(recipientSeed) != 32 || len(secret) != 32 || input.ReturnPathEncoded == nil || input.NonceIndex == nil || *input.NonceIndex != 5 {
				t.Fatal("native ACK corpus is missing explicit cryptographic/path/nonce inputs")
			}
			expectedCount := 1
			if input.ExtraACKs > 0 {
				expectedCount++
			}
			if len(tc.outputs) != expectedCount {
				t.Fatal("native ACK outputs are incomplete")
			}
			sender := meshcore.NewLocalIdentityFromSeed([32]byte(senderSeed))
			recipient := meshcore.NewLocalIdentityFromSeed([32]byte(recipientSeed))
			s, r, address := startTestServer(t, recipient, testConfig(""))
			client := testClient(t, address)
			if err := client.AddUpdateContactFull(testContext(t), protocol.AddUpdateContactCommand{
				PublicKey: sender.PublicKey(), Type: meshcore.AdvertTypeChat, Name: "Native sender",
				OutPathLen: *input.ReturnPathEncoded, OutPath: decode(input.ReturnPath),
			}); err != nil {
				t.Fatal(err)
			}
			requireOK(t, runCommand(t, s, []byte{protocol.CmdSetOtherParams, 0, 0, 0, input.ExtraACKs}))
			actualSecret, err := s.Node.SharedSecret(sender.Identity)
			if err != nil || !bytes.Equal(actualSecret, secret) {
				t.Fatal("native ACK input shared secret mismatch")
			}
			packet, err := meshcore.PacketFromBytes(decode(input.Bytes))
			if err != nil {
				t.Fatal(err)
			}
			r.mu.Lock()
			before := len(r.jobs)
			r.mu.Unlock()
			r.inject(packet)
			for i, out := range tc.outputs {
				actual := r.packet(t)
				expected, err := meshcore.PacketFromBytes(decode(out.Bytes))
				if err != nil {
					t.Fatal(err)
				}
				got, want := normalizedACK(t, actual, secret, *input.NonceIndex), normalizedACK(t, expected, secret, *input.NonceIndex)
				if !bytes.Equal(got, want) {
					t.Fatalf("ACK wire/decoded-PATH differs beyond fresh nonce\nGo: %x\nnative: %x", got, want)
				}
				r.mu.Lock()
				job := r.jobs[before+i]
				r.mu.Unlock()
				if out.EligibleMS < input.ClockMS || job.Priority != out.Priority || job.Delay != time.Duration(out.EligibleMS-input.ClockMS)*time.Millisecond {
					t.Fatalf("ACK schedule priority%d delay%s, native priority%d eligible%d clock%d", job.Priority, job.Delay, out.Priority, out.EligibleMS, input.ClockMS)
				}
			}
			messages, err := client.GetWaitingMessages(testContext(t))
			if err != nil || len(messages) != 1 || messages[0].Contact == nil || messages[0].Contact.Text != "hello" {
				t.Fatalf("native encrypted input was not delivered: %+v %v", messages, err)
			}
			r.mu.Lock()
			count := len(r.jobs) - before
			r.mu.Unlock()
			if count != len(tc.outputs) {
				t.Fatalf("Go submitted %d replies, native submitted%d", count, len(tc.outputs))
			}
		})
	}
}

func normalizedACK(t *testing.T, p *meshcore.Packet, secret []byte, nonceIndex int) []byte {
	t.Helper()
	q := p.Clone()
	switch q.PayloadType() {
	case meshcore.PayloadTypeAck:
		if len(q.Payload) != 6 {
			t.Fatal("unexpected native text ACK length")
		}
		q.Payload[nonceIndex] = 0
	case meshcore.PayloadTypeMultiPart:
		part, err := meshcore.MultiPartFromBytes(q.Payload)
		if err != nil || part.WrappedType != meshcore.PayloadTypeAck || len(part.WrappedPayload) != 6 {
			t.Fatal("unexpected multipart ACK")
		}
		q.Payload[1+nonceIndex] = 0
	case meshcore.PayloadTypePath:
		if len(q.Payload) < 4 {
			t.Fatal("short encrypted ACK path")
		}
		plain, err := meshcore.MACThenDecrypt(secret, q.Payload[2:])
		if err != nil {
			t.Fatal(err)
		}
		path, err := meshcore.ParsePathPayload(plain)
		if err != nil || path.ExtraType != meshcore.PayloadTypeAck || len(path.Extra) < 6 {
			t.Fatal("PATH does not carry an ACK")
		}
		plain[len(plain)-len(path.Extra)+nonceIndex] = 0
		q.Payload = append(append([]byte(nil), q.Payload[:2]...), plain...)
	default:
		t.Fatalf("unexpected ACK packet type%d", q.PayloadType())
	}
	data, err := q.ToBytes()
	if err != nil {
		t.Fatal(err)
	}
	return data
}

func TestNativeRuntimeAirtimePersistsWithoutBootClamp(t *testing.T) {
	file := nativeEventsFile(t)
	defer file.Close()
	values := make(map[float32]uint32)
	passed := false
	scanner := bufio.NewScanner(file)
	for scanner.Scan() {
		var event struct {
			Scenario string  `json:"scenario"`
			Event    string  `json:"event"`
			Factor   float32 `json:"airtime_factor"`
			Bits     *uint32 `json:"float32_bits"`
		}
		if err := json.Unmarshal(scanner.Bytes(), &event); err != nil {
			t.Fatal(err)
		}
		if event.Scenario != "native-runtime-airtime-factor" {
			continue
		}
		if event.Event == "passed" {
			passed = true
		}
		if event.Event == "runtime_af" && (event.Factor == 0 || event.Factor == 99 || event.Factor == 99.25) {
			if event.Bits == nil {
				t.Fatal("native runtime factor is missing its float32 representation")
			}
			values[event.Factor] = *event.Bits
		}
	}
	if err := scanner.Err(); err != nil {
		t.Fatal(err)
	}
	if !passed || len(values) != 3 {
		t.Fatal("native corpus must include executed runtime AF0, AF99 and AF99.25 cases")
	}
	for factor, bits := range values {
		t.Run(fmt.Sprintf("%g", factor), func(t *testing.T) {
			cfg := testConfig(stateDir(t))
			var applied []float64
			open := func() *Server {
				r := &sourceTestRadio{testRadio: newTestRadio(), set: func(_ context.Context, value float64) error {
					applied = append(applied, value)
					return nil
				}}
				s, err := New(testIdentity(1), r, cfg)
				if err != nil {
					t.Fatal(err)
				}
				t.Cleanup(func() {
					if err := s.Close(); err != nil {
						t.Error(err)
					}
				})
				r.packet(t)
				return s
			}
			s := open()
			cmd := make([]byte, 9)
			cmd[0] = protocol.CmdSetTuningParams
			put32(cmd[5:], uint32(factor*1000))
			requireOK(t, runCommand(t, s, cmd))
			if len(applied) == 0 || applied[len(applied)-1] != float64(math.Float32frombits(bits)) {
				t.Fatalf("PHY callback did not receive the native factor: %v", applied)
			}
			if err := s.Close(); err != nil {
				t.Fatal(err)
			}
			data, err := os.ReadFile(filepath.Join(cfg.StateDir, "companion.json"))
			if err != nil {
				t.Fatal(err)
			}
			var stored state
			if err := json.Unmarshal(data, &stored); err != nil {
				t.Fatal(err)
			}
			if math.Float32bits(stored.Preferences.AirtimeFactor) != bits {
				t.Fatal("persistence clamped or truncated native runtime factor")
			}
			previous := len(applied)
			reloaded := open()
			if len(applied) <= previous || applied[len(applied)-1] != float64(math.Float32frombits(bits)) {
				t.Fatalf("reload did not reapply the persisted source factor: %v", applied)
			}
			want := append([]byte{protocol.RespTuningParams}, cmd[1:]...)
			if got := runCommand(t, reloaded, []byte{protocol.CmdGetTuningParams}); !bytes.Equal(got, want) {
				t.Fatalf("persisted runtime tuning mismatch: %x, want %x", got, want)
			}
		})
	}
}
