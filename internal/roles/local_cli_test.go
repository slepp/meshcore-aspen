package roles

import (
	"bytes"
	"strings"
	"testing"
	"unicode/utf8"

	meshcore "github.com/meshcore-go/meshcore-go"
)

func TestLocalCLIIndependentRolesPermissionsAndReplay(t *testing.T) {
	for _, room := range []bool{false, true} {
		t.Run(map[bool]string{false: "repeater", true: "room"}[room], func(t *testing.T) {
			first, firstRadio, firstID := startRole(t, room, config(t))
			secondID, client := fixedID(4), fixedID(2)
			secondRadio := newWireRadio()
			cfg := config(t)
			cfg.Name = "Second"
			var second *Service
			var err error
			if room {
				second, err = NewRoom(secondID, secondRadio, cfg)
			} else {
				second, err = NewRepeater(secondID, secondRadio, cfg)
			}
			if err != nil {
				t.Fatal(err)
			}
			t.Cleanup(func() {
				if err := second.Close(); err != nil {
					t.Error(err)
				}
			})
			secondRadio.next(t)
			for _, instance := range []struct {
				service  *Service
				radio    *wireRadio
				id       meshcore.LocalIdentity
				password string
				admin    byte
			}{{first, firstRadio, firstID, "admin", 1}, {second, secondRadio, secondID, "room", 0}} {
				instance.radio.inject(t, loginWire(t, room, client, instance.id, 100, 0, instance.password, 2, 0, nil), true)
				reply := decrypted(t, instance.radio.next(t), client, instance.id)
				if len(reply) < 13 || reply[6] != instance.admin {
					t.Fatalf("role-specific login permissions: %x", reply)
				}
			}
			firstRadio.inject(t, textWire(t, client, firstID, 101, 4, "set name Changed"), true)
			reply := decrypted(t, firstRadio.next(t), client, firstID)
			if reply[4] != 4 || string(cstring(reply[5:])) != "OK" {
				t.Fatalf("admin CLI reply: %x", reply)
			}
			// Changed attempt and body avoid packet-level dedup; role replay still
			// must reject this command without another reply or mutation.
			firstRadio.inject(t, textWire(t, client, firstID, 101, 5, "set name Wrong"), true)
			secondRadio.inject(t, textWire(t, client, secondID, 101, 4, "set name Wrong"), true)
			for _, instance := range []struct {
				service *Service
				radio   *wireRadio
				id      meshcore.LocalIdentity
				name    string
			}{{first, firstRadio, firstID, "Changed"}, {second, secondRadio, secondID, "Second"}} {
				// A public binary request is a processing barrier after the
				// unanswered retry/guest CLI, not a retransmitted CLI command.
				instance.radio.inject(t, peerWire(t, 0, client, instance.id, []byte{102, 0, 0, 0, 1}), true)
				raw := instance.radio.next(t)
				packet, err := meshcore.PacketFromBytes(raw)
				if err != nil || packet.PayloadType() != meshcore.PayloadTypeResponse {
					t.Fatalf("retry or guest CLI produced a reply: %x %v", raw, err)
				}
				instance.radio.quiet(t)
				instance.service.mu.RLock()
				name := instance.service.state.Name
				instance.service.mu.RUnlock()
				if name != instance.name || instance.service.received.Load() != 0 {
					t.Fatalf("role name=%q RF received=%d", name, instance.service.received.Load())
				}
			}
			firstRadio.inject(t, textWire(t, client, firstID, 103, 4, "get name"), true)
			reply = decrypted(t, firstRadio.next(t), client, firstID)
			if string(cstring(reply[5:])) != "> Changed" {
				t.Fatalf("guest login changed another role's admin session: %x", reply)
			}
		})
	}
}

func TestLocalCLIReplyCapacityAndTimestamp(t *testing.T) {
	for _, room := range []bool{false, true} {
		t.Run(map[bool]string{false: "repeater", true: "room"}[room], func(t *testing.T) {
			s, _, id := startRole(t, room, config(t))
			client := fixedID(2)
			s.mu.Lock()
			m, err := s.putMember(client.Identity)
			if err != nil {
				s.mu.Unlock()
				t.Fatal(err)
			}
			stamp := s.now()
			s.state.Clock, m.LastActivity = stamp-1, stamp
			m.KnownPath = true
			reply := strings.Repeat("é", 100)
			packets, err := s.cliReply(event{packet: &meshcore.Packet{}}, m, stamp, reply)
			s.mu.Unlock()
			if err != nil || len(packets) != 1 {
				t.Fatalf("CLI response capacity: %v %v", packets, err)
			}
			raw, err := packets[0].ToBytes()
			if err != nil || len(packets[0].Payload) > meshcore.MaxPacketPayload {
				t.Fatalf("oversize CLI wire response: %x %v", raw, err)
			}
			plain := decrypted(t, raw, client, id)
			text := cstring(plain[5:])
			if u32(plain) == stamp || plain[4] != 4 || len(text) != 154 ||
				!utf8.Valid(text) || !bytes.Equal(text, []byte(strings.Repeat("é", 77))) {
				t.Fatalf("invalid bounded CLI response: %x", plain)
			}
		})
	}
}
