package companion

import (
	"bytes"
	"context"
	"encoding/json"
	"errors"
	"net"
	"os"
	"path/filepath"
	"strings"
	"sync"
	"testing"
	"time"

	meshcore "github.com/meshcore-go/meshcore-go"
	protocol "github.com/meshcore-go/meshcore-go/companion"
	"github.com/meshcore-go/meshcore-go/companion/client"
)

func capturePush(c *client.Client, code byte) <-chan protocol.Response {
	out := make(chan protocol.Response, 16)
	c.OnPush(code, func(r protocol.Response) { out <- r })
	return out
}

func nextPush(t *testing.T, ch <-chan protocol.Response) protocol.Response {
	t.Helper()
	select {
	case r := <-ch:
		return r
	case <-time.After(3 * time.Second):
		t.Fatal("missing push")
		return protocol.Response{}
	}
}

func datagram(t *testing.T, from, to meshcore.LocalIdentity, typ byte, plain []byte) *meshcore.Packet {
	t.Helper()
	secret, err := from.SharedSecret(to.Identity)
	if err != nil {
		t.Fatal(err)
	}
	cipher, err := meshcore.EncryptThenMAC(secret, plain)
	if err != nil {
		t.Fatal(err)
	}
	payload := append([]byte{to.PublicKey()[0], from.PublicKey()[0]}, cipher...)
	return &meshcore.Packet{Header: meshcore.MakeHeader(meshcore.RouteTypeDirect, typ, 0), Payload: payload}
}

func TestTransmitUsesIdentityChannelCryptoAndBroadcastsACK(t *testing.T) {
	id, peer := testIdentity(1), testIdentity(2)
	_, r, addr := startTestServer(t, id, testConfig(""))
	a, b := testClient(t, addr), testClient(t, addr)
	ctx := testContext(t)
	if err := a.AddUpdateContactFull(ctx, protocol.AddUpdateContactCommand{PublicKey: peer.PublicKey(), Type: meshcore.AdvertTypeChat, OutPathLen: 2, OutPath: []byte{0x21, 0x32}, Name: "Peer"}); err != nil {
		t.Fatal(err)
	}
	acksA, acksB := capturePush(a, protocol.PushSendConfirmed), capturePush(b, protocol.PushSendConfirmed)
	sent, err := a.SendTextMessage(ctx, peer.Identity, "wire hello", protocol.TxtTypePlain)
	if err != nil {
		t.Fatal(err)
	}
	pkt := r.packet(t)
	if pkt.IsRouteFlood() || !bytes.Equal(pkt.Path, []byte{0x21, 0x32}) {
		t.Fatalf("wrong routing: %+v", pkt)
	}
	msg, err := meshcore.TextMessageFromBytes(pkt.Payload)
	if err != nil {
		t.Fatal(err)
	}
	secret, _ := peer.SharedSecret(id.Identity)
	plain := msg.Decrypt(secret)
	if len(plain) < 15 || string(bytes.TrimRight(plain[5:], "\x00")) != "wire hello" || msg.Source != id.PublicKey()[0] {
		t.Fatalf("wrong encrypted message: %x", plain)
	}
	wantACK := meshcore.CalcAckHash(plain[:5+len("wire hello")], id.PublicKeyBytes())
	if sent.Tag != wantACK || sent.EstTimeout == 0 {
		t.Fatalf("sent response: %+v", sent)
	}
	ack := make([]byte, 4)
	put32(ack, wantACK)
	wrapped, err := (&meshcore.MultiPart{Remaining: 2, WrappedType: meshcore.PayloadTypeAck, WrappedPayload: ack}).ToBytes()
	if err != nil {
		t.Fatal(err)
	}
	r.inject(&meshcore.Packet{Header: meshcore.MakeHeader(meshcore.RouteTypeDirect, meshcore.PayloadTypeMultiPart, 0), Payload: wrapped})
	for _, ch := range []<-chan protocol.Response{acksA, acksB} {
		got := nextPush(t, ch).Data.(protocol.PushSendConfirmedResponse)
		if got.AckCode != wantACK {
			t.Fatalf("ACK %x, want %x", got.AckCode, wantACK)
		}
	}
	sent, err = b.SendTextMessage(ctx, peer.Identity, "ver", protocol.TxtTypeCLIData)
	if err != nil || sent.Tag != 0 {
		t.Fatalf("CLI send: %+v %v", sent, err)
	}
	r.packet(t)
	_, err = b.SendChannelTextMessage(ctx, 0, "to public", protocol.TxtTypePlain)
	if err != nil {
		t.Fatal(err)
	}
	pkt = r.packet(t)
	group, err := meshcore.GroupTextFromBytes(pkt.Payload)
	if err != nil {
		t.Fatal(err)
	}
	public, _ := meshcore.NewChannelFromBase64("Public", "izOH6cXN6mrJ5e26oRXNcg==")
	content, err := group.DecryptStruct(public.PSK[:])
	if err != nil || content.Text != "to public" || content.Sender != "Host test" {
		t.Fatalf("group ciphertext: %+v %v", content, err)
	}
}

