// SPDX-License-Identifier: Apache-2.0
package roles

import (
	"meshcore.local/meshcore/internal/buildinfo"
	"strings"
	"testing"
)

func TestAdminHelpAndProfile(t *testing.T) {
	for _, room := range []bool{false, true} {
		s, _, _ := startRole(t, room, config(t))
		s.mu.Lock()
		for _, command := range []string{"help", "help get", "help set", "help radio", "help owner", "help region"} {
			reply, packets, err := s.command(command)
			if err != nil || len(packets) != 0 || reply == "" || len(reply) > 146 || strings.HasPrefix(reply, "Error:") {
				t.Errorf("room=%v command=%q reply=%q packets=%d err=%v", room, command, reply, len(packets), err)
			}
		}
		for _, command := range []string{"wifi", "help wifi", "get wifi.enabled", "set wifi.ssid example", "set wifi.pwd private"} {
			reply, _, err := s.command(command)
			if err != nil || !strings.Contains(reply, "external modem") {
				t.Errorf("room=%v command=%q reply=%q err=%v", room, command, reply, err)
			}
		}
		for _, command := range []string{"get", "set", "set name", "get name extra"} {
			reply, _, err := s.command(command)
			if err != nil || !strings.HasPrefix(reply, "Error: usage:") {
				t.Errorf("room=%v command=%q reply=%q err=%v", room, command, reply, err)
			}
		}
		reply, _, err := s.command("ver")
		if err != nil || !strings.Contains(reply, "Birch "+buildinfo.ProductVersion) || !strings.Contains(reply, "MeshCore "+buildinfo.MeshCoreReference) {
			t.Errorf("room=%v version=%q err=%v", room, reply, err)
		}
		s.mu.Unlock()
	}
}
