package main

import (
	"bytes"
	"context"
	"encoding/binary"
	"encoding/hex"
	"encoding/json"
	"errors"
	"fmt"
	"net/http"
	"net/http/httptest"
	"strings"
	"testing"
	"time"

	meshcore "github.com/meshcore-go/meshcore-go"
	protocol "github.com/meshcore-go/meshcore-go/companion"
	"meshcore.local/meshcore/internal/app"
)

func remoteFixture() (app.Config, remoteSnapshot, meshcore.Identity) {
	cfg := app.DefaultConfig()
	s := remoteSnapshot{Version: 1, Publication: 1, Uptime: 100}
	s.Profile.Frequency, s.Profile.Bandwidth = cfg.Radio.FreqHz, cfg.Radio.BwHz
	s.Profile.SF, s.Profile.CR, s.Profile.TxPower = cfg.Radio.SF, cfg.Radio.CR, cfg.TxPower
	s.Profile.Generation, s.Profile.Committed = 7, true
	for i, name := range []string{"companion", "repeater", "room", "bot", "sensor"} {
		key := [32]byte{byte(i + 1)}
		slot := i + 4
		s.Roles = append(s.Roles, remoteRole{Role: name, PublicKey: hex.EncodeToString(key[:]), Ready: true, Slot: &slot, Generation: uint32(i + 10)})
	}
	s.Stream = &remoteSource{Slot: 9, Generation: 99, Negotiated: true, Connected: true}
	return cfg, s, meshcore.NewIdentity([32]byte{42, 43, 44})
}

func remoteAdvertEvents(id meshcore.Identity, source remoteSource) []remoteEvent {
	// Zero-hop native advert prefix: header 0x12, path length 0, public key.
	raw := append([]byte{0x12, 0}, id.PublicKeyBytes()...)
	preview := hex.EncodeToString(raw[:16])
	rf := uint32(91)
	return []remoteEvent{{Sequence: 1, Direction: "tx", State: 2, Slot: source.Slot, Generation: source.Generation, Job: 123, Length: 110, RFMillis: &rf, Preview: preview}}
}

func TestRemoteProfileAndFiveIdentityGate(t *testing.T) {
	cfg, s, peer := remoteFixture()
	if err := s.validate(cfg); err != nil {
		t.Fatal(err)
	}
	roles := make(map[string]meshcore.Identity)
	for _, role := range s.Roles {
		id, err := meshcore.NewIdentityFromHex(role.PublicKey)
		if err != nil {
			t.Fatal(err)
		}
		roles[role.Role] = id
	}
	device := protocol.DeviceInfoResponse{FirmwareVersion: 13, FirmwareVersionStr: "v1.17.1", Model: "Xiao S3"}
	self := protocol.SelfInfoResponse{PublicKey: peer.PublicKey()}
	if err := verifyRemoteIdentity(device, self, roles); err != nil {
		t.Fatal(err)
	}
	device.Model = "unsupported board"
	if err := verifyRemoteIdentity(device, self, roles); err == nil {
		t.Fatal("unsupported board accepted")
	}
	device.Model = "PHY-less Xiao"
	self.PublicKey = roles["bot"].PublicKey()
	if err := verifyRemoteIdentity(device, self, roles); err == nil {
		t.Fatal("fifth-role identity collision accepted")
	}
	for _, mutate := range []func(*remoteSnapshot){
		func(s *remoteSnapshot) { s.Profile.Frequency++ },
		func(s *remoteSnapshot) { s.Profile.Generation = 0 },
		func(s *remoteSnapshot) { s.Profile.Committed = false },
		func(s *remoteSnapshot) { s.Profile.Fault = true },
		func(s *remoteSnapshot) { s.Roles = s.Roles[:4] },
		func(s *remoteSnapshot) { s.Roles[4].Ready = false },
		func(s *remoteSnapshot) { s.Roles[4].PublicKey = s.Roles[0].PublicKey },
	} {
		_, changed, _ := remoteFixture()
		mutate(&changed)
		if err := changed.validate(cfg); err == nil {
			t.Fatal("accepted missing identity/readiness or mismatched/uncommitted radio profile")
		}
	}
}

