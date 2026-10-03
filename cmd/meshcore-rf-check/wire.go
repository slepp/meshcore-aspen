package main

import (
	"bytes"
	"context"
	"crypto/rand"
	"crypto/sha256"
	"encoding/binary"
	"errors"
	"fmt"
	"math"
	"sync/atomic"
	"time"

	meshcore "github.com/meshcore-go/meshcore-go"
)

type reception struct {
	packet *meshcore.Packet
	snr    float32
	rssi   int8
}

type capture struct {
	frames      chan reception
	txDone      chan []byte
	transmitted atomic.Uint64
	physical    atomic.Uint64
	rejected    atomic.Uint64
	dropped     atomic.Uint64
}

func newCapture() *capture {
	return &capture{frames: make(chan reception, 256), txDone: make(chan []byte, 32)}
}

// Link invokes this callback only after its flow-controlled KissModem.SendData
// returns successfully, meaning the firmware supplied TX_DONE rather than merely
// accepting a host queue entry.
func (c *capture) sent(raw []byte) {
	c.transmitted.Add(1)
	select {
	case c.txDone <- bytes.Clone(raw):
	default:
		c.dropped.Add(1)
	}
}

func (c *capture) waitTX(ctx context.Context, stage string, raw []byte) error {
	for {
		if c.dropped.Load() != 0 {
			return fmt.Errorf("%s: RF packet capture full", stage)
		}
		select {
		case sent := <-c.txDone:
			if bytes.Equal(sent, raw) {
				return nil
			}
		case <-ctx.Done():
			return fmt.Errorf("%s TX_DONE: %w (completed_tx=%d physical_rx=%d)", stage, ctx.Err(), c.transmitted.Load(), c.physical.Load())
		}
	}
}
func physicalSignal(snr float32, rssi int8, hasSignal bool) bool {
	return hasSignal && !(snr == -32 && rssi == 127) && !math.IsNaN(float64(snr)) && !math.IsInf(float64(snr), 0)
}
func (c *capture) receive(raw []byte, snr float32, rssi int8, signal bool) {
	if !physicalSignal(snr, rssi, signal) {
		c.rejected.Add(1)
		return
	}
	packet, err := meshcore.PacketFromBytes(bytes.Clone(raw))
	if err != nil {
		return
	}
	c.physical.Add(1)
	select {
	case c.frames <- reception{packet, snr, rssi}:
	default:
		c.dropped.Add(1)
	}
}
func (c *capture) wait(ctx context.Context, stage string, match func(*meshcore.Packet) bool) (reception, error) {
	for {
		if c.dropped.Load() != 0 {
			return reception{}, fmt.Errorf("%s: bounded RF capture overflow", stage)
		}
		select {
		case frame := <-c.frames:
			if match(frame.packet) {
				return frame, nil
			}
		case <-ctx.Done():
			return reception{}, fmt.Errorf("%s: %w (completed_tx=%d physical_rx=%d rejected_local_or_unmeasured=%d)", stage, ctx.Err(), c.transmitted.Load(), c.physical.Load(), c.rejected.Load())
		}
	}
}
func groupPacket(key [16]byte, text string, timestamp uint32) (*meshcore.Packet, error) {
	hash := sha256.Sum256(key[:])
	plain := binary.LittleEndian.AppendUint32(nil, timestamp)
	plain = append(plain, 0)
	plain = append(plain, "rf-check: "...)
	plain = append(plain, text...)
	encrypted, err := meshcore.EncryptThenMAC(key[:], plain)
	if err != nil {
		return nil, err
	}
	return &meshcore.Packet{Header: 0x15, PathLength: 0x80, Payload: append([]byte{hash[0]}, encrypted...)}, nil
}
func forwardedBy(p *meshcore.Packet, original *meshcore.Packet, id meshcore.Identity) bool {
	key := id.PublicKey()
	return p.IsRouteFlood() && p.PayloadType() == original.PayloadType() && bytes.Equal(p.Payload, original.Payload) &&
		p.PathHashSize() == 3 && len(p.Path) >= 3 && bytes.Equal(p.Path[len(p.Path)-3:], key[:3])
}
func roomLogin(id meshcore.LocalIdentity, room meshcore.Identity, secret []byte, stamp uint32, password string) (*meshcore.Packet, error) {
	plain := binary.LittleEndian.AppendUint32(nil, stamp)
	plain = binary.LittleEndian.AppendUint32(plain, uint32(time.Now().Unix()))
	plain = append(plain, password...)
	plain = append(plain, 0)
	encrypted, err := meshcore.EncryptThenMAC(secret, plain)
	if err != nil {
		return nil, err
	}
	payload := append([]byte{room.PublicKey()[0]}, id.PublicKeyBytes()...)
	payload = append(payload, encrypted...)
	return &meshcore.Packet{Header: 0x1d, Payload: payload}, nil
}
func decryptFrom(p *meshcore.Packet, self, peer meshcore.Identity, secret []byte) []byte {
	if len(p.Payload) < 4 || p.Payload[0] != self.PublicKey()[0] || p.Payload[1] != peer.PublicKey()[0] {
		return nil
	}
	plain, err := meshcore.MACThenDecrypt(secret, p.Payload[2:])
	if err != nil {
		return nil
	}
	return plain
}

