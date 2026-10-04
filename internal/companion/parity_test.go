package companion

import (
	"bytes"
	"context"
	"encoding/json"
	"errors"
	"math"
	"net"
	"os"
	"path/filepath"
	"testing"
	"time"

	meshcore "github.com/meshcore-go/meshcore-go"
	protocol "github.com/meshcore-go/meshcore-go/companion"
	"meshcore.local/meshcore/internal/policy"
)

func commandSession(t *testing.T, s *Server) *session {
	t.Helper()
	a, b := net.Pipe()
	c := &session{server: s, conn: a, done: make(chan struct{}), out: make(chan []byte, 512), version: 13}
	t.Cleanup(func() { a.Close(); b.Close() })
	return c
}

func sessionCommand(t *testing.T, c *session, b []byte) []byte {
	t.Helper()
	c.server.mu.Lock()
	c.server.command(c, b)
	c.server.mu.Unlock()
	select {
	case frame := <-c.out:
		return frame
	default:
		t.Fatal("command produced no response")
		return nil
	}
}

func requireOK(t *testing.T, frame []byte) {
	t.Helper()
	if !bytes.Equal(frame, []byte{protocol.RespOk}) {
		t.Fatalf("expected OK, got %x", frame)
	}
}

func TestConfigPolicyPreservesPersistedValuesAndExplicitZero(t *testing.T) {
	cfg := testConfig(stateDir(t))
	mode := policy.PathHashMode(2)
	rx := float32(7)
	interval := uint32(3600)
	zeroRX := float32(0)
	zeroInterval := uint32(0)
	for _, tc := range []struct {
		name   string
		policy policy.Overrides
		legacy time.Duration
		rx     float32
		flood  uint32
	}{
		{"set", policy.Overrides{PathHashMode: &mode, RXDelay: &rx, FloodAdvertSeconds: &interval}, 0, 7, 3600},
		{"preserve", policy.Overrides{}, 0, 7, 3600},
		{"explicit zero beats legacy interval", policy.Overrides{RXDelay: &zeroRX, FloodAdvertSeconds: &zeroInterval}, time.Hour, 0, 0},
		{"preserve zero", policy.Overrides{}, 0, 0, 0},
	} {
		cfg.Policy = tc.policy
		cfg.AdvertInterval = tc.legacy
		s, err := New(testIdentity(1), newTestRadio(), cfg)
		if err != nil {
			t.Fatalf("%s: %v", tc.name, err)
		}
		prefs := *s.preferences.Load()
		if err := s.Close(); err != nil {
			t.Fatalf("%s: close: %v", tc.name, err)
		}
		if prefs.PathHashMode != 2 || prefs.RXDelay != tc.rx || prefs.FloodAdvertSeconds != tc.flood {
			t.Fatalf("%s: unexpected preferences %+v", tc.name, prefs)
		}
	}
}

func TestPublishedPolicyDoesNotAliasStagedRegionChanges(t *testing.T) {
	key := [16]byte{1, 2, 3, 4}
	regions := []policy.Region{{ID: 1, Name: "$original", Keys: [][16]byte{key}}}
	cfg := testConfig("")
	cfg.Policy.Regions = &regions
	s, _, _ := startTestServer(t, testIdentity(1), cfg)
	pkt := &meshcore.Packet{Header: meshcore.MakeHeader(meshcore.RouteTypeTransportFlood, meshcore.PayloadTypeRawCustom, 0), Payload: []byte{1, 2, 3, 4}}
	pkt.TransportCode1 = meshcore.RegionKey(key).CalcTransportCode(pkt.PayloadType(), pkt.Payload)
	before := s.receiveContext(pkt)
	s.mu.Lock()
	s.state.Preferences.Regions[0].Name = "$staged"
	s.state.Preferences.Regions[0].Keys[0][0] ^= 0xff
	during := s.receiveContext(pkt)
	s.publishPreferences()
	after := s.receiveContext(pkt)
	s.mu.Unlock()
	if !before.ScopeKnown || !during.ScopeKnown || during.Scope.Name != "$original" || during.Scope.Key != key {
		t.Fatal("staged mutable region state changed the published receive policy")
	}
	if after.ScopeKnown {
		t.Fatal("publishing the new policy did not change region admission")
	}
}