func TestRemotePhysicalProofRequiresOneExternalNegotiatedGeneration(t *testing.T) {
	_, initial, id := remoteFixture()
	events := remoteAdvertEvents(id, *initial.Stream)
	journal := newRemoteJournal(initial)
	next := initial
	next.Publication++
	next.Totals.Accepted, next.Totals.Succeeded = 9, 1
	next.History.Events = events
	if err := journal.ingest(next); err != nil {
		t.Fatal(err)
	}
	proof, found, err := journal.advert(0, id, nil)
	if err != nil || !found || proof.Terminal.Job != 123 || proof.Terminal.Generation != 99 {
		t.Fatalf("exact queued lifecycle not found: %+v %t %v", proof, found, err)
	}
	if journal.admissionDelta() != 9 {
		t.Fatal("aggregate admissions were replaced with a per-job count")
	}
	for _, tc := range []struct {
		name   string
		change func(*remoteSnapshot)
	}{
		{"no-UART-projection", func(s *remoteSnapshot) { s.Stream = nil }},
		{"not-negotiated", func(s *remoteSnapshot) { s.Stream.Negotiated = false }},
		{"disconnected", func(s *remoteSnapshot) { s.Stream.Connected = false }},
		{"faulted", func(s *remoteSnapshot) { s.Stream.Fault = true }},
		{"different-generation", func(s *remoteSnapshot) { s.Stream.Generation++ }},
		{"local-role-slot", func(s *remoteSnapshot) { s.Stream.Slot = *s.Roles[0].Slot }},
	} {
		t.Run(tc.name, func(t *testing.T) {
			_, baseline, id := remoteFixture()
			tc.change(&baseline)
			j := newRemoteJournal(baseline)
			n := baseline
			n.Totals.Accepted, n.Totals.Succeeded = 500, 500
			n.History.Events = events
			if err := j.ingest(n); err != nil {
				t.Fatal(err)
			}
			if _, ok, err := j.advert(0, id, nil); err != nil || ok {
				t.Fatalf("unrelated counter growth became external proof: found=%t err=%v", ok, err)
			}
		})
	}
	changed := next
	stream := *next.Stream
	stream.Generation++
	changed.Stream = &stream
	if err := journal.ingest(changed); err == nil {
		t.Fatal("selected source renegotiation retained evidence")
	}
}

func TestRemoteJobLifecycleRejectsFalseSuccess(t *testing.T) {
	_, snapshot, id := remoteFixture()
	source := *snapshot.Stream
	for _, tc := range []struct {
		name string
		edit func([]remoteEvent) []remoteEvent
	}{
		{"accepted-only", func(e []remoteEvent) []remoteEvent { e[0].State = 1; return e }},
		{"zero-job", func(e []remoteEvent) []remoteEvent { e[0].Job = 0; return e }},
		{"different-slot", func(e []remoteEvent) []remoteEvent { e[0].Slot++; return e }},
		{"different-generation", func(e []remoteEvent) []remoteEvent { e[0].Generation++; return e }},
		{"failed", func(e []remoteEvent) []remoteEvent { e[0].State = 3; return e }},
		{"unknown", func(e []remoteEvent) []remoteEvent { e[0].State = 4; return e }},
		{"reason-error", func(e []remoteEvent) []remoteEvent { e[0].Reason = 5; return e }},
		{"duplicate-job", func(e []remoteEvent) []remoteEvent {
			duplicate := e[0]
			duplicate.Sequence++
			return append(e, duplicate)
		}},
		{"missing-airtime", func(e []remoteEvent) []remoteEvent { e[0].RFMillis = nil; return e }},
		{"zero-airtime", func(e []remoteEvent) []remoteEvent { e[0].RFMillis = new(uint32); return e }},
		{"different-preview", func(e []remoteEvent) []remoteEvent { e[0].Preview = "00"; return e }},
		{"rx-not-tx", func(e []remoteEvent) []remoteEvent { e[0].Direction = "rx"; return e }},
		{"stale", func(e []remoteEvent) []remoteEvent { e[0].Sequence = 0; return e }},
	} {
		t.Run(tc.name, func(t *testing.T) {
			proof, err := remoteJobs(tc.edit(remoteAdvertEvents(id, source)), 0, &source, remoteAdvertMatch(id))
			if err == nil && len(proof) > 0 {
				t.Fatal("incomplete/wrong lifecycle counted as physical completion")
			}
			if (tc.name == "failed" || tc.name == "unknown" || tc.name == "reason-error") && err == nil {
				t.Fatal("terminal failure did not surface immediately")
			}
		})
	}
}

