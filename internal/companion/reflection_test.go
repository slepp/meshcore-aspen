package companion

import (
	"testing"

	meshcore "github.com/meshcore-go/meshcore-go"
)

func TestLocalReflectionDeliversDespiteUnconsumedDirectPath(t *testing.T) {
	id, peer := testIdentity(1), testIdentity(2)
	_, radio, address := startTestServer(t, id, testConfig(""))
	c := testClient(t, address)
	ctx := testContext(t)
	if err := c.AddUpdateContact(ctx, peer.Identity, "Local peer"); err != nil {
		t.Fatal(err)
	}
	waiting := watchMessages(c)
	pkt := datagram(t, peer, id, meshcore.PayloadTypeTxtMsg, append([]byte{1, 0, 0, 0, 0}, []byte("same station")...))
	pkt.PathLength, pkt.Path = 1, []byte{id.PublicKey()[0] ^ 0xff}
	radio.inject(pkt)
	msgs, err := c.GetWaitingMessages(ctx)
	if err != nil || len(msgs) != 0 {
		t.Fatalf("ordinary in-transit packet delivered: %+v %v", msgs, err)
	}
	pkt.HasSignalInfo, pkt.SNR, pkt.RSSI = true, -32, 127
	pkt.MarkDoNotRetransmit()
	radio.inject(pkt)
	waitMessage(t, waiting)
	msgs, err = c.GetWaitingMessages(ctx)
	if err != nil || len(msgs) != 1 || msgs[0].Contact.Text != "same station" {
		t.Fatalf("local reflection dropped: %+v %v", msgs, err)
	}
	ack := radio.packet(t)
	if ack.PayloadType() != meshcore.PayloadTypeAck {
		t.Fatalf("reflected packet was forwarded instead of acknowledged: %+v", ack)
	}
}
