package companion

import (
	"bytes"
	"crypto/rand"
	"crypto/sha256"
	"errors"
	"fmt"
	"time"

	meshcore "github.com/meshcore-go/meshcore-go"
	protocol "github.com/meshcore-go/meshcore-go/companion"
	"github.com/meshcore-go/meshcore-go/node"
	"meshcore.local/meshcore/internal/policy"
)

func (s *Server) selfAdvert() (*meshcore.Packet, error) {
	data := meshcore.AdvertAppData{Type: "CHAT", Name: s.state.Name}
	if s.state.AdvertLocationPolicy == 1 {
		data.Lat, data.Lon, data.HasLocation = s.state.Latitude, s.state.Longitude, true
	}
	app, err := data.ToBytes()
	if err != nil {
		return nil, err
	}
	id := s.Node.Identity()
	adv := &meshcore.Advert{PublicKey: id.Identity, Timestamp: s.now(), RawAppData: app}
	adv.SignWith(id)
	payload, err := adv.ToBytes()
	return &meshcore.Packet{Header: meshcore.MakeHeader(meshcore.RouteTypeFlood, meshcore.PayloadTypeAdvert, 0), Payload: payload}, err
}

func (s *Server) sendAdvert(flood bool) error {
	pkt, err := s.selfAdvert()
	if err != nil {
		return err
	}
	if !flood {
		pkt.Header = meshcore.MakeHeader(meshcore.RouteTypeDirect, meshcore.PayloadTypeAdvert, 0)
	}
	if flood {
		s.prepareFlood(pkt, s.state.Preferences.DefaultScope)
	}
	return s.sendOrigin(pkt, 0)
}

func (s *Server) routed(p *contact, typ byte, payload []byte, flood bool) *meshcore.Packet {
	pkt := &meshcore.Packet{Header: meshcore.MakeHeader(meshcore.RouteTypeFlood, typ, 0), Payload: payload}
	s.prepareFlood(pkt, s.state.Preferences.DefaultScope)
	if !flood && p.OutPathLen != 255 {
		size, count, _ := decodePath(p.OutPathLen)
		pkt.Header = meshcore.MakeHeader(meshcore.RouteTypeDirect, typ, 0)
		pkt.PathLength = p.OutPathLen
		pkt.Path = append([]byte{}, p.OutPath[:size*count]...)
	}
	return pkt
}

func (s *Server) encrypted(p *contact, typ byte, plain []byte, anonymous, flood bool) (*meshcore.Packet, error) {
	secret, err := s.Node.SharedSecret(p.Identity())
	if err != nil {
		return nil, err
	}
	cipher, err := meshcore.EncryptThenMAC(secret, plain)
	if err != nil {
		return nil, err
	}
	payload := []byte{p.PublicKey[0]}
	if anonymous {
		payload = append(payload, s.Node.Identity().PublicKeyBytes()...)
	} else {
		payload = append(payload, s.Node.Identity().PublicKey()[0])
	}
	payload = append(payload, cipher...)
	return s.routed(p, typ, payload, flood), nil
}

func (s *Server) sentFrame(pkt *meshcore.Packet, tag uint32) []byte {
	airtime := uint32(1000)
	if s.cfg.AirtimeEstimator != nil {
		data, _ := pkt.ToBytes()
		airtime = s.cfg.AirtimeEstimator(len(data))
	}
	timeout := node.CalcDirectTimeout(airtime, pkt.PathHashCount())
	out := make([]byte, 10)
	out[0] = protocol.RespSent
	if pkt.IsRouteFlood() {
		out[1] = 1
		timeout = node.CalcFloodTimeout(airtime)
	}
	put32(out[2:6], tag)
	put32(out[6:10], uint32(timeout.Milliseconds()))
	return out
}

