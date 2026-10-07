//go:build ignore

// Public deterministic fixtures. Never use these identities on a real network.
package main

import (
	"crypto/sha512"
	"encoding/base64"
	"encoding/binary"
	"encoding/hex"
	"encoding/json"
	meshcore "github.com/meshcore-go/meshcore-go"
	"os"
)

func main() {
	out := map[string]any{}
	peers := map[string]meshcore.LocalIdentity{}
	for name, n := range map[string]byte{"room": 1, "otherRoom": 2, "thirdRoom": 4, "author": 0, "reader": 3} {
		var seed [32]byte
		for i := 0; i < 32; i += 4 {
			seed[i] = n
		}
		key := sha512.Sum512(seed[:])
		key[0] &= 248
		key[31] &= 63
		key[31] |= 64
		id, err := meshcore.NewLocalIdentityFromExpandedKey(key[:])
		if err != nil {
			panic(err)
		}
		peers[name] = id
		out[name] = map[string]string{"key": hex.EncodeToString(key[:]), "publicKey": id.String()}
	}
	// Give SharedB the same short room prefix as A, with a distinct full key.
	for counter := uint32(1); ; counter++ {
		var seed [32]byte
		binary.LittleEndian.PutUint32(seed[:], counter)
		key := sha512.Sum512(seed[:])
		key[0] &= 248
		key[31] &= 63
		key[31] |= 64
		id, err := meshcore.NewLocalIdentityFromExpandedKey(key[:])
		if err != nil {
			panic(err)
		}
		if id.PublicKey()[0] == peers["room"].PublicKey()[0] && id.String() != peers["room"].String() {
			peers["otherRoom"] = id
			out["otherRoom"] = map[string]string{"key": hex.EncodeToString(key[:]), "publicKey": id.String()}
			break
		}
	}
	seal := func(roomName, client string, kind byte, plain []byte) string {
		room, peer := peers[roomName], peers[client]
		secret, err := peer.SharedSecret(room.Identity)
		if err != nil {
			panic(err)
		}
		cipher, err := meshcore.EncryptThenMAC(secret, plain)
		if err != nil {
			panic(err)
		}
		payload := []byte{room.PublicKey()[0]}
		route := byte(meshcore.RouteTypeDirect)
		if kind == meshcore.PayloadTypeAnonReq {
			payload = append(payload, peer.PublicKeyBytes()...)
			route = meshcore.RouteTypeFlood
		} else {
			payload = append(payload, peer.PublicKey()[0])
		}
		payload = append(payload, cipher...)
		p := meshcore.Packet{Header: meshcore.MakeHeader(route, kind, 0), PathLength: 0x82, Path: []byte{1, 2, 3, 4, 5, 6}, Payload: payload}
		b, err := p.ToBytes()
		if err != nil {
			panic(err)
		}
		return base64.StdEncoding.EncodeToString(b)
	}
	login := binary.LittleEndian.AppendUint32(nil, 1699999900)
	login = binary.LittleEndian.AppendUint32(login, 0)
	login = append(login, []byte("room\x00")...)
	out["authorLogin"] = seal("room", "author", meshcore.PayloadTypeAnonReq, login)
	out["readerLogin"] = seal("room", "reader", meshcore.PayloadTypeAnonReq, login)
	out["sharedReaderLogin"] = seal("otherRoom", "reader", meshcore.PayloadTypeAnonReq, login)
	for i, text := range []string{"hello", "second"} {
		body := binary.LittleEndian.AppendUint32(nil, 1700000000+uint32(i))
		body = append(body, 0)
		body = append(body, []byte(text)...)
		out[[]string{"post", "secondPost"}[i]] = seal("room", "author", meshcore.PayloadTypeTxtMsg, body)
	}
	body := binary.LittleEndian.AppendUint32(nil, 1700000100)
	body = append(body, 2)
	body = binary.LittleEndian.AppendUint32(body, 0)
	out["refresh"] = seal("room", "reader", meshcore.PayloadTypeReq, body)
	// Native PATH supplies the learned return route.
	out["path"] = seal("room", "reader", meshcore.PayloadTypePath, []byte{0x82, 6, 5, 4, 3, 2, 1, 15})
	enc := json.NewEncoder(os.Stdout)
	enc.SetIndent("", "  ")
	if err := enc.Encode(out); err != nil {
		panic(err)
	}
}
