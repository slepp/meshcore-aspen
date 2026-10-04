package observer

import (
	"bytes"
	"context"
	"crypto/ed25519"
	"encoding/base64"
	"encoding/hex"
	"encoding/json"
	"os"
	"os/exec"
	"strings"
	"testing"
	"time"
)

func TestHewObserverIdentityToken(t *testing.T) {
	binary := os.Getenv("MESHCORE_HEW_OBSERVER_AUTH_BIN")
	if binary == "" {
		t.Skip("set MESHCORE_HEW_OBSERVER_AUTH_BIN to the native identity codec checks")
	}
	ctx, cancel := context.WithTimeout(context.Background(), 10*time.Second)
	defer cancel()
	out, err := exec.CommandContext(ctx, binary).CombinedOutput()
	if err != nil {
		t.Fatalf("native identity checks failed: %v\n%s", err, out)
	}
	var result struct {
		Username string `json:"username"`
		Token    string `json:"token"`
	}
	if err := json.Unmarshal(out, &result); err != nil {
		t.Fatal(err)
	}
	now := time.Unix(1735689600, 0)
	key, err := verifyAuthToken(result.Username, result.Token, "willow-fixture", now)
	if err != nil {
		t.Fatalf("Go verifier rejected native identity token: %v", err)
	}
	private := ed25519.NewKeyFromSeed(bytes.Repeat([]byte{3}, 32))
	want := strings.ToUpper(hex.EncodeToString(private.Public().(ed25519.PublicKey)))
	if key != want || result.Username != "v1_"+want {
		t.Fatal("native token changed the synthetic retained identity")
	}
	parts := strings.Split(result.Token, ".")
	if len(parts) != 3 || len(parts[2]) != 128 || parts[2] != strings.ToUpper(parts[2]) {
		t.Fatal("signature is not the MeshCore hexadecimal Ed25519 form")
	}
	payload, err := base64.RawURLEncoding.DecodeString(parts[1])
	if err != nil {
		t.Fatal(err)
	}
	var claims tokenClaims
	if err := json.Unmarshal(payload, &claims); err != nil {
		t.Fatal(err)
	}
	if claims.Issued != now.Unix() || claims.Expires != now.Add(24*time.Hour).Unix() {
		t.Fatal("native token validity changed")
	}
	for _, at := range []time.Time{now.Add(-time.Second), now.Add(24 * time.Hour)} {
		if _, err := verifyAuthToken(result.Username, result.Token, "willow-fixture", at); err == nil {
			t.Fatal("Go verifier accepted token outside its validity")
		}
	}
	if _, err := verifyAuthToken(result.Username, result.Token, "other-audience", now); err == nil {
		t.Fatal("Go verifier accepted a different audience")
	}
	t.Log("Native token preserves identity, audience, 24-hour validity and hexadecimal signature")
}