func (s *Server) sendText(c *session, p *contact, typ, attempt byte, timestamp uint32, text []byte) ([]byte, error) {
	if len(text) > meshcore.MaxTextLen || (attempt > 3 && typ == protocol.TxtTypePlain && len(text) > 158) || (typ == protocol.TxtTypePlain && len(s.acks) >= 128) {
		return nil, errors.New("message or pending ACK capacity exceeded")
	}
	if typ == protocol.TxtTypeCLIData {
		timestamp = s.uniqueTag()
		attempt &= 3
	}
	plain := meshcore.BuildTextPlaintextWithAttempt(time.Unix(int64(timestamp), 0), typ<<2, text, int(attempt))
	pkt, err := s.encrypted(p, meshcore.PayloadTypeTxtMsg, plain, false, false)
	if err != nil {
		return nil, err
	}
	crc := meshcore.CalcAckHash(plain[:5+len(text)], s.Node.Identity().PublicKeyBytes())
	if err := s.sendForSession(c, pkt, 0); err != nil {
		return nil, err
	}
	if typ == protocol.TxtTypePlain {
		s.acks[crc] = time.Now()
	} else {
		crc = 0
	}
	return s.sentFrame(pkt, crc), nil
}

func (s *Server) uniqueTag() uint32 {
	now := s.now()
	if now <= s.nextTag {
		now = s.nextTag + 1
	}
	s.nextTag = now
	return now
}

func (s *Server) pendingLogin(key [32]byte) (uint32, bool) {
	for tag, req := range s.pending {
		if req.Key == key && req.Kind == protocol.CmdSendLogin {
			return tag, true
		}
	}
	return 0, false
}

func (s *Server) expireLoginFences(now time.Time) {
	for tag, fence := range s.loginFences {
		if !now.Before(fence.Expires) {
			delete(s.loginFences, tag)
		}
	}
}

func (s *Server) sendRequest(c *session, p *contact, kind byte, data []byte) ([]byte, error) {
	tag := s.uniqueTag()
	var previousTag uint32
	var replacingLogin bool
	plain := make([]byte, 4)
	put32(plain, tag)
	typ, anonymous, flood := byte(meshcore.PayloadTypeReq), false, false
	switch kind {
	case protocol.CmdSendLogin:
		data = []byte(cstring(data))
		if len(data) > 15 {
			data = data[:15]
		}
		previousTag, replacingLogin = s.pendingLogin(p.PublicKey)
		s.expireLoginFences(time.Now())
		if replacingLogin && len(s.loginFences) >= maxPendingRequests {
			return nil, errors.New("superseded login response capacity exceeded")
		}
		if p.Type == meshcore.AdvertTypeRoom {
			since := make([]byte, 4)
			put32(since, p.SyncSince)
			plain = append(plain, since...)
		}
		plain = append(plain, data...)
		typ, anonymous = meshcore.PayloadTypeAnonReq, true
	case protocol.CmdSendStatusReq:
		plain = append(plain, 1, 0, 0, 0, 0)
	case protocol.CmdSendTelemetryReq:
		plain = append(plain, 3, 0, 0, 0, 0)
	case protocol.CmdSendPathDiscoveryReq:
		plain = append(plain, 3, 0xfe, 0, 0, 0)
		var nonce [4]byte
		if _, err := rand.Read(nonce[:]); err != nil {
			return nil, err
		}
		plain = append(plain, nonce[:]...)
		flood = true
	case protocol.CmdSendBinaryReq, protocol.CmdSendAnonReq:
		if len(data) == 0 || len(data) > protocol.MaxFrameSize-33 {
			return nil, errors.New("request payload exceeds companion frame capacity")
		}
		plain = append(plain, data...)
		if kind == protocol.CmdSendAnonReq {
			typ, anonymous = meshcore.PayloadTypeAnonReq, true
		}
	}
	pkt, err := s.encrypted(p, typ, plain, anonymous, flood)
	if err != nil {
		return nil, err
	}
	if err := s.sendForSession(c, pkt, 0); err != nil {
		return nil, err
	}
	expires := time.Now().Add(2 * time.Minute)
	if replacingLogin {
		// Retire only after TX admission; a failed retry leaves the old request intact.
		delete(s.pending, previousTag)
		s.loginFences[previousTag] = loginFence{Key: p.PublicKey, Expires: expires}
	}
	if kind == protocol.CmdSendLogin {
		for oldTag, fence := range s.loginFences {
			if fence.Key == p.PublicKey {
				fence.Expires = expires
				s.loginFences[oldTag] = fence
			}
		}
	}
	s.pending[tag] = pending{Client: c, Key: p.PublicKey, Kind: kind, Expires: expires}
	if kind == protocol.CmdSendLogin {
		return s.sentFrame(pkt, u32(p.PublicKey[:4])), nil
	}
	return s.sentFrame(pkt, tag), nil
}