func TestRemoteCommissioningTerminalOnlyAdvertShape(t *testing.T) {
	// Public terminal event shape observed during commissioning; no admission
	// event exists in native dashboard history.
	const wire = `{"sequence":73,"direction":"tx","state":2,"reason":0,"source_slot":0,"source_generation":8,"job_id":1,"rf_ms":114,"length":120,"preview_hex":"1200e9781d16e034a49851117640461a"}`
	var event remoteEvent
	if err := json.Unmarshal([]byte(wire), &event); err != nil {
		t.Fatal(err)
	}
	raw, err := hex.DecodeString(event.Preview)
	if err != nil {
		t.Fatal(err)
	}
	var key [32]byte
	copy(key[:], raw[2:])
	source := remoteSource{Slot: 0, Generation: 8}
	proofs, err := remoteJobs([]remoteEvent{event}, 72, &source, remoteAdvertMatch(meshcore.NewIdentity(key)))
	if err != nil || len(proofs) != 1 || proofs[0].Terminal != event {
		t.Fatalf("real terminal-only shape was not sufficient: %+v %v", proofs, err)
	}
	encoded, err := json.Marshal(proofs[0])
	if err != nil {
		t.Fatal(err)
	}
	var fields map[string]json.RawMessage
	if err := json.Unmarshal(encoded, &fields); err != nil {
		t.Fatal(err)
	}
	if len(fields) != 1 || fields["terminal"] == nil {
		t.Fatalf("proof fabricated an admission event: %s", encoded)
	}
	for _, other := range []remoteSource{{Slot: 1, Generation: 8}, {Slot: 0, Generation: 9}} {
		if got, err := remoteJobs([]remoteEvent{event}, 72, &other, remoteAdvertMatch(meshcore.NewIdentity(key))); err != nil || len(got) != 0 {
			t.Fatalf("terminal proof escaped source generation: %+v %v", got, err)
		}
	}
}

func TestRemoteJournalGapsAndRestartInvalidateRun(t *testing.T) {
	for _, tc := range []struct {
		name string
		edit func(*remoteSnapshot)
	}{
		{"missed-event", func(s *remoteSnapshot) { s.History.Events[0].Sequence = 20 }},
		{"profile-change", func(s *remoteSnapshot) { s.Profile.Generation++ }},
		{"role-restart", func(s *remoteSnapshot) { s.Roles[0].Generation++ }},
		{"identity-change", func(s *remoteSnapshot) { s.Roles[0].PublicKey = s.Roles[1].PublicKey }},
		{"unready", func(s *remoteSnapshot) { s.Roles[0].Ready = false }},
		{"modem-reboot", func(s *remoteSnapshot) { s.Uptime = 1 }},
	} {
		t.Run(tc.name, func(t *testing.T) {
			_, first, id := remoteFixture()
			j := newRemoteJournal(first)
			_, next, _ := remoteFixture()
			next.History.Events = remoteAdvertEvents(id, *next.Stream)
			tc.edit(&next)
			if err := j.ingest(next); err == nil {
				t.Fatal("invalidated evidence window accepted")
			}
		})
	}
}

