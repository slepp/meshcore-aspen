// SPDX-License-Identifier: Apache-2.0
package roles

import (
	"strings"
	"sync/atomic"
	"testing"
)

func TestNodeBackupRequiresNativeAdministrator(t *testing.T) {
	for _, room := range []bool{false, true} {
		cfg := config(t)
		var admitted atomic.Int32
		cfg.BackupCommand = func(command string, radio bool, budget int) string {
			if command != "backup status" || !radio || (budget != 159 && budget != 145) {
				t.Errorf("unexpected backup call: %s radio=%v", command, radio)
			}
			admitted.Add(1)
			return "READY " + strings.Repeat("a", 16) + " bytes=384 sha=" + strings.Repeat("a", 64)
		}
		s, radio, id := startRole(t, room, cfg)
		admin, guest := fixedID(2), fixedID(3)
		loggedIn(t, s, radio, guest, id, room, 1, "room")
		radio.inject(t, textWire(t, guest, id, 2, 4, "backup status"), false)
		radio.quiet(t)
		if admitted.Load() != 0 {
			t.Fatal("guest reached backup service")
		}
		loggedIn(t, s, radio, admin, id, room, 1, "admin")
		for index, prefix := range []string{"ab|", "0123456789abcdef|"} {
			radio.inject(t, textWire(t, admin, id, uint32(index+2), 4, prefix+"backup status"), false)
			body := decrypted(t, radio.next(t), admin, id)
			if !strings.HasPrefix(string(cstring(body[5:])), prefix+"READY ") || admitted.Load() != int32(index+1) {
				t.Fatal("native administrator did not receive tagged backup status")
			}
		}
	}
}

func TestSplitCommandPreservesNativeAndOperatorTags(t *testing.T) {
	for _, test := range []struct{ command, prefix, body string }{
		{"ab|backup status", "ab|", "backup status"},
		{" \tab| get name", "ab|", "get name"},
		{"0123456789abcdef| backup status", "0123456789abcdef|", "backup status"},
		{"0123456789abcdeg|backup status", "", "0123456789abcdeg|backup status"},
		{"backup status", "", "backup status"},
	} {
		prefix, body := splitCommand(test.command)
		if prefix != test.prefix || body != test.body {
			t.Fatalf("split %q: %q %q", test.command, prefix, body)
		}

	}
}

func TestBase64BackupRepliesUseNativeTextCapacity(t *testing.T) {
	for _, room := range []bool{false, true} {
		cfg := config(t)
		cfg.BackupCommand = func(command string, radio bool, budget int) string {
			if !radio || command != "backup read64 aaaaaaaaaaaaaaaa 0" {
				t.Fatal("unexpected backup read")
			}
			header := "CHUNK64 aaaaaaaaaaaaaaaa 0 "
			count := (budget - len(header)) * 3 / 4
			return header + strings.Repeat("A", (count*4+2)/3)
		}
		s, radio, id := startRole(t, room, cfg)
		admin := fixedID(2)
		loggedIn(t, s, radio, admin, id, room, 1, "admin")
		for index, prefix := range []string{"", "ab|", "0123456789abcdef|"} {
			radio.inject(t, textWire(t, admin, id, uint32(index+2), 4,
				prefix+"backup read64 aaaaaaaaaaaaaaaa 0"), false)
			body := decrypted(t, radio.next(t), admin, id)
			reply := string(cstring(body[5:]))
			if !strings.HasPrefix(reply, prefix+"CHUNK64 ") || len(reply) < 161 || len(reply) > 162 {
				t.Fatalf("native backup response truncated: %q", reply)
			}
		}
	}
}
