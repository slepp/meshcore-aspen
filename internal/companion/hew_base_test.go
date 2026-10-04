package companion

import (
	"bufio"
	"bytes"
	"context"
	"encoding/binary"
	"encoding/hex"
	"encoding/json"
	"fmt"
	"io"
	"os"
	"os/exec"
	"path/filepath"
	"reflect"
	"strconv"
	"strings"
	"testing"
	"time"

	meshcore "github.com/meshcore-go/meshcore-go"
	protocol "github.com/meshcore-go/meshcore-go/companion"
	"github.com/meshcore-go/meshcore-go/hardware"
	rolestate "meshcore.local/meshcore/internal/state"
)

type hewBaseProcess struct {
	t       *testing.T
	command *exec.Cmd
	input   io.WriteCloser
	output  *bufio.Scanner
	stderr  bytes.Buffer
}

func TestHewBaseRollbackAuthority(t *testing.T) {
	dir := os.Getenv("MESHCORE_HEW_BASE_ROLLBACK_DIR")
	if dir == "" {
		t.Skip("set MESHCORE_HEW_BASE_ROLLBACK_DIR to a synthetic reverse handover")
	}
	expected, err := hex.DecodeString(os.Getenv("MESHCORE_HEW_BASE_ROLLBACK_PUBLIC"))
	if err != nil || len(expected) != 32 {
		t.Fatal("rollback test requires an explicit synthetic public key")
	}
	id, err := rolestate.Identity(filepath.Dir(dir), filepath.Base(dir))
	if err != nil || !bytes.Equal(id.PublicKeyBytes(), expected) {
		t.Fatalf("Go identity authority load: %v", err)
	}
	document, err := rolestate.ReadRoleJSON(filepath.Join(dir, "companion.json"))
	if os.IsNotExist(err) {
		document = []byte("null")
	} else if err != nil {
		t.Fatal(err)
	}
	clone := t.TempDir()
	for _, name := range []string{"identity-state.json", "identity.expanded", "identity.seed", "companion.json"} {
		data, err := os.ReadFile(filepath.Join(dir, name))
		if os.IsNotExist(err) {
			continue
		}
		if err != nil {
			t.Fatal(err)
		}
		if err := os.WriteFile(filepath.Join(clone, name), data, 0600); err != nil {
			t.Fatal(err)
		}
	}
	cfg := testConfig(clone)
	s, err := New(id, &sourceTestRadio{testRadio: newTestRadio(), set: func(context.Context, float64) error { return nil }}, cfg)
	if err != nil {
		t.Fatal(err)
	}
	defer s.Close()
	var wanted struct {
		Name     string
		Contacts []json.RawMessage
		Messages []json.RawMessage
		Channels []json.RawMessage
		Sequence uint64
	}
	if err := json.Unmarshal(document, &wanted); err != nil {
		t.Fatal(err)
	}
	if len(bytes.TrimSpace(document)) == 0 || bytes.Equal(bytes.TrimSpace(document), []byte("null")) {
		wanted.Name = cfg.Name
		wanted.Channels = make([]json.RawMessage, maxChannels)
	}
	if s.state.Name != wanted.Name || len(s.state.Contacts) != len(wanted.Contacts) ||
		len(s.state.Messages) != len(wanted.Messages) || len(s.state.Channels) != len(wanted.Channels) ||
		s.state.Sequence != wanted.Sequence {
		t.Fatal("Go companion did not load the reverse-handover document")
	}
}

func startHewBase(t *testing.T, path string, seed []byte) *hewBaseProcess {
	t.Helper()
	binary := os.Getenv("MESHCORE_HEW_BASE_BIN")
	if binary == "" {
		t.Skip("set MESHCORE_HEW_BASE_BIN to run Hew companion differential")
	}
	p := &hewBaseProcess{t: t}
	p.command = exec.Command(binary, path, hex.EncodeToString(seed))
	var err error
	p.input, err = p.command.StdinPipe()
	if err != nil {
		t.Fatal(err)
	}
	output, err := p.command.StdoutPipe()
	if err != nil {
		t.Fatal(err)
	}
	p.output = bufio.NewScanner(output)
	p.output.Buffer(make([]byte, 4096), 16*1024*1024)
	p.command.Stderr = &p.stderr
	if err := p.command.Start(); err != nil {
		t.Fatal(err)
	}
	t.Cleanup(func() {
		_ = p.input.Close()
		_ = p.command.Process.Kill()
		_ = p.command.Wait()
	})
	return p
}