func TestOriginatedWidthsAndSessionScopeIsolation(t *testing.T) {
	s, r, _ := startTestServer(t, testIdentity(1), testConfig(""))
	first, second := commandSession(t, s), commandSession(t, s)
	base := policy.AutoScope("base")
	override := policy.AutoScope("override")
	cmd := make([]byte, 48)
	cmd[0] = protocol.CmdSetDefaultFloodScope
	copy(cmd[1:32], base.Name)
	copy(cmd[32:], base.Key[:])
	requireOK(t, sessionCommand(t, second, cmd))
	requireOK(t, sessionCommand(t, first, append([]byte{protocol.CmdSetFloodScopeKey, 0}, override.Key[:]...)))
	for mode := byte(0); mode < 3; mode++ {
		requireOK(t, sessionCommand(t, first, []byte{protocol.CmdSetPathHashMode, 0, mode}))
		for _, tc := range []struct {
			client *session
			key    [16]byte
		}{{first, override.Key}, {second, base.Key}} {
			requireOK(t, sessionCommand(t, tc.client, []byte{protocol.CmdSendChannelData, 0, 255, 0x34, 0x12, mode, 1}))
			p := r.packet(t)
			if p.PathLength != mode<<6 || len(p.Path) != 0 || p.Header&3 != meshcore.RouteTypeTransportFlood {
				t.Fatalf("mode%d packet %+v", mode, p)
			}
			if p.TransportCode1 != meshcore.RegionKey(tc.key).CalcTransportCode(meshcore.PayloadTypeGrpData, p.Payload) {
				t.Fatal("client scope crossed sessions")
			}
			r.mu.Lock()
			job := r.jobs[len(r.jobs)-1]
			r.mu.Unlock()
			if job.Priority != 1 || job.Delay != 0 {
				t.Fatalf("origin scheduling %+v", job)
			}
		}
	}
	requireOK(t, sessionCommand(t, first, []byte{protocol.CmdSetFloodScopeKey, 1}))
	requireOK(t, sessionCommand(t, first, []byte{protocol.CmdSendChannelData, 0, 255, 1, 0, 99}))
	if p := r.packet(t); p.Header&3 != meshcore.RouteTypeFlood {
		t.Fatalf("explicit unscoped ignored %+v", p)
	}
}

func TestDatagramBoundaryIndependentInboxesAndReload(t *testing.T) {
	dir := stateDir(t)
	id := testIdentity(1)
	s, r, _ := startTestServer(t, id, testConfig(dir))
	a, b := commandSession(t, s), commandSession(t, s)
	data := bytes.Repeat([]byte{0xab}, 167)
	requireOK(t, sessionCommand(t, a, append([]byte{protocol.CmdSendChannelData, 0, 255, 0x34, 0x12}, data...)))
	p := r.packet(t)
	group, err := meshcore.GroupDataFromBytes(p.Payload)
	if err != nil {
		t.Fatal(err)
	}
	ch := s.Node.Channel(0)
	plain := group.Decrypt(ch.PSK[:])
	if !bytes.Equal(plain[:170], append([]byte{0x34, 0x12, 167}, data...)) {
		t.Fatalf("wrong decrypted native datagram %x", plain)
	}
	p.SNR = 2.25
	p.PathLength = 0x81
	p.Path = []byte{1, 2, 3}
	s.mu.Lock()
	s.incomingDatagram(p)
	s.mu.Unlock()
	for _, c := range []*session{a, b} {
		frame := sessionCommand(t, c, []byte{protocol.CmdSyncNextMessage})
		want := append([]byte{27, 9, 0, 0, 0, 0x81, 0x34, 0x12, 167}, data...)
		if !bytes.Equal(frame, want) {
			t.Fatalf("inbox native frame %x", frame)
		}
		if _, err := protocol.ParseResponse(frame); err != nil {
			t.Fatal(err)
		}
		if got := sessionCommand(t, c, []byte{protocol.CmdSyncNextMessage}); !bytes.Equal(got, []byte{protocol.RespNoMoreMessages}) {
			t.Fatalf("duplicate %x", got)
		}
	}
	if err := s.Close(); err != nil {
		t.Fatal(err)
	}
	reloaded, _, _ := startTestServer(t, id, testConfig(dir))
	if got := runCommand(t, reloaded, []byte{protocol.CmdSyncNextMessage}); len(got) != 176 || got[0] != 27 {
		t.Fatalf("datagram not persisted: %x", got)
	}
}

