package radio

import (
	"context"
	"errors"
	"fmt"

	"github.com/meshcore-go/meshcore-go/hardware"
)

var ErrCapacityUnavailable = errors.New("queued CAPACITY command unavailable")

type ClientCapacity struct {
	ExternalTCPSlots uint8
	LocalSourceSlots uint8
	// LogicalPorts is populated only by a negotiated aggregate session and
	// includes port 0. Legacy CAPACITY does not report virtual ports.
	LogicalPorts uint8
}

// ClientCapacity queries the negotiated link without claiming PHY ownership.
func (l *Link) ClientCapacity(ctx context.Context) (ClientCapacity, error) {
	l.request.Lock()
	defer l.request.Unlock()
	if err := ctx.Err(); err != nil {
		return ClientCapacity{}, err
	}
	l.mu.RLock()
	modem := l.modem
	l.mu.RUnlock()
	if modem == nil {
		return ClientCapacity{}, ErrOffline
	}
	capacity, err := clientCapacity(ctx, modem)
	if err != nil && !errors.Is(err, ErrCapacityUnavailable) {
		// An uncorrelated late response must not satisfy the next modem request.
		l.requestReset(modem)
		return ClientCapacity{}, errors.Join(fmt.Errorf("radio CAPACITY: %w", err), modem.Close())
	}
	return capacity, err
}

func clientCapacity(ctx context.Context, modem *hardware.KissModem) (ClientCapacity, error) {
	reply, err := modem.Request(ctx, HWQueuedCapacity, []byte{QueuedProtocolVersion})
	if errors.Is(err, hardware.HwErrorFor(hardware.HW_ERR_UNKNOWN_CMD)) ||
		errors.Is(err, hardware.HwErrorFor(hardware.HW_ERR_NO_CALLBACK)) {
		return ClientCapacity{}, ErrCapacityUnavailable
	}
	if err == nil && (len(reply) != 3 || reply[0] != QueuedProtocolVersion || reply[1] == 0) {
		err = fmt.Errorf("invalid queued CAPACITY response: %x", reply)
	}
	if err != nil {
		return ClientCapacity{}, err
	}
	return ClientCapacity{ExternalTCPSlots: reply[1], LocalSourceSlots: reply[2]}, nil
}
