package companion

import (
	"bytes"
	"net"
	"os"
	"path/filepath"
	"testing"

	meshcore "github.com/meshcore-go/meshcore-go"
	protocol "github.com/meshcore-go/meshcore-go/companion"
)

func runCommand(t *testing.T, s *Server, command []byte) []byte {
	t.Helper()
	a, b := net.Pipe()
	defer a.Close()
	defer b.Close()
	c := &session{server: s, conn: a, done: make(chan struct{}), out: make(chan []byte, 512)}
	s.mu.Lock()
	s.command(c, command)
	s.mu.Unlock()
	select {
	case frame := <-c.out:
		return frame
	default:
		t.Fatal("command produced no response")
		return nil
	}
}

func TestAdvertLocationNoneAppliesToTransmitAndExport(t *testing.T) {
	id := testIdentity(1)
	s, r, addr := startTestServer(t, id, testConfig(""))
	c := testClient(t, addr)
	ctx := testContext(t)
	if err := c.SetAdvertLatLon(ctx, 49123456, -123456789); err != nil {
		t.Fatal(err)
	}
	if got := runCommand(t, s, []byte{protocol.CmdSetOtherParams, 0, 0, 0}); !bytes.Equal(got, []byte{protocol.RespOk}) {
		t.Fatalf("set none: %x", got)
	}
	exported, err := c.ExportContact(ctx, id.Identity)
	if err != nil {
		t.Fatal(err)
	}
	p, err := meshcore.PacketFromBytes(exported.AdvertData)
	if err != nil {
		t.Fatal(err)
	}
	a, err := meshcore.AdvertFromBytes(p.Payload)
	if err != nil {
		t.Fatal(err)
	}
	if a.RawAppData[0]&meshcore.AdvertLatLonMask != 0 {
		t.Fatal("location-disabled export reveals coordinates")
	}
	if err := c.SendSelfAdvert(ctx, 1); err != nil {
		t.Fatal(err)
	}
	a, err = meshcore.AdvertFromBytes(r.packet(t).Payload)
	if err != nil {
		t.Fatal(err)
	}
	if a.RawAppData[0]&meshcore.AdvertLatLonMask != 0 {
		t.Fatal("location-disabled RF advert reveals coordinates")
	}
}

func TestDiscoveryReportsWithoutChangingKnownPathOrReplying(t *testing.T) {
	id, peer := testIdentity(1), testIdentity(2)
	_, r, addr := startTestServer(t, id, testConfig(""))
	c := testClient(t, addr)
	ctx := testContext(t)
	if err := c.AddUpdateContactFull(ctx, protocol.AddUpdateContactCommand{
		PublicKey: peer.PublicKey(), Type: meshcore.AdvertTypeChat, Name: "Peer", OutPathLen: 1, OutPath: []byte{0x44},
	}); err != nil {
		t.Fatal(err)
	}
	push := capturePush(c, protocol.PushPathDiscoveryResponse)
	sent, err := c.SendPathDiscoveryReq(ctx, peer.Identity)
	if err != nil {
		t.Fatal(err)
	}
	r.packet(t)
	plain := []byte{0x81, 1, 2, 3, meshcore.PayloadTypeResponse, 0, 0, 0, 0, 0}
	put32(plain[5:9], sent.Tag)
	p := datagram(t, peer, id, meshcore.PayloadTypePath, plain)
	p.Header = meshcore.MakeHeader(meshcore.RouteTypeFlood, meshcore.PayloadTypePath, 0)
	r.inject(p)
	got := nextPush(t, push).Data.(protocol.PushPathDiscoveryResp)
	if got.OutPathLen != 0x81 || !bytes.Equal(got.OutPath, []byte{1, 2, 3}) {
		t.Fatalf("discovery: %+v", got)
	}
	contact, err := c.GetContactByKey(ctx, peer.Identity)
	if err != nil {
		t.Fatal(err)
	}
	if contact.OutPathLen != 1 || contact.OutPath[0] != 0x44 {
		t.Fatalf("discovery changed route: %+v", contact)
	}
	if len(r.out) != 0 {
		t.Fatal("discovery sent an unsolicited reciprocal path")
	}
}

func TestFailedSignedMessageCommitDoesNotAdvanceRoomCursor(t *testing.T) {
	dir := stateDir(t)
	s, r, addr := startTestServer(t, testIdentity(1), testConfig(dir))
	room := testIdentity(2)
	c := testClient(t, addr)
	ctx := testContext(t)
	if err := c.AddUpdateContact(ctx, room.Identity, "Room"); err != nil {
		t.Fatal(err)
	}
	before, err := os.ReadFile(filepath.Join(dir, "companion.json"))
	if err != nil {
		t.Fatal(err)
	}
	restore := blockStateReplacement(t, dir)
	s.mu.Lock()
	p := s.findContact(room.PublicKeyBytes())
	s.incomingText(p, &meshcore.Packet{Header: meshcore.MakeHeader(meshcore.RouteTypeDirect, meshcore.PayloadTypeTxtMsg, 0)},
		[]byte{123, 0, 0, 0, protocol.TxtTypeSignedPlain << 2, 1, 2, 3, 4, 'h', 'i'})
	gotCursor, gotMessages := p.SyncSince, len(s.state.Messages)
	s.mu.Unlock()
	restore()
	after, err := os.ReadFile(filepath.Join(dir, "companion.json"))
	if err != nil {
		t.Fatal(err)
	}
	if gotCursor != 0 || gotMessages != 0 || !bytes.Equal(before, after) {
		t.Fatalf("failed commit advanced state: cursor=%d messages=%d", gotCursor, gotMessages)
	}
	if len(r.out) != 0 {
		t.Fatal("failed commit emitted an ACK")
	}
}