func (s *Server) storeAdvert(pkt *meshcore.Packet, adv *meshcore.Advert, force bool) (*contact, [][]byte) {
	app := adv.AppData()
	// Native hasName checks the first parsed name byte, not just ADV_NAME_MASK.
	if app.Name == "" || app.Name[0] == 0 {
		return nil, nil
	}
	var events [][]byte
	key := adv.PublicKey.PublicKey()
	p := s.findContact(key[:])
	if p != nil && p.Type == 0 {
		p = nil
	}
	isNew := p == nil
	if isNew {
		candidate := contact{ContactResponse: protocol.ContactResponse{PublicKey: key, OutPathLen: 255}}
		candidate.AdvertName, candidate.Type = app.Name, advertType(app.Type)
		candidate.LastAdvert = adv.Timestamp
		if app.HasLocation {
			candidate.AdvertLatitude, candidate.AdvertLongitude = app.Lat, app.Lon
		}
		candidate.LastModified = s.now()
		autoType := s.state.ManualAdd == 0 || (candidate.Type >= 1 && candidate.Type <= 4 && s.state.AutoAddConfig&(1<<candidate.Type) != 0)
		withinHops := s.state.AutoAddMaxHops == 0 || pkt.PathHashCount() < s.state.AutoAddMaxHops
		if !force && (!autoType || !withinHops) {
			return nil, [][]byte{contactFrame(protocol.PushNewAdvert, &candidate)}
		}
		if len(s.state.Contacts) >= maxContacts {
			oldest := -1
			if s.state.AutoAddConfig&1 != 0 {
				for i := range s.state.Contacts {
					c := &s.state.Contacts[i]
					if c.Flags&1 == 0 && (oldest < 0 || c.LastModified < s.state.Contacts[oldest].LastModified) {
						oldest = i
					}
				}
			}
			if oldest < 0 {
				return nil, [][]byte{contactFrame(protocol.PushNewAdvert, &candidate), {protocol.PushContactsFull}}
			}
			evicted := s.state.Contacts[oldest].PublicKey
			s.state.Contacts = append(s.state.Contacts[:oldest], s.state.Contacts[oldest+1:]...)
			events = append(events, append([]byte{protocol.PushContactDeleted}, evicted[:]...))
		}
		s.state.Contacts = append(s.state.Contacts, candidate)
		p = &s.state.Contacts[len(s.state.Contacts)-1]
	} else if !force && adv.Timestamp <= p.LastAdvert {
		return nil, nil
	}
	p.AdvertName, p.Type = app.Name, advertType(app.Type)
	p.LastAdvert, p.LastModified = adv.Timestamp, s.modified()
	if app.HasLocation {
		p.AdvertLatitude, p.AdvertLongitude = app.Lat, app.Lon
	}
	p.HeardPath, p.HeardPathLen, p.HeardAt = append([]byte(nil), pkt.Path...), pkt.PathLength, s.now()
	// Exported contacts are zero-path signed adverts, independent of how they arrived.
	export := *pkt
	export.Path = nil
	export.PathLength = 0
	export.Header = meshcore.MakeHeader(meshcore.RouteTypeFlood, meshcore.PayloadTypeAdvert, 0)
	p.Advert, _ = export.ToBytes()
	if !force {
		if isNew {
			events = append(events, contactFrame(protocol.PushNewAdvert, p))
		} else {
			events = append(events, append([]byte{protocol.PushAdvert}, key[:]...))
		}
	}
	return p, events
}

func advertType(name string) byte {
	switch name {
	case "CHAT":
		return meshcore.AdvertTypeChat
	case "REPEATER":
		return meshcore.AdvertTypeRepeater
	case "ROOM":
		return meshcore.AdvertTypeRoom
	case "SENSOR":
		return meshcore.AdvertTypeSensor
	}
	return 0
}

