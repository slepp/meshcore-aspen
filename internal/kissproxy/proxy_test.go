package kissproxy

import (
	"bytes"
	"crypto/ed25519"
	"encoding/hex"
	"testing"

	meshcore "github.com/meshcore-go/meshcore-go"
	"github.com/meshcore-go/meshcore-go/hardware"
	"meshcore.local/meshcore/internal/radio"
)

func TestBotCannotClaimQueuedConfigurationAuthority(t *testing.T) {
	config := hardware.RadioConfig{FreqHz: 910525000, BwHz: 62500, SF: 7, CR: 5}
	server := New(meshcore.NewLocalIdentityFromSeed([32]byte{1}), Config{Radio: config, TxPower: 22})
	setProfile := append([]byte{1, 1, 1, 0, 0, 0}, config.ToBytes()...)
	setProfile = append(setProfile, 22, 0, 0, 128, 63, 0, 0, 0)
	for _, tc := range []struct {
		name    string
		command byte
		payload []byte
		blocked bool
	}{
		{"claim-owner", radio.HWQueuedHello, []byte{1, 1}, true},
		{"change-profile", radio.HWQueuedConfig, setProfile, true},
		{"forge-role-presence", radio.HWQueuedRolePresence, append([]byte{1, 2}, make([]byte, 32)...), true},
		{"join", radio.HWQueuedHello, []byte{1, 0}, false},
		{"read-profile", radio.HWQueuedConfig, []byte{1, 0}, false},
		{"read-statistics", radio.HWQueuedStats, []byte{1}, false},
		{"set-own-budget", radio.HWQueuedSourcePolicy, []byte{1, 1, 0, 0, 0, 0, 0, 128, 63}, false},
	} {
		t.Run(tc.name, func(t *testing.T) {
			wire, handled := server.local(&hardware.KissFrame{
				Command: hardware.KISS_CMD_SETHARDWARE,
				Data:    append([]byte{tc.command}, tc.payload...),
			})
			if handled != tc.blocked {
				t.Fatalf("forwarding decision: handled=%v, want blocked=%v", handled, tc.blocked)
			}
			if tc.blocked {
				frame, err := hardware.DecodeFrame(wire)
				if err != nil || !bytes.Equal(frame.Data, []byte{hardware.HW_RESP_ERROR, hardware.HW_ERR_INVALID_PARAM}) {
					t.Fatalf("missing explicit shared-authority rejection: %v, %v", frame, err)
				}
			}
		})
	}
}

func TestUnannouncedBotKISSDataCanBeForwarded(t *testing.T) {
	server := New(meshcore.NewLocalIdentityFromSeed([32]byte{1}), Config{RequireParity: true})
	if _, handled := server.local(&hardware.KissFrame{
		Command: hardware.KISS_CMD_DATA, Data: []byte{0x15, 0, 1, 2},
	}); handled {
		t.Fatal("unannounced bot KISS data was blocked by presence policy")
	}
}

func TestQueuedBotSettingsRequirePhysicalConfirmation(t *testing.T) {
	config := hardware.RadioConfig{FreqHz: 910525000, BwHz: 62500, SF: 7, CR: 5}
	server := New(meshcore.NewLocalIdentityFromSeed([32]byte{1}), Config{
		Radio: config, TxPower: 22, RequireParity: true,
	})
	for _, data := range [][]byte{
		append([]byte{hardware.HW_CMD_SET_RADIO}, config.ToBytes()...),
		{hardware.HW_CMD_SET_TX_POWER, 22},
	} {
		if _, handled := server.local(&hardware.KissFrame{Command: hardware.KISS_CMD_SETHARDWARE, Data: data}); handled {
			t.Fatal("proxy fabricated success instead of checking the physical modem")
		}
	}
	config.FreqHz++
	if _, handled := server.local(&hardware.KissFrame{
		Command: hardware.KISS_CMD_SETHARDWARE,
		Data:    append([]byte{hardware.HW_CMD_SET_RADIO}, config.ToBytes()...),
	}); !handled {
		t.Fatal("conflicting bot tuning escaped to the modem")
	}
}

