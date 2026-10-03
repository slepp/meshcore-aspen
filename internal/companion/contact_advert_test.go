package companion

import (
	"bytes"
	"os"
	"path/filepath"
	"testing"
	"time"

	meshcore "github.com/meshcore-go/meshcore-go"
	protocol "github.com/meshcore-go/meshcore-go/companion"
)

func advertTestServer(t *testing.T, cfg Config) (*Server, *session, *session) {
	t.Helper()
	cfg.AdvertInterval = 0
	s, err := New(testIdentity(1), newTestRadio(), cfg)
	if err != nil {
		t.Fatal(err)
	}
	t.Cleanup(func() {
		if err := s.Close(); err != nil {
			t.Error(err)
		}
	})
	a, b := commandSession(t, s), commandSession(t, s)
	s.mu.Lock()
	s.clients[a], s.clients[b] = struct{}{}, struct{}{}
	s.mu.Unlock()
	return s, a, b
}

func signedWireAdvert(t *testing.T, peer meshcore.LocalIdentity, stamp uint32, app []byte) *meshcore.Packet {
	t.Helper()
	adv := &meshcore.Advert{PublicKey: peer.Identity, Timestamp: stamp, RawAppData: app}
	adv.SignWith(peer)
	payload, err := adv.ToBytes()
	if err != nil {
		t.Fatal(err)
	}
	return &meshcore.Packet{
		Header:     meshcore.MakeHeader(meshcore.RouteTypeFlood, meshcore.PayloadTypeAdvert, 0),
		PathLength: 0x81, Path: []byte{1, 2, 3}, Payload: payload,
	}
}

func receiveWireAdvert(t *testing.T, s *Server, pkt *meshcore.Packet) {
	t.Helper()
	raw, err := pkt.ToBytes()
	if err != nil {
		t.Fatal(err)
	}
	parsed, err := meshcore.PacketFromBytes(raw)
	if err != nil {
		t.Fatal(err)
	}
	s.receive(parsed)
	// Dequeuing this no-op ACK requires the preceding advert to finish.
	s.receive(&meshcore.Packet{Header: meshcore.MakeHeader(meshcore.RouteTypeDirect, meshcore.PayloadTypeAck, 0)})
	deadline := time.Now().Add(3 * time.Second)
	for {
		s.mu.Lock()
		empty := len(s.packets) == 0
		s.mu.Unlock()
		if empty {
			return
		}
		if time.Now().After(deadline) {
			t.Fatal("advert receive queue did not drain")
		}
		time.Sleep(time.Millisecond)
	}
}

func requireAdvertPush(t *testing.T, c *session, code byte, key [32]byte) {
	t.Helper()
	select {
	case frame := <-c.out:
		if len(frame) < 33 || frame[0] != code || !bytes.Equal(frame[1:33], key[:]) {
			t.Fatalf("advert push = %x, want code %d for %x", frame, code, key)
		}
	default:
		t.Fatal("missing advert push")
	}
}

func requireQuietSession(t *testing.T, c *session) {
	t.Helper()
	select {
	case frame := <-c.out:
		t.Fatalf("unexpected queued frame: %x", frame)
	default:
	}
}

func TestAdvertNameWireSemantics(t *testing.T) {
	for _, tc := range []struct {
		name string
		app  []byte
	}{
		{"no app data", nil},
		{"no name flag", []byte{0x01}},
		{"unflagged trailing bytes", []byte{0x01, 'n', 'a', 'm', 'e'}},
		{"flag with empty name", []byte{0x81}},
		{"flag with leading NUL", []byte{0x81, 0, 'n', 'a', 'm', 'e'}},
		{"truncated location", []byte{0x91, 1, 2, 3}},
		{"truncated feature", []byte{0xa1, 1}},
	} {
		t.Run(tc.name, func(t *testing.T) {
			for _, manual := range []byte{0, 1} {
				cfg := testConfig(stateDir(t))
				s, a, b := advertTestServer(t, cfg)
				requireOK(t, sessionCommand(t, a, []byte{protocol.CmdSetOtherParams, manual}))
				before, err := os.ReadFile(filepath.Join(cfg.StateDir, "companion.json"))
				if err != nil {
					t.Fatal(err)
				}
				peer := testIdentity(2)
				pkt := signedWireAdvert(t, peer, 100, tc.app)
				receiveWireAdvert(t, s, pkt)
				for _, c := range []*session{a, b} {
					requireQuietSession(t, c)
				}
				frame := sessionCommand(t, a, append([]byte{protocol.CmdGetContactByKey}, peer.PublicKeyBytes()...))
				if !bytes.Equal(frame, []byte{protocol.RespErr, protocol.ErrCodeNotFound}) {
					t.Fatalf("manual=%d nameless RF contact: %x", manual, frame)
				}
				raw, err := pkt.ToBytes()
				if err != nil {
					t.Fatal(err)
				}
				frame = sessionCommand(t, a, append([]byte{protocol.CmdImportContact}, raw...))
				if !bytes.Equal(frame, []byte{protocol.RespErr, protocol.ErrCodeIllegalArg}) {
					t.Fatalf("manual=%d nameless import response: %x", manual, frame)
				}
				after, err := os.ReadFile(filepath.Join(cfg.StateDir, "companion.json"))
				if err != nil {
					t.Fatal(err)
				}
				if !bytes.Equal(before, after) {
					t.Fatal("rejected advert changed persisted state")
				}
				for _, c := range []*session{a, b} {
					requireQuietSession(t, c)
				}
			}
		})
	}
}

