package roles

import (
	"errors"
	"math/rand/v2"
	"time"

	meshcore "github.com/meshcore-go/meshcore-go"
	"github.com/meshcore-go/meshcore-go/node"
	"meshcore.local/meshcore/internal/policy"
)

func (s *Service) validatePolicyInputs(p policy.Preferences) error {
	if p.RXDelay != 0 && s.cfg.RXScore == nil {
		return errors.New("nonzero rxdelay requires a native radio-score callback")
	}
	if s.cfg.AirtimeEstimator == nil && (p.RXDelay != 0 || (p.Repeat && (p.TXDelay != 0 || p.DirectTXDelay != 0))) {
		return errors.New("active receive/relay delays require an airtime estimator")
	}
	return nil
}

func (s *Service) receiveContext(p *meshcore.Packet) (policy.ReceiveContext, error) {
	prefs := s.policySnapshot.Load()
	if prefs == nil {
		return policy.ReceiveContext{}, errors.New("role policy is not initialized")
	}
	path, err := policy.ParsePath(p.PathLength, p.Path)
	if err != nil {
		return policy.ReceiveContext{}, err
	}
	return policy.ResolveReceiveContext(*prefs, policy.Route(p.RouteType()), path,
		p.PayloadType(), p.Payload, p.TransportCode1), nil
}

func (s *Service) allowForward(p *meshcore.Packet) bool {
	if localReflection(p) {
		return false
	}
	s.mu.RLock()
	defer s.mu.RUnlock()
	ctx, err := s.receiveContext(p)
	return err == nil && !s.commitUncertain && !s.restartPending &&
		policy.AllowForward(s.profile, s.state.Preferences, ctx, p.PayloadType(), s.id.PublicKey())
}

func (s *Service) retransmitDelay(direct bool) node.RetransmitDelayFunc {
	return func(_ int, airtime uint32) time.Duration {
		s.mu.RLock()
		prefs := s.state.Preferences
		s.mu.RUnlock()
		delay, err := policy.RetransmitDelayMillis(s.profile, prefs, direct, airtime,
			func(min, max uint32) uint32 { return min + rand.Uint32N(max-min) })
		if err != nil {
			s.report(err)
			return 0
		}
		return time.Duration(delay) * time.Millisecond
	}
}

func (s *Service) rxDelay(p *meshcore.Packet, size int, airtime uint32) time.Duration {
	s.mu.RLock()
	prefs := s.state.Preferences
	s.mu.RUnlock()
	if localReflection(p) || prefs.RXDelay == 0 {
		return 0
	}
	score, err := s.cfg.RXScore(p, size, airtime)
	if err != nil {
		p.MarkDoNotRetransmit()
		s.report(err)
		return 0
	}
	delay, err := policy.RXDelayMillis(prefs, score, airtime)
	if err != nil {
		p.MarkDoNotRetransmit()
		s.report(err)
		return 0
	}
	if delay <= 0 {
		return 0
	}
	return time.Duration(delay) * time.Millisecond
}

func applyScope(p *meshcore.Packet, scope policy.Scope) {
	p.Header = meshcore.MakeHeader(meshcore.RouteTypeFlood, p.PayloadType(), 0)
	p.TransportCode1, p.TransportCode2 = 0, 0
	if !scope.IsNull() {
		p.Header = meshcore.MakeHeader(meshcore.RouteTypeTransportFlood, p.PayloadType(), 0)
		p.TransportCode1 = policy.TransportCode(scope, p.PayloadType(), p.Payload)
	}
}

func (s *Service) prepareReply(p *meshcore.Packet, ctx policy.ReceiveContext) *meshcore.Packet {
	if p != nil && p.IsRouteFlood() {
		p.PathLength = (ctx.Path.Width() - 1) << 6
		applyScope(p, policy.ChooseReplyScope(ctx, s.state.Preferences.DefaultScope))
	}
	return p
}
