package roles

import (
	"bytes"
	"crypto/subtle"
	"encoding/binary"
	"errors"
	"fmt"
	"log/slog"
	"sort"
	"time"

	meshcore "github.com/meshcore-go/meshcore-go"
	"meshcore.local/meshcore/internal/buildinfo"
)

func u32(b []byte) uint32      { return binary.LittleEndian.Uint32(b) }
func put32(b []byte, n uint32) { binary.LittleEndian.PutUint32(b, n) }
func cstring(b []byte) []byte {
	if end := bytes.IndexByte(b, 0); end >= 0 {
		return b[:end]
	}
	return b
}

func (s *Service) uniqueTime() uint32 {
	now := s.now()
	if now <= s.state.Clock {
		now = s.state.Clock + 1
	}
	s.state.Clock = now
	return now
}

func (s *Service) now() uint32 { return uint32(time.Now().Unix() + s.state.RTCOffset) }

type transmission struct {
	*meshcore.Packet
	delay time.Duration
}

func (s *Service) handle(e event) ([]transmission, bool, error) {
	if e.ownerCommand != nil {
		if err := e.ownerCommand.ctx.Err(); err != nil {
			return nil, false, err
		}
		s.commandOwner = "host-owner-console"
		defer func() { s.commandOwner = "" }()
		reply, packets, err := s.commandWithTelemetry(0, e.ownerCommand.command, e.telemetry, e.telemetryErr)
		e.ownerCommand.reply = reply
		return packets, e.ownerCommand.mutates, err
	}
	switch e.packet.PayloadType() {
	case meshcore.PayloadTypeAdvert:
		s.observeNeighbour(e.packet)
		return nil, false, nil
	case meshcore.PayloadTypeControl:
		return s.control(e.packet)
	case meshcore.PayloadTypeAnonReq:
		return s.login(e)
	case meshcore.PayloadTypeAck, meshcore.PayloadTypeMultiPart:
		return nil, s.acknowledge(u32(e.plain)), nil
	}
	m := s.state.Members[e.key]
	if m == nil {
		return nil, false, nil
	}
	if e.packet.PayloadType() == meshcore.PayloadTypePath {
		path, err := meshcore.ParsePathPayload(e.plain)
		if err != nil {
			return nil, false, nil
		}
		m.Path, m.PathLength, m.KnownPath = bytes.Clone(path.Path), path.PathLength, true
		m.LastActivity = s.now()
		m.Active = m.LastActivity != 0
		if path.ExtraType == meshcore.PayloadTypeAck && len(path.Extra) >= 4 {
			s.acknowledge(u32(path.Extra))
		}
		return nil, true, nil
	}
	if len(e.plain) < 5 {
		return nil, false, nil
	}
	timestamp := u32(e.plain)
	if timestamp < m.LastTimestamp {
		return nil, false, nil
	}
	retry := timestamp == m.LastTimestamp
	switch e.packet.PayloadType() {
	case meshcore.PayloadTypeTxtMsg:
		return s.text(e, m, timestamp, retry)
	case meshcore.PayloadTypeReq:
		if !s.room && retry {
			return nil, false, nil
		}
		return s.request(e, m, timestamp)
	}
	return nil, false, nil
}

func (s *Service) login(e event) ([]transmission, bool, error) {
	offset := 4
	if s.room {
		offset = 8
	}
	if len(e.plain) <= offset {
		return nil, false, nil
	}
	if !s.room && e.plain[4] > 0 && e.plain[4] < 32 {
		return s.anonymousQuery(e)
	}
	password := cstring(e.plain[offset:])
	adminPassword, guestPassword := s.cfg.AdminPassword, s.cfg.Password
	if s.state.AdminPasswordOverride != nil {
		adminPassword = *s.state.AdminPasswordOverride
	}
	if s.state.GuestPasswordOverride != nil {
		guestPassword = *s.state.GuestPasswordOverride
	}
	m := s.state.Members[e.key]
	perms := byte(0)
	aclLogin := len(password) == 0 && m != nil
	switch {
	case aclLogin:
		perms = m.Permissions
	case adminPassword != "" && subtle.ConstantTimeCompare(password, []byte(adminPassword)) == 1:
		perms = 3
	case subtle.ConstantTimeCompare(password, []byte(guestPassword)) == 1:
		if s.room {
			perms = 2
		}
	case s.room && s.state.AllowReadOnly:
		perms = 0
	default:
		// Firmware deliberately gives no response to an incorrect password.
		return nil, false, nil
	}
	timestamp := u32(e.plain)
	if !aclLogin && (timestamp == 0 || (m != nil && timestamp <= m.LastTimestamp)) {
		return nil, false, nil
	}
	if m == nil {
		id, err := meshcore.NewIdentityFromHex(e.key)
		if err != nil {
			return nil, false, err
		}
		m, err = s.putMember(id)
		if err != nil {
			return nil, false, err
		}
	}
	if !aclLogin {
		m.Permissions = m.Permissions&^3 | perms&3
		m.LastTimestamp, m.LastActivity = timestamp, s.now()
		m.Pending, m.Failures = false, 0
		if s.room {
			m.SyncSince = u32(e.plain[4:])
		}
	}
	m.Active = true
	perms = m.Permissions
	if e.packet.IsRouteFlood() {
		m.KnownPath, m.Path, m.PathLength = false, nil, 0
	}
	reply := make([]byte, 13)
	put32(reply, s.uniqueTime())
	if perms&3 == 3 {
		reply[6] = 1
	} else if s.room && perms == 0 {
		reply[6] = 2
	}
	reply[7], reply[12] = perms, 1
	if !s.room {
		reply[12] = 2
	}
	if _, err := s.random(reply[8:12]); err != nil {
		return nil, false, err
	}
	packet, err := s.response(e, m, reply)
	s.nextPush = time.Now().Add(2 * time.Second)
	return delayed(packet, 300*time.Millisecond), true, err
}

