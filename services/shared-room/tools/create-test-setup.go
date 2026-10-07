// Operator-owned local preparation; no upload or existing identity reads.
package main

import (
	"crypto/rand"
	"crypto/sha512"
	"encoding/base64"
	"encoding/hex"
	"encoding/json"
	"flag"
	"fmt"
	"os"
	"path/filepath"

	meshcore "github.com/meshcore-go/meshcore-go"
)

func random(size int) []byte {
	b := make([]byte, size)
	if _, err := rand.Read(b); err != nil {
		panic("secure random generation failed")
	}
	return b
}
func save(path string, value any) {
	b, err := json.MarshalIndent(value, "", "  ")
	if err != nil {
		panic("configuration encoding failed")
	}
	defer clear(b)
	f, err := os.OpenFile(path, os.O_WRONLY|os.O_CREATE|os.O_EXCL, 0600)
	if err != nil {
		panic("private configuration creation failed")
	}
	if _, err = f.Write(b); err == nil {
		err = f.Sync()
	}
	closeErr := f.Close()
	if err != nil || closeErr != nil {
		panic("private configuration persistence failed")
	}
}
func main() {
	dir := flag.String("directory", "", "new private local directory")
	flag.Parse()
	if *dir == "" || flag.NArg() != 0 {
		fmt.Fprintln(os.Stderr, "Specify --directory NEW_PRIVATE_DIRECTORY")
		os.Exit(2)
	}
	// Refuse existing directories/files rather than overwrite any prior identity.
	if err := os.Mkdir(*dir, 0700); err != nil {
		fmt.Fprintln(os.Stderr, "Cannot create a new private directory")
		os.Exit(1)
	}
	aliases := map[string]any{}
	keys := map[string]string{}
	prefixes := map[byte]bool{0: true, 255: true}
	for _, alias := range []string{"TestA", "TestB"} {
		for {
			seed := random(32)
			expanded := sha512.Sum512(seed)
			clear(seed)
			expanded[0] &= 248
			expanded[31] &= 63
			expanded[31] |= 64
			id, err := meshcore.NewLocalIdentityFromExpandedKey(expanded[:])
			if err != nil {
				panic("native identity construction failed")
			}
			if prefixes[id.PublicKey()[0]] {
				clear(expanded[:])
				continue
			}
			prefixes[id.PublicKey()[0]] = true
			keys[alias] = hex.EncodeToString(expanded[:])
			clear(expanded[:])
			aliases[alias] = map[string]any{"backend": "AspenSharedTest", "publicKey": id.String(),
				"name": "AspenShared" + alias, "password": base64.RawURLEncoding.EncodeToString(random(10))}
			break
		}
	}
	frontends := map[string]any{}
	for _, name := range []string{"TestFront1", "TestFront2"} {
		frontends[name] = map[string]any{"token": hex.EncodeToString(random(32)), "aliases": []string{"TestA", "TestB"}}
	}
	save(filepath.Join(*dir, "service-config.json"), map[string]any{"ALIASES": aliases, "FRONTENDS": frontends})
	save(filepath.Join(*dir, "worker-room-keys.json"), map[string]any{"worker": "aspen-shared-room", "keyLocation": "worker", "expandedKeys": keys})
	save(filepath.Join(*dir, "manifest.json"), map[string]any{"worker": "aspen-shared-room", "keyLocation": "worker", "status": "local-preparation-only",
		"aliases": []string{"TestA", "TestB"}, "backend": "AspenSharedTest", "frontends": []string{"TestFront1", "TestFront2"}})
	fmt.Println("Prepared two new test identities and two alias-restricted frontend credentials in private local files. No upload or frontend key provisioning.")
}
