package companion

import (
	"encoding/binary"
	"time"

	meshcore "github.com/meshcore-go/meshcore-go"
	protocol "github.com/meshcore-go/meshcore-go/companion"
	"github.com/meshcore-go/meshcore-go/node"
	"meshcore.local/meshcore/internal/policy"
)

type traceRequest struct {
	Client  *session
	Expires time.Time
}

func (s *Server) packetCommand(c *session, b []byte) ([]byte, bool) {
	illegal := []byte{protocol.RespErr, protocol.ErrCodeIllegalArg}
	var pkt *meshcore.Packet
	var traceKey uint64
	switch b[0] {
	case protocol.CmdSendRawPacket:
		if len(b) < 4 {
			return illegal, true
		}
		p, err := meshcore.PacketFromBytes(b[2:])
		if err != nil {
			return illegal, true
		}
		if err = s.Node.SendPacketDelayed(p, b[1], 0); err != nil {
			s.report(err)
			return []byte{protocol.RespErr, protocol.ErrCodeTableFull}, true
		}
		return []byte{protocol.RespOk}, true
	case protocol.CmdSendRawData:
		if len(b) < 6 {
			return illegal, true
		}
		if b[1] == 255 {
			return []byte{protocol.RespErr, protocol.ErrCodeUnsupportedCmd}, true
		}
		width, count, ok := decodePath(b[1])
		length := width * count
		if !ok || len(b) < 2+length+4 {
			return illegal, true
		}
		pkt = &meshcore.Packet{Header: meshcore.MakeHeader(meshcore.RouteTypeDirect, meshcore.PayloadTypeRawCustom, 0), PathLength: b[1], Path: append([]byte(nil), b[2:2+length]...), Payload: append([]byte(nil), b[2+length:]...)}
	case protocol.CmdSendControlData:
		if len(b) < 2 || b[1]&0x80 == 0 {
			return illegal, true
		}
		pkt = &meshcore.Packet{Header: meshcore.MakeHeader(meshcore.RouteTypeDirect, meshcore.PayloadTypeControl, 0), Payload: append([]byte(nil), b[1:]...)}
	case protocol.CmdSendTracePath:
		if len(b) <= 10 {
			return illegal, true
		}
		path, err := policy.ParseTracePath(b[9], b[10:])
		if err != nil {
			return illegal, true
		}
		traceKey = binary.LittleEndian.Uint64(b[1:9])
		if _, exists := s.traces[traceKey]; exists {
			return []byte{protocol.RespErr, protocol.ErrCodeBadState}, true
		}
		if len(s.traces) >= 128 {
			return []byte{protocol.RespErr, protocol.ErrCodeTableFull}, true
		}
		pkt = &meshcore.Packet{Header: meshcore.MakeHeader(meshcore.RouteTypeDirect, meshcore.PayloadTypeTrace, 0), Payload: append([]byte(nil), b[1:]...)}
		if err = s.sendOrigin(pkt, 0); err != nil {
			s.report(err)
			return []byte{protocol.RespErr, protocol.ErrCodeTableFull}, true
		}
		s.traces[traceKey] = traceRequest{Client: c, Expires: time.Now().Add(2 * time.Minute)}
		out := s.sentFrame(pkt, u32(b[1:5]))
		airtime := uint32(1000)
		if s.cfg.AirtimeEstimator != nil {
			data, _ := pkt.ToBytes()
			airtime = s.cfg.AirtimeEstimator(len(data))
		}
		put32(out[6:], uint32(node.CalcDirectTimeout(airtime, byte(path.Count())).Milliseconds()))
		return out, true
	case protocol.CmdSendChannelData:
		if len(b) < 5 || b[1] >= maxChannels {
			return illegal, true
		}
		width, count, ok := decodePath(b[2])
		length := width * count
		if b[2] == 255 {
			length = 0
		}
		if !ok || len(b) < 5+length {
			return illegal, true
		}
		plain := b[3+length:]
		if binary.LittleEndian.Uint16(plain[:2]) == 0 || len(plain)-2 > 167 {
			return illegal, true
		}
		ch := s.Node.Channel(int(b[1]))
		if ch == nil {
			return []byte{protocol.RespErr, protocol.ErrCodeNotFound}, true
		}
		body := append([]byte{plain[0], plain[1], byte(len(plain) - 2)}, plain[2:]...)
		cipher, err := meshcore.EncryptThenMAC(ch.PSK[:], body)
		if err != nil {
			s.report(err)
			return []byte{protocol.RespErr, protocol.ErrCodeBadState}, true
		}
		pkt = &meshcore.Packet{Header: meshcore.MakeHeader(meshcore.RouteTypeDirect, meshcore.PayloadTypeGrpData, 0), PathLength: b[2], Path: append([]byte(nil), b[3:3+length]...), Payload: append([]byte{ch.Hash}, cipher...)}
		if b[2] == 255 {
			pkt.Header = meshcore.MakeHeader(meshcore.RouteTypeFlood, meshcore.PayloadTypeGrpData, 0)
		}
	default:
		return nil, false
	}
	if err := s.sendForSession(c, pkt, 0); err != nil {
		s.report(err)
		return []byte{protocol.RespErr, protocol.ErrCodeTableFull}, true
	}
	return []byte{protocol.RespOk}, true
}