func TestNativeACKPrioritiesDelaysAndReplyScope(t *testing.T) {
	id, peer := testIdentity(1), testIdentity(2)
	s, r, addr := startTestServer(t, id, testConfig(""))
	client := testClient(t, addr)
	if err := client.AddUpdateContactFull(testContext(t), protocol.AddUpdateContactCommand{PublicKey: peer.PublicKey(), Type: 1, OutPathLen: 0, Name: "Peer"}); err != nil {
		t.Fatal(err)
	}
	requireOK(t, runCommand(t, s, []byte{protocol.CmdSetOtherParams, 0, 0, 0, 4}))
	plain := append([]byte{4, 0, 0, 0, 0}, []byte("ack timing")...)
	pkt := datagram(t, peer, id, meshcore.PayloadTypeTxtMsg, plain)
	r.inject(pkt)
	first, second := r.packet(t), r.packet(t)
	if first.PayloadType() != meshcore.PayloadTypeMultiPart || second.PayloadType() != meshcore.PayloadTypeAck {
		t.Fatalf("ACK sequence: %d %d", first.PayloadType(), second.PayloadType())
	}
	r.mu.Lock()
	jobs := append([]testTransmission(nil), r.jobs[len(r.jobs)-2:]...)
	r.mu.Unlock()
	if jobs[0].Delay != 200*time.Millisecond || jobs[1].Delay != 500*time.Millisecond || jobs[0].Priority != 0 || jobs[1].Priority != 0 {
		t.Fatalf("ACK schedule %+v", jobs)
	}
	region := policy.AutoScope("incoming")
	s.mu.Lock()
	s.state.Preferences.Regions = []policy.Region{{ID: 1, Name: region.Name}}
	s.state.Preferences.PathHashMode = 2
	s.publishPreferences()
	pkt = datagram(t, peer, id, meshcore.PayloadTypeTxtMsg, append([]byte{5, 0, 0, 0, 0}, []byte("scope snapshot")...))
	pkt.Header = meshcore.MakeHeader(meshcore.RouteTypeTransportFlood, meshcore.PayloadTypeTxtMsg, 0)
	pkt.PathLength = 0x81
	pkt.Path = []byte{7, 8, 9}
	pkt.TransportCode1 = meshcore.RegionKey(region.Key).CalcTransportCode(pkt.PayloadType(), pkt.Payload)
	frozen := s.receiveContext(pkt)
	s.state.Preferences.DefaultScope = policy.AutoScope("changed")
	s.state.Preferences.Regions = nil
	s.publishPreferences()
	s.handlePacket(pkt, frozen)
	s.mu.Unlock()
	path := r.packet(t)
	if path.PayloadType() != meshcore.PayloadTypePath || path.PathLength != 0x80 || path.Header&3 != meshcore.RouteTypeTransportFlood {
		t.Fatalf("PATH reply %+v", path)
	}
	if path.TransportCode1 != meshcore.RegionKey(region.Key).CalcTransportCode(path.PayloadType(), path.Payload) {
		t.Fatal("reply scope was recomputed after queueing")
	}
	r.mu.Lock()
	job := r.jobs[len(r.jobs)-1]
	r.mu.Unlock()
	if job.Priority != 2 || job.Delay != 200*time.Millisecond {
		t.Fatalf("PATH ACK scheduling %+v", job)
	}
}