func TestRemoteRequestsOwnedByRequestingClientAndLearnPaths(t *testing.T) {
	id, peer := testIdentity(1), testIdentity(2)
	s, r, addr := startTestServer(t, id, testConfig(stateDir(t)))
	a, b := testClient(t, addr), testClient(t, addr)
	ctx := testContext(t)
	if err := a.AddUpdateContactFull(ctx, protocol.AddUpdateContactCommand{PublicKey: peer.PublicKey(), Type: meshcore.AdvertTypeRoom, OutPathLen: 255, Name: "Room"}); err != nil {
		t.Fatal(err)
	}
	as, bs := capturePush(a, protocol.PushStatusResponse), capturePush(b, protocol.PushStatusResponse)
	if err := a.SendStatusReq(ctx, peer.Identity); err != nil {
		t.Fatal(err)
	}
	p1 := r.packet(t)
	req1, _ := meshcore.RequestFromBytes(p1.Payload)
	secret, _ := peer.SharedSecret(id.Identity)
	q1 := req1.Decrypt(secret)
	if err := b.SendStatusReq(ctx, peer.Identity); err != nil {
		t.Fatal(err)
	}
	p2 := r.packet(t)
	req2, _ := meshcore.RequestFromBytes(p2.Payload)
	q2 := req2.Decrypt(secret)
	if u32(q1) == u32(q2) || q1[4] != 1 || q2[4] != 1 {
		t.Fatalf("requests not unique status requests: %x %x", q1, q2)
	}
	// Respond in reverse order; source identity alone cannot identify the client.
	for _, q := range [][]byte{q2, q1} {
		response := append([]byte{}, q[:4]...)
		response = append(response, byte(u32(q)))
		r.inject(datagram(t, peer, id, meshcore.PayloadTypeResponse, response))
	}
	for i, ch := range []<-chan protocol.Response{as, bs} {
		response := nextPush(t, ch).Data.(protocol.PushStatusResp)
		expected := q1[0]
		if i == 1 {
			expected = q2[0]
		}
		if response.StatusData[0] != expected {
			t.Fatalf("response reached wrong client: %x", response.StatusData)
		}
	}
	login := capturePush(a, protocol.PushLoginSuccess)
	loginOther := capturePush(b, protocol.PushLoginSuccess)
	if err := a.SendLogin(ctx, peer.Identity, "secret"); err != nil {
		t.Fatal(err)
	}
	pkt := r.packet(t)
	anon, err := meshcore.AnonReqFromBytes(pkt.Payload)
	if err != nil {
		t.Fatal(err)
	}
	body := anon.Decrypt(secret)
	if anon.EphemeralPubKey != id.PublicKey() || u32(body[4:8]) != 0 || string(bytes.TrimRight(body[8:], "\x00")) != "secret" {
		t.Fatalf("room login layout: %x", body)
	}
	// Firmware returns server time, permissions, ACL and firmware level.
	response := make([]byte, 13)
	put32(response, 42)
	response[5], response[6], response[7], response[12] = 1, 1, 3, 7
	r.inject(datagram(t, peer, id, meshcore.PayloadTypeResponse, response))
	got := nextPush(t, login).Data.(protocol.PushLoginSuccessResponse)
	if !got.HasServerInfo || got.ServerTime != 42 || got.Permissions != 1 || got.ACL != 3 || got.FirmwareLevel != 7 {
		t.Fatalf("login push: %+v", got)
	}
	sharedLogin := nextPush(t, loginOther).Data.(protocol.PushLoginSuccessResponse)
	if sharedLogin != got {
		t.Fatalf("second client did not receive shared login state: %+v", sharedLogin)
	}
	if err := b.HasConnection(ctx, peer.Identity); err != nil {
		t.Fatal(err)
	}
	discovery := capturePush(b, protocol.PushPathDiscoveryResponse)
	sent, err := b.SendPathDiscoveryReq(ctx, peer.Identity)
	if err != nil {
		t.Fatal(err)
	}
	pkt = r.packet(t)
	request, _ := meshcore.RequestFromBytes(pkt.Payload)
	plain := request.Decrypt(secret)
	if !pkt.IsRouteFlood() || plain[4] != 3 || plain[5] != 0xfe || u32(plain) != sent.Tag {
		t.Fatalf("discovery request: %+v %x", pkt, plain)
	}
	// Two-byte hop hashes, two hops. The returned path must not be reversed.
	path := []byte{0x42, 0x11, 0x12, 0x21, 0x22, meshcore.PayloadTypeResponse}
	tag := make([]byte, 4)
	put32(tag, sent.Tag)
	path = append(path, tag...)
	path = append(path, 0)
	pkt = datagram(t, peer, id, meshcore.PayloadTypePath, path)
	pkt.Header = meshcore.MakeHeader(meshcore.RouteTypeFlood, meshcore.PayloadTypePath, 0)
	pkt.PathLength = 1
	pkt.Path = []byte{0x31}
	r.inject(pkt)
	paths := nextPush(t, discovery).Data.(protocol.PushPathDiscoveryResp)
	if paths.OutPathLen != 0x42 || !bytes.Equal(paths.OutPath, []byte{0x11, 0x12, 0x21, 0x22}) || !bytes.Equal(paths.InPath, []byte{0x31}) {
		t.Fatalf("discovery paths: %+v", paths)
	}
	contact, err := a.GetContactByKey(ctx, peer.Identity)
	if err != nil || contact.OutPathLen != 255 {
		t.Fatalf("discovery must not learn a route: %+v %v", contact, err)
	}
	if len(r.out) != 0 {
		t.Fatal("discovery must not send a reciprocal path")
	}
	updated := capturePush(a, protocol.PushPathUpdated)
	ordinary := datagram(t, peer, id, meshcore.PayloadTypePath, []byte{0x42, 0x11, 0x12, 0x21, 0x22, 0xff, 1, 2, 3, 4})
	ordinary.Header = meshcore.MakeHeader(meshcore.RouteTypeFlood, meshcore.PayloadTypePath, 0)
	r.inject(ordinary)
	nextPush(t, updated)
	contact, err = a.GetContactByKey(ctx, peer.Identity)
	if err != nil || contact.OutPathLen != 0x42 || contact.OutPath[0] != 0x11 {
		t.Fatalf("learned path: %+v %v", contact, err)
	}
	r.packet(t) // Reciprocal path, routed over the learned path.
	s.mu.Lock()
	s.keepAlive(time.Now().Add(17 * time.Second))
	s.mu.Unlock()
	pkt = r.packet(t)
	request, _ = meshcore.RequestFromBytes(pkt.Payload)
	plain = request.Decrypt(secret)
	if plain[4] != 2 || pkt.PathLength != 0x42 {
		t.Fatalf("keepalive: %+v %x", pkt, plain)
	}
	if err := b.Logout(ctx, peer.Identity); err != nil {
		t.Fatal(err)
	}
	if err := a.HasConnection(ctx, peer.Identity); err != nil {
		t.Fatal("another session's logout cancelled the owner")
	}
	if err := a.Logout(ctx, peer.Identity); err != nil {
		t.Fatal(err)
	}
	if err := a.HasConnection(ctx, peer.Identity); err == nil {
		t.Fatal("logout retained room connection")
	}
}