func (s *Server) incomingDatagram(pkt *meshcore.Packet) {
	group, err := meshcore.GroupDataFromBytes(pkt.Payload)
	if err != nil {
		return
	}
	for i, ch := range s.state.Channels {
		if ch == nil || ch.Name == "" || ch.Hash != group.ChannelHash {
			continue
		}
		plain := group.Decrypt(ch.PSK[:])
		if len(plain) < 3 || int(plain[2]) > len(plain)-3 || plain[2] > 167 || binary.LittleEndian.Uint16(plain[:2]) == 0 {
			continue
		}
		frame := []byte{protocol.RespChannelDataRecv, byte(int8(pkt.SNR * 4)), 0, 0, byte(i), messagePath(pkt), plain[0], plain[1], plain[2]}
		frame = append(frame, plain[3:3+int(plain[2])]...)
		s.queueMessage(frame)
		return
	}
}

func (s *Server) incomingDiagnostic(pkt *meshcore.Packet) {
	var out []byte
	switch pkt.PayloadType() {
	case meshcore.PayloadTypeRawCustom:
		out = append([]byte{protocol.PushRawData, byte(int8(pkt.SNR * 4)), byte(pkt.RSSI), 255}, pkt.Payload...)
	case meshcore.PayloadTypeControl:
		out = append([]byte{protocol.PushControlData, byte(int8(pkt.SNR * 4)), byte(pkt.RSSI), pkt.PathLength}, pkt.Payload...)
	case meshcore.PayloadTypeTrace:
		trace, err := meshcore.TraceFromBytes(pkt.Payload)
		if err != nil {
			return
		}
		path, err := policy.ParseTracePath(trace.Flags, trace.PathHashes)
		if err != nil || len(pkt.Path) != path.Count() {
			return
		}
		out = append([]byte{protocol.PushTraceData, 0, byte(len(trace.PathHashes)), trace.Flags}, pkt.Payload[:8]...)
		out = append(out, trace.PathHashes...)
		out = append(out, pkt.Path...)
		out = append(out, byte(int8(pkt.SNR*4)))
		if len(out) > protocol.MaxFrameSize {
			return
		}
		key := binary.LittleEndian.Uint64(pkt.Payload[:8])
		if req, ok := s.traces[key]; ok {
			delete(s.traces, key)
			req.Client.send(out)
			return
		}
	}
	if len(out) <= protocol.MaxFrameSize {
		s.broadcast(out)
	}
}

func (s *Server) rawReceive(data []byte, snr float32, rssi int8, _ bool) {
	if len(data)+3 > protocol.MaxFrameSize {
		return
	}
	frame := append([]byte{protocol.PushLogRxData, byte(int8(snr * 4)), byte(rssi)}, data...)
	select {
	case <-s.done:
		return
	default:
	}
	select {
	case s.diagnostics <- frame:
	default:
		s.signalOverflow()
	}
}