func TestAdvertLocationFlagPreservesContactMetadata(t *testing.T) {
	cfg := testConfig(stateDir(t))
	s, a, b := advertTestServer(t, cfg)
	peer := testIdentity(2)
	cmd := make([]byte, 144)
	cmd[0] = protocol.CmdAddUpdateContact
	copy(cmd[1:33], peer.PublicKeyBytes())
	cmd[33], cmd[34], cmd[35] = meshcore.AdvertTypeChat, 1, 0x81
	copy(cmd[36:39], []byte{4, 5, 6})
	copy(cmd[100:132], "Before")
	put32(cmd[132:136], 99)
	put32(cmd[136:140], 1234)
	put32(cmd[140:144], 5678)
	requireOK(t, sessionCommand(t, a, cmd))
	for _, c := range []*session{a, b} {
		requireQuietSession(t, c)
	}
	// Both feature fields precede the name; neither is a location field.
	app := append([]byte{0xe1, 0x34, 0x12, 0x78, 0x56}, []byte("After")...)
	pkt := signedWireAdvert(t, peer, 100, app)
	receiveWireAdvert(t, s, pkt)
	for _, c := range []*session{a, b} {
		requireAdvertPush(t, c, protocol.PushAdvert, peer.PublicKey())
		requireQuietSession(t, c)
	}
	get := append([]byte{protocol.CmdGetContactByKey}, peer.PublicKeyBytes()...)
	frame := sessionCommand(t, a, get)
	if len(frame) != 148 || frame[0] != protocol.RespContact || cstring(frame[100:132]) != "After" ||
		u32(frame[132:136]) != 100 || u32(frame[136:140]) != 1234 || u32(frame[140:144]) != 5678 ||
		frame[34] != 1 || frame[35] != 0x81 || !bytes.Equal(frame[36:39], []byte{4, 5, 6}) {
		t.Fatalf("location-free update lost contact fields: %x", frame)
	}
	export := append([]byte{protocol.CmdExportContact}, peer.PublicKeyBytes()...)
	exported := sessionCommand(t, a, export)
	saved, err := meshcore.PacketFromBytes(exported[1:])
	if err != nil || !bytes.Equal(saved.Payload, pkt.Payload) || saved.PathLength != 0 {
		t.Fatalf("export lost signed advert metadata: %x %v", exported, err)
	}
	for _, bad := range []struct {
		stamp uint32
		app   []byte
	}{
		{99, append([]byte{0x91, 0, 0, 0, 0, 0, 0, 0, 0}, []byte("Stale")...)},
		{101, []byte{0x81}},
		{101, []byte{0x81, 0, 'x'}},
	} {
		receiveWireAdvert(t, s, signedWireAdvert(t, peer, bad.stamp, bad.app))
		if got := sessionCommand(t, a, get); !bytes.Equal(got, frame) {
			t.Fatalf("stale/nameless advert changed contact: %x", got)
		}
		for _, c := range []*session{a, b} {
			requireQuietSession(t, c)
		}
	}
	if err := s.Close(); err != nil {
		t.Fatal(err)
	}
	s, a, b = advertTestServer(t, cfg)
	if got := sessionCommand(t, a, get); !bytes.Equal(got, frame) {
		t.Fatalf("restart changed preserved contact: %x", got)
	}
	zero := append([]byte{0x91, 0, 0, 0, 0, 0, 0, 0, 0}, []byte("Zero")...)
	receiveWireAdvert(t, s, signedWireAdvert(t, peer, 102, zero))
	for _, c := range []*session{a, b} {
		requireAdvertPush(t, c, protocol.PushAdvert, peer.PublicKey())
	}
	if got := sessionCommand(t, a, get); u32(got[136:140]) != 0 || u32(got[140:144]) != 0 || cstring(got[100:132]) != "Zero" {
		t.Fatalf("explicit zero location was not applied: %x", got)
	}
	if err := s.Close(); err != nil {
		t.Fatal(err)
	}
	_, a, _ = advertTestServer(t, cfg)
	if got := sessionCommand(t, a, get); u32(got[136:140]) != 0 || u32(got[140:144]) != 0 {
		t.Fatalf("restart restored superseded coordinates: %x", got)
	}
}