func (s *Server) handlePacket(pkt *meshcore.Packet, rx policy.ReceiveContext) {
	if s.storageFault != nil {
		return
	}
	switch pkt.PayloadType() {
	case meshcore.PayloadTypeGrpData:
		s.incomingDatagram(pkt)
	case meshcore.PayloadTypeTrace, meshcore.PayloadTypeControl, meshcore.PayloadTypeRawCustom:
		s.incomingDiagnostic(pkt)
	case meshcore.PayloadTypeAdvert:
		adv, err := meshcore.AdvertFromBytes(pkt.Payload)
		if err != nil || !adv.Verify() || adv.PublicKey.Matches(s.Node.Identity().Identity) || cstring([]byte(adv.AppData().Name)) == "" {
			return
		}
		oldContacts, oldModified := append([]contact(nil), s.state.Contacts...), s.state.LastModified
		key := adv.PublicKey.PublicKey()
		s.rememberAdvertPath(key, pkt)
		oldTransient := s.transient[key]
		p, events := s.storeAdvert(pkt, adv, false)
		if p != nil {
			if err := s.save(); err != nil {
				if s.storageFault == nil {
					s.state.Contacts, s.state.LastModified = oldContacts, oldModified
					if oldTransient != nil {
						s.transient[key] = oldTransient
					}
				}
				s.report(err)
				return
			}
			delete(s.transient, key)
			s.hydrate()
		}
		for _, frame := range events {
			s.broadcast(frame)
		}
	case meshcore.PayloadTypeGrpTxt:
		msg, ch, err := s.Node.DecryptGroupText(pkt)
		if err != nil || msg.Flags>>2 != 0 {
			return
		}
		for i, entry := range s.state.Channels {
			if entry == nil || entry.Name == "" || entry.PSK != ch.PSK {
				continue
			}
			text := msg.Text
			if msg.Sender != "" {
				text = msg.Sender + ": " + text
			}
			frame := []byte{protocol.RespChannelMsgRecvV3, byte(int8(pkt.SNR * 4)), 0, 0, byte(i), messagePath(pkt), 0}
			ts := make([]byte, 4)
			put32(ts, msg.Timestamp)
			frame = append(frame, ts...)
			frame = append(frame, []byte(text)...)
			s.queueMessage(frame)
			return
		}
	case meshcore.PayloadTypeAck:
		if len(pkt.Payload) >= 4 {
			s.ack(u32(pkt.Payload))
		}
	case meshcore.PayloadTypeMultiPart:
		part, err := meshcore.MultiPartFromBytes(pkt.Payload)
		if err == nil && part.WrappedType == meshcore.PayloadTypeAck && len(part.WrappedPayload) >= 4 {
			s.ack(u32(part.WrappedPayload))
		}
	case meshcore.PayloadTypeTxtMsg, meshcore.PayloadTypePath, meshcore.PayloadTypeResponse, meshcore.PayloadTypeReq:
		if len(pkt.Payload) < 4 || pkt.Payload[0] != s.Node.Identity().PublicKey()[0] {
			return
		}
		contacts := make([]*contact, 0, len(s.state.Contacts)+len(s.transient))
		for i := range s.state.Contacts {
			contacts = append(contacts, &s.state.Contacts[i])
		}
		for _, p := range s.transient {
			contacts = append(contacts, p)
		}
		for _, p := range contacts {
			if p.PublicKey[0] != pkt.Payload[1] {
				continue
			}
			secret, err := s.Node.SharedSecret(p.Identity())
			if err != nil {
				continue
			}
			plain, err := meshcore.MACThenDecrypt(secret, pkt.Payload[2:])
			if err != nil {
				continue
			}
			s.active(p.PublicKey)
			switch pkt.PayloadType() {
			case meshcore.PayloadTypeTxtMsg:
				s.incomingTextContext(p, pkt, plain, rx)
			case meshcore.PayloadTypeResponse:
				s.response(p, plain)
			case meshcore.PayloadTypePath:
				s.incomingPath(p, pkt, plain, rx)
			case meshcore.PayloadTypeReq:
				s.incomingRequest(p, pkt, plain, rx)
			}
			return
		}
	}
}

func messagePath(pkt *meshcore.Packet) byte {
	if pkt.IsRouteFlood() {
		return pkt.PathLength
	}
	return 255
}