func TestLegacyStatusReplyWithoutMatchingTagReachesRequestingClient(t *testing.T) {
	id, peer := testIdentity(1), testIdentity(2)
	s, r, addr := startTestServer(t, id, testConfig(""))
	a, b := testClient(t, addr), testClient(t, addr)
	ctx := testContext(t)
	if err := a.AddUpdateContactFull(ctx, protocol.AddUpdateContactCommand{
		PublicKey: peer.PublicKey(), Type: meshcore.AdvertTypeRepeater, OutPathLen: 0, Name: "Repeater",
	}); err != nil {
		t.Fatal(err)
	}
	owner, other := capturePush(a, protocol.PushStatusResponse), capturePush(b, protocol.PushStatusResponse)
	if err := a.SendStatusReq(ctx, peer.Identity); err != nil {
		t.Fatal(err)
	}
	pkt := r.packet(t)
	request, err := meshcore.RequestFromBytes(pkt.Payload)
	if err != nil {
		t.Fatal(err)
	}
	secret, err := peer.SharedSecret(id.Identity)
	if err != nil {
		t.Fatal(err)
	}
	plain := request.Decrypt(secret)
	if len(plain) < 9 || plain[4] != 1 || !bytes.Equal(plain[5:9], []byte{0, 0, 0, 0}) {
		t.Fatalf("not a native status request: %x", plain)
	}
	legacy := make([]byte, 4)
	put32(legacy, u32(plain[:4])^0xffffffff)
	status := []byte{0x12, 0x34, 0x56, 0x78, 0x9a, 0xbc, 0xde, 0xf0, 0x11, 0x22, 0x33, 0x44}
	r.inject(datagram(t, peer, id, meshcore.PayloadTypeResponse, append(legacy, status...)))
	got := nextPush(t, owner).Data.(protocol.PushStatusResp)
	if got.PubKeyPrefix != peer.Identity.Prefix() || !bytes.Equal(got.StatusData, status) {
		t.Fatalf("legacy status response: %+v", got)
	}
	s.mu.Lock()
	outstanding := len(s.pending)
	s.mu.Unlock()
	if outstanding != 0 {
		t.Fatalf("legacy reply left %d pending requests", outstanding)
	}
	r.inject(datagram(t, peer, id, meshcore.PayloadTypeResponse, append(legacy, status...)))
	select {
	case push := <-owner:
		t.Fatalf("duplicate legacy status delivered: %+v", push)
	case push := <-other:
		t.Fatalf("status leaked to another client: %+v", push)
	case <-time.After(100 * time.Millisecond):
	}
}