func (s *Service) anonymousQuery(e event) ([]transmission, bool, error) {
	if !e.packet.IsRouteDirect() || len(e.plain) < 6 {
		return nil, false, nil
	}
	length := e.plain[5]
	size := int(length&63) * int((length>>6)+1)
	if !meshcore.IsValidPathLen(length) || len(e.plain) < 6+size {
		return nil, false, nil
	}
	id, err := meshcore.NewIdentityFromHex(e.key)
	if err != nil {
		return nil, false, err
	}
	m := &member{Key: id.PublicKey(), Path: bytes.Clone(e.plain[6 : 6+size]), PathLength: length, KnownPath: true}
	reply := make([]byte, 8)
	put32(reply, u32(e.plain))
	put32(reply[4:], s.now())
	switch e.plain[4] {
	case 1:
		reply = append(reply, []byte(regionNames(s.state.Preferences, false, 172))...)
	case 2:
		reply = append(reply, []byte(s.state.Name+"\n"+s.state.Preferences.OwnerInfo)...)
	case 3:
		flags := byte(0)
		if !s.state.Preferences.Repeat {
			flags = 0x80
		}
		reply = append(reply, flags)
	default:
		return nil, false, fmt.Errorf("%w: anonymous query %d", ErrUnsupported, e.plain[4])
	}
	if !s.anonLimit.allow(s.now(), 180, 4) {
		return nil, false, nil
	}
	packet, err := s.datagram(m, meshcore.PayloadTypeResponse, reply)
	return delayed(packet, 300*time.Millisecond), false, err
}

func one(p *meshcore.Packet) []transmission {
	return delayed(p, 0)
}

func delayed(p *meshcore.Packet, delay time.Duration) []transmission {
	if p == nil {
		return nil
	}
	return []transmission{{p, delay}}
}

func (s *Service) datagram(m *member, kind byte, plain []byte) (*meshcore.Packet, error) {
	if len(plain)+2+15 > meshcore.MaxPacketPayload {
		return nil, errors.New("role datagram exceeds native plaintext limit")
	}
	secret, err := s.Node.SharedSecret(meshcore.NewIdentity(m.Key))
	if err != nil {
		return nil, err
	}
	encrypted, err := meshcore.EncryptThenMAC(secret, plain)
	if err != nil {
		return nil, err
	}
	payload := append([]byte{m.Key[0], s.id.PublicKey()[0]}, encrypted...)
	if len(payload) > meshcore.MaxPacketPayload {
		return nil, errors.New("role response exceeds maximum packet payload")
	}
	return s.routed(m, kind, payload), nil
}

func (s *Service) routed(m *member, kind byte, payload []byte) *meshcore.Packet {
	p := &meshcore.Packet{Header: meshcore.MakeHeader(meshcore.RouteTypeFlood, kind, 0), Payload: payload}
	p.PathLength = byte(s.state.Preferences.PathHashMode) << 6
	applyScope(p, s.state.Preferences.DefaultScope)
	if m.KnownPath {
		p.Header = meshcore.MakeHeader(meshcore.RouteTypeDirect, kind, 0)
		p.PathLength, p.Path = m.PathLength, bytes.Clone(m.Path)
	}
	return p
}