func (p *hewBaseProcess) send(line string) {
	p.t.Helper()
	if _, err := fmt.Fprintln(p.input, line); err != nil {
		p.t.Fatal(err)
	}
}

func (p *hewBaseProcess) line() string {
	p.t.Helper()
	if !p.output.Scan() {
		p.t.Fatalf("Hew companion exited: %s (%v)", p.stderr.String(), p.output.Err())
	}
	return p.output.Text()
}

func (p *hewBaseProcess) events(line string) map[int64][][]byte {
	events, _ := p.exchange(line)
	return events
}

func (p *hewBaseProcess) exchange(line string) (map[int64][][]byte, []testTransmission) {
	p.t.Helper()
	p.send(line)
	result := map[int64][][]byte{}
	var transmissions []testTransmission
	for {
		line := p.line()
		if line == "END" {
			break
		}
		if strings.HasPrefix(line, "TX ") {
			fields := strings.Fields(line)
			if len(fields) != 4 {
				p.t.Fatalf("invalid TX plan: %q", line)
			}
			priority, err := strconv.ParseUint(fields[1], 10, 8)
			if err != nil {
				p.t.Fatal(err)
			}
			delay, err := strconv.ParseInt(fields[2], 10, 64)
			if err != nil {
				p.t.Fatal(err)
			}
			raw, err := hex.DecodeString(fields[3])
			if err != nil {
				p.t.Fatal(err)
			}
			transmissions = append(transmissions, testTransmission{Data: raw, Priority: uint8(priority), Delay: time.Duration(delay) * time.Millisecond})
			continue
		}
		fields := strings.SplitN(line, " ", 3)
		if len(fields) != 3 || fields[0] != "FRAME" {
			p.t.Fatalf("invalid Hew companion event: %q", line)
		}
		id, err := strconv.ParseInt(fields[1], 10, 64)
		if err != nil {
			p.t.Fatal(err)
		}
		frame, err := hex.DecodeString(fields[2])
		if err != nil {
			p.t.Fatal(err)
		}
		result[id] = append(result[id], frame)
	}
	return result, transmissions
}

