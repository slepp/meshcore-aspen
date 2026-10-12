// This executable is a test oracle only; the Hew service never invokes Go.
package main

import (
	"bytes"
	"crypto/aes"
	"encoding/binary"
	"encoding/hex"
	"encoding/json"
	"fmt"
	"os"

	mesh "github.com/meshcore-go/meshcore-go"
	"meshcore.local/meshcore/internal/policy"
	"meshcore.local/meshcore/internal/state"
)

func main() {
	if root := os.Getenv("MESHCORE_WILLOW_IDENTITY_BINDINGS"); root != "" {
		identityBindings(root)
		return
	}
	id := func(n byte) mesh.LocalIdentity {
		var seed [32]byte
		for i := range seed {
			seed[i] = n
		}
		return mesh.NewLocalIdentityFromSeed(seed)
	}
	role, alice, bob := id(1), id(2), id(3)
	out := map[string]any{
		"role": role.String(), "alice": alice.String(), "bob": bob.String(),
	}
	key, _ := mesh.DeriveSharedSecret(bytes.Repeat([]byte{1}, 32), alice.PublicKeyBytes())
	out["secret"] = hex.EncodeToString(key)
	var cipher []map[string]string
	for size := 1; size <= 167; size++ {
		data := make([]byte, size)
		for i := range data {
			data[i] = byte(i*31 + size)
		}
		sealed, _ := mesh.EncryptThenMAC(key, data)
		cipher = append(cipher, map[string]string{"plain": hex.EncodeToString(data), "sealed": hex.EncodeToString(sealed)})
	}
	out["cipher"] = cipher
	ad := mesh.Advert{PublicKey: role.Identity, Timestamp: 1000, RawAppData: append([]byte{0x83}, []byte("Hew Room")...)}
	ad.SignWith(role)
	payload, _ := ad.ToBytes()
	p := &mesh.Packet{Header: mesh.MakeHeader(1, 4, 0), Payload: payload}
	raw, _ := p.ToBytes()
	out["advert"] = hex.EncodeToString(raw)
	var packets []string
	for width := 1; width <= 3; width++ {
		for count := 0; count*width <= 64 && count <= 63; count++ {
			for route := byte(0); route <= 3; route++ {
				p := &mesh.Packet{Header: mesh.MakeHeader(route, 5, 0), PathLength: byte((width-1)<<6 | count),
					Path: bytes.Repeat([]byte{0xa5}, width*count), Payload: []byte{1, 2, 0xc0, 0xdb},
					TransportCode1: 321, TransportCode2: 123}
				raw, _ := p.ToBytes()
				packets = append(packets, hex.EncodeToString(raw))
			}
		}
	}
	out["packets"] = packets
	region := policy.AutoScope("#test")
	out["region"] = hex.EncodeToString(region.Key[:])
	var forwarding []map[string]any
	for width := byte(1); width <= 3; width++ {
		for count := byte(0); count < 23; count++ {
			path := bytes.Repeat([]byte{0xa5}, int(width)*int(count))
			parsed, err := policy.NewPath(width, path)
			if err != nil {
				continue
			}
			for _, scoped := range []bool{false, true} {
				prefs := policy.Preferences{Repeat: true, FloodMaxHops: 16, UnscopedMaxHops: 16, AdvertMaxHops: 8, Loop: policy.LoopStrict}
				ctx := policy.ReceiveContext{Route: policy.Flood, Path: parsed, Unscoped: true}
				p := &mesh.Packet{Header: mesh.MakeHeader(1, 5, 0), PathLength: parsed.Encoded(), Path: path, Payload: []byte{byte(width), count}}
				if scoped {
					ctx.Route = policy.TransportFlood
					ctx.ScopeKnown = true
					ctx.Scope = region
					p.Header = mesh.MakeHeader(0, 5, 0)
					p.TransportCode1 = policy.TransportCode(region, 5, p.Payload)
					p.TransportCode2 = p.TransportCode1
				}
				allowed := policy.AllowForward(policy.Repeater, prefs, ctx, 5, role.PublicKey())
				raw, _ := p.ToBytes()
				want := ""
				if allowed {
					p.PathLength++
					p.Path = append(p.Path, role.PublicKeyBytes()[:width]...)
					b, _ := p.ToBytes()
					want = hex.EncodeToString(b)
				}
				forwarding = append(forwarding, map[string]any{"scoped": scoped, "raw": hex.EncodeToString(raw), "want": want})
			}
		}
	}
	out["forwarding"] = forwarding
	out["ack"] = fmt.Sprintf("%08x", mesh.CalcAckHash([]byte("test"), alice.PublicKeyBytes()))
	var keep [9]byte
	binary.LittleEndian.PutUint32(keep[:], 20)
	keep[4] = 2
	out["keepalive"] = hex.EncodeToString(keep[:])
	json.NewEncoder(os.Stdout).Encode(out)
}

func identityBindings(root string) {
	peer := mesh.NewLocalIdentityFromSeed([32]byte(bytes.Repeat([]byte{42}, 32)))
	message := []byte("Willow key check")
	out := map[string]map[string]string{}
	for _, role := range []string{"repeater", "room", "observer"} {
		id, err := state.Identity(root, role)
		if err != nil {
			panic(err)
		}
		shared, err := id.SharedSecret(peer.Identity)
		if err != nil {
			panic(err)
		}
		cipher, err := aes.NewCipher(shared[:16])
		if err != nil {
			panic(err)
		}
		sealed := make([]byte, 16)
		cipher.Encrypt(sealed, message)
		out[role] = map[string]string{"public": id.String(), "signature": hex.EncodeToString(id.Sign(message)),
			"cipher": hex.EncodeToString(sealed)}
	}
	json.NewEncoder(os.Stdout).Encode(out)
}