func TestTraceOwnerAndRawExplicitPriority(t *testing.T) {
	s, r, _ := startTestServer(t, testIdentity(1), testConfig(""))
	a, b := commandSession(t, s), commandSession(t, s)
	trace := []byte{protocol.CmdSendTracePath, 1, 2, 3, 4, 5, 6, 7, 8, 1, 0xa1, 0xa2, 0xb1, 0xb2}
	sent := sessionCommand(t, a, trace)
	if len(sent) != 10 || sent[0] != protocol.RespSent || u32(sent[6:]) != 19250 {
		t.Fatalf("trace sent %x", sent)
	}
	p := r.packet(t)
	if p.PathLength != 0 || !bytes.Equal(p.Payload, trace[1:]) {
		t.Fatalf("trace wire %+v", p)
	}
	if got := sessionCommand(t, b, trace); !bytes.Equal(got, []byte{protocol.RespErr, protocol.ErrCodeBadState}) {
		t.Fatalf("duplicate trace acquired owner %x", got)
	}
	p.PathLength = 2
	p.Path = []byte{4, 8}
	p.SNR = 3
	s.mu.Lock()
	s.incomingDiagnostic(p)
	s.mu.Unlock()
	select {
	case frame := <-a.out:
		want := []byte{0x89, 0, 4, 1, 1, 2, 3, 4, 5, 6, 7, 8, 0xa1, 0xa2, 0xb1, 0xb2, 4, 8, 12}
		if !bytes.Equal(frame, want) {
			t.Fatalf("trace push %x", frame)
		}
		if _, err := protocol.ParseResponse(frame); err != nil {
			t.Fatal(err)
		}
	default:
		t.Fatal("trace owner missing push")
	}
	select {
	case frame := <-b.out:
		t.Fatalf("trace leaked %x", frame)
	default:
	}
	raw := []byte{meshcore.MakeHeader(meshcore.RouteTypeDirect, meshcore.PayloadTypeRawCustom, 0), 0, 1, 2, 3, 4}
	requireOK(t, sessionCommand(t, b, append([]byte{protocol.CmdSendRawPacket, 73}, raw...)))
	r.packet(t)
	r.mu.Lock()
	job := r.jobs[len(r.jobs)-1]
	r.mu.Unlock()
	if job.Priority != 73 || !bytes.Equal(job.Data, raw) {
		t.Fatalf("raw priority/data changed %+v", job)
	}
}

func TestTraceWireWidthsAreNotOrdinaryPathModes(t *testing.T) {
	s, r, _ := startTestServer(t, testIdentity(1), testConfig(""))
	client := commandSession(t, s)
	for flags := byte(0); flags < 4; flags++ {
		width := 1 << flags
		command := []byte{protocol.CmdSendTracePath, flags + 1, 0, 0, 0, 5, 6, 7, 8, flags}
		for i := 0; i < width*2; i++ {
			command = append(command, byte(i+1))
		}
		if got := sessionCommand(t, client, command); len(got) != 10 || got[0] != protocol.RespSent {
			t.Fatalf("native TRACE flags %d (width %d) failed: %x", flags, width, got)
		}
		packet := r.packet(t)
		if packet.PathLength != 0 || len(packet.Path) != 0 ||
			!bytes.Equal(packet.Payload, command[1:]) {
			t.Fatalf("TRACE flags %d changed wire path or width: %+v", flags, packet)
		}
	}
	for _, flags := range []byte{1, 3} {
		command := []byte{protocol.CmdSendTracePath, flags + 10, 0, 0, 0, 5, 6, 7, 8, flags, 1, 2, 3}
		if got := sessionCommand(t, client, command); !bytes.Equal(got, []byte{protocol.RespErr, protocol.ErrCodeIllegalArg}) {
			t.Fatalf("TRACE flags %d accepted three-byte hash: %x", flags, got)
		}
		select {
		case raw := <-r.out:
			t.Fatalf("invalid TRACE transmitted %x", raw)
		default:
		}
	}
}

func TestTelemetryPermissionsAndNativeSelfFrame(t *testing.T) {
	cfg := testConfig("")
	cfg.Battery = func(context.Context) (uint16, error) { return 3310, nil }
	id, peer := testIdentity(1), testIdentity(2)
	s, r, addr := startTestServer(t, id, cfg)
	c := testClient(t, addr)
	if err := c.AddUpdateContactFull(testContext(t), protocol.AddUpdateContactCommand{PublicKey: peer.PublicKey(), Type: 1, OutPathLen: 0, Flags: 2, Name: "Peer"}); err != nil {
		t.Fatal(err)
	}
	frame := runCommand(t, s, []byte{protocol.CmdSendTelemetryReq, 0, 0, 0})
	want := append(append([]byte{0x8b, 0}, id.PublicKeyBytes()[:6]...), 1, 116, 1, 75)
	if !bytes.Equal(frame, want) {
		t.Fatalf("self telemetry %x", frame)
	}
	requireOK(t, runCommand(t, s, []byte{protocol.CmdSetOtherParams, 0, 1}))
	req := datagram(t, peer, id, meshcore.PayloadTypeReq, []byte{9, 0, 0, 0, 3, 0, 0, 0, 0})
	r.inject(req)
	reply := r.packet(t)
	secret, err := peer.SharedSecret(id.Identity)
	if err != nil {
		t.Fatal(err)
	}
	plain, err := meshcore.MACThenDecrypt(secret, reply.Payload[2:])
	if err != nil {
		t.Fatal(err)
	}
	if !bytes.Equal(plain[:8], []byte{9, 0, 0, 0, 1, 116, 1, 75}) {
		t.Fatalf("remote telemetry %x", plain)
	}
	r.mu.Lock()
	job := r.jobs[len(r.jobs)-1]
	r.mu.Unlock()
	if job.Delay != 300*time.Millisecond || job.Priority != 0 {
		t.Fatalf("response scheduling %+v", job)
	}
	requireOK(t, runCommand(t, s, []byte{protocol.CmdSetOtherParams, 0, 0}))
	s.mu.Lock()
	p := s.findContact(peer.PublicKeyBytes())
	s.incomingRequest(p, req, []byte{10, 0, 0, 0, 3, 0}, policy.ReceiveContext{})
	s.mu.Unlock()
	select {
	case raw := <-r.out:
		t.Fatalf("permission-denied telemetry transmitted %x", raw)
	default:
	}
}

