package app

import (
	"bytes"
	"crypto/sha256"
	"encoding/binary"
	"encoding/hex"
	"fmt"
	"strings"
	"testing"
	"time"
)

func nativeBotDataRoundTrip(t *testing.T, admin func(string) string, bot string, seedSchedulers ...bool) func() {
	t.Helper()
	const size = 2422
	principal := strings.Repeat("ab", 32)
	await := func(prefix string) string {
		t.Helper()
		var reply string
		waitAuthority(t, func() bool {
			reply = admin("data status")
			if strings.HasPrefix(reply, "Error:") || strings.HasPrefix(reply, "UNKNOWN") ||
				(strings.HasPrefix(reply, "REJECTED") && prefix != "REJECTED") {
				t.Fatalf("native data operation failed: %s", reply)
			}
			return strings.HasPrefix(strings.Join(strings.Fields(reply), " "), strings.TrimSpace(prefix))
		})
		return reply
	}
	export := func(kind string) []byte {
		t.Helper()
		if reply := admin("data export " + kind + " caller " + principal); !strings.HasPrefix(reply, "PENDING") {
			t.Fatalf("native %s export not admitted: %s", kind, reply)
		}
		manifest := strings.Fields(await("EXPORTED "))
		if len(manifest) != 3 || len(manifest[1]) != 64 || manifest[2] != manifest[1][:16] {
			t.Fatalf("native data manifest invalid: %v", manifest)
		}
		var data []byte
		for offset := 0; offset < size; offset += 48 {
			reply := admin(fmt.Sprintf("data read %s %d", manifest[2], offset/48))
			if !strings.HasPrefix(reply, "DATA ") {
				t.Fatalf("native data chunk missing: %s", reply)
			}
			chunk, err := hex.DecodeString(strings.TrimPrefix(reply, "DATA "))
			if err != nil || len(chunk) != min(48, size-offset) {
				t.Fatalf("native data chunk shape: %d, %v", len(chunk), err)
			}
			data = append(data, chunk...)
		}
		digest := sha256.Sum256(data)
		envelope := sha256.Sum256(data[:size-32])
		magic := map[string]string{"kv": "BKD\x01", "timers": "BTD\x01", "reminders": "BRD\x01"}[kind]
		if len(data) != size || string(data[:4]) != magic ||
			hex.EncodeToString(data[4:36]) != bot ||
			hex.EncodeToString(data[37:69]) != principal ||
			fmt.Sprintf("%x", digest) != manifest[1] || !bytes.Equal(data[size-32:], envelope[:]) {
			t.Fatalf("native %s export identity, shape or digest differs", kind)
		}
		return data
	}
	stage := func(data []byte, accepted bool) string {
		t.Helper()
		digest := sha256.Sum256(data)
		hash := fmt.Sprintf("%x", digest)
		id := hash[:16]
		if reply := admin("data begin " + id + " " + hash); !strings.HasPrefix(reply, "UPLOADING "+id) {
			t.Fatalf("native data begin failed: %s", reply)
		}
		for offset := 0; offset < len(data); offset += 48 {
			end := min(offset+48, len(data))
			reply := admin(fmt.Sprintf("data chunk %s %d %x", id, offset/48, data[offset:end]))
			if reply != fmt.Sprintf("RECEIVED %d", end) {
				t.Fatalf("native data upload failed: %s", reply)
			}
		}
		if reply := admin("data stage " + id); !strings.HasPrefix(reply, "PENDING") {
			t.Fatalf("native data stage not admitted: %s", reply)
		}
		if accepted {
			await("STAGED " + id)
		} else {
			await("REJECTED")
		}
		return id
	}
	restore := func(id, policy string) {
		t.Helper()
		if reply := admin("data restore " + id + policy); !strings.HasPrefix(reply, "PENDING") {
			t.Fatalf("native data restore not admitted: %s", reply)
		}
		await("COMMITTED " + id)
	}
	data := export("kv")
	if data[69] != 0 {
		t.Fatal("fresh test principal already has KV records")
	}
	data[69] = 1
	copy(data[70:103], "host-note")
	copy(data[103:360], "retained through restart")
	seal := func(data []byte) {
		digest := sha256.Sum256(data[:size-32])
		copy(data[size-32:], digest[:])
	}
	seal(data)
	restore(stage(data, true), "")
	savedSchedulers := make(map[string][]byte)
	check := func() {
		t.Helper()
		if got := export("kv"); !bytes.Equal(got, data) {
			t.Fatal("native scoped data changed during restore or restart")
		}
		for kind, snapshot := range savedSchedulers {
			if got := export(kind); !bytes.Equal(got, snapshot) {
				t.Fatalf("native scoped %s changed during restart/rekey", kind)
			}
		}
	}
	check()
	foreign := append([]byte(nil), data...)
	foreign[4] ^= 1
	seal(foreign)
	stage(foreign, false)
	check()
	for _, kind := range []string{"timers", "reminders"} {
		snapshot := export(kind)
		if len(seedSchedulers) != 0 && seedSchedulers[0] {
			// Seed cancelled native records through the production restore
			// path, so identity retention is checked without scheduling RF.
			recordSize, digestOffset := 144, 112
			if kind == "reminders" {
				recordSize, digestOffset = 244, 212
			}
			record := snapshot[70 : 70+recordSize]
			magic := "BTM\x01"
			if kind == "reminders" {
				magic = "BRM\x01"
			}
			copy(record[:4], magic)
			copy(record[4:36], snapshot[4:36])
			copy(record[36:68], snapshot[37:69])
			due := uint32(time.Now().Unix()) + 60
			if kind == "timers" {
				copy(record[68:101], "old-private-timer")
				record[101], record[102] = snapshot[36], 3 // Cancelled.
				binary.LittleEndian.PutUint32(record[104:], due)
				binary.LittleEndian.PutUint32(record[108:], 1)
			} else {
				binary.LittleEndian.PutUint32(record[68:], 1)
				binary.LittleEndian.PutUint32(record[72:], 1)
				binary.LittleEndian.PutUint32(record[76:], due)
				binary.LittleEndian.PutUint32(record[80:], 1)
				record[84] = 4 // Cancelled.
				copy(record[88:212], "old private reminder")
			}
			digest := sha256.Sum256(record[:digestOffset])
			copy(record[digestOffset:], digest[:])
			snapshot[69] = 1
			seal(snapshot)
		}
		id := stage(snapshot, true)
		if reply := admin("data restore " + id); !strings.Contains(reply, "requires ID no-rearm") {
			t.Fatalf("native %s restore bypassed non-rearming policy: %s", kind, reply)
		}
		restore(id, " no-rearm")
		got := export(kind)
		if snapshot[69] == 0 && !bytes.Equal(got, snapshot) {
			t.Fatalf("empty native %s snapshot changed on restore", kind)
		}
		if got[69] != snapshot[69] {
			t.Fatalf("native %s restore lost scheduler record", kind)
		}
		savedSchedulers[kind] = got
	}
	return check
}