func TestAmbiguousLegacyStatusDoesNotGuessAClient(t *testing.T) {
	id, peer := testIdentity(1), testIdentity(2)
	s, r, addr := startTestServer(t, id, testConfig(""))
	a, b := testClient(t, addr), testClient(t, addr)
	ctx := testContext(t)
	if err := a.AddUpdateContactFull(ctx, protocol.AddUpdateContactCommand{
		PublicKey: peer.PublicKey(), Type: meshcore.AdvertTypeRepeater, OutPathLen: 0, Name: "Repeater",
	}); err != nil {
		t.Fatal(err)
	}
	first, second := capturePush(a, protocol.PushStatusResponse), capturePush(b, protocol.PushStatusResponse)
	secret, err := peer.SharedSecret(id.Identity)
	if err != nil {
		t.Fatal(err)
	}
	status := []byte{1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12}
	if err := a.SendStatusReq(ctx, peer.Identity); err != nil {
		t.Fatal(err)
	}
	requestA, err := meshcore.RequestFromBytes(r.packet(t).Payload)
	if err != nil {
		t.Fatal(err)
	}
	if err := b.SendStatusReq(ctx, peer.Identity); err != nil {
		t.Fatal(err)
	}
	requestB, err := meshcore.RequestFromBytes(r.packet(t).Payload)
	if err != nil {
		t.Fatal(err)
	}
	tagA, tagB := u32(requestA.Decrypt(secret)), u32(requestB.Decrypt(secret))
	if tagA == tagB {
		t.Fatal("concurrent requests reused a tag")
	}
	unknown := tagA ^ 0xffffffff
	if unknown == tagB {
		unknown++
	}
	sendStatus := func(tag uint32) {
		body := make([]byte, 4, 16)
		put32(body, tag)
		r.inject(datagram(t, peer, id, meshcore.PayloadTypeResponse, append(body, status...)))
	}
	sendStatus(unknown)
	select {
	case push := <-first:
		t.Fatalf("ambiguous response reached first client: %+v", push)
	case push := <-second:
		t.Fatalf("ambiguous response reached second client: %+v", push)
	case <-time.After(100 * time.Millisecond):
	}
	sendStatus(tagB)
	if got := nextPush(t, second).Data.(protocol.PushStatusResp); !bytes.Equal(got.StatusData, status) {
		t.Fatalf("tagged status did not reach second client: %+v", got)
	}
	s.mu.Lock()
	_, remaining := s.pending[tagA]
	count := len(s.pending)
	s.mu.Unlock()
	if !remaining || count != 1 {
		t.Fatalf("tagged response left unexpected pending requests: first=%t count=%d", remaining, count)
	}
	anotherUnknown := unknown + 1
	for anotherUnknown == tagA || anotherUnknown == tagB {
		anotherUnknown++
	}
	sendStatus(anotherUnknown)
	if got := nextPush(t, first).Data.(protocol.PushStatusResp); !bytes.Equal(got.StatusData, status) {
		t.Fatalf("now-unambiguous legacy status did not reach first client: %+v", got)
	}
}