type sourceTestRadio struct {
	*testRadio
	set func(context.Context, float64) error
}

func (r *sourceTestRadio) SetSourceAirtimeFactor(ctx context.Context, factor float64) error {
	return r.set(ctx, factor)
}

func TestDefaultSourcePolicyReplacesReusedTuning(t *testing.T) {
	failure := errors.New("source policy readback failed")
	for _, tc := range []struct {
		name string
		err  error
	}{{"restores default", nil}, {"propagates failure", failure}} {
		t.Run(tc.name, func(t *testing.T) {
			effective := float64(99)
			r := &sourceTestRadio{testRadio: newTestRadio(), set: func(_ context.Context, factor float64) error {
				if tc.err != nil {
					return tc.err
				}
				effective = factor
				return nil
			}}
			s, err := New(testIdentity(1), r, testConfig(""))
			if s != nil {
				t.Cleanup(func() {
					if err := s.Close(); err != nil {
						t.Error(err)
					}
				})
			}
			if !errors.Is(err, tc.err) {
				t.Fatalf("startup error %v, want %v", err, tc.err)
			}
			if tc.err != nil && s != nil {
				t.Fatal("startup returned a server without confirming source policy")
			}
			if tc.err == nil && effective != 1 {
				t.Fatalf("native default inherited the previous source factor %g", effective)
			}
		})
	}
}

func TestInvalidAirtimeOverridesFailBeforePersistenceOrPHY(t *testing.T) {
	for name, value := range map[string]float32{"negative": -1, "nan": float32(math.NaN()), "positive infinity": float32(math.Inf(1)), "negative infinity": float32(math.Inf(-1))} {
		t.Run(name, func(t *testing.T) {
			cfg := testConfig(stateDir(t))
			cfg.Policy.AirtimeFactor = &value
			called := false
			r := &sourceTestRadio{testRadio: newTestRadio(), set: func(context.Context, float64) error { called = true; return nil }}
			s, err := New(testIdentity(1), r, cfg)
			if err == nil {
				s.Close()
				t.Fatal("invalid factor was accepted")
			}
			if called {
				t.Fatal("invalid factor reached the PHY")
			}
			if _, err := os.Stat(filepath.Join(cfg.StateDir, "companion.json")); !errors.Is(err, os.ErrNotExist) {
				t.Fatalf("invalid factor changed persistent state: %v", err)
			}
		})
	}
}

func TestUnrepresentableTuningReadbackNeverClaimsZeroFactor(t *testing.T) {
	for name, factor := range map[string]float32{"max float32": math.MaxFloat32, "uint32 rounding boundary": float32(math.MaxUint32) / 1000} {
		t.Run(name, func(t *testing.T) {
			cfg := testConfig(stateDir(t))
			cfg.Policy.AirtimeFactor = &factor
			applied := float64(-1)
			r := &sourceTestRadio{testRadio: newTestRadio(), set: func(_ context.Context, value float64) error { applied = value; return nil }}
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
			if applied != float64(factor) {
				t.Fatal("valid large runtime factor was clamped")
			}
			data, err := os.ReadFile(filepath.Join(cfg.StateDir, "companion.json"))
			if err != nil {
				t.Fatal(err)
			}
			var saved state
			if err := json.Unmarshal(data, &saved); err != nil {
				t.Fatal(err)
			}
			if math.Float32bits(saved.Preferences.AirtimeFactor) != math.Float32bits(factor) {
				t.Fatal("valid factor was changed at persistence")
			}
			if frame := runCommand(t, s, []byte{protocol.CmdGetTuningParams}); !bytes.Equal(frame, []byte{protocol.RespErr, protocol.ErrCodeBadState}) {
				t.Fatalf("unrepresentable tuning value produced misleading readback %x", frame)
			}
			if frame := runCommand(t, s, []byte{protocol.CmdGetDeviceTime}); frame[0] != protocol.RespCurrTime {
				t.Fatal("readback limitation invalidated otherwise valid preferences")
			}
		})
	}
}

