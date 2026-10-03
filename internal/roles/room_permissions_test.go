package roles

import (
	"bytes"
	"encoding/binary"
	"fmt"
	"testing"

	meshcore "github.com/meshcore-go/meshcore-go"
)

func TestRoomIntentionalReadOnlyPolicyPostsACKsCLIAndRevocation(t *testing.T) {
	for _, permissions := range []byte{0, 1, 2, 3, 0x81, 0x82, 0x83} {
		t.Run(fmt.Sprintf("permissions_%02x", permissions), func(t *testing.T) {
			s, r, id := startRole(t, true, config(t))
			admin, client := fixedID(2), fixedID(3)
			loggedIn(t, s, r, admin, id, true, 1, "admin")
			adminCommand := func(stamp uint32, command string) {
				t.Helper()
				r.inject(t, textWire(t, admin, id, stamp, 4, command), false)
				if body := decrypted(t, r.next(t), admin, id); body[4] != 4 || string(cstring(body[5:])) != "OK" {
					t.Fatalf("admin command %q: %x", command, body)
				}
			}
			adminCommand(2, "set allow.read.only on")
			r.inject(t, loginWire(t, true, client, id, 10, 0, "wrong", 2, 0, nil), false)
			if body := decrypted(t, r.next(t), client, id); body[7] != 0 {
				t.Fatalf("guest login: %x", body)
			}
			if permissions != 0 {
				adminCommand(3, fmt.Sprintf("setperm %s %d", client.Identity.String(), permissions))
				r.inject(t, loginWire(t, true, client, id, 0, 0, "", 2, 0, nil), false)
				if body := decrypted(t, r.next(t), client, id); body[7] != permissions {
					t.Fatalf("ACL login lost permission bits: %x", body)
				}
			}
			canPost := permissions&3 >= 2
			checkPost := func(stamp uint32, attempt byte, wantACK bool) {
				t.Helper()
				r.inject(t, textWire(t, client, id, stamp, attempt, "hello"), false)
				if !wantACK {
					r.quiet(t)
					return
				}
				plain := binary.LittleEndian.AppendUint32(nil, stamp)
				plain = append(plain, attempt)
				plain = append(plain, "hello"...)
				want := append([]byte{0x0d, 0}, ackProof(plain, client.PublicKey())...)
				if got := r.next(t); !bytes.Equal(got, want) {
					t.Fatalf("plain post ACK: %x, want %x", got, want)
				}
			}
			checkPost(20, 0, canPost)
			checkPost(20, 1, canPost)
			checkPost(19, 0, false)
			r.inject(t, textWire(t, client, id, 21, 4, "set name Changed"), false)
			if permissions&3 == 3 {
				if body := decrypted(t, r.next(t), client, id); body[4] != 4 || string(cstring(body[5:])) != "OK" {
					t.Fatalf("administrator CLI reply: %x", body)
				}
			}
			// A status response is a worker barrier after an unanswered CLI.
			r.inject(t, peerWire(t, 0, client, id, []byte{22, 0, 0, 0, 1}), false)
			raw := r.next(t)
			pkt, err := meshcore.PacketFromBytes(raw)
			if err != nil || pkt.PayloadType() != meshcore.PayloadTypeResponse {
				t.Fatalf("non-admin CLI produced a response: %x %v", raw, err)
			}
			r.quiet(t)
			s.mu.RLock()
			historyCount, name := len(s.state.History), s.state.Name
			s.mu.RUnlock()
			wantHistory := 0
			if canPost {
				wantHistory = 1
			}
			wantName := "Host"
			if permissions&3 == 3 {
				wantName = "Changed"
			}
			if historyCount != wantHistory || name != wantName {
				t.Fatalf("post/replay/CLI state: history=%d name=%q, want %d/%q", historyCount, name, wantHistory, wantName)
			}
			adminCommand(4, "setperm "+client.Identity.String()+" 0")
			checkPost(25, 0, false)
			r.inject(t, textWire(t, client, id, 26, 4, "set name Revoked"), false)
			r.quiet(t)
			s.mu.RLock()
			_, exists := s.state.Members[client.Identity.String()]
			unchanged := len(s.state.History) == wantHistory && s.state.Name == wantName
			s.mu.RUnlock()
			if exists || !unchanged {
				t.Fatal("revoked member regained access or changed room state")
			}
		})
	}
}
