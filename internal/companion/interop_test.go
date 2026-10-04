package companion

import (
	"bytes"
	"strings"
	"testing"
	"time"

	meshcore "github.com/meshcore-go/meshcore-go"
	protocol "github.com/meshcore-go/meshcore-go/companion"
	"meshcore.local/meshcore/internal/roles"
)

func TestColocatedAdminCLI(t *testing.T) {
	for _, isRoom := range []bool{false, true} {
		t.Run(map[bool]string{false: "repeater", true: "room"}[isRoom], func(t *testing.T) {
			baseID, roleID := testIdentity(1), testIdentity(2)
			base, baseRadio, address := startTestServer(t, baseID, testConfig(""))
			roleRadio := newTestRadio()
			cfg := roles.Config{StateDir: stateDir(t), Name: "Local role", Password: "guest", AdminPassword: "admin",
				AirtimeEstimator: func(int) uint32 { return 100 },
				Telemetry: func() (roles.Telemetry, error) {
					return roles.Telemetry{HasMCUTemperature: true, MCUTemperatureC: 25}, nil
				}}
			var role *roles.Service
			var err error
			if isRoom {
				role, err = roles.NewRoom(roleID, roleRadio, cfg)
			} else {
				role, err = roles.NewRepeater(roleID, roleRadio, cfg)
			}
			if err != nil {
				t.Fatal(err)
			}
			t.Cleanup(func() {
				if err := role.Close(); err != nil {
					t.Error(err)
				}
			})
			deliver := func(r *testRadio, p *meshcore.Packet) {
				p.HasSignalInfo, p.SNR, p.RSSI = true, -32, 127
				p.MarkDoNotRetransmit()
				r.inject(p)
			}
			c := testClient(t, address)
			ctx := testContext(t)
			if _, err := c.DeviceQuery(ctx); err != nil {
				t.Fatal(err)
			}
			advert := capturePush(c, protocol.PushNewAdvert)
			login := capturePush(c, protocol.PushLoginSuccess)
			deliver(baseRadio, roleRadio.packet(t))
			nextPush(t, advert)
			if err := c.SendLogin(ctx, roleID.Identity, "admin"); err != nil {
				t.Fatal(err)
			}
			deliver(roleRadio, baseRadio.packet(t))
			deliver(baseRadio, roleRadio.packet(t))
			if got := nextPush(t, login).Data.(protocol.PushLoginSuccessResponse); got.Permissions != 1 || got.ACL != 3 {
				t.Fatalf("login did not grant admin: %+v", got)
			}
			deliver(roleRadio, baseRadio.packet(t))
			binary := capturePush(c, protocol.PushBinaryResponse)
			for _, kind := range []byte{1, 3} {
				if err := c.SendBinaryReq(ctx, roleID.Identity, []byte{kind}); err != nil {
					t.Fatal(err)
				}
				deliver(roleRadio, baseRadio.packet(t))
				deliver(baseRadio, roleRadio.packet(t))
				nextPush(t, binary)
			}
			kind := byte(meshcore.AdvertTypeRepeater)
			if isRoom {
				kind = meshcore.AdvertTypeRoom
			}
			if err := c.AddUpdateContactFull(ctx, protocol.AddUpdateContactCommand{
				PublicKey: roleID.PublicKey(), Type: kind, Name: "Local role",
				OutPathLen: 0x81, OutPath: []byte{0x91, 0x92, 0x93},
			}); err != nil {
				t.Fatal(err)
			}
			deliver(roleRadio, datagram(t, baseID, roleID, meshcore.PayloadTypePath,
				[]byte{0x81, 0xa1, 0xa2, 0xa3, 0xff, 1, 2, 3, 4}))
			base.mu.Lock()
			for i := uint32(1); i <= maxMessages; i++ {
				if !base.queueMessage(privateFrame(i)) {
					base.mu.Unlock()
					t.Fatal("could not fill replay inbox")
				}
			}
			base.mu.Unlock()
			if messages, err := c.GetWaitingMessages(ctx); err != nil || len(messages) != maxMessages {
				t.Fatalf("replay messages=%d error=%v", len(messages), err)
			}
			waiting := capturePush(c, protocol.PushMsgWaiting)
			var lastTimestamp uint32
			for _, command := range []string{"get name", "stats help", "stats sensors", "stats role", "get stats", "get name"} {
				if _, err := c.SendTextMessage(ctx, roleID.Identity, command, protocol.TxtTypeCLIData); err != nil {
					t.Fatal(err)
				}
				request := baseRadio.packet(t)
				secret, err := roleID.SharedSecret(baseID.Identity)
				if err != nil {
					t.Fatal(err)
				}
				plain, err := meshcore.MACThenDecrypt(secret, request.Payload[2:])
				if err != nil || len(plain) < 5 || plain[4]>>2 != 1 || !bytes.HasPrefix(plain[5:], []byte(command)) {
					t.Fatalf("invalid CLI request: %x %v", plain, err)
				}
				if u32(plain) <= lastTimestamp {
					t.Fatal("independent CLI submissions reused a replay timestamp")
				}
				lastTimestamp = u32(plain)
				deliver(roleRadio, request)
				reply := roleRadio.packet(t)
				if reply.PayloadType() != meshcore.PayloadTypeTxtMsg || reply.PathLength != 0x81 {
					t.Fatalf("expected CLI text reply, got %+v", reply)
				}
				roleRadio.mu.Lock()
				job := roleRadio.jobs[len(roleRadio.jobs)-1]
				roleRadio.mu.Unlock()
				wantDelay := 1500 * time.Millisecond
				if isRoom {
					wantDelay = 300 * time.Millisecond
				}
				if job.Priority != 0 || job.Delay != wantDelay {
					t.Fatalf("CLI reply source admission priority/delay: %+v", job)
				}
				deliver(baseRadio, reply)
				nextPush(t, waiting)
				messages, err := c.GetWaitingMessages(ctx)
				if err != nil || len(messages) != 1 {
					t.Fatalf("%s: messages=%+v error=%v", command, messages, err)
				}
				message := messages[0].Contact
				if message == nil || message.TxtType != protocol.TxtTypeCLIData ||
					message.PubKeyPrefix != roleID.Identity.Prefix() || message.SenderTimestamp == lastTimestamp {
					t.Fatalf("invalid CLI response: %+v", messages[0])
				}
				switch command {
				case "get name":
					if message.Text != "> Local role" {
						t.Fatalf("wrong role name: %q", message.Text)
					}
				case "stats help":
					if !strings.HasPrefix(message.Text, "stats [role|sensors|radio|signal|airtime];") {
						t.Fatalf("wrong stats help: %q", message.Text)
					}
				case "stats sensors":
					if message.Text != "schema=1 scope=modem battery_mv=unavailable mcu_temp_c=25.00" {
						t.Fatalf("wrong sensors: %q", message.Text)
					}
				case "stats role", "get stats":
					if !strings.Contains(message.Text, "rx_packets=0") && !strings.HasPrefix(message.Text, "recv=0 sent=") {
						t.Fatalf("local CLI reception counted as RF: %q", message.Text)
					}
				}
				deliver(baseRadio, reply)
				if messages, err := c.GetWaitingMessages(ctx); err != nil || len(messages) != 0 {
					t.Fatalf("duplicate CLI reply: %+v %v", messages, err)
				}
				deliver(roleRadio, request)
				if err := c.SendBinaryReq(ctx, roleID.Identity, []byte{1}); err != nil {
					t.Fatal(err)
				}
				deliver(roleRadio, baseRadio.packet(t))
				barrier := roleRadio.packet(t)
				if barrier.PayloadType() != meshcore.PayloadTypeResponse {
					t.Fatalf("duplicate CLI request emitted another response: %+v", barrier)
				}
				deliver(baseRadio, barrier)
				nextPush(t, binary)
			}
		})
	}
}