func TestManualContactCommandsDoNotPushButImportAndRFDo(t *testing.T) {
	cfg := testConfig(stateDir(t))
	s, a, b := advertTestServer(t, cfg)
	peer := testIdentity(2)
	cmd := make([]byte, 132)
	cmd[0] = protocol.CmdAddUpdateContact
	copy(cmd[1:33], peer.PublicKeyBytes())
	cmd[33], cmd[35] = meshcore.AdvertTypeChat, 255
	for _, name := range []string{"Added", "Updated"} {
		clear(cmd[100:132])
		copy(cmd[100:132], name)
		requireOK(t, sessionCommand(t, a, cmd))
		for _, c := range []*session{a, b} {
			requireQuietSession(t, c)
		}
	}

	if err := s.Close(); err != nil {
		t.Fatal(err)
	}
	s, a, b = advertTestServer(t, cfg)
	get := append([]byte{protocol.CmdGetContactByKey}, peer.PublicKeyBytes()...)
	if got := sessionCommand(t, a, get); len(got) != 148 || cstring(got[100:132]) != "Updated" {
		t.Fatalf("manual update did not survive restart: %x", got)
	}
	pkt := signedWireAdvert(t, peer, 100, append([]byte{0x81}, []byte("Imported")...))
	raw, err := pkt.ToBytes()
	if err != nil {
		t.Fatal(err)
	}
	requireOK(t, sessionCommand(t, a, append([]byte{protocol.CmdImportContact}, raw...)))
	for _, c := range []*session{a, b} {
		requireAdvertPush(t, c, protocol.PushNewAdvert, peer.PublicKey())
		requireQuietSession(t, c)
	}
	for i, remote := range []meshcore.LocalIdentity{peer, testIdentity(3)} {
		pkt = signedWireAdvert(t, remote, 101, append([]byte{0x81}, []byte("RF")...))
		receiveWireAdvert(t, s, pkt)
		code := byte(protocol.PushAdvert)
		if i == 1 {
			code = protocol.PushNewAdvert
		}
		for _, c := range []*session{a, b} {
			requireAdvertPush(t, c, code, remote.PublicKey())
			requireQuietSession(t, c)
		}
	}
}

func TestManualContactCapacityAndTransientCommandsDoNotPush(t *testing.T) {
	s, a, b := advertTestServer(t, testConfig(""))
	s.mu.Lock()
	for i := range maxContacts {
		var seed [32]byte
		put32(seed[:4], uint32(i+10))
		id := meshcore.NewLocalIdentityFromSeed(seed)
		s.state.Contacts = append(s.state.Contacts, contact{ContactResponse: protocol.ContactResponse{
			PublicKey: id.PublicKey(), Type: meshcore.AdvertTypeChat, OutPathLen: 255,
		}})
	}
	existing := s.state.Contacts[0].PublicKey
	s.mu.Unlock()
	cmd := make([]byte, 132)
	cmd[0], cmd[33], cmd[35] = protocol.CmdAddUpdateContact, meshcore.AdvertTypeChat, 255
	copy(cmd[100:132], "Manual")
	peer := testIdentity(2)
	copy(cmd[1:33], peer.PublicKeyBytes())
	full := []byte{protocol.RespErr, protocol.ErrCodeTableFull}
	if got := sessionCommand(t, a, cmd); !bytes.Equal(got, full) {
		t.Fatalf("full contact table accepted new contact: %x", got)
	}
	copy(cmd[1:33], existing[:])
	requireOK(t, sessionCommand(t, a, cmd))
	copy(cmd[1:33], peer.PublicKeyBytes())
	cmd[33] = 0
	requireOK(t, sessionCommand(t, a, cmd))
	cmd[33] = meshcore.AdvertTypeChat
	if got := sessionCommand(t, a, cmd); !bytes.Equal(got, full) {
		t.Fatalf("full contact table promoted transient contact: %x", got)
	}
	s.mu.Lock()
	count := len(s.state.Contacts)
	transient := s.transient[peer.PublicKey()]
	s.mu.Unlock()
	if count != maxContacts || transient == nil || transient.Type != 0 {
		t.Fatalf("contact limit/transient state changed: count=%d transient=%+v", count, transient)
	}
	for _, c := range []*session{a, b} {
		requireQuietSession(t, c)
	}
}