func (s *Service) response(in event, m *member, plain []byte) (*meshcore.Packet, error) {
	if !in.packet.IsRouteFlood() {
		p, err := s.datagram(m, meshcore.PayloadTypeResponse, plain)
		return s.prepareReply(p, in.context), err
	}
	if len(in.packet.Path)+len(plain)+5 > meshcore.MaxPacketPayload-2-16 {
		return nil, errors.New("role PATH response exceeds native combined-path limit")
	}
	body := append([]byte{in.packet.PathLength}, in.packet.Path...)
	body = append(body, meshcore.PayloadTypeResponse)
	body = append(body, plain...)
	unknown := *m
	unknown.KnownPath = false
	p, err := s.datagram(&unknown, meshcore.PayloadTypePath, body)
	return s.prepareReply(p, in.context), err
}

func (s *Service) text(e event, m *member, timestamp uint32, retry bool) ([]transmission, bool, error) {
	kind := e.plain[4] >> 2
	text := cstring(e.plain[5:])
	if (len(text) == 0 && !(kind == 1 && s.regionLoad != nil)) || (kind != 0 && kind != 1) {
		return nil, false, nil
	}
	isCommand := !s.room || kind == 1
	if isCommand && m.Permissions&3 != 3 {
		return nil, false, nil
	}
	// Host room policy keeps role 1 read-only; native rooms deny only role 0.
	if !isCommand && m.Permissions&3 < 2 {
		return nil, false, nil
	}
	m.LastTimestamp, m.Active, m.Failures = timestamp, true, 0
	m.LastActivity = s.now()
	if s.room {
		m.LastActivity = s.uniqueTime()
	}
	var packets []transmission
	if kind == 0 {
		ack := make([]byte, 4)
		put32(ack, meshcore.CalcAckHash(e.plain[:5+len(text)], m.Key[:]))
		delay := 200 * time.Millisecond
		if s.room && m.KnownPath && s.state.MultiACKs != 0 {
			packets = append(packets, delayed(s.routed(m, meshcore.PayloadTypeMultiPart, append([]byte{0x13}, ack...)), delay)...)
			delay += 300 * time.Millisecond
		}
		packets = append(packets, delayed(s.prepareReply(s.routed(m, meshcore.PayloadTypeAck, ack), e.context), delay)...)
	}
	if retry {
		return packets, false, nil
	}
	if !isCommand {
		s.addPost(m.Key, string(text))
		return packets, true, nil
	}
	s.commandOwner = e.key
	reply, extra, err := s.commandWithTelemetry(timestamp, string(text), e.telemetry, e.telemetryErr)
	s.commandOwner = ""
	if err != nil {
		return nil, false, err
	}
	packets = append(packets, extra...)
	if reply == "" {
		return packets, true, nil
	}
	response, err := s.cliReply(e, m, timestamp, reply)
	return append(packets, response...), true, err
}

func (s *Service) cliReply(e event, m *member, timestamp uint32, reply string) ([]transmission, error) {
	plain := make([]byte, 5)
	now := m.LastActivity
	if !s.room {
		now = s.uniqueTime()
	}
	if now == timestamp {
		now++
	}
	put32(plain, now)
	plain[4] = 1 << 2
	plain = append(plain, meshcore.TruncateUTF8(reply, 155)...)
	p, err := s.datagram(m, meshcore.PayloadTypeTxtMsg, plain)
	p = s.prepareReply(p, e.context)
	delay := 1500 * time.Millisecond
	if s.room {
		delay = 300 * time.Millisecond
	}
	return delayed(p, delay), err
}