func TestRoomRoleLoginPostACKUsesExtendedSentTag(t *testing.T) {
	baseID, roomID := testIdentity(1), testIdentity(2)
	_, baseRadio, address := startTestServer(t, baseID, testConfig(""))
	roomRadio := newTestRadio()
	room, err := roles.NewRoom(roomID, roomRadio, roles.Config{
		StateDir: stateDir(t), Name: "Interop room", Password: "hello",
	})
	if err != nil {
		t.Fatal(err)
	}
	t.Cleanup(func() {
		if err := room.Close(); err != nil {
			t.Error(err)
		}
	})
	c := testClient(t, address)
	ctx := testContext(t)
	advert := capturePush(c, protocol.PushNewAdvert)
	login := capturePush(c, protocol.PushLoginSuccess)
	confirmed := capturePush(c, protocol.PushSendConfirmed)
	deliver := func(r *testRadio, p *meshcore.Packet) {
		p.HasSignalInfo, p.SNR, p.RSSI = true, -32, 127
		p.MarkDoNotRetransmit()
		r.inject(p)
	}
	deliver(baseRadio, roomRadio.packet(t))
	nextPush(t, advert)
	if err := c.SendLogin(ctx, roomID.Identity, "hello"); err != nil {
		t.Fatal(err)
	}
	deliver(roomRadio, baseRadio.packet(t))
	deliver(baseRadio, roomRadio.packet(t))
	nextPush(t, login)
	// Complete reciprocal path learning before the client posts.
	deliver(roomRadio, baseRadio.packet(t))
	sent, err := c.SendTextMessage(ctx, roomID.Identity, "interop post", protocol.TxtTypePlain)
	if err != nil {
		t.Fatal(err)
	}
	if !sent.HasExtended || sent.HasAckCode {
		t.Fatalf("firmware extended RESP_SENT must expose Tag, not legacy AckCode: %+v", sent)
	}
	deliver(roomRadio, baseRadio.packet(t))
	ack := roomRadio.packet(t)
	if ack.PayloadType() != meshcore.PayloadTypeAck {
		t.Fatalf("room did not acknowledge: %+v", ack)
	}
	if u32(ack.Payload) != sent.Tag {
		t.Fatalf("wire ACK %x differs from sent tag %x", u32(ack.Payload), sent.Tag)
	}
	deliver(baseRadio, ack)
	push := nextPush(t, confirmed).Data.(protocol.PushSendConfirmedResponse)
	if push.AckCode != sent.Tag {
		t.Fatalf("confirmed ACK %x differs from extended sent tag %x", push.AckCode, sent.Tag)
	}
}