func TestSourcePolicyCommittedBeforeApplyAndFailsClosed(t *testing.T) {
	dir := stateDir(t)
	failApply := false
	r := &sourceTestRadio{testRadio: newTestRadio()}
	r.set = func(_ context.Context, factor float64) error {
		data, err := os.ReadFile(filepath.Join(dir, "companion.json"))
		if err != nil {
			return err
		}
		var saved state
		if err := json.Unmarshal(data, &saved); err != nil {
			return err
		}
		if float64(saved.Preferences.AirtimeFactor) != factor {
			return errors.New("source policy applied before durable preference")
		}
		if failApply {
			return errors.New("indeterminate PHY response")
		}
		return nil
	}
	s, err := New(testIdentity(1), r, testConfig(dir))
	if err != nil {
		t.Fatal(err)
	}
	t.Cleanup(func() { s.Close() })
	r.packet(t)
	cmd := make([]byte, 9)
	cmd[0] = protocol.CmdSetTuningParams
	put32(cmd[1:5], 1200)
	put32(cmd[5:], 2500)
	requireOK(t, runCommand(t, s, cmd))
	if frame := runCommand(t, s, []byte{protocol.CmdGetTuningParams}); !bytes.Equal(frame, append([]byte{protocol.RespTuningParams}, cmd[1:]...)) {
		t.Fatalf("tuning readback %x", frame)
	}
	failApply = true
	put32(cmd[5:], 3000)
	if got := runCommand(t, s, cmd); !bytes.Equal(got, []byte{protocol.RespErr, protocol.ErrCodeBadState}) {
		t.Fatalf("uncertain apply claimed success %x", got)
	}
	if !s.faulted.Load() {
		t.Fatal("source uncertainty did not fail closed")
	}
	if got := runCommand(t, s, []byte{protocol.CmdGetDeviceTime}); !bytes.Equal(got, []byte{protocol.RespErr, protocol.ErrCodeFileIoError}) {
		t.Fatalf("continued after uncertain apply %x", got)
	}
}

func privateFrame(timestamp uint32) []byte {
	frame := []byte{protocol.RespContactMsgRecvV3, 0, 0, 0, 1, 2, 3, 4, 5, 6, 255, 0, 0, 0, 0, 0, 'x'}
	put32(frame[12:16], timestamp)
	return frame
}

func TestNativeRetentionProtectsDirectMessagesAndIndependentReaders(t *testing.T) {
	cfg := testConfig("")
	cfg.Retention = NativeQueue
	s, _, _ := startTestServer(t, testIdentity(1), cfg)
	a, b := commandSession(t, s), commandSession(t, s)
	s.mu.Lock()
	s.clients[a] = struct{}{}
	s.clients[b] = struct{}{}
	queued := s.queueMessage(privateFrame(1))
	s.mu.Unlock()
	if !queued {
		t.Fatal("initial message rejected")
	}
	<-a.out
	<-b.out
	if got := sessionCommand(t, a, []byte{protocol.CmdSyncNextMessage}); got[0] != protocol.RespContactMsgRecvV3 {
		t.Fatalf("first reader %x", got)
	}
	s.mu.Lock()
	retained := len(s.state.Messages)
	s.mu.Unlock()
	if retained != 1 {
		t.Fatal("first reader consumed another reader's message")
	}
	if got := sessionCommand(t, b, []byte{protocol.CmdSyncNextMessage}); got[0] != protocol.RespContactMsgRecvV3 {
		t.Fatalf("second reader %x", got)
	}
	s.mu.Lock()
	defer s.mu.Unlock()
	if len(s.state.Messages) != 0 {
		t.Fatal("native retention did not consume fully read message")
	}
	delete(s.clients, a)
	delete(s.clients, b)
	for i := uint32(1); i <= maxMessages; i++ {
		frame := privateFrame(i)
		if i == 2 {
			frame = []byte{protocol.RespChannelMsgRecvV3, 0, 0, 0, 0, 0, 0, 2, 0, 0, 0, 'x'}
		}
		if !s.queueMessage(frame) {
			t.Fatalf("fill message %d rejected", i)
		}
	}
	if !s.queueMessage(privateFrame(300)) {
		t.Fatal("channel eviction failed")
	}
	if len(s.state.Messages) != maxMessages || s.state.Messages[0].Frame[0] != protocol.RespContactMsgRecvV3 {
		t.Fatal("oldest direct message discarded")
	}
	for _, m := range s.state.Messages {
		if m.Frame[0] == protocol.RespChannelMsgRecvV3 {
			t.Fatal("channel was not selected for eviction")
		}
	}
	before := s.state.Sequence
	if s.queueMessage(privateFrame(301)) || s.state.Sequence != before {
		t.Fatal("full direct-message queue admitted an unretained message")
	}
}