func TestConcurrentLoginAndStatusDoNotStealUntaggedResponse(t *testing.T) {
	id, peer := testIdentity(1), testIdentity(2)
	s, r, addr := startTestServer(t, id, testConfig(""))
	a, b := testClient(t, addr), testClient(t, addr)
	ctx := testContext(t)
	if err := a.AddUpdateContactFull(ctx, protocol.AddUpdateContactCommand{
		PublicKey: peer.PublicKey(), Type: meshcore.AdvertTypeRoom, OutPathLen: 0, Name: "Room",
	}); err != nil {
		t.Fatal(err)
	}
	loginFail := capturePush(a, protocol.PushLoginFail)
	loginA, loginB := capturePush(a, protocol.PushLoginSuccess), capturePush(b, protocol.PushLoginSuccess)
	statusA, statusB := capturePush(a, protocol.PushStatusResponse), capturePush(b, protocol.PushStatusResponse)
	secret, err := peer.SharedSecret(id.Identity)
	if err != nil {
		t.Fatal(err)
	}
	if err := a.SendLogin(ctx, peer.Identity, "password"); err != nil {
		t.Fatal(err)
	}
	loginReq, err := meshcore.AnonReqFromBytes(r.packet(t).Payload)
	if err != nil {
		t.Fatal(err)
	}
	loginPlain := loginReq.Decrypt(secret)
	if len(loginPlain) < 8 || string(bytes.TrimRight(loginPlain[8:], "\x00")) != "password" {
		t.Fatalf("invalid native login request: %x", loginPlain)
	}
	if err := b.SendStatusReq(ctx, peer.Identity); err != nil {
		t.Fatal(err)
	}
	statusReq, err := meshcore.RequestFromBytes(r.packet(t).Payload)
	if err != nil {
		t.Fatal(err)
	}
	statusPlain := statusReq.Decrypt(secret)
	if len(statusPlain) < 9 || statusPlain[4] != 1 {
		t.Fatalf("invalid native status request: %x", statusPlain)
	}
	loginTag, statusTag := u32(loginPlain), u32(statusPlain)
	if loginTag == statusTag {
		t.Fatal("concurrent requests reused a tag")
	}
	unknownTag := statusTag ^ 0xffffffff
	if unknownTag == loginTag {
		unknownTag++
	}
	status := []byte{0x12, 0x34, 0x56, 0x78, 0x9a, 0xbc, 0xde, 0xf0, 0x11, 0x22, 0x33, 0x44}
	response := make([]byte, 4, 16)
	put32(response, unknownTag)
	r.inject(datagram(t, peer, id, meshcore.PayloadTypeResponse, append(response, status...)))
	select {
	case push := <-loginFail:
		t.Fatalf("status response consumed the login: %+v", push)
	case push := <-loginA:
		t.Fatalf("status response appeared to be a login: %+v", push)
	case push := <-statusA:
		t.Fatalf("status response reached the wrong client: %+v", push)
	case push := <-statusB:
		t.Fatalf("ambiguous status response reached a client: %+v", push)
	case <-time.After(100 * time.Millisecond):
	}
	s.mu.Lock()
	_, loginPending := s.pending[loginTag]
	_, statusPending := s.pending[statusTag]
	count := len(s.pending)
	s.mu.Unlock()
	if !loginPending || !statusPending || count != 2 {
		t.Fatalf("ambiguous response consumed a request: login=%t status=%t count=%d", loginPending, statusPending, count)
	}
	response = make([]byte, 4, 16)
	put32(response, statusTag)
	r.inject(datagram(t, peer, id, meshcore.PayloadTypeResponse, append(response, status...)))
	if got := nextPush(t, statusB).Data.(protocol.PushStatusResp); got.PubKeyPrefix != peer.Identity.Prefix() || !bytes.Equal(got.StatusData, status) {
		t.Fatalf("tagged status reached the wrong client or lost data: %+v", got)
	}
	s.mu.Lock()
	_, loginPending = s.pending[loginTag]
	count = len(s.pending)
	s.mu.Unlock()
	if !loginPending || count != 1 {
		t.Fatalf("tagged status consumed login: login=%t count=%d", loginPending, count)
	}
	serverTime := unknownTag + 1
	for serverTime == loginTag || serverTime == statusTag {
		serverTime++
	}
	loginResponse := make([]byte, 13)
	put32(loginResponse, serverTime)
	loginResponse[5], loginResponse[6], loginResponse[7], loginResponse[12] = 1, 1, 3, 7
	r.inject(datagram(t, peer, id, meshcore.PayloadTypeResponse, loginResponse))
	for _, ch := range []<-chan protocol.Response{loginA, loginB} {
		got := nextPush(t, ch).Data.(protocol.PushLoginSuccessResponse)
		if !got.HasServerInfo || got.ServerTime != serverTime || got.Permissions != 1 || got.ACL != 3 || got.FirmwareLevel != 7 {
			t.Fatalf("untagged native login response: %+v", got)
		}
	}
	s.mu.Lock()
	count = len(s.pending)
	s.mu.Unlock()
	if count != 0 {
		t.Fatalf("valid replies left %d pending requests", count)
	}
	if err := b.HasConnection(ctx, peer.Identity); err != nil {
		t.Fatal("native login did not establish a connection:", err)
	}
	select {
	case push := <-loginFail:
		t.Fatalf("delayed false login failure: %+v", push)
	case push := <-statusA:
		t.Fatalf("status leaked to login client: %+v", push)
	default:
	}
}

