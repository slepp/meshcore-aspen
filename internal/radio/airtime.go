package radio

import (
	"context"
	"encoding/binary"
	"fmt"

	"github.com/meshcore-go/meshcore-go/hardware"
	"github.com/meshcore-go/meshcore-go/node"
)

// AirtimeEstimator returns a live lookup of the modem's GET_AIRTIME values,
// in milliseconds for lengths 0..255, for this link's current effective PHY.
// It performs no I/O: Config.PHYGroup refreshes the table whenever the link
// verifies a new modulation, including after a retune or reconnect. The
// lookup keeps the last verified table while offline, and returns 0 for a
// length outside 0..255. It is nil when the link has no PHYGroup.
func (l *Link) AirtimeEstimator() node.AirtimeEstimator {
	if l.config.PHYGroup == nil {
		return nil
	}
	return func(packetLen int) uint32 {
		table := l.airtime.Load()
		if table == nil || packetLen < 0 || packetLen >= len(table) {
			return 0
		}
		return table[packetLen]
	}
}

// AirtimeTable copies the current airtime lookup, or returns nil before one
// has been read.
func (l *Link) AirtimeTable() []uint32 {
	table := l.airtime.Load()
	if table == nil {
		return nil
	}
	return append([]uint32(nil), table[:]...)
}

// readAirtime returns only a complete table. GET_AIRTIME replies have no
// correlation ID, so every failure must retire the connection before a late
// reply can satisfy a later request; callers do so.
func (l *Link) readAirtime(ctx context.Context, modem *hardware.KissModem) (*airtimeTable, error) {
	var table airtimeTable
	for length := range table {
		if err := ctx.Err(); err != nil {
			return nil, fmt.Errorf("PHY airtime for length %d: %w", length, err)
		}
		reply, err := modem.Request(ctx, hardware.HW_CMD_GET_AIRTIME, []byte{byte(length)})
		if err == nil && len(reply) != 4 {
			err = fmt.Errorf("invalid response length %d, want 4", len(reply))
		}
		if err != nil {
			return nil, fmt.Errorf("PHY airtime for length %d: %w", length, err)
		}
		table[length] = binary.LittleEndian.Uint32(reply)
	}
	return &table, nil
}
