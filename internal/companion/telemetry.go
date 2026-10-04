package companion

import (
	"context"
	"encoding/binary"
	"errors"
	"math"
	"time"

	meshcore "github.com/meshcore-go/meshcore-go"
	protocol "github.com/meshcore-go/meshcore-go/companion"
	"meshcore.local/meshcore/internal/policy"
)

// Telemetry providers run on the requesting TCP session, never on radio ingress.
// Providers must honour context cancellation; cached snapshots are preferable.
func (s *Server) telemetry(cmd []byte) ([]byte, byte, error) {
	ctx, cancel := context.WithTimeout(s.ctx, 3*time.Second)
	defer cancel()
	if cmd[0] == protocol.CmdGetBattAndStorage {
		if len(cmd) != 1 {
			return nil, protocol.ErrCodeIllegalArg, nil
		}

		if s.cfg.Battery == nil {
			return nil, protocol.ErrCodeUnsupportedCmd, nil
		}

		mv, err := s.cfg.Battery(ctx)
		if err != nil {
			return nil, protocol.ErrCodeBadState, err
		}
		out := []byte{protocol.RespBattAndStorage, 0, 0}
		binary.LittleEndian.PutUint16(out[1:], mv)
		if s.cfg.Storage != nil {
			used, total, err := s.cfg.Storage(ctx)
			if err != nil {
				return nil, protocol.ErrCodeBadState, err
			}
			if used > total {
				return nil, protocol.ErrCodeBadState, errors.New("storage provider returned used capacity greater than total")
			}
			out = append(out, make([]byte, 8)...)
			put32(out[3:7], used)
			put32(out[7:11], total)
		}
		return out, 0, nil
	}
	if len(cmd) != 2 || cmd[1] > protocol.StatsTypePackets {
		return nil, protocol.ErrCodeIllegalArg, nil
	}
	if s.cfg.Stats == nil {
		return nil, protocol.ErrCodeUnsupportedCmd, nil
	}
	stats, err := s.cfg.Stats(ctx, cmd[1])
	if err != nil {
		return nil, protocol.ErrCodeBadState, err
	}
	if stats.StatsType != cmd[1] {
		return nil, protocol.ErrCodeBadState, errors.New("telemetry provider returned the wrong stats type")
	}
	switch cmd[1] {
	case protocol.StatsTypeCore:
		v := stats.Core
		if v == nil {
			return nil, protocol.ErrCodeUnsupportedCmd, nil
		}
		out := make([]byte, 11)
		out[0], out[1] = protocol.RespStats, cmd[1]
		binary.LittleEndian.PutUint16(out[2:4], v.BatteryMV)
		put32(out[4:8], v.UptimeSecs)
		binary.LittleEndian.PutUint16(out[8:10], v.ErrFlags)
		out[10] = v.QueueLen
		return out, 0, nil
	case protocol.StatsTypeRadio:
		v := stats.Radio
		if v == nil {
			return nil, protocol.ErrCodeUnsupportedCmd, nil
		}
		if math.IsNaN(float64(v.LastSNR)) || v.LastSNR < -32 || v.LastSNR > 31.75 {
			return nil, protocol.ErrCodeBadState, errors.New("telemetry provider returned an invalid SNR")
		}
		out := make([]byte, 14)
		out[0], out[1] = protocol.RespStats, cmd[1]
		binary.LittleEndian.PutUint16(out[2:4], uint16(v.NoiseFloor))
		out[4], out[5] = byte(v.LastRSSI), byte(int8(v.LastSNR*4))
		put32(out[6:10], v.TxAirSecs)
		put32(out[10:14], v.RxAirSecs)
		return out, 0, nil
	case protocol.StatsTypePackets:
		v := stats.Packets
		if v == nil {
			return nil, protocol.ErrCodeUnsupportedCmd, nil
		}
		out := make([]byte, 30)
		out[0], out[1] = protocol.RespStats, cmd[1]
		for i, n := range []uint32{v.PacketsRecv, v.PacketsSent, v.SentFlood, v.SentDirect, v.RecvFlood, v.RecvDirect, v.RecvErrors} {
			put32(out[2+4*i:], n)
		}
		return out, 0, nil
	}
	return nil, protocol.ErrCodeUnsupportedCmd, nil
}

func (s *Server) telemetryData(permissions byte) ([]byte, error) {
	ctx, cancel := context.WithTimeout(s.ctx, 3*time.Second)
	defer cancel()
	var out []byte
	if s.cfg.Battery != nil && permissions&1 != 0 {
		mv, err := s.cfg.Battery(ctx)
		if err != nil {
			return nil, err
		}
		// CayenneLPP scales float32 volts before truncating to centivolts.
		volts := float32(mv) / 1000
		centivolts := uint16(volts * 100)
		out = []byte{1, meshcore.LPPVoltage, byte(centivolts >> 8), byte(centivolts)}
	}
	if s.cfg.SensorTelemetry != nil {
		data, err := s.cfg.SensorTelemetry(ctx, permissions)
		if err != nil {
			return nil, err
		}
		if _, err := meshcore.LPPDecode(data); err != nil {
			return nil, err
		}
		out = append(out, data...)
	}
	if len(out) == 0 {
		return nil, errors.New("telemetry measurements unavailable")
	}
	if len(out) > 168 {
		return nil, errors.New("sensor telemetry exceeds companion frame capacity")
	}
	return out, nil
}

func (s *Server) selfTelemetry() ([]byte, error) {
	data, err := s.telemetryData(255)
	if err != nil {
		return nil, err
	}
	out := append([]byte{protocol.PushTelemetryResponse, 0}, s.Node.Identity().PublicKeyBytes()[:6]...)
	return append(out, data...), nil
}

func (s *Server) incomingRequest(p *contact, pkt *meshcore.Packet, plain []byte, rx policy.ReceiveContext) {
	if len(plain) < 6 || plain[4] != 3 {
		return
	}
	permissions := byte(0)
	for i := byte(0); i < 3; i++ {
		mode := (s.state.TelemetryModes >> (i * 2)) & 3
		if mode == 2 || (mode == 1 && (p.Flags>>1)&(1<<i) != 0) {
			permissions |= 1 << i
		}
	}
	permissions &= ^plain[5]
	if permissions&1 == 0 {
		return
	}
	data, err := s.telemetryData(permissions)
	if err != nil {
		s.report(err)
		return
	}
	response := append(append([]byte(nil), plain[:4]...), data...)
	if pkt.IsRouteFlood() {
		s.report(s.returnPath(p, pkt, meshcore.PayloadTypeResponse, response, true, rx))
		return
	}
	reply, err := s.encrypted(p, meshcore.PayloadTypeResponse, response, false, false)
	if err == nil {
		err = s.sendReply(reply, rx, 300*time.Millisecond)
	}
	s.report(err)
}