func TestSignedAdvertsImportExportAndDiscovery(t *testing.T) {
	id, peer := testIdentity(1), testIdentity(2)
	_, r, addr := startTestServer(t, id, testConfig(""))
	c := testClient(t, addr)
	ctx := testContext(t)
	app, _ := (&meshcore.AdvertAppData{Type: "CHAT", Name: "Discovered", Lat: 1234, Lon: -5678}).ToBytes()
	adv := &meshcore.Advert{PublicKey: peer.Identity, Timestamp: 100, RawAppData: app}
	adv.SignWith(peer)
	data, _ := adv.ToBytes()
	push := capturePush(c, protocol.PushNewAdvert)
	r.inject(&meshcore.Packet{Header: meshcore.MakeHeader(meshcore.RouteTypeFlood, meshcore.PayloadTypeAdvert, 0), PathLength: 2, Path: []byte{0x12, 0x34}, Payload: data})
	discovered := nextPush(t, push).Data.(protocol.PushNewAdvertResponse)
	if discovered.AdvertName != "Discovered" || discovered.OutPathLen != 255 {
		t.Fatalf("discovered: %+v", discovered)
	}
	path, err := c.GetAdvertPath(ctx, peer.Identity)
	if err != nil || !bytes.Equal(path.Path, []byte{0x12, 0x34}) {
		t.Fatalf("advert path: %+v %v", path, err)
	}
	exported, err := c.ExportContact(ctx, peer.Identity)
	if err != nil {
		t.Fatal(err)
	}
	pkt, err := meshcore.PacketFromBytes(exported.AdvertData)
	if err != nil {
		t.Fatal(err)
	}
	check, err := meshcore.AdvertFromBytes(pkt.Payload)
	if err != nil || !check.Verify() || check.PublicKey.PublicKey() != peer.PublicKey() || len(pkt.Path) != 0 {
		t.Fatalf("export not signed peer advert: %+v %v", check, err)
	}
	if err := c.RemoveContact(ctx, peer.Identity); err != nil {
		t.Fatal(err)
	}
	if err := c.ImportContact(ctx, exported.AdvertData); err != nil {
		t.Fatal(err)
	}
	contact, err := c.GetContactByKey(ctx, peer.Identity)
	if err != nil || contact.AdvertName != "Discovered" {
		t.Fatalf("import: %+v %v", contact, err)
	}
	exported.AdvertData[len(exported.AdvertData)-1] ^= 1
	if err := c.ImportContact(ctx, exported.AdvertData); err == nil {
		t.Fatal("tampered signed advert accepted")
	}
	if err := c.ShareContact(ctx, peer.Identity); err != nil {
		t.Fatal(err)
	}
	shared := r.packet(t)
	if shared.IsRouteFlood() || shared.PayloadType() != meshcore.PayloadTypeAdvert {
		t.Fatalf("shared contact wrong route: %+v", shared)
	}
}

func TestSigningBuffersArePerClient(t *testing.T) {
	id := testIdentity(1)
	_, _, addr := startTestServer(t, id, testConfig(""))
	a, b := testClient(t, addr), testClient(t, addr)
	ctx := testContext(t)
	for _, c := range []*client.Client{a, b} {
		if _, err := c.SignStart(ctx); err != nil {
			t.Fatal(err)
		}
	}
	if err := a.SignData(ctx, []byte("alpha")); err != nil {
		t.Fatal(err)
	}
	if err := b.SignData(ctx, []byte("beta")); err != nil {
		t.Fatal(err)
	}
	for i, c := range []*client.Client{a, b} {
		sig, err := c.SignFinish(ctx)
		if err != nil {
			t.Fatal(err)
		}
		if !id.Identity.Verify([]byte([]string{"alpha", "beta"}[i]), sig.Signature[:]) {
			t.Fatal("signature belongs to another client")
		}
	}
}

func TestPersistenceFailureReturnsErrorAndRollsBack(t *testing.T) {
	dir := stateDir(t)
	_, _, addr := startTestServer(t, testIdentity(1), testConfig(dir))
	c := testClient(t, addr)
	ctx := testContext(t)
	restore := blockStateReplacement(t, dir)
	if err := c.SetAdvertName(ctx, "not committed"); err == nil {
		t.Fatal("failed persistence reported success")
	}
	info, err := c.AppStart(ctx, 3, "test")
	if err != nil || info.Name != "Host test" {
		t.Fatalf("failed write changed live state: %+v %v", info, err)
	}
	restore()
}