func (s *Server) queueMessage(frame []byte) bool {
	if len(frame) > protocol.MaxFrameSize {
		s.report(errors.New("received message exceeds companion frame limit"))
		return false
	}
	// Radio retries may change path, SNR and attempt flags, but not the application message.
	semantic := append([]byte{}, frame[4:]...)
	if frame[0] == protocol.RespContactMsgRecvV3 {
		semantic[6] = 0
	} else {
		semantic[1] = 0
	}
	digest := sha256.Sum256(append([]byte{frame[0]}, semantic...))
	for _, old := range s.state.Messages {
		if old.Digest == digest {
			if err := s.save(); err != nil {
				s.report(err)
				return false
			}
			return true
		}
	}
	oldMessages, oldSequence := s.state.Messages, s.state.Sequence
	evict := -1
	if len(oldMessages) >= maxMessages {
		for i, m := range oldMessages {
			if m.Frame[0] == protocol.RespChannelMsgRecvV3 || m.Frame[0] == protocol.RespChannelDataRecv {
				evict = i
				break
			}
		}
		if evict < 0 && s.state.Retention == DurableReplay && len(s.clients) > 0 {
			readThrough := s.state.Sequence
			for c := range s.clients {
				readThrough = min(readThrough, c.cursor)
			}
			for i, m := range oldMessages {
				if m.Sequence <= readThrough {
					evict = i
					break
				}
			}
		}
		if evict < 0 {
			s.report(errors.New("companion message queue full of unread direct messages; fetch queued messages before receiving new replies"))
			return false
		}
	}
	s.state.Sequence++
	s.state.Messages = append(append([]message(nil), s.state.Messages...), message{Sequence: s.state.Sequence, Frame: frame, Digest: digest})
	if evict >= 0 {
		s.state.Messages = append(s.state.Messages[:evict], s.state.Messages[evict+1:]...)
	}
	if err := s.save(); err != nil {
		if s.storageFault == nil {
			s.state.Messages, s.state.Sequence = oldMessages, oldSequence
		}
		s.report(err)
		for c := range s.clients {
			c.close()
		}
		return false
	}
	first := s.state.Messages[0].Sequence
	for c := range s.clients {
		if c.cursor+1 < first || (evict >= 0 && c.cursor < oldMessages[evict].Sequence) {
			c.close()
		} else {
			c.send([]byte{protocol.PushMsgWaiting})
		}
	}
	return true
}

func (s *Server) incomingText(p *contact, pkt *meshcore.Packet, plain []byte) {
	s.incomingTextContext(p, pkt, plain, s.receiveContext(pkt))
}

func (s *Server) incomingTextContext(p *contact, pkt *meshcore.Packet, plain []byte, rx policy.ReceiveContext) {
	if len(plain) < 5 {
		return
	}
	typ, offset := plain[4]>>2, 5
	if typ == protocol.TxtTypeSignedPlain {
		offset = 9
	}
	if typ > protocol.TxtTypeSignedPlain || len(plain) < offset {
		return
	}
	text := []byte(cstring(plain[offset:]))
	frame := []byte{protocol.RespContactMsgRecvV3, byte(int8(pkt.SNR * 4)), 0, 0}
	frame = append(frame, p.PublicKey[:6]...)
	frame = append(frame, messagePath(pkt), typ)
	frame = append(frame, plain[:4]...)
	if offset == 9 {
		frame = append(frame, plain[5:9]...)
	}
	frame = append(frame, text[:min(len(text), protocol.MaxFrameSize-len(frame))]...)
	oldSync, oldModified, oldGlobalModified := p.SyncSince, p.LastModified, s.state.LastModified
	if typ != protocol.TxtTypeCLIData {
		p.LastModified = s.modified()
	}
	if typ == protocol.TxtTypeSignedPlain && u32(plain[:4]) > p.SyncSince {
		p.SyncSince = u32(plain[:4])
	}
	if !s.queueMessage(frame) {
		if s.storageFault == nil {
			p.SyncSince = oldSync
			p.LastModified, s.state.LastModified = oldModified, oldGlobalModified
		}
		return
	}
	var extra []byte
	extraType := byte(0)
	if typ != protocol.TxtTypeCLIData {
		key := p.PublicKey[:]
		if typ == protocol.TxtTypeSignedPlain {
			key = s.Node.Identity().PublicKeyBytes()
		}
		crc := meshcore.CalcAckHash(plain[:offset+len(text)], key)
		extra = make([]byte, 4)
		put32(extra, crc)
		if typ == protocol.TxtTypePlain {
			var nonce [1]byte
			if _, err := rand.Read(nonce[:]); err != nil {
				s.report(err)
				return
			}
			var attempt byte
			if len(plain) > offset+len(text)+1 {
				attempt = plain[offset+len(text)+1]
			}
			extra = append(extra, attempt, nonce[0])
		}
		extraType = meshcore.PayloadTypeAck
	}
	if pkt.IsRouteFlood() {
		s.report(s.returnPath(p, pkt, extraType, extra, true, rx))
	} else if extra != nil {
		delay := 200 * time.Millisecond
		if p.OutPathLen != 255 && s.state.MultiACKs > 0 {
			payload, err := (&meshcore.MultiPart{Remaining: 1, WrappedType: meshcore.PayloadTypeAck, WrappedPayload: extra}).ToBytes()
			if err != nil {
				s.report(err)
				return
			}
			if err = s.sendReply(s.routed(p, meshcore.PayloadTypeMultiPart, payload, false), rx, delay); err != nil {
				s.report(err)
				return
			}
			delay += 300 * time.Millisecond
		}
		s.report(s.sendReply(s.routed(p, meshcore.PayloadTypeAck, extra, false), rx, delay))
	}
}

