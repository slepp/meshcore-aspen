package radio

import (
	"context"
	"encoding/binary"
	"strings"
	"testing"
	"time"

	"github.com/meshcore-go/meshcore-go/hardware"
	"github.com/meshcore-go/meshcore-go/node"
)

func presenceReply(generation uint32, reason, mask, flags byte) []byte {
	reply := make([]byte, 9)
	reply[0], reply[1], reply[2] = HWQueuedRolePresence|0x80, QueuedProtocolVersion, reason
	binary.LittleEndian.PutUint32(reply[3:], generation)
	reply[7], reply[8] = mask, flags
	return reply
}

func presenceConfig(phy *testPHY) Config {
	cfg := followConfig(phy, nil)
	cfg.RoleAnnouncement = &RoleAnnouncement{Role: RoomRole, PublicKey: [32]byte{7}}
	return cfg
}

func TestRolePresenceFailuresAreWarningOnly(t *testing.T) {
	for _, tc := range []struct {
		name  string
		reply func(generation uint32) []byte
		want  string
	}{
		{"hardware-error", func(uint32) []byte {
			return []byte{hardware.HW_RESP_ERROR, hardware.HW_ERR_INVALID_PARAM}
		}, "ROLE_PRESENCE failed"},
		{"reason", func(g uint32) []byte { return presenceReply(g, 5, 0, 0) }, "reason 5"},
		{"malformed", func(g uint32) []byte { return presenceReply(g, 0, 0, 0)[:8] }, "invalid ROLE_PRESENCE"},
		{"stale", func(g uint32) []byte { return presenceReply(g+1, 0, 0, 0) }, "stale ROLE_PRESENCE"},
		{"unknown-bits", func(g uint32) []byte { return presenceReply(g, 0, 0x12, 0x88) }, "unrecognized native mask 0x10 or warning flags 0x80; also another connected host"},
	} {
		t.Run(tc.name, func(t *testing.T) {
			phy := followPHY(t)
			phy.presenceResponse = func(_ int32, generation uint32) []byte { return tc.reply(generation) }
			ctx, cancel := context.WithTimeout(context.Background(), 10*time.Second)
			defer cancel()
			link, err := Open(ctx, presenceConfig(phy))
			if err != nil {
				t.Fatalf("advisory presence denied admission: %v", err)
			}
			defer link.Close()
			status := link.RolePresenceStatus()
			if status == nil || status.State != "unknown" || !status.Online ||
				!strings.Contains(status.Warning, "Overlap unknown for room") ||
				!strings.Contains(status.Warning, tc.want) {
				t.Fatalf("presence status %+v, want unknown with %q", status, tc.want)
			}
			if phy.generation.Load() != 1 || phy.presenceRequests.Load() != 1 {
				t.Fatalf("correlated reply retired the stream: connections=%d requests=%d",
					phy.generation.Load(), phy.presenceRequests.Load())
			}
		})
	}
}

func TestRolePresenceTimeoutReconnectsWithoutDiscovery(t *testing.T) {
	previous := presenceTimeout
	presenceTimeout = 100 * time.Millisecond
	t.Cleanup(func() { presenceTimeout = previous })
	phy := followPHY(t)
	phy.presenceResponse = func(attempt int32, generation uint32) []byte {
		if attempt == 1 {
			return nil
		}
		return presenceReply(generation, 0, 0, 0)
	}
	ctx, cancel := context.WithTimeout(context.Background(), 10*time.Second)
	defer cancel()
	link, err := Open(ctx, presenceConfig(phy))
	if err != nil {
		t.Fatalf("presence timeout denied admission: %v", err)
	}
	defer link.Close()
	status := link.RolePresenceStatus()
	if status == nil || status.State != "unknown" || !strings.Contains(status.Warning, "discovery skipped") ||
		phy.generation.Load() != 2 || phy.presenceRequests.Load() != 1 {
		t.Fatalf("uncertain stream reused or discovery retried: %+v connections=%d requests=%d",
			status, phy.generation.Load(), phy.presenceRequests.Load())
	}
	<-phy.conns
	second := <-phy.conns
	results := make(chan TXResult, 4)
	link.AddTXResultHandler(func(result TXResult) { results <- result })
	radio, stop := link.Radio()
	defer stop()
	if !radio.(node.TxRadio).Enqueue([]byte{0x11}, 1, 0) {
		t.Fatal("enqueue")
	}
	nextJob(t, phy, []byte{0x11})
	if result := nextResult(t, results); result.State != TXAccepted {
		t.Fatalf("fresh connection did not carry TX: %+v", result)
	}
	// A later reconnect announces again.
	second.Close()
	waitUntil(t, "rediscovery", func() bool {
		status := link.RolePresenceStatus()
		return status.State == "announced" && status.Online && phy.presenceRequests.Load() == 2
	})
}