func TestBoundedQueuesDisconnectLaggingClientsAndCancel(t *testing.T) {
	s, _, addr := startTestServer(t, testIdentity(1), testConfig(""))
	a, b := testClient(t, addr), testClient(t, addr)
	ctx := testContext(t)
	// Ensure both have been admitted before snapshotting the session cursors.
	a.GetDeviceTime(ctx)
	b.GetDeviceTime(ctx)
	s.mu.Lock()
	var lagging *session
	for c := range s.clients {
		lagging = c
		break
	}
	other := (*session)(nil)
	for c := range s.clients {
		if c != lagging {
			other = c
		}
	}
	for i := 0; i < maxMessages+1; i++ {
		frame := []byte{protocol.RespChannelMsgRecvV3, 0, 0, 0, 0, 255, 0, 0, 0, 0, 0}
		put32(frame[7:], uint32(i+1))
		frame = append(frame, []byte("bounded")...)
		if !s.queueMessage(frame) {
			t.Fatal("queue failed")
		}
		other.cursor = s.state.Sequence
	}
	select {
	case <-lagging.done:
	default:
		t.Error("lagging cursor not disconnected")
	}
	select {
	case <-other.done:
		t.Error("up-to-date client disconnected")
	default:
	}
	if len(s.state.Messages) != maxMessages {
		t.Errorf("journal grew past bound: %d", len(s.state.Messages))
	}
	s.mu.Unlock()
	if err := s.Close(); err != nil {
		t.Fatal(err)
	}
	conn, err := net.DialTimeout("tcp", addr, 100*time.Millisecond)
	if err == nil {
		conn.Close()
		t.Fatal("listener remains after Close")
	}

	r := newTestRadio()
	srv, err := New(testIdentity(3), r, testConfig(""))
	if err != nil {
		t.Fatal(err)
	}
	l, err := net.Listen("tcp", "127.0.0.1:0")
	if err != nil {
		t.Fatal(err)
	}
	cancelCtx, cancel := context.WithCancel(context.Background())
	done := make(chan error, 1)
	go func() { done <- srv.Serve(cancelCtx, l) }()
	raw, err := net.Dial("tcp", l.Addr().String())
	if err != nil {
		t.Fatal(err)
	}
	raw.Write([]byte{'<'}) // Cancellation must unblock even an incomplete frame.
	cancel()
	select {
	case err := <-done:
		if !errors.Is(err, context.Canceled) {
			t.Fatalf("Serve cancellation: %v", err)
		}
	case <-time.After(3 * time.Second):
		t.Fatal("Serve leaked on cancellation")
	}
	raw.SetReadDeadline(time.Now().Add(time.Second))
	var one [1]byte
	if _, err := raw.Read(one[:]); err == nil {
		t.Fatal("cancelled client remains open")
	}
	raw.Close()
}

func TestConcurrentDisconnectsAndCommands(t *testing.T) {
	_, _, addr := startTestServer(t, testIdentity(1), testConfig(""))
	var wg sync.WaitGroup
	for range 16 {
		wg.Go(func() {
			conn, err := net.Dial("tcp", addr)
			if err != nil {
				t.Error(err)
				return
			}
			defer conn.Close()
			conn.SetDeadline(time.Now().Add(3 * time.Second))
			frame, _ := protocol.FrameEncode('<', []byte{protocol.CmdGetDeviceTime})
			for range 5 {
				if _, err := conn.Write(frame); err != nil {
					t.Error(err)
					return
				}
				if _, err := readFrame(conn); err != nil {
					t.Error(err)
					return
				}
			}
		})
	}
	wg.Wait()
}

func TestRoomSignedMessageACKAndPersistentSyncCursor(t *testing.T) {
	id, room := testIdentity(1), testIdentity(2)
	s, r, addr := startTestServer(t, id, testConfig(stateDir(t)))
	c := testClient(t, addr)
	ctx := testContext(t)
	if _, err := c.DeviceQuery(ctx); err != nil {
		t.Fatal(err)
	}
	if err := c.AddUpdateContactFull(ctx, protocol.AddUpdateContactCommand{PublicKey: room.PublicKey(), Type: meshcore.AdvertTypeRoom, OutPathLen: 0, Name: "Room"}); err != nil {
		t.Fatal(err)
	}
	waiting := watchMessages(c)
	plain := []byte{123, 0, 0, 0, protocol.TxtTypeSignedPlain << 2, 0xaa, 0xbb, 0xcc, 0xdd}
	plain = append(plain, []byte("from room member")...)
	r.inject(datagram(t, room, id, meshcore.PayloadTypeTxtMsg, plain))
	waitMessage(t, waiting)
	msgs, err := c.GetWaitingMessages(ctx)
	if err != nil || len(msgs) != 1 || msgs[0].Contact.TxtType != protocol.TxtTypeSignedPlain ||
		!bytes.Equal(msgs[0].Contact.SenderPrefix, []byte{0xaa, 0xbb, 0xcc, 0xdd}) || msgs[0].Contact.Text != "from room member" {
		t.Fatalf("room signed message: %+v %v", msgs, err)
	}
	ack := r.packet(t)
	if ack.PayloadType() != meshcore.PayloadTypeAck || u32(ack.Payload) != meshcore.CalcAckHash(plain, id.PublicKeyBytes()) {
		t.Fatalf("room ACK did not use recipient key: %+v", ack)
	}
	s.mu.Lock()
	syncSince := s.findContact(room.PublicKeyBytes()).SyncSince
	s.mu.Unlock()
	if syncSince != 123 {
		t.Fatalf("room sync timestamp: %d", syncSince)
	}
	// The next login must request only newer room messages.
	if err := c.SendLogin(ctx, room.Identity, ""); err != nil {
		t.Fatal(err)
	}
	login := r.packet(t)
	anon, err := meshcore.AnonReqFromBytes(login.Payload)
	if err != nil {
		t.Fatal(err)
	}
	secret, _ := room.SharedSecret(id.Identity)
	if got := u32(anon.Decrypt(secret)[4:8]); got != 123 {
		t.Fatalf("login lost room cursor: %d", got)
	}
	data, err := os.ReadFile(filepath.Join(s.cfg.StateDir, "companion.json"))
	if err != nil {
		t.Fatal(err)
	}
	var saved state
	if err := json.Unmarshal(data, &saved); err != nil {
		t.Fatal(err)
	}
	found := false
	for _, contact := range saved.Contacts {
		if contact.PublicKey == room.PublicKey() && contact.SyncSince == 123 {
			found = true
		}
	}
	if !found || len(saved.Messages) != 1 {
		t.Fatal("room sync cursor not persisted with incoming message")
	}
}

