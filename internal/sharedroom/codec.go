package sharedroom

import (
	"bytes"
	"context"
	"crypto/rand"
	"crypto/sha256"
	"encoding/base64"
	"encoding/binary"
	"encoding/hex"
	"errors"
	"fmt"
	"time"
	"unicode/utf8"

	meshcore "github.com/meshcore-go/meshcore-go"
)

type Route struct {
	Known  bool   `json:"known"`
	Length byte   `json:"length"`
	Path   []byte `json:"path,omitempty"`
}

// The Worker stores this as opaque canonical base64. Native route v1 is
// version, flags (known route), encoded path length, path bytes.
func (r Route) encode() string {
	flags := byte(0)
	if r.Known {
		flags |= 1
	}
	b := append([]byte{1, flags, r.Length}, r.Path...)
	return base64.StdEncoding.EncodeToString(b)
}
func route(raw string, fallback Route) (Route, error) {
	if raw == "" {
		return fallback, nil
	}
	var out Route
	b, err := base64.StdEncoding.Strict().DecodeString(raw)
	if err != nil || len(b) < 3 || b[0] != 1 || b[1]&^byte(1) != 0 || !meshcore.IsValidPathLen(b[2]) {
		return out, errors.New("invalid stored radio route")
	}
	pathSize := int(b[2]&63) * int(b[2]>>6+1)
	expected := 3 + pathSize
	if len(b) != expected {
		return out, errors.New("invalid stored radio path")
	}
	out.Known, out.Length, out.Path = b[1]&1 != 0, b[2], bytes.Clone(b[3:3+pathSize])
	return out, nil
}

type Lookup func(context.Context, string, string) ([]Member, error)
type Decoded struct {
	Identity  Identity
	Operation Operation
	Packet    *meshcore.Packet
	Plain     []byte // exact unpadded content used for client post/REQ ACK proofs
	Return    Route
}
type Codec struct{ Identities []Identity }