func TestBotIdentityCryptoAndSharedRadioProtection(t *testing.T) {
	id := meshcore.NewLocalIdentityFromSeed([32]byte{1, 2, 3})
	config := hardware.RadioConfig{FreqHz: 910525000, BwHz: 62500, SF: 7, CR: 5}
	server := New(id, Config{Radio: config, TxPower: 22})
	request := func(cmd byte, data []byte) []byte {
		t.Helper()
		reply, handled := server.local(&hardware.KissFrame{
			Command: hardware.KISS_CMD_SETHARDWARE, Data: append([]byte{cmd}, data...),
		})
		if !handled {
			t.Fatalf("command %x leaked to physical identity", cmd)
		}

		frame, err := hardware.DecodeFrame(reply)
		if err != nil {
			t.Fatal(err)
		}
		return frame.Data
	}
	if got := request(1, nil); !bytes.Equal(got, append([]byte{0x81}, id.PublicKeyBytes()...)) {
		t.Fatalf("incorrect bot identity: %x", got)
	}
	message := []byte("independent bot")
	signature := request(4, message)
	if signature[0] != 0x84 ||
		!ed25519.Verify(ed25519.PublicKey(id.PublicKeyBytes()), message, signature[1:]) {
		t.Fatal("signature is not from the bot identity")
	}
	// FIPS-197 AES-128 known answer; the protocol prepends its two-byte HMAC.
	key, _ := hex.DecodeString("000102030405060708090a0b0c0d0e0f")
	plaintext, _ := hex.DecodeString("00112233445566778899aabbccddeeff")
	key = append(key, make([]byte, 16)...)
	ciphertext := request(5, append(append([]byte{}, key...), plaintext...))
	if got := hex.EncodeToString(ciphertext[3:]); got != "69c4e0d86a7b0430d8cdb78070b4c55a" {
		t.Fatalf("AES wire ciphertext: %s", got)
	}
	decrypted := request(6, append(append([]byte{}, key...), ciphertext[1:]...))
	if !bytes.Equal(decrypted, append([]byte{0x86}, plaintext...)) {
		t.Fatalf("decrypt: %x", decrypted)
	}
	ciphertext[1] ^= 1
	if got := request(6, append(key, ciphertext[1:]...)); !bytes.Equal(got, []byte{0xf1, 4}) {
		t.Fatalf("invalid MAC accepted: %x", got)
	}
	if got := request(9, config.ToBytes()); !bytes.Equal(got, []byte{0xf0}) {
		t.Fatalf("matching shared tuning rejected: %x", got)
	}
	config.FreqHz++
	for _, command := range []struct {
		cmd  byte
		data []byte
	}{
		{9, config.ToBytes()}, {10, []byte{20}}, {24, nil},
	} {
		if got := request(command.cmd, command.data); !bytes.Equal(got, []byte{0xf1, 2}) {
			t.Fatalf("shared-modem mutation accepted: %x", got)
		}
	}
}

func TestLegacySettersFollowEffectivePHY(t *testing.T) {
	configured := hardware.RadioConfig{FreqHz: 910525000, BwHz: 62500, SF: 7, CR: 5}
	live, power, verified := hardware.RadioConfig{FreqHz: 915000000, BwHz: 250000, SF: 11, CR: 8}, uint8(14), true
	server := New(meshcore.NewLocalIdentityFromSeed([32]byte{1}), Config{
		Radio: configured, TxPower: 22, RequireParity: true,
		EffectivePHY: func() (hardware.RadioConfig, uint8, bool) { return live, power, verified },
	})
	rejected := func(data []byte) bool {
		t.Helper()
		reply, handled := server.local(&hardware.KissFrame{Command: hardware.KISS_CMD_SETHARDWARE, Data: data})
		if !handled {
			return false
		}
		frame, err := hardware.DecodeFrame(reply)
		if err != nil || !bytes.Equal(frame.Data, []byte{hardware.HW_RESP_ERROR, hardware.HW_ERR_INVALID_PARAM}) {
			t.Fatalf("unexpected local reply %x %v", reply, err)
		}
		return true
	}
	setRadio := func(config hardware.RadioConfig) []byte {
		return append([]byte{hardware.HW_CMD_SET_RADIO}, config.ToBytes()...)
	}
	if rejected(setRadio(live)) || rejected([]byte{hardware.HW_CMD_SET_TX_POWER, 14}) {
		t.Fatal("effective profile was not forwarded for physical confirmation")
	}
	if !rejected(setRadio(configured)) || !rejected([]byte{hardware.HW_CMD_SET_TX_POWER, 22}) {
		t.Fatal("stale configured profile escaped to the modem")
	}
	verified = false
	if !rejected(setRadio(live)) || !rejected([]byte{hardware.HW_CMD_SET_TX_POWER, 14}) {
		t.Fatal("unverified profile escaped to the modem")
	}
}
