package roles

import (
	"bytes"
	"strings"
	"testing"
	"time"
)

func TestNativePostByteTruncationSurvivesDurableReplay(t *testing.T) {
	cfg := config(t)
	s, r, id := startRole(t, true, cfg)
	alice, bob := fixedID(2), fixedID(3)
	loggedIn(t, s, r, alice, id, true, 1, "room")
	loggedIn(t, s, r, bob, id, true, 1, "room")
	learnPath(t, s, r, bob, id, 0, nil)
	text := strings.Repeat("a", 149) + "\xc3\xa9" + "z"
	r.inject(t, textWire(t, alice, id, 2, 0, text), false)
	wantACK := append([]byte{0x0d, 0}, ackProof(append([]byte{2, 0, 0, 0, 0}, []byte(text)...), alice.PublicKey())...)
	if got := r.next(t); !bytes.Equal(got, wantACK) {
		t.Fatalf("ACK did not authenticate full original text: %x want %x", got, wantACK)
	}
	s.transmit(driveSync(t, s, time.Now().Add(time.Minute), 2), nil)
	body := decrypted(t, r.next(t), bob, id)
	wantText := append(bytes.Repeat([]byte{'a'}, 149), 0xc3)
	if len(body) != 160 || !bytes.Equal(body[9:159], wantText) || body[159] != 0 {
		t.Fatalf("native 150-byte truncation: %x", body)
	}
	if err := s.Close(); err != nil {
		t.Fatal(err)
	}
	restarted, _, _ := startRole(t, true, cfg)
	restarted.mu.RLock()
	defer restarted.mu.RUnlock()
	if len(restarted.state.History) != 1 || !bytes.Equal([]byte(restarted.state.History[0].text()), wantText) {
		t.Fatal("durable extension changed native byte-truncated post")
	}
}
