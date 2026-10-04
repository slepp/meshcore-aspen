package roles

import (
	"context"
	"strings"
	"testing"

	"meshcore.local/meshcore/internal/policy"
)

func TestOwnerRegionHierarchyHomeAndScopeSurviveRestart(t *testing.T) {
	cfg := config(t)
	s, radio, id := startRole(t, false, cfg)
	ctx := context.Background()
	for _, step := range []struct{ command, reply string }{
		{"region def can ab edm", "*^ F\n can F\n  ab F\n   edm F\n"},
		{"region home ab", " home is now ab"},
		{"region home", " home is ab"},
		{"region get edm", " edm (ab) F"},
		{"region default", " default scope is <null>"},
		{"region save", "OK"},
	} {
		if reply, err := s.OwnerAdmin(ctx, step.command); err != nil || reply != step.reply {
			t.Fatalf("%s: %q %v", step.command, reply, err)
		}
	}
	if err := s.Close(); err != nil {
		t.Fatal(err)
	}
	restarted, err := NewRepeater(id, newWireRadio(), cfg)
	if err != nil {
		t.Fatal(err)
	}
	t.Cleanup(func() { _ = restarted.Close() })
	for command, want := range map[string]string{
		"region":         "* F\n can F\n  ab^ F\n   edm F\n",
		"region home":    " home is ab",
		"region default": " default scope is <null>",
		"region get ab":  " ab (can) F",
		"region get edm": " edm (ab) F",
	} {
		if reply, err := restarted.OwnerAdmin(ctx, command); err != nil || reply != want {
			t.Fatalf("restored %s: %q %v", command, reply, err)
		}
	}
	if reply, err := restarted.OwnerAdmin(ctx, "region home AB"); err != nil || reply != "Err - unknown region" {
		t.Fatalf("region case was changed: %q %v", reply, err)
	}
	for _, command := range []string{"region default ab", "region denyf *", "region denyf edm"} {
		if reply, err := restarted.OwnerAdmin(ctx, command); err != nil || strings.HasPrefix(reply, "Err") {
			t.Fatalf("%s: %q %v", command, reply, err)
		}
	}
	p := *restarted.policySnapshot.Load()
	if p.DefaultScope != policy.AutoScope("ab") {
		t.Fatalf("wrong public scope: %+v", p.DefaultScope)
	}
	payload := []byte("region test")
	for _, name := range []string{"can", "ab", "edm", "AB"} {
		code := policy.TransportCode(policy.AutoScope(name), 15, payload)
		ctx := policy.ResolveReceiveContext(p, policy.TransportFlood, policy.Path{}, 15, payload, code)
		if ctx.ScopeKnown != (name == "can" || name == "ab") {
			t.Fatalf("%s scope admission=%v; hierarchy must not inherit permissions", name, ctx.ScopeKnown)
		}
	}
	unscoped := policy.ResolveReceiveContext(p, policy.Flood, policy.Path{}, 15, payload, 0)
	if unscoped.Unscoped {
		t.Fatal("denied unscoped flood admitted")
	}
	radio.quiet(t)
}
