package app

import (
	"fmt"

	"meshcore.local/meshcore/internal/roles"
	"meshcore.local/meshcore/internal/state"
)

// Full-host startup selects activated authority before logging any identities
// or constructing nodes. Logical restart uses the same transaction after Close.
func activatePendingRoleIdentities(cfg Config) error {
	for _, role := range []string{"repeater", "room"} {
		if !cfg.roleEnabled(role) {
			continue
		}
		if err := state.ActivateRoleIdentity(cfg.StateDir, role, roles.RebindStateIdentity); err != nil {
			return fmt.Errorf("%s pending identity activation: %w", role, err)
		}
	}
	return nil
}
