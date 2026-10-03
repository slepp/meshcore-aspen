package companion

import (
	"context"
	"math/rand/v2"
	"time"

	meshcore "github.com/meshcore-go/meshcore-go"
	"github.com/meshcore-go/meshcore-go/hardware"
	"github.com/meshcore-go/meshcore-go/node"
	"meshcore.local/meshcore/internal/policy"
)

type sourceAirtime interface {
	SetSourceAirtimeFactor(context.Context, float64) error
}

func (s *Server) publishPreferences() {
	p := s.state.Preferences.Clone()
	s.preferences.Store(&p)
	s.multiACKs.Store(uint32(s.state.MultiACKs))
}

func (s *Server) receiveContext(pkt *meshcore.Packet) policy.ReceiveContext {
	path, err := policy.ParsePath(pkt.PathLength, pkt.Path)
	if err != nil {
		return policy.ReceiveContext{Route: policy.Route(pkt.Header & 3)}
	}
	return policy.ResolveReceiveContext(*s.preferences.Load(), policy.Route(pkt.Header&3), path, pkt.PayloadType(), pkt.Payload, pkt.TransportCode1)
}

// rxScore uses the spreading factor currently in effect on the shared PHY.
func (s *Server) rxScore(pkt *meshcore.Packet, length int) float32 {
	radio, _ := s.phy()
	return float32(hardware.PacketScore(float64(pkt.SNR), radio.SF, length))
}

func (s *Server) nodePolicyOptions() []node.Option {
	delay := func(direct bool) node.RetransmitDelayFunc {
		return func(_ int, airtime uint32) time.Duration {
			ms, err := policy.RetransmitDelayMillis(policy.Companion, *s.preferences.Load(), direct, airtime,
				func(min, max uint32) uint32 { return min + rand.Uint32N(max-min) })
			if err != nil {
				s.report(err)
				return 0
			}
			return time.Duration(ms) * time.Millisecond
		}
	}
	return []node.Option{
		node.WithExtraAckTransmitCount(func() uint8 { return uint8(s.multiACKs.Load()) }),
		node.WithAirtimeFactor(0),
		node.WithFloodRetransmitDelay(delay(false)),
		node.WithDirectRetransmitDelay(delay(true)),
		node.WithRxDelay(func(pkt *meshcore.Packet, length int, airtime uint32) time.Duration {
			if !pkt.HasSignalInfo || localReflection(pkt) {
				return 0
			}
			score := s.rxScore(pkt, length)
			ms, err := policy.RXDelayMillis(*s.preferences.Load(), score, airtime)
			if err != nil {
				s.report(err)
				return 0
			}
			if ms <= 0 {
				return 0
			}
			return time.Duration(ms) * time.Millisecond
		}),
		node.WithAllowForwardHandler(func(pkt *meshcore.Packet) bool {
			return !s.faulted.Load() && !localReflection(pkt) && policy.AllowForward(policy.Companion, *s.preferences.Load(), s.receiveContext(pkt), pkt.PayloadType(), s.identity.Load().PublicKey())
		}),
	}
}

func localReflection(pkt *meshcore.Packet) bool {
	return pkt.HasSignalInfo && pkt.SNR == -32 && pkt.RSSI == 127
}

func (s *Server) prepareFlood(pkt *meshcore.Packet, scope policy.Scope) {
	width, _ := s.state.Preferences.PathHashMode.Width()
	pkt.PathLength = (width - 1) << 6
	pkt.Path = nil
	pkt.Header = meshcore.MakeHeader(meshcore.RouteTypeFlood, pkt.PayloadType(), 0)
	if !scope.IsNull() {
		pkt.Header = meshcore.MakeHeader(meshcore.RouteTypeTransportFlood, pkt.PayloadType(), 0)
		pkt.TransportCode1 = policy.TransportCode(scope, pkt.PayloadType(), pkt.Payload)
		pkt.TransportCode2 = 0
	}
}

func (s *Server) sendOrigin(pkt *meshcore.Packet, delay time.Duration) error {
	if s.storageFault != nil {
		return s.storageFault
	}
	priority, err := policy.OriginatedPriority(policy.Route(pkt.Header&3), pkt.PayloadType(), pkt.IsRouteDirect() && pkt.PathLength == 0 && pkt.PayloadType() != meshcore.PayloadTypePath && pkt.PayloadType() != meshcore.PayloadTypeTrace)
	if err != nil {
		return err
	}
	return s.Node.SendPacketDelayed(pkt, priority, delay)
}

func (s *Server) sendForSession(c *session, pkt *meshcore.Packet, delay time.Duration) error {
	if pkt.IsRouteFlood() {
		s.prepareFlood(pkt, c.scope.Resolve(s.state.Preferences.DefaultScope))
	}

	return s.sendOrigin(pkt, delay)
}

func (s *Server) sendReply(pkt *meshcore.Packet, rx policy.ReceiveContext, delay time.Duration) error {
	if pkt.IsRouteFlood() {
		s.prepareFlood(pkt, policy.ChooseReplyScope(rx, s.state.Preferences.DefaultScope))
	}
	return s.sendOrigin(pkt, delay)
}