func TestHewBaseRFDifferential(t *testing.T) {
	if os.Getenv("MESHCORE_HEW_BASE_BIN") == "" {
		t.Skip("set MESHCORE_HEW_BASE_BIN")
	}
	id, peer := testIdentity(1), testIdentity(2)
	cfg := testConfig("")
	cfg.Name, cfg.AdvertInterval = "Birch-Base", 0
	radio := newTestRadio()
	s, err := New(id, radio, cfg)
	if err != nil {
		t.Fatal(err)
	}
	defer s.Close()
	s.mu.Lock()
	s.state.Preferences.PathHashMode = 2
	c := contact{ContactResponse: protocol.ContactResponse{PublicKey: peer.PublicKey(), Type: meshcore.AdvertTypeChat, OutPathLen: 129, AdvertName: "Peer"}}
	copy(c.OutPath[:], []byte{17, 18, 19})
	s.state.Contacts = []contact{c}
	var psk [16]byte
	for i := range psk {
		psk[i] = byte(i)
	}
	s.state.Channels[0], err = meshcore.NewChannelFromPSK("Synthetic", psk[:])
	if err != nil {
		s.mu.Unlock()
		t.Fatal(err)
	}
	s.hydrate()
	dir := t.TempDir()
	path := filepath.Join(dir, "companion.json")
	saved, err := json.Marshal(s.state)
	s.mu.Unlock()
	if err != nil {
		t.Fatal(err)
	}
	if err := os.WriteFile(path, saved, 0600); err != nil {
		t.Fatal(err)
	}
	seed := id.Seed()
	p := startHewBase(t, path, seed[:])
	p.send("CONNECT 1")
	if p.line() != "CONNECTED true" {
		t.Fatal("Hew client did not connect")
	}
	client := commandSession(t, s)
	s.mu.Lock()
	s.clients[client] = struct{}{}
	s.mu.Unlock()
	radio.mu.Lock()
	radio.jobs = nil
	radio.mu.Unlock()
	for _, attempt := range []byte{0, 4} {
		cmd := []byte{2, 0, attempt, 0, 0, 0, 0}
		binary.LittleEndian.PutUint32(cmd[3:], 1700000000)
		key := peer.PublicKey()
		cmd = append(cmd, key[:6]...)
		cmd = append(cmd, []byte("differential DM")...)
		s.mu.Lock()
		s.command(client, cmd)
		s.mu.Unlock()
		want := drain(client)
		got, plans := p.exchange(fmt.Sprintf("SEND 1 %x %d 1000 1", cmd, time.Now().Unix()))
		if !reflect.DeepEqual(want, got[1]) || len(plans) != 1 {
			t.Fatalf("DM responses Go=%x Hew=%x plans=%v", want, got, plans)
		}
		radio.mu.Lock()
		job := radio.jobs[len(radio.jobs)-1]
		radio.mu.Unlock()
		goPacket, err := meshcore.PacketFromBytes(job.Data)
		if err != nil {
			t.Fatal(err)
		}
		hewPacket, err := meshcore.PacketFromBytes(plans[0].Data)
		if err != nil {
			t.Fatal(err)
		}
		if goPacket.Header != hewPacket.Header || goPacket.PathLength != hewPacket.PathLength || !bytes.Equal(goPacket.Path, hewPacket.Path) || job.Priority != plans[0].Priority || job.Delay != plans[0].Delay {
			t.Fatalf("DM routing/schedule differs: Go=%+v Hew=%+v", job, plans[0])
		}
		secret, _ := peer.SharedSecret(id.Identity)
		goText, _ := meshcore.TextMessageFromBytes(goPacket.Payload)
		hewText, _ := meshcore.TextMessageFromBytes(hewPacket.Payload)
		goPlain, hewPlain := goText.Decrypt(secret), hewText.Decrypt(secret)
		length := 5 + len("differential DM")
		if attempt > 3 {
			length += 2
		}
		if len(goPlain) < length || len(hewPlain) < length || !bytes.Equal(goPlain[:length], hewPlain[:length]) {
			t.Fatalf("DM semantic plaintext Go=%x Hew=%x", goPlain, hewPlain)
		}
	}
	cmd := []byte{3, 0, 0, 0, 0, 0, 0}
	binary.LittleEndian.PutUint32(cmd[3:], 1700000010)
	cmd = append(cmd, []byte("channel differential")...)
	s.mu.Lock()
	s.command(client, cmd)
	s.mu.Unlock()
	want := drain(client)
	got, plans := p.exchange(fmt.Sprintf("SEND 1 %x %d 2000 1", cmd, time.Now().Unix()))
	if !reflect.DeepEqual(want, got[1]) || len(plans) != 1 {
		t.Fatalf("channel responses Go=%x Hew=%x", want, got)
	}
	radio.mu.Lock()
	job := radio.jobs[len(radio.jobs)-1]
	radio.mu.Unlock()
	goPacket, err := meshcore.PacketFromBytes(job.Data)
	if err != nil {
		t.Fatal(err)
	}
	hewPacket, err := meshcore.PacketFromBytes(plans[0].Data)
	if err != nil {
		t.Fatal(err)
	}
	if !bytes.Equal(goPacket.Payload, hewPacket.Payload) || goPacket.Header != hewPacket.Header || goPacket.PathLength != hewPacket.PathLength || job.Priority != plans[0].Priority || job.Delay != plans[0].Delay {
		t.Fatalf("channel wire/routing differs: Go=%+v Hew=%+v", job, plans[0])
	}
	raw := []byte{62, 0, 9, 8, 7}
	cmd = append([]byte{65, 73}, raw...)
	s.mu.Lock()
	s.command(client, cmd)
	s.mu.Unlock()
	want = drain(client)
	got, plans = p.exchange(fmt.Sprintf("SEND 1 %x %d 3000 1", cmd, time.Now().Unix()))
	if !reflect.DeepEqual(want, got[1]) || len(plans) != 1 || !bytes.Equal(plans[0].Data, raw) || plans[0].Priority != 73 {
		t.Fatalf("raw packet Go=%x Hew=%x plans=%+v", want, got, plans)
	}
	second := commandSession(t, s)
	s.mu.Lock()
	s.clients[second] = struct{}{}
	s.mu.Unlock()
	p.send("CONNECT 2")
	if p.line() != "CONNECTED true" {
		t.Fatal("second Hew client did not connect")
	}
	clients := map[int64]*session{1: client, 2: second}
	send := func(clientID int64, command []byte, wall uint32) ([][]byte, testTransmission) {
		t.Helper()
		s.mu.Lock()
		s.command(clients[clientID], command)
		s.mu.Unlock()
		frames := drain(clients[clientID])
		events, tx := p.exchange(fmt.Sprintf("SEND %d %x %d 4000 1", clientID, command, wall))
		if !reflect.DeepEqual(frames, events[clientID]) || len(tx) != 1 {
			t.Fatalf("command %d responses Go=%x Hew=%x transmissions=%d", command[0], frames, events[clientID], len(tx))
		}
		radio.mu.Lock()
		job := radio.jobs[len(radio.jobs)-1]
		radio.mu.Unlock()
		if job.Priority != tx[0].Priority || job.Delay != tx[0].Delay {
			t.Fatalf("command %d schedule Go=%+v Hew=%+v", command[0], job, tx[0])
		}
		return frames, tx[0]
	}
	receive := func(packet *meshcore.Packet) {
		t.Helper()
		packet.SNR, packet.RSSI = 3, -90
		data, err := packet.ToBytes()
		if err != nil {
			t.Fatal(err)
		}
		s.mu.Lock()
		s.handlePacket(packet, s.receiveContext(packet))
		s.mu.Unlock()
		events, tx := p.exchange(fmt.Sprintf("RX %x 12 -90 %d 5000", data, time.Now().Unix()))
		if len(tx) != 0 {
			t.Fatalf("response unexpectedly transmitted: %+v", tx)
		}
		for id, c := range clients {
			var received [][]byte
			for _, frame := range append(events[0], events[id]...) {
				if frame[0] != protocol.PushLogRxData {
					received = append(received, frame)
				}
			}
			if frames := drain(c); !reflect.DeepEqual(frames, received) {
				t.Fatalf("RF %d client %d Go=%x Hew=%x", packet.PayloadType(), id, frames, received)
			}
		}
	}
	secret, err := peer.SharedSecret(id.Identity)
	if err != nil {
		t.Fatal(err)
	}
	peerKey := peer.PublicKey()
	ownKey := id.PublicKey()
	replyPacket := func(kind byte, plain []byte) *meshcore.Packet {
		t.Helper()
		cipher, err := meshcore.EncryptThenMAC(secret, plain)
		if err != nil {
			t.Fatal(err)
		}
		return &meshcore.Packet{Header: meshcore.MakeHeader(meshcore.RouteTypeDirect, kind, 0),
			Payload: append([]byte{ownKey[0], peerKey[0]}, cipher...)}
	}
	for index, kind := range []byte{26, 27, 39, 50, 52, 57} {
		command := []byte{kind}
		if kind == 39 {
			command = append(command, 0, 0, 0)
		} else if kind == 52 {
			command = append(command, 0)
		}
		command = append(command, peerKey[:]...)
		if kind == 26 {
			command = append(command, []byte("01234567890123456789")...)
		} else if kind == 50 || kind == 57 {
			command = append(command, 0x99, 1, 2, 3)
		}
		requester := int64(index%2 + 1)
		s.mu.Lock()
		s.command(clients[requester], command)
		s.mu.Unlock()
		frames := drain(clients[requester])
		radio.mu.Lock()
		job := radio.jobs[len(radio.jobs)-1]
		radio.mu.Unlock()
		goPacket, err := meshcore.PacketFromBytes(job.Data)
		if err != nil {
			t.Fatal(err)
		}
		offset := 2
		if kind == 26 || kind == 57 {
			offset = 33
		}
		goPlain, err := meshcore.MACThenDecrypt(secret, goPacket.Payload[offset:])
		if err != nil {
			t.Fatal(err)
		}
		tag := binary.LittleEndian.Uint32(goPlain)
		events, tx := p.exchange(fmt.Sprintf("SEND %d %x %d 4000 1", requester, command, tag))
		if !reflect.DeepEqual(frames, events[requester]) || len(tx) != 1 {
			t.Fatalf("request %d Go=%x Hew=%x transmissions=%d", kind, frames, events[requester], len(tx))
		}
		hewPacket, err := meshcore.PacketFromBytes(tx[0].Data)
		if err != nil {
			t.Fatal(err)
		}
		hewPlain, err := meshcore.MACThenDecrypt(secret, hewPacket.Payload[offset:])
		if err != nil {
			t.Fatal(err)
		}
		length := 9
		if kind == 26 {
			length = 19
		} else if kind == 50 || kind == 57 {
			length = 8
		}
		if goPacket.Header != hewPacket.Header || goPacket.PathLength != hewPacket.PathLength ||
			!bytes.Equal(goPacket.Path, hewPacket.Path) || !bytes.Equal(goPacket.Payload[:offset], hewPacket.Payload[:offset]) ||
			len(goPlain) < length || len(hewPlain) < length || !bytes.Equal(goPlain[:length], hewPlain[:length]) ||
			job.Priority != tx[0].Priority || job.Delay != tx[0].Delay {
			t.Fatalf("request %d Go=%+v plain=%x Hew=%+v plain=%x", kind, goPacket, goPlain, hewPacket, hewPlain)
		}
		data := make([]byte, 4)
		binary.LittleEndian.PutUint32(data, tag)
		if kind == 26 {
			data = append(data, 'O', 'K')
		} else {
			data = append(data, 0x99, 1, 2, 3)
		}
		responseType := byte(meshcore.PayloadTypeResponse)
		if kind == 52 {
			data = append([]byte{129, 17, 18, 19, meshcore.PayloadTypeResponse}, data...)
			responseType = meshcore.PayloadTypePath
		}
		receive(replyPacket(responseType, data))
	}
	for _, encoded := range []byte{0, 1, 65, 129} {
		width, count, _ := decodePath(encoded)
		path := bytes.Repeat([]byte{17}, width*count)
		for _, command := range [][]byte{
			append(append([]byte{25, encoded}, path...), 1, 2, 3, 4),
			append(append([]byte{62, 0, encoded}, path...), 1, 0, 9, 8, 7),
		} {
			_, tx := send(1, command, uint32(time.Now().Unix()))
			radio.mu.Lock()
			job := radio.jobs[len(radio.jobs)-1]
			radio.mu.Unlock()
			if !bytes.Equal(job.Data, tx.Data) {
				t.Fatalf("command %d path %d wire Go=%x Hew=%x", command[0], encoded, job.Data, tx.Data)
			}
		}
	}
	for _, size := range []int{1, 167} {
		body := bytes.Repeat([]byte{byte(size)}, size)
		command := append([]byte{62, 0, 255, 7, 0}, body...)
		_, tx := send(1, command, uint32(time.Now().Unix()))
		radio.mu.Lock()
		job := radio.jobs[len(radio.jobs)-1]
		radio.mu.Unlock()
		if !bytes.Equal(job.Data, tx.Data) {
			t.Fatalf("flood datagram length %d wire Go=%x Hew=%x", size, job.Data, tx.Data)
		}
		plain := append([]byte{8, 0, byte(size)}, body...)
		cipher, err := meshcore.EncryptThenMAC(psk[:], plain)
		if err != nil {
			t.Fatal(err)
		}
		hash := s.state.Channels[0].Hash
		receive(&meshcore.Packet{Header: meshcore.MakeHeader(meshcore.RouteTypeDirect, meshcore.PayloadTypeGrpData, 0),
			Payload: append([]byte{hash}, cipher...)})
		for clientID, c := range clients {
			s.mu.Lock()
			s.command(c, []byte{protocol.CmdSyncNextMessage})
			s.mu.Unlock()
			want := drain(c)
			got := p.events(fmt.Sprintf("CMD %d 0a %d", clientID, time.Now().Unix()))
			if !reflect.DeepEqual(want, got[clientID]) {
				t.Fatalf("datagram length %d journal client %d Go=%x Hew=%x", size, clientID, want, got[clientID])
			}
		}
	}
	for flags := byte(0); flags < 4; flags++ {
		width := 1 << flags
		tag := []byte{flags + 1, 2, 3, 4, 5, 6, 7, 8}
		body := append(append(append(tag, flags|0xa0), peerKey[:width]...), ownKey[:width]...)
		requester := int64(flags%2 + 1)
		send(requester, append([]byte{36}, body...), uint32(time.Now().Unix()))
		receive(&meshcore.Packet{Header: meshcore.MakeHeader(meshcore.RouteTypeDirect, meshcore.PayloadTypeTrace, 0),
			PathLength: 2, Path: []byte{4, 8}, Payload: body})
	}
	send(2, []byte{55, 0x80, 1, 2, 3}, uint32(time.Now().Unix()))
	receive(&meshcore.Packet{Header: meshcore.MakeHeader(meshcore.RouteTypeDirect, meshcore.PayloadTypeControl, 0), Payload: []byte{0x80, 4, 5}})
	receive(&meshcore.Packet{Header: meshcore.MakeHeader(meshcore.RouteTypeDirect, meshcore.PayloadTypeRawCustom, 0), Payload: []byte{5, 6, 7, 8}})
	p.send("STOP")
	if err := p.command.Wait(); err != nil {
		t.Fatal(err, p.stderr.String())
	}
}

