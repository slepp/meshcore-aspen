package observer

import (
	"crypto/ed25519"
	"encoding/base64"
	"encoding/hex"
	"encoding/json"
	"errors"
	"fmt"
	"math"
	"strconv"
	"strings"
	"time"

	meshcore "github.com/meshcore-go/meshcore-go"
)

const PublicFormat = "observer-v1"
const CaptureFormat = "capture-v1"
const tokenLifetime = 24 * time.Hour
const renewalMargin = 5 * time.Minute

func publicFormat(format string) bool {
	return format == PublicFormat || format == CaptureFormat
}

type tokenClaims struct {
	PublicKey string `json:"publicKey"`
	Audience  string `json:"aud"`
	Issued    int64  `json:"iat"`
	Expires   int64  `json:"exp"`
}

// MeshCore uses a hex Ed25519 signature, not a base64url JWS signature.
// Sign must be the observer identity's signer; expanded keys are never exported.
func authToken(key, audience string, now time.Time, sign func([]byte) []byte) (string, error) {
	if sign == nil || audience == "" || now.Unix() < 1735689600 {
		return "", errors.New("observer JWT requires an identity signer, audience and synchronized clock")
	}
	payload, err := json.Marshal(tokenClaims{strings.ToUpper(key), audience, now.Unix(), now.Add(tokenLifetime).Unix()})
	if err != nil {
		return "", err
	}
	input := "eyJhbGciOiJFZDI1NTE5IiwidHlwIjoiSldUIn0." + base64.RawURLEncoding.EncodeToString(payload)
	signature := sign([]byte(input))
	public, err := hex.DecodeString(key)
	if err != nil || len(public) != ed25519.PublicKeySize || len(signature) != ed25519.SignatureSize || !ed25519.Verify(public, []byte(input), signature) {
		return "", errors.New("observer JWT signer does not match the observer public key")
	}
	return input + "." + strings.ToUpper(hex.EncodeToString(signature)), nil
}

func verifyAuthToken(username, token, audience string, now time.Time) (string, error) {
	parts := strings.Split(token, ".")
	if len(parts) != 3 || len(token) > 4096 || !strings.HasPrefix(username, "v1_") {
		return "", errors.New("invalid observer identity credentials")
	}
	var header struct {
		Algorithm string `json:"alg"`
		Type      string `json:"typ"`
	}
	head, err := base64.RawURLEncoding.DecodeString(parts[0])
	if err != nil || json.Unmarshal(head, &header) != nil || header.Algorithm != "Ed25519" || header.Type != "JWT" {
		return "", errors.New("invalid observer JWT header")
	}
	payload, err := base64.RawURLEncoding.DecodeString(parts[1])
	var claims tokenClaims
	if err != nil || json.Unmarshal(payload, &claims) != nil || claims.Audience != audience ||
		claims.PublicKey != strings.ToUpper(claims.PublicKey) || username != "v1_"+claims.PublicKey ||
		now.Unix() < 1735689600 || claims.Issued > now.Unix() || claims.Expires <= now.Unix() ||
		claims.Issued < 1735689600 || claims.Expires <= claims.Issued ||
		claims.Expires-claims.Issued > int64(tokenLifetime/time.Second) {
		return "", errors.New("invalid observer JWT identity, audience or validity")
	}
	key, err := hex.DecodeString(claims.PublicKey)
	signature, sigErr := hex.DecodeString(parts[2])
	if err != nil || sigErr != nil || len(key) != ed25519.PublicKeySize || len(signature) != ed25519.SignatureSize ||
		!ed25519.Verify(key, []byte(parts[0]+"."+parts[1]), signature) {
		return "", errors.New("invalid observer JWT signature")
	}
	return claims.PublicKey, nil
}

func (o *Observer) publicStatus(state string, now time.Time) string {
	payload, _ := json.Marshal(map[string]string{
		"status": state, "timestamp": now.UTC().Format(time.RFC3339Nano),
		"origin": o.cfg.Origin, "origin_id": strings.ToUpper(o.identity),
		"model": o.cfg.Model, "firmware_version": o.cfg.FirmwareVersion,
		"radio": o.cfg.Radio, "client_version": "meshcore-kiss/observer-v1",
	})
	return string(payload)
}

func (o *Observer) publicPacket(item observation) ([]byte, error) {
	packet, err := meshcore.PacketFromBytes(item.data)
	if err != nil {
		return nil, err
	}
	if item.timestamp.Unix() < 1735689600 || !item.hasSignal ||
		(item.snr == -32 && item.rssi == 127) ||
		math.IsNaN(float64(item.snr)) || math.IsInf(float64(item.snr), 0) ||
		(o.cfg.PacketFilter != nil && *o.cfg.PacketFilter&(uint16(1)<<packet.PayloadType()) == 0) {
		return nil, errors.New("packet has no synchronized RF reception or is filtered")
	}
	route := "F"
	if packet.RouteType() == 2 {
		route = "D"
	} else if packet.RouteType() == 3 && o.cfg.Format == CaptureFormat {
		route = "T"
	} else if packet.RouteType() == 3 {
		route = "D"
	}
	hash := packet.PacketHash()
	utc := item.timestamp.UTC()
	signal := fmt.Sprintf("%.1f", item.snr)
	if o.cfg.Format == CaptureFormat {
		signal = strconv.FormatFloat(float64(item.snr), 'f', -1, 32)
		if !strings.Contains(signal, ".") {
			signal += ".0"
		}
	}
	payload := map[string]any{
		"origin": o.cfg.Origin, "origin_id": strings.ToUpper(o.identity),
		"timestamp": utc.Format(time.RFC3339Nano), "type": "PACKET", "direction": "rx",
		"time": utc.Format("15:04:05"), "date": utc.Format("02/01/2006"),
		"len": fmt.Sprint(len(item.data)), "packet_type": fmt.Sprint(packet.PayloadType()),
		"route": route, "payload_len": fmt.Sprint(len(packet.Payload)),
		"raw": strings.ToUpper(hex.EncodeToString(item.data)),
		"SNR": signal, "RSSI": fmt.Sprint(item.rssi),
		"hash": strings.ToUpper(hex.EncodeToString(hash[:])),
	}
	if (packet.PathHashCount() > 0 && o.cfg.Format != CaptureFormat && route == "D") ||
		(o.cfg.Format == CaptureFormat && route == "D") {
		path := make([]string, 0, packet.PathHashCount())
		for _, hop := range packet.PathHashes() {
			path = append(path, hex.EncodeToString(hop))
		}
		if o.cfg.Format == CaptureFormat {
			payload["path"] = strings.Join(path, ",")
		} else {
			payload["path"] = path
		}
	}
	return json.Marshal(payload)
}