func TestRemoteHTTPReadAndUnavailableUARTFailBeforeOpeningDevices(t *testing.T) {
	cfg, snapshot, _ := remoteFixture()
	snapshot.Stream = nil
	server := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		if r.URL.Path != "/api/status" {
			t.Errorf("unexpected status path %s", r.URL.Path)
		}
		if err := json.NewEncoder(w).Encode(snapshot); err != nil {
			t.Error(err)
		}
	}))
	defer server.Close()
	cfg.StatusListen = strings.TrimPrefix(server.URL, "http://")
	got, err := readRemoteSnapshot(context.Background(), cfg.StatusListen)
	if err != nil || got.Profile != snapshot.Profile {
		t.Fatalf("dashboard read: %+v %v", got, err)
	}
	var output bytes.Buffer
	err = runRemote(context.Background(), cfg, options{
		serial: "MUST-NOT-BE-OPENED", config: "explicit", onchip: true,
		timeout: 5 * time.Second, phaseTimeout: time.Second,
	}, &output)
	if err == nil || !strings.Contains(err.Error(), "no negotiated external source") {
		t.Fatalf("missing observable source did not block before serial open: %v", err)
	}
	if strings.Contains(output.String(), `"phase":"complete"`) {
		t.Fatal("blocked run claimed completion")
	}
}

func TestRemotePreviewDoesNotConfusePayloadOrPath(t *testing.T) {
	_, s, id := remoteFixture()
	e := remoteAdvertEvents(id, *s.Stream)[0]
	if !remoteAdvertMatch(id)(e) {
		t.Fatal("native zero-hop advert prefix rejected")
	}
	for _, raw := range []string{
		"1201" + strings.Repeat("2a", 14), // nonempty path moves identity.
		"0a00" + strings.Repeat("2a", 14), // encrypted text, not advert.
		"zz", "1200",
	} {
		e.Preview = raw
		if remoteAdvertMatch(id)(e) {
			t.Fatalf("invalid preview became advert evidence: %s", raw)
		}
		for _, raw := range []string{"0e0078563412", "2a001378563412"} {
			e := remoteEvent{Preview: raw, Length: len(raw) / 2}
			if !remoteACKMatch(0x12345678)(e) || remoteACKMatch(0x12345679)(e) {
				t.Fatalf("native ACK/multipart tag matching failed: %s", raw)
			}
			e.Length++
			if remoteACKMatch(0x12345678)(e) {
				t.Fatal("truncated preview accepted as a complete ACK")
			}
		}
	}
}

func TestRemoteModeRequiresExplicitOnchipAdmission(t *testing.T) {
	err := runRemote(context.Background(), app.DefaultConfig(), options{}, &bytes.Buffer{})
	if err == nil || !strings.Contains(err.Error(), "requires -onchip") || errors.Is(err, context.Canceled) {
		t.Fatalf("admission: %v", err)
	}
}

// Only the three SDK command boundaries used by remoteExchange are supplied.
// This is not a native application or encrypted-radio implementation.
type remoteCommandFixture struct {
	response func(protocol.Response)
	send     func([]byte) error
}

func (*remoteCommandFixture) Connect(context.Context) error                  { return nil }
func (*remoteCommandFixture) Close() error                                   { return nil }
func (*remoteCommandFixture) SetErrorHandler(func(error))                    {}
func (*remoteCommandFixture) SetDisconnectHandler(func())                    {}
func (f *remoteCommandFixture) SetResponseHandler(h func(protocol.Response)) { f.response = h }
func (f *remoteCommandFixture) Send(b []byte) error                          { return f.send(b) }
func (f *remoteCommandFixture) reply(body []byte) error {
	r, err := protocol.ParseResponse(body)
	if err == nil {
		f.response(r)
	}
	return err
}

