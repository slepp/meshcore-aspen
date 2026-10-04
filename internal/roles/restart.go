package roles

import (
	"context"
	"errors"
	"fmt"
	"time"

	hoststate "meshcore.local/meshcore/internal/state"
)

func (s *Service) admitRestart(e event, prefix string) {
	ctx, cancel := context.WithTimeout(context.Background(), 5*time.Second)
	err := s.cfg.RequestRestart(ctx)
	cancel()
	s.mu.Lock()
	if err == nil {
		s.restartPending = true
		s.mu.Unlock()
		return
	}
	if errors.Is(err, hoststate.ErrCommitIndeterminate) || errors.Is(err, ErrCommitUncertain) {
		err = errors.Join(ErrCommitUncertain, err)
		s.failClosed(err)
		s.mu.Unlock()
		s.report(err)
		return
	}
	s.mu.Unlock()
	s.report(fmt.Errorf("role restart admission: %w", err))
	s.sendCLIResult(e, prefix+"Error, restart request rejected")
}

func (s *Service) sendCLIResult(e event, reply string) {
	s.mu.Lock()
	before := s.state.clone()
	packets, replyErr := s.cliReply(e, s.state.Members[e.key], u32(e.plain), reply)
	if replyErr == nil {
		replyErr = s.save()
	}
	if replyErr != nil {
		s.lastErr = replyErr
		if !s.commitUncertain {
			s.state = before
		}
	}
	s.mu.Unlock()
	s.transmit(packets, replyErr)
}