// Decode tries every matching room and client prefix and requires a single
// authenticated result. A one-byte prefix is never an identity assertion.
func (c Codec) Decode(ctx context.Context, wire []byte, lookup Lookup) (*Decoded, error) {
	p, err := meshcore.PacketFromBytes(wire)
	if err != nil {
		return nil, nil
	}
	if p.PayloadType() == meshcore.PayloadTypeAck || p.PayloadType() == meshcore.PayloadTypeMultiPart {
		payload := p.Payload
		if p.PayloadType() == meshcore.PayloadTypeMultiPart {
			part, err := meshcore.MultiPartFromBytes(payload)
			if err != nil || part.WrappedType != meshcore.PayloadTypeAck {
				return nil, nil
			}
			payload = part.WrappedPayload
		}
		if len(payload) < 4 {
			return nil, nil
		}
		proof := fmt.Sprintf("%08x", binary.LittleEndian.Uint32(payload))
		var match *Decoded
		for _, identity := range c.Identities {
			members, err := lookup(ctx, identity.Alias, "")
			if err != nil {
				return nil, err
			}
			for _, member := range members {
				if member.Pending == nil || member.Pending.Proof == nil || *member.Pending.Proof != proof {
					continue
				}
				if match != nil {
					return nil, nil
				} // Bare native ACK has no full identity: reject collisions.
				match = &Decoded{Identity: identity, Packet: p, Operation: Operation{Op: "ack", Client: member.Client,
					DeliveryID: member.Pending.DeliveryID, Proof: proof}}
			}
		}
		return match, nil
	}
	kind := p.PayloadType()
	if kind != meshcore.PayloadTypeAnonReq && kind != meshcore.PayloadTypeTxtMsg &&
		kind != meshcore.PayloadTypeReq && kind != meshcore.PayloadTypePath {
		return nil, nil
	}
	if len(p.Payload) < 4 {
		return nil, nil
	}
	hash := sha256.Sum256(append([]byte{kind}, p.Payload...))
	attempt := hex.EncodeToString(hash[:])
	var match *Decoded
	for _, identity := range c.Identities {
		if identity.Key.PublicKey()[0] != p.Payload[0] {
			continue
		}
		type candidate struct {
			key    meshcore.Identity
			member Member
			plain  []byte
		}
		var candidates []candidate
		if kind == meshcore.PayloadTypeAnonReq {
			a, err := meshcore.AnonReqFromBytes(p.Payload)
			if err != nil {
				continue
			}
			peer := meshcore.NewIdentity(a.EphemeralPubKey)
			secret, err := identity.Key.SharedSecret(peer)
			if err != nil {
				continue
			}
			if plain := a.Decrypt(secret); plain != nil {
				members, err := lookup(ctx, identity.Alias, peer.String())
				if err != nil {
					return nil, err
				}
				member := Member{}
				if len(members) == 1 {
					member = members[0]
				}
				candidates = append(candidates, candidate{peer, member, plain})
			}
		} else {
			request, err := meshcore.RequestFromBytes(p.Payload)
			if err != nil {
				continue
			}
			members, err := lookup(ctx, identity.Alias, fmt.Sprintf("%02x", request.Source))
			if err != nil {
				return nil, err
			}
			for _, member := range members {
				peer, err := meshcore.NewIdentityFromHex(member.Client)
				if err != nil {
					continue
				}
				secret, err := identity.Key.SharedSecret(peer)
				if err != nil {
					continue
				}
				if plain := request.Decrypt(secret); plain != nil {
					candidates = append(candidates, candidate{peer, member, plain})
				}
			}
		}
		for _, candidate := range candidates {
			plain := candidate.plain
			fallback := Route{Length: identity.PathHashMode << 6}
			back, err := route(candidate.member.Route, fallback)
			if err != nil {
				return nil, err
			}
			op := Operation{Client: candidate.key.String(), Attempt: attempt}
			decoded := &Decoded{Identity: identity, Packet: p, Return: back}
			switch kind {
			case meshcore.PayloadTypeAnonReq:
				if len(plain) < 9 {
					continue
				}
				since := binary.LittleEndian.Uint32(plain[4:])
				password := cstring(plain[8:])
				if !utf8.Valid(password) {
					continue
				}
				op.Op, op.Timestamp, op.Since, op.Password = "login", binary.LittleEndian.Uint32(plain), &since, string(password)
				if p.IsRouteFlood() {
					back = fallback
					decoded.Return = back
				}
				op.Route = back.encode()
			case meshcore.PayloadTypeTxtMsg:
				if len(plain) < 6 || plain[4]>>2 != 0 {
					continue
				} // Signed room deliveries never enter shared history.
				text := cstring(plain[5:])
				if len(text) == 0 || !utf8.Valid(text) {
					continue
				}
				op.Op, op.Timestamp, op.Text, op.Source = "post", binary.LittleEndian.Uint32(plain), meshcore.TruncateUTF8(string(text), 150), "client"
				decoded.Plain = bytes.Clone(plain[:5+len(text)])
			case meshcore.PayloadTypeReq:
				if len(plain) < 9 || plain[4] != 2 || !p.IsRouteDirect() {
					continue
				}
				since := binary.LittleEndian.Uint32(plain[5:])
				op.Op, op.Timestamp, op.Since, op.Route = "refresh", binary.LittleEndian.Uint32(plain), &since, back.encode()
				decoded.Plain = bytes.Clone(plain[:9])
			case meshcore.PayloadTypePath:
				path, err := meshcore.ParsePathPayload(plain)
				if err != nil {
					continue
				}
				back.Known, back.Length, back.Path = true, path.PathLength, bytes.Clone(path.Path)
				op.Op, op.Route = "path", back.encode()
				if path.ExtraType == meshcore.PayloadTypeAck && len(path.Extra) >= 4 && candidate.member.Pending != nil {
					proof := fmt.Sprintf("%08x", binary.LittleEndian.Uint32(path.Extra))
					if candidate.member.Pending.Proof != nil && *candidate.member.Pending.Proof == proof {
						op.Proof, op.DeliveryID = proof, candidate.member.Pending.DeliveryID
					}
				}
			}
			decoded.Operation = op
			if match != nil {
				return nil, nil
			}
			match = decoded
		}
	}
	return match, nil
}
func cstring(raw []byte) []byte {
	if i := bytes.IndexByte(raw, 0); i >= 0 {
		return raw[:i]
	}
	return raw
}