func (s *Server) returnPath(p *contact, received *meshcore.Packet, typ byte, extra []byte, flood bool, rx policy.ReceiveContext) error {
	plain := append([]byte{received.PathLength}, received.Path...)
	if len(extra) == 0 {
		typ = 0xff
		extra = make([]byte, 4)
		if _, err := rand.Read(extra); err != nil {
			return err
		}
	}
	plain = append(plain, typ)
	plain = append(plain, extra...)
	pkt, err := s.encrypted(p, meshcore.PayloadTypePath, plain, false, flood)
	if err != nil {
		return err
	}
	delay := time.Duration(0)
	if typ == meshcore.PayloadTypeAck {
		delay = 200 * time.Millisecond
	}
	if typ == meshcore.PayloadTypeResponse {
		delay = 300 * time.Millisecond
	}
	return s.sendReply(pkt, rx, delay)
}

func (s *Server) incomingPath(p *contact, pkt *meshcore.Packet, plain []byte, rx policy.ReceiveContext) {
	path, err := meshcore.ParsePathPayload(plain)
	if err != nil {
		return
	}
	if path.ExtraType == meshcore.PayloadTypeResponse && len(path.Extra) > 4 {
		tag := u32(path.Extra)
		if req, ok := s.pending[tag]; ok && req.Key == p.PublicKey && req.Kind == protocol.CmdSendPathDiscoveryReq {
			out := append([]byte{protocol.PushPathDiscoveryResponse, 0}, p.PublicKey[:6]...)
			out = append(out, path.PathLength)
			out = append(out, path.Path...)
			out = append(out, pkt.PathLength)
			out = append(out, pkt.Path...)
			req.Client.send(out)
			delete(s.pending, tag)
			return
		}
	}
	old, oldModified := *p, s.state.LastModified
	p.OutPathLen = path.PathLength
	p.OutPath = [64]byte{}
	copy(p.OutPath[:], path.Path)
	p.LastModified = s.modified()
	if err := s.save(); err != nil {
		if s.storageFault == nil {
			*p = old
			s.state.LastModified = oldModified
		}
		s.report(err)
		return
	}
	s.hydrateContact(p)
	s.broadcast(append([]byte{protocol.PushPathUpdated}, p.PublicKey[:]...))
	if path.ExtraType == meshcore.PayloadTypeAck && len(path.Extra) >= 4 {
		s.ack(u32(path.Extra))
	}
	if path.ExtraType == meshcore.PayloadTypeResponse && len(path.Extra) >= 4 {
		s.response(p, path.Extra)
	}
	if pkt.IsRouteFlood() {
		s.report(s.returnPath(p, pkt, 0, nil, false, rx))
	}
}

func (s *Server) ack(crc uint32) {
	if at, ok := s.acks[crc]; ok {
		delete(s.acks, crc)
		out := make([]byte, 9)
		out[0] = protocol.PushSendConfirmed
		put32(out[1:5], crc)
		put32(out[5:9], uint32(time.Since(at).Milliseconds()))
		s.broadcast(out)
	}
	for key, conn := range s.connections {
		if conn.ACK == crc {
			conn.ACK = 0
			s.active(key)
		}
	}
}

