package sharedroom

import (
	"bytes"
	"context"
	"encoding/binary"
	"encoding/hex"
	"encoding/json"
	"os"
	"strings"
	"testing"

	meshcore "github.com/meshcore-go/meshcore-go"
)

func unhex(t *testing.T, text string) []byte {
	t.Helper()
	b, err := hex.DecodeString(text)
	if err != nil {
		t.Fatal(err)
	}
	return b
}
func peers() (Identity, meshcore.LocalIdentity) {
	var roomSeed, clientSeed [32]byte
	for i := 0; i < 32; i += 4 {
		roomSeed[i] = 1
	}
	room := meshcore.NewLocalIdentityFromSeed(roomSeed)
	client := meshcore.NewLocalIdentityFromSeed(clientSeed)
	return Identity{Alias: "A", Name: "WelcomeYEG", Key: room, PathHashMode: 2}, client
}
func wire(t *testing.T, p *meshcore.Packet) []byte {
	t.Helper()
	b, e := p.ToBytes()
	if e != nil {
		t.Fatal(e)
	}
	return b
}
func anon(t *testing.T, room Identity, client meshcore.LocalIdentity, stamp uint32) *meshcore.Packet {
	t.Helper()
	plain := binary.LittleEndian.AppendUint32(nil, stamp)
	plain = binary.LittleEndian.AppendUint32(plain, 0)
	plain = append(plain, []byte("room\x00")...)
	secret, e := client.SharedSecret(room.Key.Identity)
	if e != nil {
		t.Fatal(e)
	}
	sealed, e := meshcore.EncryptThenMAC(secret, plain)
	if e != nil {
		t.Fatal(e)
	}
	payload := append([]byte{room.Key.PublicKey()[0]}, client.PublicKeyBytes()...)
	payload = append(payload, sealed...)
	return &meshcore.Packet{Header: meshcore.MakeHeader(meshcore.RouteTypeFlood, meshcore.PayloadTypeAnonReq, 0), PathLength: 0x82, Path: []byte{1, 2, 3, 4, 5, 6}, Payload: payload}
}
func TestPinnedNativePacketAndRoomExchange(t *testing.T) {
	room, client := peers()
	codec := Codec{Identities: []Identity{room}}
	member := Member{Client: client.String(), Route: Route{Known: true, Length: 0x82, Path: []byte{1, 2, 3, 4, 5, 6}}.encode()}
	lookup := func(_ context.Context, alias, prefix string) ([]Member, error) {
		if alias != room.Alias {
			t.Fatal("wrong alias lookup")
		}
		if strings.HasPrefix(member.Client, prefix) {
			return []Member{member}, nil
		}
		return nil, nil
	}
	// Consume the committed C++ MeshCore oracle vector rather than generating
	// the ciphertext using the decoder being checked.
	data, err := os.ReadFile("../../testdata/parity/native-events.jsonl")
	if err != nil {
		t.Fatal(err)
	}
	var native []byte
	for _, line := range bytes.Split(data, []byte{'\n'}) {
		var event struct{ Scenario, Event, Bytes string }
		if json.Unmarshal(line, &event) == nil && event.Scenario == "encrypted-wire-vectors" && event.Event == "encrypted_packet" {
			native = unhex(t, event.Bytes)
		}
	}
	if len(native) == 0 {
		t.Fatal("native ciphertext fixture missing")
	}
	decoded, err := codec.Decode(context.Background(), native, lookup)
	if err != nil || decoded == nil {
		t.Fatalf("decode: %v", err)
	}
	if decoded.Operation.Op != "post" || decoded.Operation.Client != client.String() || decoded.Operation.Text != "hello" || decoded.Operation.Timestamp != 1700000000 {
		t.Fatalf("decoded native operation: %+v", decoded.Operation)
	}
	ack, _, err := codec.Response(decoded, Result{Respond: true})
	if err != nil {
		t.Fatal(err)
	}
	// Native oracle ACK for the same five-byte header, hello and sender key.
	if !bytes.Equal(ack.Payload, unhex(t, "8ae1e1f3")) {
		t.Fatalf("native ACK differs: %x", ack.Payload)
	}
	altered := bytes.Clone(native)
	altered[len(altered)-1] ^= 1
	invalid, err := codec.Decode(context.Background(), altered, lookup)
	if err != nil || invalid != nil {
		t.Fatal("bad MAC was accepted")
	}
	login, err := codec.Decode(context.Background(), wire(t, anon(t, room, client, 1700000001)), lookup)
	if err != nil || login == nil || login.Operation.Op != "login" || login.Operation.Password != "room" || *login.Operation.Since != 0 {
		t.Fatalf("native login: %+v %v", login, err)
	}
	response, _, err := codec.Response(login, Result{Respond: true})
	if err != nil {
		t.Fatal(err)
	}
	request, err := meshcore.RequestFromBytes(response.Payload)
	if err != nil {
		t.Fatal(err)
	}
	secret, _ := client.SharedSecret(room.Key.Identity)
	path, err := meshcore.ParsePathPayload(request.Decrypt(secret))
	if err != nil {
		t.Fatal(err)
	}
	if path.ExtraType != meshcore.PayloadTypeResponse || !bytes.Equal(path.Path, login.Packet.Path) || len(path.Extra) < 13 || path.Extra[7] != 2 || path.Extra[12] != 1 {
		t.Fatalf("native login response: %+v", path)
	}
	delivery := Delivery{Alias: "A", Client: client.String(), Route: member.Route, Message: Message{Timestamp: 1700000002, Author: room.Key.String(), Text: "shared hello"}}
	packet, proof, err := codec.Delivery(room, delivery)
	if err != nil {
		t.Fatal(err)
	}
	request, err = meshcore.RequestFromBytes(packet.Payload)
	if err != nil {
		t.Fatal(err)
	}
	plain := request.Decrypt(secret)
	if len(plain) < 21 || plain[4]>>2 != 2 || !bytes.Equal(plain[5:9], room.Key.PublicKeyBytes()[:4]) || string(cstring(plain[9:])) != "shared hello" {
		t.Fatalf("native signed delivery: %x", plain)
	}
	want := meshcore.CalcAckHash(plain[:9+len("shared hello")], client.PublicKeyBytes())
	if proof != hexUint(want) {
		t.Fatalf("recipient-bound native proof %s != %s", proof, hexUint(want))
	}
	advert, err := codec.Advertisement(room, 1700000003)
	if err != nil {
		t.Fatal(err)
	}
	if advert.PayloadType() != meshcore.PayloadTypeAdvert {
		t.Fatal("room advert type")
	}
}
func hexUint(n uint32) string {
	const alphabet = "0123456789abcdef"
	b := make([]byte, 8)
	for i := 7; i >= 0; i-- {
		b[i] = alphabet[n&15]
		n >>= 4
	}
	return string(b)
}
func TestNativePathAndAmbiguousAck(t *testing.T) {
	room, client := peers()
	codec := Codec{Identities: []Identity{room}}
	proof := "11223344"
	member := Member{Client: client.String(), Route: Route{Known: true}.encode(), Pending: &Pending{DeliveryID: "current", Proof: &proof, State: "sent"}}
	lookup := func(_ context.Context, _ string, _ string) ([]Member, error) { return []Member{member}, nil }
	plain := []byte{0x82, 9, 8, 7, 6, 5, 4, meshcore.PayloadTypeAck, 0x44, 0x33, 0x22, 0x11}
	// Encryption reuses the pinned library. The service receives the bundled
	// PATH proof atomically with its new route, never after pumping the next post.
	clientSide := Identity{Key: client, PathHashMode: 2}
	packet, err := datagram(clientSide, room.Key.String(), meshcore.PayloadTypePath, plain, Route{})
	if err != nil {
		t.Fatal(err)
	}
	decoded, err := codec.Decode(context.Background(), wire(t, packet), lookup)
	if err != nil || decoded == nil || decoded.Operation.Op != "path" || decoded.Operation.Proof != proof || decoded.Operation.DeliveryID != "current" {
		t.Fatalf("PATH: %+v %v", decoded, err)
	}
	back, err := route(decoded.Operation.Route, Route{})
	if err != nil {
		t.Fatal(err)
	}
	if !back.Known || !bytes.Equal(back.Path, plain[1:7]) {
		t.Fatalf("PATH route: %+v", back)
	}
	ack := routed(room, meshcore.PayloadTypeAck, plain[8:], Route{})
	decoded, err = codec.Decode(context.Background(), wire(t, ack), lookup)
	if err != nil || decoded == nil || decoded.Operation.Client != client.String() {
		t.Fatal("owned ACK not found")
	}
	ambiguous := func(_ context.Context, _ string, _ string) ([]Member, error) { return []Member{member, member}, nil }
	decoded, err = codec.Decode(context.Background(), wire(t, ack), ambiguous)
	if err != nil || decoded != nil {
		t.Fatal("ambiguous bare ACK accepted")
	}
	// Signed room packets are outbound history, never new logical posts.
	signed, _, err := codec.Delivery(room, Delivery{Alias: "A", Client: client.String(), Message: Message{Author: room.Key.String(), Text: "no relay"}})
	if err != nil {
		t.Fatal(err)
	}
	reverse := Codec{Identities: []Identity{{Alias: "B", Key: client}}}
	decoded, err = reverse.Decode(context.Background(), wire(t, signed), func(_ context.Context, _ string, _ string) ([]Member, error) {
		return []Member{{Client: room.Key.String()}}, nil
	})
	if err != nil || decoded != nil {
		t.Fatal("signed delivery became a post")
	}
}