func TestAutoAddHopBoundaryAndUnknownAdvertPath(t *testing.T) {
	s, _, _ := startTestServer(t, testIdentity(1), testConfig(""))
	requireOK(t, runCommand(t, s, []byte{protocol.CmdSetOtherParams, 1}))
	requireOK(t, runCommand(t, s, []byte{protocol.CmdSetAutoAddConfig, 2, 2}))
	peer := testIdentity(2)
	app, _ := (&meshcore.AdvertAppData{Type: "CHAT", Name: "Discovery"}).ToBytes()
	adv := &meshcore.Advert{PublicKey: peer.Identity, Timestamp: 22, RawAppData: app}
	adv.SignWith(peer)
	payload, err := adv.ToBytes()
	if err != nil {
		t.Fatal(err)
	}
	pkt := &meshcore.Packet{Header: meshcore.MakeHeader(meshcore.RouteTypeFlood, meshcore.PayloadTypeAdvert, 0), PathLength: 0x82, Path: []byte{1, 2, 3, 4, 5, 6}, Payload: payload}
	s.mu.Lock()
	s.handlePacket(pkt, s.receiveContext(pkt))
	count := len(s.state.Contacts)
	s.mu.Unlock()
	if count != 0 {
		t.Fatal("native max-hops boundary must be exclusive")
	}
	cmd := append([]byte{protocol.CmdGetAdvertPath, 0}, peer.PublicKeyBytes()...)
	if frame := runCommand(t, s, cmd); len(frame) != 12 || frame[0] != protocol.RespAdvertPath || frame[5] != 0x82 {
		t.Fatalf("unknown advert path not available %x", frame)
	}
	pkt.PathLength = 0x81
	pkt.Path = pkt.Path[:3]
	s.mu.Lock()
	s.handlePacket(pkt, s.receiveContext(pkt))
	count = len(s.state.Contacts)
	s.mu.Unlock()
	if count != 1 {
		t.Fatal("eligible CHAT advert not added")
	}
}

