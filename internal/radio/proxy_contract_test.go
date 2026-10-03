package radio

import (
	"testing"

	"github.com/meshcore-go/meshcore-go/hardware"
)

func TestSharedPHYCommandGuard(t *testing.T) {
	for _, tc := range []struct {
		name    string
		command byte
		payload []byte
		want    byte
	}{
		{"non-owner-hello", HWQueuedHello, []byte{QueuedProtocolVersion, 0}, 0},
		{"owner-claim", HWQueuedHello, []byte{QueuedProtocolVersion, 1}, hardware.HW_ERR_INVALID_PARAM},
		{"reserved-hello-flags", HWQueuedHello, []byte{QueuedProtocolVersion, 128}, hardware.HW_ERR_INVALID_PARAM},
		{"unknown-version", HWQueuedHello, []byte{QueuedProtocolVersion + 1, 0}, hardware.HW_ERR_INVALID_PARAM},
		{"short-hello", HWQueuedHello, []byte{QueuedProtocolVersion}, hardware.HW_ERR_INVALID_LENGTH},
		{"long-hello", HWQueuedHello, []byte{QueuedProtocolVersion, 0, 0}, hardware.HW_ERR_INVALID_LENGTH},
		{"configuration-readback", HWQueuedConfig, []byte{QueuedProtocolVersion, 0}, 0},
		{"configuration-write", HWQueuedConfig, append([]byte{QueuedProtocolVersion, 1}, make([]byte, 22)...), hardware.HW_ERR_INVALID_PARAM},
		{"configuration-unknown-operation", HWQueuedConfig, []byte{QueuedProtocolVersion, 2}, hardware.HW_ERR_INVALID_PARAM},
		{"short-config", HWQueuedConfig, nil, hardware.HW_ERR_INVALID_LENGTH},
		{"ambiguous-config", HWQueuedConfig, []byte{QueuedProtocolVersion, 0, 1}, hardware.HW_ERR_INVALID_LENGTH},
		{"capacity-readback", HWQueuedCapacity, []byte{QueuedProtocolVersion}, 0},
		{"short-capacity", HWQueuedCapacity, nil, hardware.HW_ERR_INVALID_LENGTH},
		{"long-capacity", HWQueuedCapacity, []byte{QueuedProtocolVersion, 0}, hardware.HW_ERR_INVALID_LENGTH},
		{"capacity-wrong-version", HWQueuedCapacity, []byte{QueuedProtocolVersion + 1}, hardware.HW_ERR_INVALID_PARAM},
		{"role-presence-forbidden", HWQueuedRolePresence, append([]byte{QueuedProtocolVersion, 2}, make([]byte, 32)...), hardware.HW_ERR_INVALID_PARAM},
		{"submit-forwarded-to-MCU", HWQueuedSubmit, nil, 0},
		{"source-policy-forwarded-to-MCU", HWQueuedSourcePolicy, nil, 0},
		{"statistics-forwarded-to-MCU", HWQueuedStats, nil, 0},
		{"legacy-config-keeps-proxy-handler", hardware.HW_CMD_SET_RADIO, nil, 0},
	} {
		t.Run(tc.name, func(t *testing.T) {
			if got := SharedPHYCommandErrorCode(tc.command, tc.payload); got != tc.want {
				t.Fatalf("guard error=%d, want %d", got, tc.want)
			}
		})
	}
}
