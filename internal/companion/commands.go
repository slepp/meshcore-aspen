package companion

import (
	"bytes"
	"context"
	"encoding/json"
	"errors"
	"math"
	"time"
	"unicode/utf8"

	meshcore "github.com/meshcore-go/meshcore-go"
	protocol "github.com/meshcore-go/meshcore-go/companion"
	"meshcore.local/meshcore/internal/buildinfo"
	"meshcore.local/meshcore/internal/policy"
)

func (s *Server) command(c *session, b []byte) {
	if s.storageFault != nil {
		c.send([]byte{protocol.RespErr, protocol.ErrCodeFileIoError})
		return
	}
	var replies [][]byte
	var events [][]byte
	var afterCommit []func()
	var applySource func() error
	reply := func(b []byte) { replies = append(replies, b) }
	fail := func(code byte) { replies = [][]byte{{protocol.RespErr, code}} }
	ok := func() { reply([]byte{protocol.RespOk}) }
	valid := func(condition bool) bool {
		if !condition {
			fail(protocol.ErrCodeIllegalArg)
		}
		return condition
	}
	var before []byte
	mutate := func() {
		if before == nil {
			before, _ = json.Marshal(s.state)
		}
	}
	defer func() {
		if before != nil {
			if err := s.save(); err != nil {
				if s.storageFault == nil {
					_ = json.Unmarshal(before, &s.state)
				}
				fail(protocol.ErrCodeFileIoError)
				events = nil
				afterCommit = nil
				s.report(err)
			} else if applySource != nil {
				if err := applySource(); err != nil {
					s.failClosed(err)
					fail(protocol.ErrCodeBadState)
					events = nil
					afterCommit = nil
					s.report(err)
				}
			}
			s.hydrate()
			s.publishPreferences()
		}
		for _, action := range afterCommit {
			action()
		}
		for _, frame := range replies {
			c.send(frame)
		}
		for _, frame := range events {
			s.broadcast(frame)
		}
	}()
	find := func(key []byte) *contact {
		p := s.findContact(key)
		if p == nil {
			fail(protocol.ErrCodeNotFound)
		}
		return p
	}
	sent := func(err error) bool {
		if err != nil {
			s.report(err)
			fail(protocol.ErrCodeBadState)
			return false
		}
		return true
	}
	switch b[0] {
	case protocol.CmdDeviceQuery:
		if !valid(len(b) == 2) {
			return
		}
		c.version = b[1]
		out := make([]byte, 82)
		out[0], out[1], out[2], out[3] = protocol.RespDeviceInfo, 13, maxContacts/2, maxChannels
		copy(out[20:60], "MeshCore Go TCP host")
		copy(out[60:80], buildinfo.HostVersion)
		if s.state.Preferences.Repeat {
			out[80] = 1
		}
		out[81] = byte(s.state.Preferences.PathHashMode)
		reply(out)
	case protocol.CmdAppStart:
		if !valid(len(b) >= 8) {
			return
		}
		radio, power := s.phy()
		out := make([]byte, 58+len(s.state.Name))
		out[0], out[1], out[2], out[3] = protocol.RespSelfInfo, meshcore.AdvertTypeChat, byte(power), byte(power)
		key := s.Node.Identity().PublicKey()
		copy(out[4:36], key[:])
		put32(out[36:40], uint32(s.state.Latitude))
		put32(out[40:44], uint32(s.state.Longitude))
		out[45] = s.state.AdvertLocationPolicy
		out[44], out[46] = s.state.MultiACKs, s.state.TelemetryModes
		out[47] = s.state.ManualAdd
		put32(out[48:52], radio.FreqHz/1000)
		put32(out[52:56], radio.BwHz)
		out[56], out[57] = radio.SF, radio.CR
		copy(out[58:], s.state.Name)
		reply(out)
		if c.cursor < s.state.Sequence {
			reply([]byte{protocol.PushMsgWaiting})
		}
	case protocol.CmdGetDeviceTime:
		if !valid(len(b) == 1) {
			return
		}
		out := make([]byte, 5)
		out[0] = protocol.RespCurrTime
		put32(out[1:], s.now())
		reply(out)
	case protocol.CmdSetDeviceTime:
		if !valid(len(b) == 5 && u32(b[1:]) >= s.now()) {
			return
		}
		mutate()
		s.state.ClockOffset = int64(u32(b[1:])) - time.Now().Unix()
		ok()
	case protocol.CmdSetAdvertName:
		if !valid(len(b) >= 2 && len(b) <= 33 && !bytes.ContainsRune(b[1:], 0) &&
			utf8.Valid(b[1:])) {
			return
		}
		mutate()
		s.state.Name = meshcore.TruncateUTF8(string(b[1:]), 31)
		ok()
	case protocol.CmdSetAdvertLatLon:
		if !valid(len(b) == 9) {
			return
		}
		lat, lon := int32(u32(b[1:5])), int32(u32(b[5:9]))
		if !valid(lat >= -90000000 && lat <= 90000000 && lon >= -180000000 && lon <= 180000000) {
			return
		}
		mutate()
		s.state.Latitude, s.state.Longitude = lat, lon
		ok()
	case protocol.CmdSetRadioParams:
		if !valid(len(b) == 11 || len(b) == 12) {
			return
		}
		radio, _ := s.phy()
		if u32(b[1:5]) != radio.FreqHz/1000 || u32(b[5:9]) != radio.BwHz ||
			b[9] != radio.SF || b[10] != radio.CR {
			fail(protocol.ErrCodeUnsupportedCmd)
			return
		}
		repeat := len(b) == 12 && b[11] != 0
		if repeat && !valid(allowedRepeatFrequency(u32(b[1:5]))) {
			return
		}
		mutate()
		s.state.Preferences.Repeat = repeat
		ok()
	case protocol.CmdSetRadioTxPower:
		if !valid(len(b) == 2) {
			return
		}
		if _, power := s.phy(); int8(b[1]) != power {
			fail(protocol.ErrCodeUnsupportedCmd)
			return
		}
		ok()
	case protocol.CmdSendSelfAdvert:
		if !valid(len(b) == 1 || (len(b) == 2 && b[1] <= 1)) {
			return
		}
		if sent(s.sendAdvert(len(b) == 2 && b[1] == 1)) {
			ok()
		}
	case protocol.CmdGetContacts:
		if !valid(len(b) == 1 || len(b) == 5) {
			return
		}
		var since uint32
		if len(b) == 5 {
			since = u32(b[1:])
		}
		var contacts [][]byte
		for i := range s.state.Contacts {
			p := &s.state.Contacts[i]
			if len(b) == 1 || p.LastModified > since {
				contacts = append(contacts, contactFrame(protocol.RespContact, p))
			}
		}
		start := make([]byte, 5)
		start[0] = protocol.RespContactsStart
		put32(start[1:], uint32(len(s.state.Contacts)))
		reply(start)
		for _, f := range contacts {
			reply(f)
		}
		end := make([]byte, 5)
		end[0] = protocol.RespEndOfContacts
		put32(end[1:], s.state.LastModified)
		reply(end)
	case protocol.CmdAddUpdateContact:
		if !valid(len(b) >= 36) {
			return
		}
		size, count, good := decodePath(b[35])
		if !valid(good && (b[35] == 255 || len(b) >= 36+size*count) && b[33] <= meshcore.AdvertTypeSensor) {
			return
		}
		p := s.findContact(b[1:33])
		permanent := -1
		for i := range s.state.Contacts {
			if bytes.Equal(s.state.Contacts[i].PublicKey[:], b[1:33]) {
				permanent = i
				break
			}
		}
		if permanent < 0 && b[33] != 0 && len(s.state.Contacts) == maxContacts {
			fail(protocol.ErrCodeTableFull)
			return
		}
		mutate()
		updated := contact{}
		if p != nil {
			updated = *p
		}
		p = &updated
		copy(p.PublicKey[:], b[1:33])
		p.Type, p.Flags, p.OutPathLen = b[33], b[34], b[35]
		p.OutPath = [64]byte{}
		copy(p.OutPath[:], b[36:min(len(b), 100)])
		if len(b) >= 132 {
			p.AdvertName = cstring(b[100:132])
		}
		if len(b) >= 136 {
			p.LastAdvert = u32(b[132:136])
		}
		if len(b) >= 144 {
			p.AdvertLatitude = int32(u32(b[136:140]))
			p.AdvertLongitude = int32(u32(b[140:144]))
		}
		p.LastModified = s.modified()
		if p.Type == 0 {
			if permanent >= 0 {
				s.state.Contacts = append(s.state.Contacts[:permanent], s.state.Contacts[permanent+1:]...)
			}
			afterCommit = append(afterCommit, func() { s.addTransient(p) })
		} else {
			if permanent >= 0 {
				s.state.Contacts[permanent] = *p
			} else {
				s.state.Contacts = append(s.state.Contacts, *p)
			}
			afterCommit = append(afterCommit, func() { delete(s.transient, p.PublicKey); s.hydrateContact(p) })
		}
		ok()
	case protocol.CmdGetContactByKey, protocol.CmdResetPath, protocol.CmdRemoveContact, protocol.CmdShareContact:
		if !valid(len(b) == 33 || (b[0] == protocol.CmdRemoveContact && len(b) == 7)) {
			return
		}
		p := find(b[1:])
		if p == nil {
			return
		}
		switch b[0] {
		case protocol.CmdGetContactByKey:
			reply(contactFrame(protocol.RespContact, p))
		case protocol.CmdResetPath:
			mutate()
			if p.Type == 0 {
				updated := *p
				p = &updated
				afterCommit = append(afterCommit, func() { s.addTransient(p) })
			}
			p.OutPathLen = 255
			p.OutPath = [64]byte{}
			p.LastModified = s.modified()
			events = append(events, append([]byte{protocol.PushPathUpdated}, p.PublicKey[:]...))
			ok()
		case protocol.CmdRemoveContact:
			mutate()
			key := p.PublicKey
			for i := range s.state.Contacts {
				if s.state.Contacts[i].PublicKey == key {
					s.state.Contacts = append(s.state.Contacts[:i], s.state.Contacts[i+1:]...)
					break
				}
			}
			afterCommit = append(afterCommit, func() { delete(s.connections, key); delete(s.transient, key); s.Node.Peers().Remove(key) })
			s.modified()
			events = append(events, append([]byte{protocol.PushContactDeleted}, key[:]...))
			ok()
		case protocol.CmdShareContact:
			if len(p.Advert) == 0 {
				fail(protocol.ErrCodeNotFound)
				return
			}
			pkt, err := meshcore.PacketFromBytes(p.Advert)
			if err != nil {
				fail(protocol.ErrCodeBadState)
				return
			}
			pkt.Header = meshcore.MakeHeader(meshcore.RouteTypeDirect, meshcore.PayloadTypeAdvert, 0)
			pkt.Path = nil
			pkt.PathLength = 0
			if sent(s.sendForSession(c, pkt, 0)) {
				ok()
			}
		}
	case protocol.CmdExportContact:
		if !valid(len(b) == 1 || len(b) == 33) {
			return
		}
		var data []byte
		if len(b) == 1 || bytes.Equal(b[1:], s.Node.Identity().PublicKeyBytes()) {
			pkt, err := s.selfAdvert()
			if !sent(err) {
				return
			}
			data, err = pkt.ToBytes()
			if !sent(err) {
				return
			}
		} else {
			p := find(b[1:])
			if p == nil {
				return
			}
			data = p.Advert
		}
		if len(data) == 0 {
			fail(protocol.ErrCodeNotFound)
			return
		}
		reply(append([]byte{protocol.RespExportContact}, data...))
	case protocol.CmdImportContact:
		pkt, err := meshcore.PacketFromBytes(b[1:])
		if !valid(err == nil && pkt.PayloadType() == meshcore.PayloadTypeAdvert) {
			return
		}
		adv, err := meshcore.AdvertFromBytes(pkt.Payload)
		if !valid(err == nil && adv.Verify() && !adv.PublicKey.Matches(s.Node.Identity().Identity) && cstring([]byte(adv.AppData().Name)) != "") {
			return
		}
		existing := s.findContact(adv.PublicKey.PublicKeyBytes())
		if (existing == nil || existing.Type == 0) && len(s.state.Contacts) == maxContacts {
			fail(protocol.ErrCodeTableFull)
			return
		}
		mutate()
		p, _ := s.storeAdvert(pkt, adv, true)
		afterCommit = append(afterCommit, func() { delete(s.transient, p.PublicKey); s.hydrateContact(p) })
		events = append(events, contactFrame(protocol.PushNewAdvert, p))
		ok()
	case protocol.CmdGetChannel:
		if !valid(len(b) == 2 && b[1] < maxChannels) {
			return
		}
		out := make([]byte, 50)
		out[0], out[1] = protocol.RespChannelInfo, b[1]
		if ch := s.state.Channels[b[1]]; ch != nil {
			copy(out[2:34], ch.Name)
			copy(out[34:], ch.PSK[:])
		}
		reply(out)
	case protocol.CmdSetChannel:
		if !valid(len(b) == 50 && b[1] < maxChannels) {
			return
		}
		ch, err := meshcore.NewChannelFromPSK(cstring(b[2:34]), b[34:])
		if !sent(err) {
			return
		}
		mutate()
		s.state.Channels[b[1]] = ch
		ok()
	case protocol.CmdSyncNextMessage:
		if !valid(len(b) == 1) {
			return
		}
		for _, m := range s.state.Messages {
			if m.Sequence <= c.cursor {
				continue
			}
			frame := m.Frame
			if c.version < 3 && (frame[0] == protocol.RespContactMsgRecvV3 || frame[0] == protocol.RespChannelMsgRecvV3) {
				frame = append([]byte{frame[0] - 9}, frame[4:]...)
			}
			reply(frame)
			sequence := m.Sequence
			if s.state.Retention == NativeQueue {
				minimum := sequence
				for other := range s.clients {
					if other != c && other.cursor < minimum {
						minimum = other.cursor
					}
				}
				first := 0
				for first < len(s.state.Messages) && s.state.Messages[first].Sequence <= minimum {
					first++
				}
				if first > 0 {
					mutate()
					s.state.Messages = append([]message(nil), s.state.Messages[first:]...)
				}
			}
			afterCommit = append(afterCommit, func() { c.cursor = sequence })
			return
		}
		reply([]byte{protocol.RespNoMoreMessages})
	case protocol.CmdSendTxtMsg:
		if !valid(len(b) >= 14 && b[1] <= protocol.TxtTypeCLIData && !bytes.ContainsRune(b[13:], 0)) {
			return
		}
		p := find(b[7:13])
		if p == nil {
			return
		}
		frame, err := s.sendText(c, p, b[1], b[2], u32(b[3:7]), b[13:])
		if sent(err) {
			reply(frame)
		}
	case protocol.CmdSendChannelTxtMsg:
		if !valid(len(b) >= 8 && b[1] == protocol.TxtTypePlain && b[2] < maxChannels && !bytes.ContainsRune(b[7:], 0)) {
			return
		}
		ch := s.Node.Channel(int(b[2]))
		if ch == nil {
			fail(protocol.ErrCodeNotFound)
			return
		}
		group, err := (&meshcore.GroupTextPayload{Timestamp: u32(b[3:7]), Sender: s.state.Name, Text: string(b[7:])}).Encrypt(ch.Hash, ch.PSK[:])
		if !sent(err) {
			return
		}
		data, err := group.ToBytes()
		if !sent(err) {
			return
		}
		pkt := &meshcore.Packet{Header: meshcore.MakeHeader(meshcore.RouteTypeFlood, meshcore.PayloadTypeGrpTxt, 0), Payload: data}
		if sent(s.sendForSession(c, pkt, 0)) {
			ok()
		}
	case protocol.CmdSendLogin, protocol.CmdSendStatusReq, protocol.CmdSendTelemetryReq, protocol.CmdSendBinaryReq, protocol.CmdSendPathDiscoveryReq, protocol.CmdSendAnonReq:
		offset := 1
		if b[0] == protocol.CmdSendPathDiscoveryReq {
			offset = 2
		}
		if b[0] == protocol.CmdSendTelemetryReq {
			if len(b) == 4 {
				frame, err := s.selfTelemetry()
				if sent(err) {
					reply(frame)
				}
				return
			}
			offset = 4
		}
		if !valid(len(b) >= offset+32) {
			return
		}
		if b[0] == protocol.CmdSendPathDiscoveryReq && !valid(b[1] == 0) {
			return
		}
		p := find(b[offset : offset+32])
		if p == nil && b[0] == protocol.CmdSendAnonReq {
			replies = nil
			p = &contact{ContactResponse: protocol.ContactResponse{Type: 0, OutPathLen: 0, LastModified: s.now()}}
			copy(p.PublicKey[:], b[offset:offset+32])
		}
		if p == nil {
			return
		}
		if len(s.pending) >= maxPendingRequests {
			_, replacingLogin := s.pendingLogin(p.PublicKey)
			if b[0] != protocol.CmdSendLogin || !replacingLogin {
				fail(protocol.ErrCodeTableFull)
				return
			}
		}
		frame, err := s.sendRequest(c, p, b[0], b[offset+32:])
		if sent(err) {
			if b[0] == protocol.CmdSendAnonReq && p.Type == 0 {
				s.addTransient(p)
			}
			reply(frame)
		}
	case protocol.CmdHasConnection, protocol.CmdLogout:
		if !valid(len(b) == 33) {
			return
		}
		var key [32]byte
		copy(key[:], b[1:])
		if b[0] == protocol.CmdLogout {
			if conn := s.connections[key]; conn != nil {
				delete(conn.Owners, c)
				if len(conn.Owners) == 0 {
					delete(s.connections, key)
				}
			}
			ok()
		} else if s.connections[key] != nil {
			ok()
		} else {
			fail(protocol.ErrCodeNotFound)
		}
	case protocol.CmdGetAdvertPath:
		if !valid(len(b) == 34 && b[1] == 0) {
			return
		}
		var key [32]byte
		copy(key[:], b[2:34])
		path, found := s.advertPaths[key]
		if !found {
			p := s.findContact(key[:])
			if p != nil && p.HeardAt != 0 {
				path = advertPath{Path: p.HeardPath, Encoded: p.HeardPathLen, At: p.HeardAt}
				found = true
			}
		}
		if !found {
			fail(protocol.ErrCodeNotFound)
			return
		}
		out := make([]byte, 6+len(path.Path))
		out[0] = protocol.RespAdvertPath
		put32(out[1:5], path.At)
		out[5] = path.Encoded
		copy(out[6:], path.Path)
		reply(out)
	case protocol.CmdGetCustomVars:
		if !valid(len(b) == 1) {
			return
		}
		reply([]byte{protocol.RespCustomVars})
	case protocol.CmdSetCustomVar:
		fail(protocol.ErrCodeIllegalArg)
	case protocol.CmdSetDevicePin:
		if !valid(len(b) == 5 && (u32(b[1:]) == 0 || (u32(b[1:]) >= 100000 && u32(b[1:]) <= 999999))) {
			return
		}
		reply([]byte{protocol.RespDisabled})
	case protocol.CmdExportPrivateKey:
		if !valid(len(b) == 1) {
			return
		}
		if s.cfg.ExportIdentity == nil {
			reply([]byte{protocol.RespDisabled})
			return
		}
		ctx, cancel := context.WithTimeout(s.ctx, 3*time.Second)
		key, err := s.cfg.ExportIdentity(ctx)
		cancel()
		if !sent(err) {
			return
		}
		id, err := meshcore.NewLocalIdentityFromExpandedKey(key)
		if !sent(err) {
			return
		}
		if id.PublicKey() != s.Node.Identity().PublicKey() {
			sent(errors.New("identity exporter does not match the active companion identity"))
			return
		}
		reply(append([]byte{protocol.RespPrivateKey}, key...))
	case protocol.CmdImportPrivateKey:
		if !valid(len(b) >= 65) {
			return
		}
		if s.cfg.ImportIdentity == nil {
			reply([]byte{protocol.RespDisabled})
			return
		}
		if code, err := s.importIdentity(c, b[1:65]); err != nil {
			s.report(err)
			fail(code)
			return
		}
		ok()
	case protocol.CmdReboot, protocol.CmdFactoryReset:
		marker, request := "reboot", s.cfg.RequestRestart
		if b[0] == protocol.CmdFactoryReset {
			marker, request = "reset", s.cfg.RequestFactoryReset
		}
		if !valid(bytes.HasPrefix(b[1:], []byte(marker))) {
			return
		}
		if request == nil {
			fail(protocol.ErrCodeUnsupportedCmd)
			return
		}
		ctx, cancel := context.WithTimeout(s.ctx, 3*time.Second)
		defer cancel()
		sent(request(ctx))
	case protocol.CmdSetOtherParams:
		if !valid(len(b) >= 2 && len(b) <= 5) {
			return
		}
		if len(b) >= 4 && !valid(b[3] <= 1) {
			return
		}
		mutate()
		s.state.ManualAdd = b[1]
		if len(b) >= 3 {
			s.state.TelemetryModes = b[2] & 63
		}
		if len(b) == 5 {
			s.state.MultiACKs = b[4]
		}
		if len(b) >= 4 {
			s.state.AdvertLocationPolicy = b[3]
		}
		ok()
	case protocol.CmdSignStart:
		if !valid(len(b) == 1) {
			return
		}
		c.signData = make([]byte, 0, 8192)
		out := make([]byte, 6)
		out[0] = protocol.RespSignStart
		put32(out[2:], 8192)
		reply(out)
	case protocol.CmdSignData:
		if c.signData == nil {
			fail(protocol.ErrCodeBadState)
			return
		}
		if len(c.signData)+len(b)-1 > 8192 {
			fail(protocol.ErrCodeTableFull)
			return
		}
		c.signData = append(c.signData, b[1:]...)
		ok()
	case protocol.CmdSignFinish:
		if !valid(len(b) == 1) {
			return
		}
		if c.signData == nil {
			fail(protocol.ErrCodeBadState)
			return
		}
		reply(append([]byte{protocol.RespSignature}, s.Node.Identity().Sign(c.signData)...))
		c.signData = nil
	case protocol.CmdGetBattAndStorage, protocol.CmdGetStats:
		frame, code, err := s.telemetry(b)
		if err != nil {
			s.report(err)
		}
		if code != 0 {
			fail(code)
		} else {
			reply(frame)
		}
	case protocol.CmdSetAutoAddConfig:
		if !valid(len(b) == 2 || len(b) == 3) {
			return
		}
		mutate()
		s.state.AutoAddConfig = b[1]
		if len(b) == 3 {
			s.state.AutoAddMaxHops = min(b[2], 64)
		}
		ok()
	case protocol.CmdGetAutoAddConfig:
		if !valid(len(b) == 1) {
			return
		}
		reply([]byte{protocol.RespAutoAddConfig, s.state.AutoAddConfig, s.state.AutoAddMaxHops})
	case protocol.CmdSetPathHashMode:
		if !valid(len(b) == 3 && b[1] == 0) {
			return
		}
		mode := policy.PathHashMode(b[2])
		if _, err := mode.Width(); !valid(err == nil) {
			return
		}
		mutate()
		s.state.Preferences.PathHashMode = mode
		ok()
	case protocol.CmdSetFloodScopeKey:
		if !valid(len(b) == 2 || len(b) == 18) {
			return
		}
		if !valid(b[1] <= 1) {
			return
		}
		c.scope = policy.SendScope{Unscoped: b[1] == 1}
		if b[1] == 0 && len(b) == 18 {
			copy(c.scope.Override.Key[:], b[2:])
		}
		ok()
	case protocol.CmdSetDefaultFloodScope:
		if !valid(len(b) == 1 || len(b) == 48) {
			return
		}
		scope := policy.Scope{}
		if len(b) == 48 {
			scope.Name = cstring(b[1:32])
			if !valid(len(scope.Name) > 0 && len(scope.Name) <= 30) {
				return
			}
			copy(scope.Key[:], b[32:48])
		}
		mutate()
		s.state.Preferences.DefaultScope = scope
		ok()
	case protocol.CmdGetDefaultFloodScope:
		if !valid(len(b) == 1) {
			return
		}
		scope := s.state.Preferences.DefaultScope
		out := []byte{protocol.RespDefaultFloodScope}
		if scope.Name != "" {
			out = make([]byte, 48)
			out[0] = protocol.RespDefaultFloodScope
			copy(out[1:32], scope.Name)
			copy(out[32:], scope.Key[:])
		}
		reply(out)
	case protocol.CmdGetAllowedRepeatFreq:
		if !valid(len(b) == 1) {
			return
		}
		out := make([]byte, 25)
		out[0] = protocol.RespAllowedRepeatFreq
		for i, freq := range []uint32{433000, 869495, 918000} {
			put32(out[1+i*8:], freq)
			put32(out[5+i*8:], freq)
		}
		reply(out)
	case protocol.CmdGetTuningParams:
		if !valid(len(b) == 1) {
			return
		}
		out := make([]byte, 9)
		out[0] = protocol.RespTuningParams
		for i, value := range []float32{s.state.Preferences.RXDelay, s.state.Preferences.AirtimeFactor} {
			scaled := value * 1000
			if math.IsNaN(float64(scaled)) || scaled < 0 || float64(scaled) > math.MaxUint32 {
				sent(errors.New("tuning value cannot be represented as uint32 thousandths"))
				return
			}
			put32(out[1+i*4:], uint32(scaled))
		}
		reply(out)
	case protocol.CmdSetTuningParams:
		if !valid(len(b) == 9) {
			return
		}
		prefs := s.state.Preferences
		prefs.RXDelay = float32(u32(b[1:5])) / 1000
		prefs.AirtimeFactor = float32(u32(b[5:9])) / 1000
		if !valid(policy.Validate(policy.Companion, prefs) == nil) {
			return
		}
		if prefs.AirtimeFactor != s.state.Preferences.AirtimeFactor {
			setter, hasSetter := s.Node.Radio().(sourceAirtime)
			if !hasSetter {
				fail(protocol.ErrCodeUnsupportedCmd)
				return
			}
			applySource = func() error {
				ctx, cancel := context.WithTimeout(s.ctx, 3*time.Second)
				defer cancel()
				return setter.SetSourceAirtimeFactor(ctx, float64(prefs.AirtimeFactor))
			}
		}
		mutate()
		s.state.Preferences = prefs
		ok()
	default:
		if frame, handled := s.packetCommand(c, b); handled {
			reply(frame)
			return
		}
		fail(protocol.ErrCodeUnsupportedCmd)
	}

}

