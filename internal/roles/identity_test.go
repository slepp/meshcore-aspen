package roles

import (
	"bytes"
	"encoding/json"
	"fmt"
	"testing"
)

func TestIdentityStatePreparationPreservesLegacyHistory(t *testing.T) {
	oldID, newID := fixedID(1), fixedID(2)
	for _, version := range []int{1, 2} {
		original := []byte(fmt.Sprintf(`{"Version":%d,"Identity":%q,"Room":true,"History":[{"Timestamp":123,"Text":"keep me"}],"extension":{"opaque":[1,2,3]}}`, version, oldID.Identity.String()))
		prepared, err := RebindStateIdentity(original, oldID.Identity, newID.Identity)
		if err != nil {
			t.Fatal(err)
		}
		var before, after map[string]json.RawMessage
		if err := json.Unmarshal(original, &before); err != nil {
			t.Fatal(err)
		}
		if err := json.Unmarshal(prepared, &after); err != nil {
			t.Fatal(err)
		}
		if !bytes.Equal(before["History"], after["History"]) || !bytes.Equal(before["extension"], after["extension"]) {
			t.Fatal("identity preparation changed history or extension data")
		}
		var id string
		if err := json.Unmarshal(after["Identity"], &id); err != nil || id != newID.Identity.String() {
			t.Fatal("identity was not rebound")
		}
		if _, exists := after["Preferences"]; exists {
			t.Fatal("preparation injected defaults into legacy state")
		}
		if _, err := RebindStateIdentity(original, newID.Identity, oldID.Identity); err == nil {
			t.Fatal("wrong previous identity accepted")
		}
	}
}