func TestMaximumLengthSignedRoomMessageFitsProtocol13Frame(t *testing.T) {
	id, room := testIdentity(1), testIdentity(2)
	_, r, addr := startTestServer(t, id, testConfig(""))
	c := testClient(t, addr)
	ctx := testContext(t)
	if info, err := c.DeviceQuery(ctx); err != nil || info.FirmwareVersion != 13 {
		t.Fatalf("protocol 13 device query: %+v %v", info, err)
	}
	if err := c.AddUpdateContactFull(ctx, protocol.AddUpdateContactCommand{
		PublicKey: room.PublicKey(), Type: meshcore.AdvertTypeRoom, OutPathLen: 0, Name: "Room",
	}); err != nil {
		t.Fatal(err)
	}
	waiting := watchMessages(c)
	text := strings.Repeat("x", meshcore.MaxTextLen)
	plain := []byte{123, 0, 0, 0, protocol.TxtTypeSignedPlain << 2, 0xaa, 0xbb, 0xcc, 0xdd}
	plain = append(plain, text...)
	r.inject(datagram(t, room, id, meshcore.PayloadTypeTxtMsg, plain))
	waitMessage(t, waiting)
	messages, err := c.GetWaitingMessages(ctx)
	if err != nil || len(messages) != 1 {
		t.Fatalf("signed room message missing: %+v %v", messages, err)
	}
	want := text[:protocol.MaxFrameSize-20]
	if got := messages[0].Contact; got.TxtType != protocol.TxtTypeSignedPlain ||
		got.Text != want || !bytes.Equal(got.SenderPrefix, plain[5:9]) {
		t.Fatalf("signed room message not truncated to companion frame: %+v", got)
	}
	ack := r.packet(t)
	if ack.PayloadType() != meshcore.PayloadTypeAck ||
		u32(ack.Payload) != meshcore.CalcAckHash(plain, id.PublicKeyBytes()) {
		t.Fatalf("signed room message was not acknowledged in full: %+v", ack)
	}
}

func TestRadioCallbackDoesNotWaitForStateOrSlowClient(t *testing.T) {
	failures := make(chan error, 4)
	cfg := testConfig("")
	cfg.ErrorHandler = func(err error) { failures <- err }
	s, _, addr := startTestServer(t, testIdentity(1), cfg)
	c := testClient(t, addr)
	if _, err := c.GetDeviceTime(testContext(t)); err != nil {
		t.Fatal(err)
	}
	// Emulate a blocked disk/command owner. Radio ingress must remain bounded and
	// nonblocking; when overloaded, it must explicitly terminate client streams.
	s.mu.Lock()
	finished := make(chan struct{})
	go func() {
		for range 300 {
			s.receive(&meshcore.Packet{Header: meshcore.MakeHeader(meshcore.RouteTypeDirect, meshcore.PayloadTypeAck, 0), Payload: []byte{1, 2, 3, 4}})
		}
		close(finished)
	}()
	select {
	case <-finished:
	case <-time.After(time.Second):
		s.mu.Unlock()
		t.Fatal("radio callback waited for server state")
	}
	s.mu.Unlock()
	select {
	case <-failures:
	case <-time.After(3 * time.Second):
		t.Fatal("receive overflow was silent")
	}
	if _, err := c.GetDeviceTime(testContext(t)); err == nil {
		t.Fatal("overflow did not disconnect client")
	}
}

func TestRejectsIncompletePersistedState(t *testing.T) {
	for _, content := range []string{`{}`, `{"Version":1}`} {
		dir := stateDir(t)
		if err := os.WriteFile(filepath.Join(dir, "companion.json"), []byte(content), 0600); err != nil {
			t.Fatal(err)
		}
		if srv, err := New(testIdentity(1), newTestRadio(), testConfig(dir)); err == nil {
			srv.Close()
			t.Fatalf("accepted incomplete state: %s", content)
		}
	}
}