func (s *Server) response(p *contact, data []byte) {
	if len(data) < 5 {
		return
	}
	responseTag := u32(data)
	tag := responseTag
	now := time.Now()
	req, ok := s.pending[tag]
	if ok && !now.Before(req.Expires) {
		delete(s.pending, tag)
		return
	}
	if !ok {
		// Login responses can use server time, and legacy status replies can
		// lack the request tag. Neither can be assigned to a client when more
		// than one request to this peer is outstanding.
		for t, candidate := range s.pending {
			if candidate.Key != p.PublicKey || !now.Before(candidate.Expires) {
				continue
			}
			if ok {
				return
			}
			tag, req, ok = t, candidate, true
		}
		if !ok || (req.Kind != protocol.CmdSendLogin && req.Kind != protocol.CmdSendStatusReq) {
			return
		}
		if fence, fenced := s.loginFences[responseTag]; fenced && fence.Key == p.PublicKey && now.Before(fence.Expires) {
			// Native server time can equal a retired request tag. Only the
			// sole current login can accept that untagged native login format.
			if req.Kind != protocol.CmdSendLogin || len(data) < 13 || data[4] != 0 {
				return
			}
		}
	}
	if !ok || req.Key != p.PublicKey {
		return
	}
	delete(s.pending, tag)
	out := append([]byte{0, 0}, p.PublicKey[:6]...)
	switch req.Kind {
	case protocol.CmdSendLogin:
		out[0] = protocol.PushLoginFail
		if len(data) >= 6 && bytes.Equal(data[4:6], []byte("OK")) {
			out[0] = protocol.PushLoginSuccess
		} else if len(data) >= 13 && data[4] == 0 {
			out[0], out[1] = protocol.PushLoginSuccess, data[6]
			out = append(out, data[:4]...)
			out = append(out, data[7], data[12])
			if data[5] != 0 {
				interval := time.Duration(data[5]) * 16 * time.Second
				conn := s.connections[p.PublicKey]
				if conn == nil {
					conn = &connection{Owners: make(map[*session]struct{})}
				}
				conn.Interval, conn.LastActivity, conn.NextPing = interval, time.Now(), time.Now().Add(interval)
				conn.Owners[req.Client] = struct{}{}
				s.connections[p.PublicKey] = conn
			}
		}
	case protocol.CmdSendStatusReq:
		out[0] = protocol.PushStatusResponse
		out = append(out, data[4:]...)
	case protocol.CmdSendTelemetryReq:
		out[0] = protocol.PushTelemetryResponse
		out = append(out, data[4:]...)
	case protocol.CmdSendBinaryReq, protocol.CmdSendAnonReq:
		out = make([]byte, 6)
		out[0] = protocol.PushBinaryResponse
		put32(out[2:], tag)
		out = append(out, data[4:]...)
	default:
		return
	}
	if len(out) > protocol.MaxFrameSize {
		s.report(fmt.Errorf("remote response exceeds companion frame: %d", len(out)))
		req.Client.close()
		return
	}
	if out[0] == protocol.PushLoginSuccess {
		s.broadcast(out)
		return
	}
	req.Client.send(out)
}

func (s *Server) active(key [32]byte) {
	if conn := s.connections[key]; conn != nil {
		conn.LastActivity = time.Now()
		conn.NextPing = time.Now().Add(conn.Interval)
	}
}

func (s *Server) keepAlive(now time.Time) {
	for key, conn := range s.connections {
		if now.Sub(conn.LastActivity) >= conn.Interval*5/2 {
			delete(s.connections, key)
			continue
		}
		if now.Before(conn.NextPing) {
			continue
		}
		p := s.findContact(key[:])
		if p == nil {
			delete(s.connections, key)
			continue
		}
		conn.NextPing = now.Add(conn.Interval)
		if p.OutPathLen == 255 {
			continue
		}
		plain := make([]byte, 9)
		put32(plain, s.uniqueTag())
		plain[4] = 2
		put32(plain[5:], p.SyncSince)
		pkt, err := s.encrypted(p, meshcore.PayloadTypeReq, plain, false, false)
		if err == nil {
			err = s.sendOrigin(pkt, 0)
		}
		if err != nil {
			s.report(err)
			continue
		}
		conn.ACK = meshcore.CalcAckHash(plain, s.Node.Identity().PublicKeyBytes())
	}
}