func allowedRepeatFrequency(frequency uint32) bool {
	return frequency == 433000 || frequency == 869495 || frequency == 918000
}

func cstring(b []byte) string {
	if i := bytes.IndexByte(b, 0); i >= 0 {
		b = b[:i]
	}
	return string(b)
}

func (s *Server) findContact(prefix []byte) *contact {
	if len(prefix) == 0 || len(prefix) > 32 {
		return nil
	}
	var found *contact
	for i := range s.state.Contacts {
		p := &s.state.Contacts[i]
		if bytes.Equal(p.PublicKey[:len(prefix)], prefix) {
			if found != nil {
				return nil
			}
			found = p
		}
	}
	for _, p := range s.transient {
		if bytes.Equal(p.PublicKey[:len(prefix)], prefix) {
			if found != nil {
				return nil
			}
			found = p
		}
	}
	return found
}

func (s *Server) addTransient(p *contact) {
	if _, ok := s.transient[p.PublicKey]; !ok && len(s.transient) >= 8 {
		var oldest *contact
		for _, c := range s.transient {
			if oldest == nil || c.LastModified < oldest.LastModified {
				oldest = c
			}
		}
		delete(s.transient, oldest.PublicKey)
		if s.Node != nil {
			s.Node.Peers().Remove(oldest.PublicKey)
		}
	}
	s.transient[p.PublicKey] = p
	if s.Node != nil {
		s.hydrateContact(p)
	}
}

func contactFrame(code byte, p *contact) []byte {
	b := make([]byte, 148)
	b[0] = code
	copy(b[1:33], p.PublicKey[:])
	b[33], b[34], b[35] = p.Type, p.Flags, p.OutPathLen
	copy(b[36:100], p.OutPath[:])
	copy(b[100:132], p.AdvertName)
	put32(b[132:136], p.LastAdvert)
	put32(b[136:140], uint32(p.AdvertLatitude))
	put32(b[140:144], uint32(p.AdvertLongitude))
	put32(b[144:], p.LastModified)
	return b
}
