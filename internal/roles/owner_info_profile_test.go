// SPDX-License-Identifier: Apache-2.0
package roles

import (
	"meshcore.local/meshcore/internal/buildinfo"
	"strings"
	"testing"

	meshcore "github.com/meshcore-go/meshcore-go"
)

func TestOwnerInfoProfileFitsDirectAndRoutedReplies(t *testing.T) {
	cfg := config(t)
	cfg.ErrorHandler = func(err error) { t.Errorf("role error: %v", err) }
	s, r, id := startRole(t, false, cfg)
	admin := fixedID(2)
	loggedIn(t, s, r, admin, id, false, 1, "admin")
	s.mu.Lock()
	s.state.Name = strings.Repeat("n", 31)
	s.state.Preferences.OwnerInfo = strings.Repeat("x", 119)
	s.mu.Unlock()
	stamp := uint32(2)
	for _, path := range [][]byte{nil, {0x11, 0x12}, {0x11, 0x12, 0x13, 0x21, 0x22, 0x23, 0x31, 0x32, 0x33}} {
		packet, err := meshcore.PacketFromBytes(peerWire(t, 0, admin, id, []byte{byte(stamp), 0, 0, 0, 7}))
		if err != nil {
			t.Fatal(err)
		}
		if len(path) != 0 {
			packet.Header = meshcore.MakeHeader(meshcore.RouteTypeFlood, meshcore.PayloadTypeReq, 0)
			packet.Path, packet.PathLength = path, byte(len(path))
			if len(path) == 9 {
				packet.PathLength = 0x83
			}
		}
		raw, err := packet.ToBytes()
		if err != nil {
			t.Fatal(err)
		}
		r.inject(t, raw, false)
		response := r.next(t)
		body := decrypted(t, response, admin, id)
		if len(path) != 0 {
			if len(body) <= len(path)+2 || body[0] != packet.PathLength ||
				body[len(path)+1] != meshcore.PayloadTypeResponse {
				t.Fatalf("owner PATH response: %x", body)
			}
			body = body[len(path)+2:]
		}
		if len(body) <= 4 || u32(body) != stamp {
			t.Fatalf("owner timestamp: %x", body)
		}
		header := buildinfo.HostVersion + "\n" + strings.Repeat("n", 31) + "\n"
		text := string(cstring(body[4:]))
		if !strings.HasPrefix(text, header) || len(text) <= len(header) || len(text) > len(header)+119 {
			t.Fatalf("invalid bounded owner body: %q", text)
		}
		stamp++
	}
	s.mu.RLock()
	if s.state.Preferences.OwnerInfo != strings.Repeat("x", 119) {
		t.Error("binary read changed durable owner text")
	}
	s.mu.RUnlock()
}
