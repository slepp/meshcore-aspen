package main

import (
	"bytes"
	"encoding/binary"
	"errors"
	"math"
	"sync"

	meshcore "github.com/meshcore-go/meshcore-go"
	protocol "github.com/meshcore-go/meshcore-go/companion"
	"github.com/meshcore-go/meshcore-go/companion/client"
)

const maxEvents = 4096

type reception struct {
	packet *meshcore.Packet
	raw    []byte
	snr    float32
	rssi   int8
}

type event struct {
	response protocol.Response
	rx       *reception
}

type capture struct {
	mu       sync.Mutex
	events   []event
	rejected int
	fail     func(error)
	sent     sentText
}

type sentText struct {
	timestamp uint32
	flags     byte
	text      string
}

func (s sentText) ack(key meshcore.Identity) uint32 {
	plain := binary.LittleEndian.AppendUint32(nil, s.timestamp)
	plain = append(plain, s.flags)
	plain = append(plain, s.text...)
	return meshcore.CalcAckHash(plain, key.PublicKeyBytes())
}

func physicalSignal(snr float32, rssi int8) bool {
	return rssi < 0 && !math.IsNaN(float64(snr)) && !math.IsInf(float64(snr), 0) &&
		snr >= -32 && snr <= 31.75
}

func (c *capture) observe(response protocol.Response) {
	e := event{response: response}
	switch v := response.Data.(type) {
	case protocol.PushLogRxDataResponse:
		pkt, err := meshcore.PacketFromBytes(v.Raw)
		if err != nil || !physicalSignal(v.LastSNR, v.LastRSSI) {
			c.mu.Lock()
			c.rejected++
			c.mu.Unlock()
			return
		}
		e.rx = &reception{packet: pkt, raw: bytes.Clone(v.Raw), snr: v.LastSNR, rssi: v.LastRSSI}
	case protocol.ContactMsgRecvV3Response, protocol.PushSendConfirmedResponse,
		protocol.PushLoginSuccessResponse, protocol.PushLoginFailResponse:
	default:
		return
	}
	c.mu.Lock()
	if len(c.events) == maxEvents {
		c.mu.Unlock()
		c.fail(errors.New("received event buffer full"))
		return
	}
	c.events = append(c.events, e)
	c.mu.Unlock()
}

func (c *capture) mark() int {
	c.mu.Lock()
	defer c.mu.Unlock()
	return len(c.events)
}

func (c *capture) since(mark int) []event {
	c.mu.Lock()
	defer c.mu.Unlock()
	return append([]event(nil), c.events[mark:]...)
}

// GetWaitingMessages normalizes V3 messages and discards SNR. Observe the parsed
// responses before SDK dispatch rather than inventing a second protocol client.
type observedTransport struct {
	client.Transport
	capture *capture
}

func (t *observedTransport) SetResponseHandler(next func(protocol.Response)) {
	t.Transport.SetResponseHandler(func(response protocol.Response) {
		t.capture.observe(response)
		next(response)
	})
}

func (t *observedTransport) Send(command []byte) error {
	if command[0] == protocol.CmdSendTxtMsg {
		t.capture.mu.Lock()
		t.capture.sent = sentText{
			timestamp: binary.LittleEndian.Uint32(command[3:7]),
			flags:     command[1]<<2 | command[2]&3,
			text:      string(command[13:]),
		}
		t.capture.mu.Unlock()
	}
	return t.Transport.Send(command)
}

func findRX(events []event, match func(*reception) bool) *reception {
	for _, e := range events {
		if e.rx != nil && match(e.rx) {
			return e.rx
		}
	}
	return nil
}

func datagramFrom(rx *reception, kind byte, sender, recipient meshcore.Identity) bool {
	p := rx.packet
	return p.PayloadType() == kind && len(p.Payload) >= 4 &&
		p.Payload[0] == recipient.PublicKey()[0] && p.Payload[1] == sender.PublicKey()[0]
}

func packetACK(packet *meshcore.Packet) (uint32, bool) {
	body := packet.Payload
	if packet.PayloadType() == meshcore.PayloadTypeMultiPart {
		part, err := meshcore.MultiPartFromBytes(body)
		if err != nil || part.WrappedType != meshcore.PayloadTypeAck {
			return 0, false
		}
		body = part.WrappedPayload
	} else if packet.PayloadType() != meshcore.PayloadTypeAck {
		return 0, false
	}
	if len(body) < 4 {
		return 0, false
	}
	return binary.LittleEndian.Uint32(body), true
}

func signedACKs(message protocol.ContactMsgRecvV3Response, recipient meshcore.Identity) ([4]uint32, error) {
	var tags [4]uint32
	if message.TxtType != protocol.TxtTypeSignedPlain || len(message.SenderPrefix) != 4 {
		return tags, errors.New("room delivery lacks the native signed-message author prefix")
	}
	plain := binary.LittleEndian.AppendUint32(nil, message.SenderTimestamp)
	plain = append(plain, 0)
	plain = append(plain, message.SenderPrefix...)
	plain = append(plain, message.Text...)
	// Sync responses omit the two attempt bits. Enumerate exactly their four
	// native values; ACK tags use the recipient's key.
	for attempt := range tags {
		plain[4] = protocol.TxtTypeSignedPlain<<2 | byte(attempt)
		tags[attempt] = meshcore.CalcAckHash(plain, recipient.PublicKeyBytes())
	}
	return tags, nil
}