func TestTransientAnonymousContactAndStorageWire(t *testing.T) {
	cfg := testConfig(stateDir(t))
	cfg.Battery = func(context.Context) (uint16, error) { return 3300, nil }
	cfg.Storage = func(context.Context) (uint32, uint32, error) { return 23, 100, nil }
	s, r, _ := startTestServer(t, testIdentity(1), cfg)
	if got := runCommand(t, s, []byte{protocol.CmdGetBattAndStorage}); !bytes.Equal(got, []byte{12, 0xe4, 0x0c, 23, 0, 0, 0, 100, 0, 0, 0}) {
		t.Fatalf("native storage layout %x", got)
	}
	peer := testIdentity(2)
	frame := runCommand(t, s, append(append([]byte{protocol.CmdSendAnonReq}, peer.PublicKeyBytes()...), 1))
	if frame[0] != protocol.RespSent {
		t.Fatalf("unknown anon request %x", frame)
	}
	pkt := r.packet(t)
	if !pkt.IsRouteDirect() || pkt.PathLength != 0 || pkt.PayloadType() != meshcore.PayloadTypeAnonReq {
		t.Fatalf("transient route %+v", pkt)
	}
	s.mu.Lock()
	permanent, transient := len(s.state.Contacts), len(s.transient)
	err := s.save()
	s.mu.Unlock()
	if permanent != 0 || transient != 1 {
		t.Fatal("anonymous transient became permanent")
	}
	if err != nil {
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
	if len(stored.Contacts) != 0 {
		t.Fatal("anonymous contact persisted")
	}
}

func TestRawLogsIncludeDuplicatesAndControlIsLive(t *testing.T) {
	_, r, addr := startTestServer(t, testIdentity(1), testConfig(""))
	c := testClient(t, addr)
	logs := capturePush(c, protocol.PushLogRxData)
	raws := capturePush(c, protocol.PushRawData)
	controls := capturePush(c, protocol.PushControlData)
	p := &meshcore.Packet{Header: meshcore.MakeHeader(meshcore.RouteTypeDirect, meshcore.PayloadTypeRawCustom, 0), Payload: []byte{1, 2, 3, 4}, SNR: 1.25, RSSI: -44, HasSignalInfo: true}
	r.inject(p)
	r.inject(p)
	wire, err := p.ToBytes()
	if err != nil {
		t.Fatal(err)
	}
	for range 2 {
		log := nextPush(t, logs).Data.(protocol.PushLogRxDataResponse)
		if !bytes.Equal(log.Raw, wire) || log.LastSNR != 1.25 || log.LastRSSI != -44 {
			t.Fatalf("RX log %+v", log)
		}
	}
	raw := nextPush(t, raws).Data.(protocol.PushRawDataResponse)
	if !bytes.Equal(raw.Payload, []byte{1, 2, 3, 4}) {
		t.Fatalf("raw push %+v", raw)
	}
	p.Header = meshcore.MakeHeader(meshcore.RouteTypeDirect, meshcore.PayloadTypeControl, 0)
	p.Payload = []byte{0x80, 7}
	r.inject(p)
	control := nextPush(t, controls).Data.(protocol.PushControlDataResp)
	if !bytes.Equal(control.Payload, []byte{0x80, 7}) || control.PathLen != 0 {
		t.Fatalf("control push %+v", control)
	}
}

func TestNativeLoginTagPasswordLimitAndCLIAttempt(t *testing.T) {
	id, peer := testIdentity(1), testIdentity(2)
	s, r, addr := startTestServer(t, id, testConfig(""))
	c := testClient(t, addr)
	if err := c.AddUpdateContactFull(testContext(t), protocol.AddUpdateContactCommand{PublicKey: peer.PublicKey(), Type: meshcore.AdvertTypeRoom, OutPathLen: 0, Name: "Room"}); err != nil {
		t.Fatal(err)
	}
	cmd := append(append([]byte{protocol.CmdSendLogin}, peer.PublicKeyBytes()...), []byte("0123456789abcdefLONG")...)
	frame := runCommand(t, s, cmd)
	if len(frame) != 10 || frame[0] != protocol.RespSent || u32(frame[2:6]) != u32(peer.PublicKeyBytes()[:4]) {
		t.Fatalf("native login correlation prefix %x", frame)
	}
	secret, err := peer.SharedSecret(id.Identity)
	if err != nil {
		t.Fatal(err)
	}
	req, err := meshcore.AnonReqFromBytes(r.packet(t).Payload)
	if err != nil {
		t.Fatal(err)
	}
	body := req.Decrypt(secret)
	if !bytes.Equal(bytes.TrimRight(body[8:], "\x00"), []byte("0123456789abcde")) {
		t.Fatalf("login truncation %x", body)
	}
	cmd = make([]byte, 13)
	cmd[0], cmd[1], cmd[2] = protocol.CmdSendTxtMsg, protocol.TxtTypeCLIData, 7
	copy(cmd[7:13], peer.PublicKeyBytes()[:6])
	cmd = append(cmd, bytes.Repeat([]byte{'x'}, 160)...)
	frame = runCommand(t, s, cmd)
	if len(frame) != 10 || frame[0] != protocol.RespSent || u32(frame[2:6]) != 0 {
		t.Fatalf("CLI send %x", frame)
	}
	pkt := r.packet(t)
	plain, err := meshcore.MACThenDecrypt(secret, pkt.Payload[2:])
	if err != nil {
		t.Fatal(err)
	}
	if plain[4] != 7 || !bytes.Equal(plain[165:], make([]byte, len(plain)-165)) {
		t.Fatalf("CLI carried non-native extended-attempt tail %x", plain)
	}
}
