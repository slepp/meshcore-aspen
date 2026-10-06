// SPDX-License-Identifier: Apache-2.0
package nodebackup

import (
	"archive/tar"
	"bytes"
	"crypto/aes"
	"crypto/cipher"
	"crypto/hmac"
	"crypto/rand"
	"crypto/sha256"
	"errors"
	"fmt"
	"io"
	"sort"
	"strings"

	meshcore "github.com/meshcore-go/meshcore-go"
)

const RawLimit = 2 * 1024 * 1024
const FileLimit = RawLimit + (RawLimit+127)/128 + 120

func validName(name string) bool {
	if len(name) == 0 || len(name) > 99 {
		return false
	}
	for _, part := range strings.Split(name, "/") {
		if part == "" || part == "." || part == ".." {
			return false
		}
		for _, c := range part {
			if c < 32 || c > 126 || c == '\\' {
				return false
			}
		}
	}
	return name == "manifest.json" || strings.HasPrefix(name, "files/") ||
		strings.HasPrefix(name, "config/") || strings.HasPrefix(name, "identities/") || strings.HasPrefix(name, "nvs/")
}

func compress(raw []byte) []byte {
	encoded := make([]byte, 0, len(raw))
	for len(raw) > 0 {
		count := min(len(raw), 128)
		block := raw[:count]
		for offset := 0; offset < count; {
			run := 1
			for offset+run < count && block[offset+run] == block[offset] {
				run++
			}
			if run >= 3 {
				encoded = append(encoded, 128|byte(run-1), block[offset])
				offset += run
				continue
			}
			first := offset
			offset += run
			for offset < count {
				run = 1
				for offset+run < count && block[offset+run] == block[offset] {
					run++
				}
				if run >= 3 {
					break
				}
				offset += run
			}
			encoded = append(encoded, byte(offset-first-1))
			encoded = append(encoded, block[first:offset]...)
		}
		raw = raw[count:]
	}
	return encoded
}

// Encode produces the same tar/RLE/AES-CTR/HMAC envelope as the on-device writer.
func Encode(records map[string][]byte, recipient [32]byte) ([]byte, error) {
	if len(records) == 0 || len(records) > 512 || records["manifest.json"] == nil {
		return nil, errors.New("backup inventory requires a manifest and at most 512 records")
	}
	names := make([]string, 0, len(records))
	total := 1024
	for name, value := range records {
		if !validName(name) || len(value) > RawLimit || total > RawLimit-512-((len(value)+511)/512)*512 {
			return nil, errors.New("backup filename, record count or decoded size exceeds format limits")
		}
		total += 512 + ((len(value)+511)/512)*512
		names = append(names, name)
	}
	sort.Strings(names)
	var archive bytes.Buffer
	writer := tar.NewWriter(&archive)
	for _, name := range names {
		value := records[name]
		if err := writer.WriteHeader(&tar.Header{Name: name, Mode: 0600, Size: int64(len(value)), Format: tar.FormatUSTAR}); err != nil {
			return nil, fmt.Errorf("backup header %s: %w", name, err)
		}
		if _, err := writer.Write(value); err != nil {
			return nil, err
		}
	}
	if err := writer.Close(); err != nil {
		return nil, err
	}
	raw := archive.Bytes()
	defer clear(raw)
	encoded := compress(raw)
	defer clear(encoded)
	ephemeral, err := meshcore.GenerateLocalIdentity(rand.Reader)
	if err != nil {
		return nil, err
	}
	shared, err := ephemeral.SharedSecret(meshcore.NewIdentity(recipient))
	if err != nil {
		return nil, fmt.Errorf("backup recipient key: %w", err)
	}
	defer clear(shared[:])
	result := make([]byte, 88+len(encoded), 120+len(encoded))
	copy(result, []byte{'M', 'C', 'B', 1, 1, 0, 0, 0})
	key := ephemeral.PublicKey()
	copy(result[8:40], key[:])
	copy(result[40:72], recipient[:])
	if _, err := io.ReadFull(rand.Reader, result[72:88]); err != nil {
		return nil, err
	}
	encryptionKey := sha256.Sum256(append([]byte("meshcore-backup-v1 encryption"), shared[:]...))
	defer clear(encryptionKey[:])
	block, err := aes.NewCipher(encryptionKey[:16])
	if err != nil {
		return nil, err
	}
	cipher.NewCTR(block, result[72:88]).XORKeyStream(result[88:], encoded)
	authKey := sha256.Sum256(append([]byte("meshcore-backup-v1 authentication"), shared[:]...))
	defer clear(authKey[:])
	mac := hmac.New(sha256.New, authKey[:])
	_, _ = mac.Write(result)
	return mac.Sum(result), nil
}