func drain(c *session) [][]byte {
	var out [][]byte
	for {
		select {
		case frame := <-c.out:
			out = append(out, frame)
		default:
			return out
		}
	}
}

func TestHewBaseSDKTCP(t *testing.T) {
	address := os.Getenv("MESHCORE_HEW_BASE_SDK_ADDRESS")
	if address == "" {
		t.Skip("set MESHCORE_HEW_BASE_SDK_ADDRESS to a synthetic Hew companion")
	}
	client := testClient(t, address)
	ctx := testContext(t)
	device, err := client.DeviceQuery(ctx)
	if err != nil || device.FirmwareVersion != 13 || device.MaxContacts != 350 || device.MaxChannels != 40 {
		t.Fatalf("SDK device handshake: %+v, %v", device, err)
	}
	self, err := client.AppStart(ctx, 3, "Hew SDK contract")
	var seed [32]byte
	copy(seed[:], bytes.Repeat([]byte{1}, 32))
	if err != nil || self.PublicKey != meshcore.NewLocalIdentityFromSeed(seed).PublicKey() || self.Name != "Birch-Base" ||
		self.RadioFrequency != 912525 || self.RadioBandwidth != 250000 {
		t.Fatalf("SDK self handshake: %+v, %v", self, err)
	}
	// The upstream Go SDK collector has a 32-response nonblocking waiter.
	// Full 167-contact wire collection is checked by the TCP scenario itself.
	contacts, err := client.GetContactsSince(ctx, 250, true)
	if err != nil || len(contacts) != 16 {
		t.Fatalf("SDK contact listing: count=%d, %v", len(contacts), err)
	}
	channel, err := client.GetChannel(ctx, 39)
	if err != nil || channel.ChannelIdx != 39 {
		t.Fatalf("SDK channel 39: %+v, %v", channel, err)
	}
}