func (s *Service) request(e event, m *member, timestamp uint32) ([]transmission, bool, error) {
	m.LastTimestamp, m.Active, m.Failures = timestamp, true, 0
	m.LastActivity = s.now()
	if s.room && e.plain[4] == 2 && e.packet.IsRouteDirect() {
		data := make([]byte, 9)
		copy(data, e.plain)
		if since := u32(data[5:]); since > 0 {
			m.SyncSince = since
		}
		m.Pending = false
		if !m.KnownPath {
			return nil, true, nil
		}
		ack := make([]byte, 5)
		put32(ack, meshcore.CalcAckHash(data, m.Key[:]))
		for _, p := range s.state.History {
			if p.Timestamp > m.SyncSince && p.Author != m.Key && ack[4] < 255 {
				ack[4]++
			}
		}
		return delayed(s.routed(m, meshcore.PayloadTypeAck, ack), 300*time.Millisecond), true, nil
	}
	reply := make([]byte, 4)
	put32(reply, timestamp)
	switch e.plain[4] {
	case 6:
		if s.room {
			return nil, false, fmt.Errorf("%w: room neighbour request", ErrUnsupported)
		}
		data, err := s.neighbourResponse(e.plain[5:])
		if err != nil {
			return nil, false, err
		}
		reply = append(reply, data...)
	case 7:
		if s.room {
			return nil, false, fmt.Errorf("%w: room owner request", ErrUnsupported)
		}
		header := buildinfo.HostVersion + "\n" + s.state.Name + "\n"
		capacity := meshcore.MaxPacketPayload - 2 - 15 - len(reply)
		if e.packet.IsRouteFlood() {
			capacity = meshcore.MaxPacketPayload - 2 - 16 - len(e.packet.Path) - 5 - len(reply)
		}
		if len(header) > capacity {
			return nil, false, errors.New("owner information header exceeds native response capacity")
		}
		owner := meshcore.TruncateUTF8(s.state.Preferences.OwnerInfo, capacity-len(header))
		if owner != s.state.Preferences.OwnerInfo {
			slog.Warn("Owner information shortened to fit native response", "role", s.state.Name,
				"full_text_command", "get owner.info")
		}
		reply = append(reply, []byte(header+owner)...)
	case 1:
		reply = append(reply, s.status(e.telemetry)...)
	case 3:
		data, err := telemetryPayload(e.telemetry)
		if err != nil {
			return nil, false, err
		}
		reply = append(reply, data...)
	case 5:
		if m.Permissions&3 != 3 || len(e.plain) < 7 || e.plain[5] != 0 || e.plain[6] != 0 {
			return nil, false, nil
		}
		for _, key := range s.memberKeys() {
			c := s.state.Members[key]
			if c.Permissions != 0 && (!s.room || c.Permissions&3 == 3) && len(reply)+7 <= meshcore.MaxPacketPayload-4 {
				reply = append(reply, c.Key[:6]...)
				reply = append(reply, c.Permissions)
			}
		}
	default:
		return nil, false, fmt.Errorf("%w: binary request %d (no wire error response defined)", ErrUnsupported, e.plain[4])
	}
	packet, err := s.response(e, m, reply)
	return delayed(packet, 300*time.Millisecond), true, err
}

func (s *Service) memberKeys() []string {
	keys := make([]string, 0, len(s.state.Members))
	seen := make(map[string]bool, len(s.state.Members))
	for _, key := range s.state.MemberOrder {
		if s.state.Members[key] != nil && !seen[key] {
			keys = append(keys, key)
			seen[key] = true
		}
	}
	var remaining []string
	for k := range s.state.Members {
		if !seen[k] {
			remaining = append(remaining, k)
		}
	}
	sort.Strings(remaining)
	return append(keys, remaining...)
}

func (s *Service) status(telemetry Telemetry) []byte {
	size := 56
	if s.room {
		size = 52
	}
	data := make([]byte, size)
	put16 := func(offset int, n uint16) { binary.LittleEndian.PutUint16(data[offset:], n) }
	if telemetry.HasBattery {
		put16(0, telemetry.BatteryMilliVolts)
	}
	put16(2, uint16(s.Node.TxQueueLen()))
	put16(4, 0x8000) // unavailable noise floor
	if telemetry.HasNoiseFloor {
		put16(4, uint16(telemetry.NoiseFloorDBm))
	}
	put16(6, 0x8000)
	put16(42, 0x8000)
	if s.haveSignal.Load() {
		put16(6, uint16(s.lastRSSI.Load()))
		put16(42, uint16(s.lastSNR.Load()))
	} else if telemetry.HasCurrentRSSI {
		put16(6, uint16(int16(telemetry.CurrentRSSIDBm)))
	}
	put32(data[8:], uint32(s.received.Load()))
	put32(data[12:], uint32(s.sent.Load()))
	if telemetry.HasCounters {
		put32(data[8:], telemetry.Counters.PacketsRecv)
		put32(data[12:], telemetry.Counters.PacketsSent)
		if !s.room {
			put32(data[52:], telemetry.Counters.PacketsErrors)
		}
	}
	if telemetry.HasAirtime || telemetry.HasTXAirtime {
		put32(data[16:], uint32(telemetry.TXAirtime/time.Second))
	}
	if !s.room && (telemetry.HasAirtime || telemetry.HasRXAirtime) {
		put32(data[48:], uint32(telemetry.RXAirtime/time.Second))
	}
	put32(data[20:], uint32(time.Since(s.started)/time.Second))
	put32(data[24:], uint32(s.sentFlood.Load()))
	put32(data[28:], uint32(s.sentDirect.Load()))
	stats := s.Node.RouteStats()
	put32(data[32:], uint32(stats.FloodReceived))
	put32(data[36:], uint32(stats.DirectReceived))
	if telemetry.HasErrorEvents {
		put16(40, telemetry.ErrorEvents)
	}
	put16(44, uint16(stats.DirectDuplicates))
	put16(46, uint16(stats.FloodDuplicates))
	if s.room {
		put16(48, uint16(s.state.Posted-s.initialPosted))
		put16(50, uint16(s.state.Pushed-s.initialPushed))
	}
	return data
}
