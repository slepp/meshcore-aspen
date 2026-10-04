package roles

import (
	"encoding/binary"
	"errors"
	"math"
	"math/rand/v2"
	"sort"
	"time"

	meshcore "github.com/meshcore-go/meshcore-go"
	"meshcore.local/meshcore/internal/policy"
)

type neighbour struct {
	Key    [32]byte
	Advert uint32
	Heard  uint32
	SNR    int8
}

func (s *Service) putNeighbour(key [32]byte, advert uint32, snr float32) {
	if math.IsNaN(float64(snr)) || math.IsInf(float64(snr), 0) || snr < -32 || snr > 31.75 {
		return
	}
	n := neighbour{Key: key, Advert: advert, Heard: s.now(), SNR: int8(snr * 4)}
	oldest := 0
	for i, existing := range s.neighbours {
		if existing.Key == key {
			s.neighbours[i] = n
			return
		}
		if existing.Heard < s.neighbours[oldest].Heard {
			oldest = i
		}
	}
	if len(s.neighbours) < s.cfg.MaxNeighbours {
		s.neighbours = append(s.neighbours, n)
	} else {
		s.neighbours[oldest] = n
	}
}

func (s *Service) observeNeighbour(p *meshcore.Packet) {
	if p.PathLength&63 != 0 || localReflection(p) || !p.HasSignalInfo ||
		(p.RouteType() == meshcore.RouteTypeTransportFlood && p.TransportCode1 == 0 && p.TransportCode2 == 0) {
		return
	}
	advert, err := meshcore.AdvertFromBytes(p.Payload)
	if err != nil || !advert.Verify() || advert.Type() != 2 {
		return
	}
	s.putNeighbour(advert.PublicKey.PublicKey(), advert.Timestamp, p.SNR)
}

func (s *Service) neighbourResponse(request []byte) ([]byte, error) {
	if len(request) < 6 || request[0] != 0 {
		return nil, errors.New("invalid neighbour request version or length")
	}
	count, offset, order, width := int(request[1]), int(binary.LittleEndian.Uint16(request[2:])), request[4], int(request[5])
	if width > 32 {
		width = 32
	}
	list := append([]neighbour(nil), s.neighbours...)
	sort.SliceStable(list, func(i, j int) bool {
		switch order {
		case 0:
			return list[i].Heard > list[j].Heard
		case 1:
			return list[i].Heard < list[j].Heard
		case 2:
			return list[i].SNR > list[j].SNR
		case 3:
			return list[i].SNR < list[j].SNR
		}
		return false
	})
	reply := make([]byte, 4)
	binary.LittleEndian.PutUint16(reply, uint16(len(list)))
	now := s.now()
	for offset < len(list) && int(binary.LittleEndian.Uint16(reply[2:])) < count && len(reply)-4+width+5 <= 130 {
		n := list[offset]
		reply = append(reply, n.Key[:width]...)
		reply = binary.LittleEndian.AppendUint32(reply, now-n.Heard)
		reply = append(reply, byte(n.SNR))
		binary.LittleEndian.PutUint16(reply[2:], binary.LittleEndian.Uint16(reply[2:])+1)
		offset++
	}
	return reply, nil
}

func (s *Service) control(p *meshcore.Packet) ([]transmission, bool, error) {
	data := p.Payload
	if len(data) < 6 || localReflection(p) {
		return nil, false, nil
	}
	now := s.now()
	switch data[0] & 0xf0 {
	case 0x80:
		if !s.state.Preferences.Repeat || !s.discoveryLimit.allow(now, 120, 4) {
			return nil, false, nil
		}
		since := uint32(0)
		if len(data) >= 10 {
			since = u32(data[6:])
		}
		if data[1]&4 == 0 || since > s.state.DiscoveryModified || !p.HasSignalInfo {
			return nil, false, nil
		}
		reply := append([]byte{0x92, byte(int8(p.SNR * 4))}, data[2:6]...)
		key := s.id.PublicKey()
		length := 32
		if data[0]&1 != 0 {
			length = 8
		}
		reply = append(reply, key[:length]...)
		packet := &meshcore.Packet{Header: meshcore.MakeHeader(meshcore.RouteTypeDirect, meshcore.PayloadTypeControl, 0), Payload: reply}
		airtime := uint32(0)
		if s.cfg.AirtimeEstimator != nil {
			airtime = s.cfg.AirtimeEstimator(len(reply) + 2)
		}
		delay, err := policy.RetransmitDelayMillis(s.profile, s.state.Preferences, false, airtime,
			func(min, max uint32) uint32 { return min + rand.Uint32N(max-min) })
		return delayed(packet, time.Duration(delay)*4*time.Millisecond), false, err
	case 0x90:
		if data[0]&15 != 2 || len(data) < 38 || s.discoveryTag == 0 ||
			time.Now().After(s.discoveryUntil) || u32(data[2:]) != s.discoveryTag || !p.HasSignalInfo {
			return nil, false, nil
		}
		var key [32]byte
		copy(key[:], data[6:38])
		if key != s.id.PublicKey() {
			s.putNeighbour(key, now, p.SNR)
		}
	}
	return nil, false, nil
}

func (s *Service) startDiscovery() (*meshcore.Packet, error) {
	data := make([]byte, 10)
	data[0], data[1] = 0x80, 4
	if _, err := s.random(data[2:6]); err != nil {
		return nil, err
	}
	s.discoveryTag, s.discoveryUntil = u32(data[2:]), time.Now().Add(time.Minute)
	return &meshcore.Packet{Header: meshcore.MakeHeader(meshcore.RouteTypeDirect, meshcore.PayloadTypeControl, 0), Payload: data}, nil
}