func TestHewBaseLocalDifferential(t *testing.T) {
	if os.Getenv("MESHCORE_HEW_BASE_BIN") == "" {
		t.Skip("set MESHCORE_HEW_BASE_BIN")
	}
	id := testIdentity(1)
	cfg := testConfig("")
	cfg.RadioConfig = hardware.RadioConfig{FreqHz: 912525000, BwHz: 250000, SF: 7, CR: 5}
	cfg.TxPower = 2
	cfg.Name = "Birch-Base"
	cfg.Retention = DurableReplay
	s, err := New(id, newTestRadio(), cfg)
	if err != nil {
		t.Fatal(err)
	}
	defer s.Close()
	s.state.Preferences.PathHashMode = 2
	for i := 0; i < 167; i++ {
		key := testIdentity(byte(i + 2)).PublicKey()
		s.state.Contacts = append(s.state.Contacts, contact{ContactResponse: protocol.ContactResponse{
			PublicKey: key, Type: meshcore.AdvertTypeChat, OutPathLen: 128, AdvertName: fmt.Sprintf("Contact-%d", i),
			LastModified: uint32(100 + i),
		}})
	}
	s.state.LastModified = 267
	for i := 0; i < 256; i++ {
		frame := append([]byte{16, 0, 0, 0}, s.state.Contacts[0].PublicKey[:6]...)
		frame = append(frame, 0, 0)
		frame = append(frame, []byte{byte(i), 1, 0, 0}...)
		frame = append(frame, []byte(fmt.Sprintf("saved-%d", i))...)
		s.state.Messages = append(s.state.Messages, message{Sequence: uint64(39 + i), Frame: frame})
	}
	s.state.Sequence = 294
	dir := t.TempDir()
	path := filepath.Join(dir, "companion.json")
	data, _ := json.Marshal(s.state)
	if len(data) < 65536 {
		t.Fatalf("fixture must exercise state above old 64 KiB bridge limit: %d", len(data))
	}
	if err := os.WriteFile(path, data, 0600); err != nil {
		t.Fatal(err)
	}
	seed := id.Seed()
	p := startHewBase(t, path, seed[:])
	a, b := commandSession(t, s), commandSession(t, s)
	s.clients[a], s.clients[b] = struct{}{}, struct{}{}
	for _, n := range []int{1, 2} {
		p.send(fmt.Sprintf("CONNECT %d", n))
		if got := p.line(); got != "CONNECTED true" {
			t.Fatalf("connect: %q", got)
		}
	}
	command := func(clientID int64, c *session, cmd []byte) {
		t.Helper()
		wall := time.Now().Unix()
		s.mu.Lock()
		s.command(c, cmd)
		s.mu.Unlock()
		want := drain(c)
		broadcast := drain(map[*session]*session{a: b, b: a}[c])
		operation := fmt.Sprintf("CMD %d %x %d", clientID, cmd, wall)
		switch cmd[0] {
		case 2, 3, 7, 16, 25, 26, 27, 28, 29, 36, 39, 50, 52, 55, 57, 62, 65:
			operation = fmt.Sprintf("SEND %d %x %d %d 1", clientID, cmd, wall, wall*1000)
		}
		got := p.events(operation)
		if cmd[0] == protocol.CmdDeviceQuery {
			// Build label/version identifies the implementing host, not wire behavior.
			for _, frames := range [][][]byte{want, got[clientID]} {
				for _, f := range frames {
					if len(f) == 82 {
						clear(f[20:80])
					}
				}
			}
		}
		if !reflect.DeepEqual(want, got[clientID]) || !reflect.DeepEqual(broadcast, got[0]) {
			t.Fatalf("command %x: Go=%x broadcast=%x Hew=%x", cmd, want, broadcast, got)
		}
	}
	for _, c := range []*session{a, b} {
		n := int64(1)
		if c == b {
			n = 2
		}
		command(n, c, []byte{22, 3})
		command(n, c, []byte{1, 0, 0, 0, 0, 0, 0, 0})
		command(n, c, []byte{4})
	}
	for _, cmd := range [][]byte{
		{31, 0}, {31, 39}, {31, 40}, {31}, {32, 40}, {8, 0}, {8, 0xff},
		{8, 'N', 'e', 'w'}, {14, 0, 0, 0, 0, 0, 0, 0, 0}, {38, 1, 63, 1, 1},
		{38, 0}, {58, 31, 255}, {59}, {61, 0, 1}, {61, 0, 2}, {61, 0, 3},
		{43}, {21, 0, 0, 0, 0, 0xe8, 3, 0, 0}, {40}, {41}, {37, 0, 0, 0, 0},
		{23}, append([]byte{24}, make([]byte, 64)...), {19, 'r', 'e', 'b', 'o', 'o', 't'},
		{51, 'r', 'e', 's', 'e', 't'}, {20}, {56, 0}, {56, 3}, {54, 1}, {54, 2}, {60}, {64}, {63},
		{255},
	} {
		command(1, a, cmd)
	}
	channel := make([]byte, 50)
	channel[0], channel[1] = 32, 39
	copy(channel[2:], "Private")
	copy(channel[34:], bytes.Repeat([]byte{7}, 16))
	command(1, a, channel)
	command(2, b, []byte{31, 39})
	command(1, a, []byte{33})
	command(2, b, []byte{33})
	command(1, a, []byte{34, 'a'})
	command(2, b, []byte{34, 'b'})
	command(1, a, []byte{35})
	command(2, b, []byte{35})
	command(1, a, []byte{35})
	for i := 0; i < 256; i++ {
		command(1, a, []byte{10})
	}
	command(1, a, []byte{10})
	newFrame := append([]byte{16, 0, 0, 0}, s.state.Contacts[0].PublicKey[:6]...)
	newFrame = append(newFrame, 0, 0, 1, 2, 3, 4, 'n', 'e', 'w')
	if s.queueMessage(newFrame) {
		t.Fatal("journal must refuse an unread full direct-message queue")
	}
	if got := p.events(fmt.Sprintf("JOURNAL %x", newFrame)); len(got) != 0 {
		t.Fatalf("Hew acknowledged full unread journal: %x", got)
	}
	for i := 0; i < 256; i++ {
		command(2, b, []byte{10})
	}
	if !s.queueMessage(newFrame) {
		t.Fatal("journal should reclaim read history after both clients fetch")
	}
	drain(a)
	drain(b)
	got := p.events(fmt.Sprintf("JOURNAL %x", newFrame))
	if len(got[-1]) != 1 || len(got[1]) != 1 || len(got[2]) != 1 {
		t.Fatalf("Hew journal commit and message-waiting events: %x", got)
	}
	command(1, a, []byte{10})
	command(2, b, []byte{10})
	for code := byte(1); code <= 65; code++ {
		command(1, a, []byte{code})
	}
	p.send("SNAPSHOT")
	var loaded state
	if err := json.Unmarshal([]byte(p.line()), &loaded); err != nil {
		t.Fatal(err)
	}
	if !reflect.DeepEqual(loaded, s.state) {
		t.Fatal("persisted Hew companion state differs from Go state after coherent local scenario")
	}
	p.send("STOP")
	_ = p.input.Close()
	if err := p.command.Wait(); err != nil {
		t.Fatalf("Hew shutdown: %v %s", err, p.stderr.String())
	}
	restarted := startHewBase(t, path, seed[:])
	restarted.send("SNAPSHOT")
	var reloaded state
	if err := json.Unmarshal([]byte(restarted.line()), &reloaded); err != nil || !reflect.DeepEqual(reloaded, loaded) {
		t.Fatalf("restart lost companion state: %v", err)
	}
}
