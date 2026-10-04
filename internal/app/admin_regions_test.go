package app

import (
	"strings"
	"testing"
)

func TestHostRoleRegionConsoleScope(t *testing.T) {
	if !strings.Contains(hostAdminPage, "const roleRead=/"+hostRoleRead.String()+"/;") ||
		!strings.Contains(hostAdminPage, "const roleWrite=/"+hostRoleWrite.String()+"/;") {
		t.Fatal("browser role command scope differs from the server")
	}
	for _, command := range []string{
		"help region", "region", "region home", "region default", "region get ab",
		"region list allowed", "region list denied",
	} {
		if !hostRoleRead.MatchString(command) || hostRoleWrite.MatchString(command) {
			t.Fatalf("region read classification: %s", command)
		}
	}
	for _, command := range []string{
		"region def can ab edm", "region put can", "region put ab can", "region put edm ab",
		"region home ab", "region default ab", "region default <null>", "region save",
		"region allowf ab", "region denyf *", "region remove edm",
	} {
		if !hostRoleWrite.MatchString(command) || hostRoleRead.MatchString(command) {
			t.Fatalf("region write classification: %s", command)
		}
	}
	for _, command := range []string{
		"region load", "region home ab\nreboot", "region home ab;reboot", "region key ab secret",
		"region default <#null>", "region save reboot",
	} {
		if hostRoleRead.MatchString(command) || hostRoleWrite.MatchString(command) {
			t.Fatalf("unsupported region command accepted: %q", command)
		}
	}
}