func TestRemoteExchangeRequiresNativeMessageTagAndBothPhysicalJobs(t *testing.T) {
	for _, wrongACK := range []bool{false, true} {
		t.Run(fmt.Sprintf("wrong-physical-ack-%t", wrongACK), func(t *testing.T) {
			_, snapshot, sender := remoteFixture()
			receiver := meshcore.NewIdentity([32]byte{1})
			source := *snapshot.Stream
			ackSource := remoteSource{Slot: 4, Generation: 10}
			journal := newRemoteJournal(snapshot)
			fromWire, toWire := &remoteCommandFixture{}, &remoteCommandFixture{}
			ctx, cancel := context.WithCancelCause(context.Background())
			defer cancel(nil)
			from := newEndpoint("peer", fromWire, cancel)
			to := newEndpoint("host", toWire, cancel)
			from.self.PublicKey, to.self.PublicKey = sender.PublicKey(), receiver.PublicKey()
			var received []byte
			fromWire.send = func(command []byte) error {
				switch command[0] {
				case protocol.CmdGetContactByKey:
					fromWire.response(protocol.Response{Code: protocol.RespContact, Data: protocol.ContactResponse{PublicKey: receiver.PublicKey(), OutPathLen: 0}})
					return nil
				case protocol.CmdSendTxtMsg:
					plain := append(bytes.Clone(command[3:7]), command[1]<<2|command[2]&3)
					plain = append(plain, command[13:]...)
					tag := meshcore.CalcAckHash(plain, sender.PublicKeyBytes())
					received = make([]byte, 16)
					received[0] = 16
					copy(received[4:10], sender.PublicKeyBytes()[:6])
					copy(received[12:16], command[3:7])
					received = append(received, command[13:]...)
					data := remoteAdvertEvents(sender, source)
					cipherPreview := append([]byte{0x0a, 0, 1, 42}, bytes.Repeat([]byte{0x7b}, 12)...)
					for i := range data {
						data[i].Length, data[i].Preview = 38, hex.EncodeToString(cipherPreview)
					}
					ack := remoteAdvertEvents(receiver, ackSource)
					physicalTag := tag
					if wrongACK {
						physicalTag++
					}
					ackRaw := binary.LittleEndian.AppendUint32([]byte{0x0e, 0}, physicalTag)
					for i := range ack {
						ack[i].Sequence++
						ack[i].Job = 456
						ack[i].Length, ack[i].Preview = 6, hex.EncodeToString(ackRaw)
					}
					journal.events = append(data, ack...)
					sent := binary.LittleEndian.AppendUint32([]byte{6, 0}, tag)
					sent = binary.LittleEndian.AppendUint32(sent, 1000)
					if err := fromWire.reply(sent); err != nil {
						return err
					}
					push := binary.LittleEndian.AppendUint32([]byte{0x82}, tag)
					return fromWire.reply(binary.LittleEndian.AppendUint32(push, 50))
				default:
					return fmt.Errorf("unexpected sender command %d", command[0])
				}
			}
			toWire.send = func(command []byte) error {
				if !bytes.Equal(command, []byte{10}) {
					return errors.New("unexpected receiver command")
				}
				if received != nil {
					body := received
					received = nil
					return toWire.reply(body)
				}
				return toWire.reply([]byte{10})
			}
			var output bytes.Buffer
			err := remoteExchange(ctx, 100*time.Millisecond, from, to, "unique-queued-message",
				journal, source, ackSource, func(phase string, value any) error {
					return json.NewEncoder(&output).Encode(map[string]any{"phase": phase, "details": value})
				})
			if wrongACK {
				if err == nil || output.Len() != 0 {
					t.Fatalf("wrong physical ACK passed: %v %s", err, output.String())
				}
			} else if err != nil || !strings.Contains(output.String(), `"delivery_medium":"shared-modem local reflection"`) {
				t.Fatalf("SDK/application plus matching physical jobs: %v %s", err, output.String())
			}
		})
	}
}