type route struct {
	known  bool
	length byte
	path   []byte
}

func loginResponse(p *meshcore.Packet, self, room meshcore.Identity, secret []byte) (route, bool) {
	var out route
	if p.PayloadType() != meshcore.PayloadTypePath && p.PayloadType() != meshcore.PayloadTypeResponse {
		return out, false
	}
	plain := decryptFrom(p, self, room, secret)
	if plain == nil {
		return out, false
	}
	if p.PayloadType() == meshcore.PayloadTypePath {
		path, err := meshcore.ParsePathPayload(plain)
		if err != nil || path.ExtraType != meshcore.PayloadTypeResponse {
			return out, false
		}
		out = route{true, path.PathLength, bytes.Clone(path.Path)}
		plain = path.Extra
	}
	return out, len(plain) >= 13 && plain[4] == 0 && plain[7]&3 >= 2 && plain[12] >= 1
}
func datagram(id meshcore.LocalIdentity, peer meshcore.Identity, secret []byte, kind byte, plain []byte, path route) (*meshcore.Packet, error) {
	encrypted, err := meshcore.EncryptThenMAC(secret, plain)
	if err != nil {
		return nil, err
	}
	p := &meshcore.Packet{Header: kind<<2 | 1, Payload: append([]byte{peer.PublicKey()[0], id.PublicKey()[0]}, encrypted...)}
	if path.known {
		p.Header = kind<<2 | 2
		p.PathLength = path.length
		p.Path = bytes.Clone(path.path)
	}
	return p, nil
}
func reciprocalPath(id meshcore.LocalIdentity, room meshcore.Identity, secret []byte, received *meshcore.Packet, out route) (*meshcore.Packet, error) {
	body := append([]byte{received.PathLength}, received.Path...)
	body = append(body, 0x0f)
	var unique [4]byte
	if _, err := rand.Read(unique[:]); err != nil {
		return nil, err
	}
	body = append(body, unique[:]...)
	return datagram(id, room, secret, 8, body, out)
}
func textPlain(timestamp uint32, text string) []byte {
	plain := binary.LittleEndian.AppendUint32(nil, timestamp)
	plain = append(plain, 0)
	return append(plain, text...)
}
func ackProof(plain []byte, key meshcore.Identity) uint32 {
	hash := sha256.New()
	hash.Write(plain)
	hash.Write(key.PublicKeyBytes())
	return binary.LittleEndian.Uint32(hash.Sum(nil)[:4])
}
func matchesACK(p *meshcore.Packet, want uint32) bool {
	payload := p.Payload
	if p.PayloadType() == meshcore.PayloadTypeMultiPart {
		part, err := meshcore.MultiPartFromBytes(payload)
		if err != nil || part.WrappedType != meshcore.PayloadTypeAck {
			return false
		}
		payload = part.WrappedPayload
	} else if p.PayloadType() != meshcore.PayloadTypeAck {
		return false
	}
	return len(payload) >= 4 && binary.LittleEndian.Uint32(payload) == want
}
func groupMatches(p *meshcore.Packet, key [16]byte, text string) bool {
	if p.PayloadType() != meshcore.PayloadTypeGrpTxt {
		return false
	}
	group, err := meshcore.GroupTextFromBytes(p.Payload)
	if err != nil {
		return false
	}
	hash := sha256.Sum256(key[:])
	if group.ChannelHash != hash[0] {
		return false
	}
	plain := group.Decrypt(key[:])
	if len(plain) < 5 || plain[4]>>2 != 0 {
		return false
	}
	body := string(cString(plain[5:]))
	return body == text || bytes.HasSuffix([]byte(body), []byte(": "+text))
}
func cString(b []byte) []byte {
	if i := bytes.IndexByte(b, 0); i >= 0 {
		return b[:i]
	}
	return b
}
func privatePlain(p *meshcore.Packet, self, base meshcore.Identity, secret []byte, text string) ([]byte, error) {
	if p.PayloadType() != meshcore.PayloadTypeTxtMsg {
		return nil, errors.New("not a text packet")
	}
	plain := decryptFrom(p, self, base, secret)
	if len(plain) < 5 || plain[4]>>2 != 0 || string(cString(plain[5:])) != text {
		return nil, errors.New("not the expected authenticated private text")
	}
	return plain[:5+len(text)], nil
}