func routed(identity Identity, kind byte, payload []byte, back Route) *meshcore.Packet {
	p := &meshcore.Packet{Header: meshcore.MakeHeader(meshcore.RouteTypeFlood, kind, 0), PathLength: identity.PathHashMode << 6, Payload: payload}
	if back.Known {
		p.Header = meshcore.MakeHeader(meshcore.RouteTypeDirect, kind, 0)
		p.PathLength, p.Path = back.Length, bytes.Clone(back.Path)
	}
	return p
}
func datagram(identity Identity, client string, kind byte, plain []byte, back Route) (*meshcore.Packet, error) {
	peer, err := meshcore.NewIdentityFromHex(client)
	if err != nil {
		return nil, err
	}
	secret, err := identity.Key.SharedSecret(peer)
	if err != nil {
		return nil, err
	}
	sealed, err := meshcore.EncryptThenMAC(secret, plain)
	if err != nil {
		return nil, err
	}
	payload := append([]byte{peer.PublicKey()[0], identity.Key.PublicKey()[0]}, sealed...)
	if len(payload) > meshcore.MaxPacketPayload {
		return nil, errors.New("room datagram exceeds native payload limit")
	}
	return routed(identity, kind, payload, back), nil
}

// Response is called only after the backend returns respond:true.
func (c Codec) Response(decoded *Decoded, result Result) (*meshcore.Packet, time.Duration, error) {
	if !result.Respond {
		return nil, 0, nil
	}
	op, identity := decoded.Operation, decoded.Identity
	if op.Op == "login" {
		plain := make([]byte, 13)
		binary.LittleEndian.PutUint32(plain, uint32(time.Now().Unix()))
		plain[7], plain[12] = 2, 1 // Native room read/write permissions and protocol version.
		if _, err := rand.Read(plain[8:12]); err != nil {
			return nil, 0, err
		}
		kind := byte(meshcore.PayloadTypeResponse)
		if decoded.Packet.IsRouteFlood() {
			body := append([]byte{decoded.Packet.PathLength}, decoded.Packet.Path...)
			body = append(body, meshcore.PayloadTypeResponse)
			plain, kind = append(body, plain...), meshcore.PayloadTypePath
		}
		packet, err := datagram(identity, op.Client, kind, plain, decoded.Return)
		return packet, 300 * time.Millisecond, err
	}
	if op.Op == "post" || op.Op == "refresh" {
		peer, err := meshcore.NewIdentityFromHex(op.Client)
		if err != nil {
			return nil, 0, err
		}
		ack := binary.LittleEndian.AppendUint32(nil, meshcore.CalcAckHash(decoded.Plain, peer.PublicKeyBytes()))
		if op.Op == "refresh" {
			ack = append(ack, result.Remaining)
		}
		return routed(identity, meshcore.PayloadTypeAck, ack, decoded.Return), 300 * time.Millisecond, nil
	}
	return nil, 0, nil
}

func (c Codec) Delivery(identity Identity, delivery Delivery) (*meshcore.Packet, string, error) {
	if delivery.Alias != identity.Alias {
		return nil, "", errors.New("delivery advertised identity mismatch")
	}
	back, err := route(delivery.Route, Route{Length: identity.PathHashMode << 6})
	if err != nil {
		return nil, "", err
	}
	author, err := meshcore.NewIdentityFromHex(delivery.Message.Author)
	if err != nil {
		return nil, "", err
	}
	peer, err := meshcore.NewIdentityFromHex(delivery.Client)
	if err != nil {
		return nil, "", err
	}
	var nonce [1]byte
	if _, err := rand.Read(nonce[:]); err != nil {
		return nil, "", err
	}
	plain := binary.LittleEndian.AppendUint32(nil, delivery.Message.Timestamp)
	plain = append(plain, 2<<2|nonce[0]&3)
	plain = append(plain, author.PublicKeyBytes()[:4]...)
	plain = append(plain, delivery.Message.Text...)
	proof := fmt.Sprintf("%08x", meshcore.CalcAckHash(plain, peer.PublicKeyBytes()))
	if proof == "00000000" {
		return nil, "", errors.New("native room ACK proof is zero")
	}
	packet, err := datagram(identity, delivery.Client, meshcore.PayloadTypeTxtMsg, plain, back)
	return packet, proof, err
}
func (c Codec) Advertisement(identity Identity, timestamp uint32) (*meshcore.Packet, error) {
	app, err := (&meshcore.AdvertAppData{Type: "ROOM", Name: identity.Name}).ToBytes()
	if err != nil {
		return nil, err
	}
	advert := &meshcore.Advert{PublicKey: identity.Key.Identity, Timestamp: timestamp, RawAppData: app}
	advert.SignWith(identity.Key)
	payload, err := advert.ToBytes()
	if err != nil {
		return nil, err
	}
	return routed(identity, meshcore.PayloadTypeAdvert, payload, Route{}), nil
}
