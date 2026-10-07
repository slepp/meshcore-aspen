package observer

import (
	"bytes"
	"context"
	"crypto/sha256"
	"crypto/tls"
	"encoding/base64"
	"encoding/hex"
	"encoding/json"
	"errors"
	"fmt"
	"io"
	"log/slog"
	"math"
	"net/http"
	"net/url"
	"os"
	"path/filepath"
	"slices"
	"strconv"
	"strings"
	"sync"
	"syscall"
	"time"

	mqtt "github.com/eclipse/paho.mqtt.golang"
	meshcore "github.com/meshcore-go/meshcore-go"
	"meshcore.local/meshcore/internal/state"
)

type ReflectorDestination struct {
	Name string   `json:"name"`
	URLs []string `json:"urls"`
}

type ReflectorConfig struct {
	StateDir     string                 `json:"state_dir"`
	SourceURL    string                 `json:"source_url"`
	UsernameEnv  string                 `json:"username_env"`
	PasswordEnv  string                 `json:"password_env"`
	NodeURL      string                 `json:"node_url"`
	PasswordFile string                 `json:"node_password_file"`
	TrustedLAN   bool                   `json:"trusted_management_lan"`
	Identity     string                 `json:"identity"`
	IATA         string                 `json:"iata"`
	Origin       string                 `json:"origin"`
	Destinations []ReflectorDestination `json:"destinations"`
}

func (c ReflectorConfig) Validate() error {
	key, err := hex.DecodeString(c.Identity)
	if err != nil || len(key) != 32 || c.Identity != strings.ToUpper(c.Identity) ||
		len(c.IATA) != 3 || strings.IndexFunc(c.IATA, func(r rune) bool { return r < 'A' || r > 'Z' }) >= 0 ||
		len(c.Origin) == 0 || len(c.Origin) > 31 || !filepath.IsAbs(c.StateDir) || !filepath.IsAbs(c.PasswordFile) ||
		c.UsernameEnv == "" || c.PasswordEnv == "" || len(c.Destinations) == 0 || len(c.Destinations) > 6 {
		return errors.New("reflector requires an uppercase observer key, IATA, origin, private absolute paths and 1..6 destinations")
	}
	source, err := url.Parse(c.SourceURL)
	if err != nil || source.Hostname() == "" || source.User != nil || source.RawQuery != "" ||
		source.Fragment != "" || (source.Path != "" && source.Path != "/") ||
		(source.Scheme != "tcp" && source.Scheme != "tls") {
		return errors.New("reflector source must be a tcp:// or tls:// MQTT URL without credentials")
	}
	node, err := url.Parse(c.NodeURL)
	if err != nil || node.Hostname() == "" || node.User != nil || node.RawQuery != "" ||
		node.Fragment != "" || node.Path != "" || (node.Scheme != "https" && !(node.Scheme == "http" && c.TrustedLAN)) {
		return errors.New("observer token endpoint requires HTTPS or explicit trusted_management_lan for HTTP")
	}
	names, urls := map[string]bool{}, map[string]bool{}
	for _, d := range c.Destinations {
		if len(d.Name) == 0 || len(d.Name) > 40 || strings.IndexFunc(d.Name, func(r rune) bool {
			return !(r >= 'a' && r <= 'z' || r >= '0' && r <= '9' || r == '-')
		}) >= 0 || names[d.Name] || len(d.URLs) == 0 || len(d.URLs) > 4 {
			return errors.New("reflector destinations require unique lowercase names and 1..4 failover URLs")
		}
		names[d.Name] = true
		for _, address := range d.URLs {
			u, err := url.Parse(address)
			if err != nil || u.Scheme != "wss" || u.Hostname() == "" || u.User != nil ||
				u.RawQuery != "" || u.Fragment != "" || urls[address] {
				return errors.New("reflector public destinations require unique wss:// URLs without credentials")
			}
			urls[address] = true
		}
	}
	return nil
}

func LoadReflectorConfig(path string) (ReflectorConfig, error) {
	var cfg ReflectorConfig
	raw, err := readReflectorFile(path, 16384)
	if err == nil {
		err = strictJSON(raw, &cfg)
	}
	if err == nil {
		err = cfg.Validate()
	}
	return cfg, err
}

func readReflectorFile(path string, maximum int64) ([]byte, error) {
	f, err := os.OpenFile(path, os.O_RDONLY|syscall.O_NOFOLLOW, 0)
	if err != nil {
		return nil, err
	}
	defer f.Close()
	info, err := f.Stat()
	if err != nil {
		return nil, err
	}
	owner, ok := info.Sys().(*syscall.Stat_t)
	if !ok || owner.Uid != uint32(os.Geteuid()) || !info.Mode().IsRegular() || info.Mode().Perm()&0077 != 0 || info.Size() > maximum {
		return nil, errors.New("reflector file must be a bounded private regular file owned by the current user")
	}
	raw, err := io.ReadAll(io.LimitReader(f, maximum+1))
	if int64(len(raw)) > maximum {
		return nil, errors.New("reflector file exceeds its size limit")
	}
	return raw, err
}

func strictJSON(raw []byte, value any) error {
	decoder := json.NewDecoder(bytes.NewReader(raw))
	decoder.DisallowUnknownFields()
	if err := decoder.Decode(value); err != nil {
		return err
	}
	var extra any
	if err := decoder.Decode(&extra); err != io.EOF {
		return errors.New("expected exactly one JSON object")
	}
	return nil
}

type reflectedPacket struct {
	Origin     string   `json:"origin"`
	Identity   string   `json:"origin_id"`
	Timestamp  string   `json:"timestamp"`
	Type       string   `json:"type"`
	Direction  string   `json:"direction"`
	Time       string   `json:"time"`
	Date       string   `json:"date"`
	Length     string   `json:"len"`
	PacketType string   `json:"packet_type"`
	Route      string   `json:"route"`
	PayloadLen string   `json:"payload_len"`
	Raw        string   `json:"raw"`
	SNR        string   `json:"SNR"`
	RSSI       string   `json:"RSSI"`
	Hash       string   `json:"hash"`
	Path       []string `json:"path,omitempty"`
}

type reflectedStatus struct {
	Status   string `json:"status"`
	Origin   string `json:"origin"`
	Identity string `json:"origin_id"`
	Time     string `json:"timestamp"`
	Model    string `json:"model"`
	Firmware string `json:"firmware_version"`
	Radio    string `json:"radio"`
	Client   string `json:"client_version"`
}

func reflectionTime(value string) (time.Time, error) {
	t, err := time.Parse(time.RFC3339Nano, value)
	_, offset := t.Zone()
	if err != nil || t.Unix() < 1735689600 || offset != 0 || t.After(time.Now().Add(time.Minute)) {
		return time.Time{}, errors.New("observer timestamp requires usable UTC reception time")
	}
	return t, nil
}

func validateReflection(cfg ReflectorConfig, topic string, raw []byte) ([]byte, bool, error) {
	base := "meshcore/" + cfg.IATA + "/" + cfg.Identity
	if len(raw) == 0 || len(raw) > 2048 {
		return nil, false, errors.New("observer message exceeds 1..2048 bytes")
	}
	if topic == base+"/status" {
		var s reflectedStatus
		if strictJSON(raw, &s) != nil || (s.Status != "online" && s.Status != "offline") ||
			s.Identity != cfg.Identity || s.Origin != cfg.Origin || s.Model == "" ||
			s.Firmware == "" || s.Radio == "" || s.Client != "meshcore-kiss/observer-v1" {
			return nil, false, errors.New("observer status identity or public schema mismatch")
		}
		if _, err := reflectionTime(s.Time); err != nil {
			return nil, false, err
		}
		payload, err := json.Marshal(s)
		return payload, true, err
	}
	if topic != base+"/packets" {
		return nil, false, errors.New("only the selected observer's packets/status topics may be reflected")
	}
	var p reflectedPacket
	if strictJSON(raw, &p) != nil || p.Identity != cfg.Identity || p.Origin != cfg.Origin ||
		p.Type != "PACKET" || p.Direction != "rx" {
		return nil, false, errors.New("observer packet identity, RF direction or public schema mismatch")
	}
	t, err := reflectionTime(p.Timestamp)
	if err != nil || p.Time != t.UTC().Format("15:04:05") || p.Date != t.UTC().Format("02/01/2006") {
		return nil, false, errors.New("observer packet UTC date/time mismatch")
	}
	wire, err := hex.DecodeString(p.Raw)
	if err != nil || p.Raw != strings.ToUpper(p.Raw) || len(wire) < 3 || len(wire) > 255 || wire[0]>>6 != 0 {
		return nil, false, errors.New("observer raw packet envelope invalid")
	}
	offset := 1
	if wire[0]&3 == 0 || wire[0]&3 == 3 {
		offset = 5
	}
	if offset >= len(wire) || wire[offset]>>6 == 3 {
		return nil, false, errors.New("observer raw packet path invalid")
	}
	pathBytes := int(wire[offset]&63) * int((wire[offset]>>6)+1)
	if pathBytes > 64 || offset+1+pathBytes >= len(wire) || len(wire)-offset-1-pathBytes > 184 {
		return nil, false, errors.New("observer raw packet lengths invalid")
	}
	packet, err := meshcore.PacketFromBytes(wire)
	if err != nil {
		return nil, false, errors.New("observer raw packet cannot be decoded")
	}
	hash := packet.PacketHash()
	route := "F"
	if packet.RouteType() == 2 || packet.RouteType() == 3 {
		route = "D"
	}
	if p.Length != strconv.Itoa(len(wire)) || p.PayloadLen != strconv.Itoa(len(packet.Payload)) ||
		p.PacketType != strconv.Itoa(int(packet.PayloadType())) || p.Route != route ||
		p.Hash != strings.ToUpper(hex.EncodeToString(hash[:])) {
		return nil, false, errors.New("observer packet metadata differs from raw RF bytes")
	}
	snr, snrErr := strconv.ParseFloat(p.SNR, 32)
	rssi, rssiErr := strconv.Atoi(p.RSSI)
	if snrErr != nil || rssiErr != nil || math.IsNaN(snr) || math.IsInf(snr, 0) ||
		rssi < -128 || rssi > 127 || (snr == -32 && rssi == 127) {
		return nil, false, errors.New("observer packet RF signal invalid or locally reflected")
	}
	var path []string
	if route == "D" {
		for _, hop := range packet.PathHashes() {
			path = append(path, hex.EncodeToString(hop))
		}
	}
	if !slices.Equal(path, p.Path) {
		return nil, false, errors.New("observer direct path differs from raw RF bytes")
	}
	payload, err := json.Marshal(p)
	return payload, false, err
}

type reflectionRecord struct {
	Topic   string          `json:"topic"`
	Payload json.RawMessage `json:"payload"`
	Pending []string        `json:"pending"`
}

type reflectionQueue struct {
	mu    sync.Mutex
	dir   string
	cfg   ReflectorConfig
	items map[string]reflectionRecord
}

func privateReflectionDirectory(path string) error {
	info, err := os.Lstat(path)
	if err != nil {
		return err
	}
	owner, ok := info.Sys().(*syscall.Stat_t)
	if !ok || !info.IsDir() || info.Mode().Perm()&0077 != 0 || owner.Uid != uint32(os.Geteuid()) {
		return errors.New("reflector queue requires an owner-only directory, not a symlink")
	}
	return nil
}

func openReflectionQueue(cfg ReflectorConfig) (*reflectionQueue, *os.File, error) {
	if err := privateReflectionDirectory(cfg.StateDir); err != nil && !errors.Is(err, os.ErrNotExist) {
		return nil, nil, err
	}
	lock, err := state.Lock(cfg.StateDir)
	if err != nil {
		return nil, nil, err
	}
	if err := privateReflectionDirectory(cfg.StateDir); err != nil {
		lock.Close()
		return nil, nil, err
	}
	q := &reflectionQueue{dir: cfg.StateDir, cfg: cfg, items: map[string]reflectionRecord{}}
	entries, err := os.ReadDir(q.dir)
	if err == nil {
		for _, entry := range entries {
			if !strings.HasSuffix(entry.Name(), ".json") {
				continue
			}
			name := entry.Name()
			if name != "status.json" && (len(name) != 69 || strings.Trim(name[:64], "0123456789abcdef") != "") {
				err = errors.New("reflector queue has an unexpected record filename")
				break
			}
			var record reflectionRecord
			var raw []byte
			raw, err = readReflectorFile(filepath.Join(q.dir, name), 4096)
			if err == nil {
				err = strictJSON(raw, &record)
			}
			if err == nil {
				var payload []byte
				var status bool
				payload, status, err = validateReflection(cfg, record.Topic, record.Payload)
				if err == nil && (status != (name == "status.json") ||
					(!status && name != reflectionID(record.Topic, payload)+".json")) {
					err = errors.New("reflector queue record does not match its validated payload")
				}
				record.Payload = payload
			}
			if err == nil {
				seen := map[string]bool{}
				for _, dest := range record.Pending {
					known := slices.ContainsFunc(cfg.Destinations, func(d ReflectorDestination) bool { return d.Name == dest })
					if !known || seen[dest] {
						err = errors.New("queued destinations differ from configuration; retain configuration while draining")
						break
					}
					seen[dest] = true
				}
			}
			if err != nil {
				break
			}
			q.items[name] = record
		}
	}
	if err == nil && len(q.items) > MaxQueueSize+1 {
		err = errors.New("reflector queue exceeds 4096 packets plus one status")
	}
	if err != nil {
		lock.Close()
		return nil, nil, fmt.Errorf("reading durable reflector queue: %w", err)
	}
	return q, lock, nil
}

func reflectionID(topic string, payload []byte) string {
	hash := sha256.Sum256(append(append([]byte(topic), 0), payload...))
	return hex.EncodeToString(hash[:])
}

func (q *reflectionQueue) enqueue(topic string, raw []byte) error {
	payload, status, err := validateReflection(q.cfg, topic, raw)
	if err != nil {
		return err
	}
	q.mu.Lock()
	defer q.mu.Unlock()
	name := reflectionID(topic, payload) + ".json"
	if status {
		name = "status.json"
	} else if _, exists := q.items[name]; exists {
		return nil
	} else {
		packets := len(q.items)
		if _, exists := q.items["status.json"]; exists {
			packets--
		}
		if packets >= MaxQueueSize {
			return errors.New("reflector queue full: 4096 packets; incoming reception dropped")
		}
	}
	record := reflectionRecord{Topic: topic, Payload: payload}
	for _, d := range q.cfg.Destinations {
		record.Pending = append(record.Pending, d.Name)
	}
	if err := state.WriteJSON(filepath.Join(q.dir, name), record); err != nil {
		return fmt.Errorf("saving reflector reception: %w", err)
	}
	q.items[name] = record
	return nil
}

func (q *reflectionQueue) next(destination string) (string, reflectionRecord) {
	q.mu.Lock()
	defer q.mu.Unlock()
	if status, ok := q.items["status.json"]; ok && slices.Contains(status.Pending, destination) {
		return "status.json", status
	}
	names := make([]string, 0, len(q.items))
	for name, record := range q.items {
		if name != "status.json" && slices.Contains(record.Pending, destination) {
			names = append(names, name)
		}
	}
	slices.Sort(names)
	if len(names) != 0 {
		return names[0], q.items[names[0]]
	}
	return "", reflectionRecord{}
}

func (q *reflectionQueue) status() (reflectionRecord, bool) {
	q.mu.Lock()
	defer q.mu.Unlock()
	record, ok := q.items["status.json"]
	return record, ok
}

func (q *reflectionQueue) acknowledge(name, destination string, sent reflectionRecord) error {
	q.mu.Lock()
	defer q.mu.Unlock()
	record, exists := q.items[name]
	if !exists || !bytes.Equal(record.Payload, sent.Payload) {
		return nil
	}
	record.Pending = slices.DeleteFunc(slices.Clone(record.Pending), func(d string) bool { return d == destination })
	if len(record.Pending) == 0 && name != "status.json" {
		if err := os.Remove(filepath.Join(q.dir, name)); err != nil {
			return err
		}
		dir, err := os.Open(q.dir)
		if err != nil {
			return err
		}
		err = errors.Join(dir.Sync(), dir.Close())
		if err != nil {
			return err
		}
		delete(q.items, name)
		return nil
	}
	if err := state.WriteJSON(filepath.Join(q.dir, name), record); err != nil {
		return err
	}
	q.items[name] = record
	return nil
}

type mastTokenSource struct {
	mu     sync.Mutex
	cfg    ReflectorConfig
	client *http.Client
	last   time.Time
}

func (s *mastTokenSource) request(ctx context.Context, route, session string, body []byte) ([]byte, error) {
	request, err := http.NewRequestWithContext(ctx, http.MethodPost, s.cfg.NodeURL+route, bytes.NewReader(body))
	if err != nil {
		return nil, err
	}
	if session != "" {
		request.Header.Set("X-Mast-Session", session)
	}
	response, err := s.client.Do(request)
	if err != nil {
		return nil, errors.New("observer token endpoint connection failed")
	}
	defer response.Body.Close()
	data, err := io.ReadAll(io.LimitReader(response.Body, 1025))
	if err != nil || len(data) > 1024 {
		return nil, errors.New("observer token endpoint response incomplete or oversized")
	}
	if response.StatusCode != http.StatusOK {
		return nil, fmt.Errorf("observer token endpoint %s returned HTTP %d", route, response.StatusCode)
	}
	return data, nil
}

func (s *mastTokenSource) token(ctx context.Context, audience string, logger *slog.Logger) (string, time.Time, error) {
	s.mu.Lock()
	defer s.mu.Unlock()
	if wait := time.Until(s.last.Add(2100 * time.Millisecond)); wait > 0 && !reflectorPause(ctx, wait) {
		return "", time.Time{}, ctx.Err()
	}
	password, err := readReflectorFile(s.cfg.PasswordFile, 15)
	if err != nil || len(password) == 0 || bytes.IndexFunc(password, func(r rune) bool { return r < 32 || r > 126 }) >= 0 {
		return "", time.Time{}, errors.New("observer token login requires a private 1..15-byte printable password file")
	}
	session, err := s.request(ctx, "/admin/login", "", password)
	clear(password)
	if err != nil {
		return "", time.Time{}, err
	}
	if len(session) != 32 || strings.Trim(string(session), "0123456789abcdef") != "" {
		return "", time.Time{}, errors.New("observer token login returned an invalid session")
	}
	defer func() {
		logoutCtx, cancel := context.WithTimeout(context.Background(), 5*time.Second)
		defer cancel()
		if _, err := s.request(logoutCtx, "/admin/logout", string(session), nil); err != nil {
			logger.Warn("observer token logout failed; session will expire", "error", err)
		}
		clear(session)
	}()
	raw, err := s.request(ctx, "/admin/observer-token", string(session), []byte(audience))
	s.last = time.Now()
	if err != nil {
		return "", time.Time{}, err
	}
	token := string(raw)
	clear(raw)
	key, err := verifyAuthToken("v1_"+s.cfg.Identity, token, audience, time.Now())
	if err != nil || key != s.cfg.Identity {
		return "", time.Time{}, errors.New("radio observer token signature, identity, audience or UTC invalid")
	}
	payload, err := decodeTokenClaims(token)
	if err != nil {
		return "", time.Time{}, errors.New("radio observer token claims invalid")
	}
	return token, time.Unix(payload.Expires, 0).Add(-renewalMargin), nil
}

func decodeTokenClaims(token string) (tokenClaims, error) {
	var claims tokenClaims
	parts := strings.Split(token, ".")
	if len(parts) != 3 {
		return claims, errors.New("invalid token framing")
	}
	payload, err := base64.RawURLEncoding.DecodeString(parts[1])
	if err == nil {
		err = json.Unmarshal(payload, &claims)
	}
	return claims, err
}

func reflectorPause(ctx context.Context, delay time.Duration) bool {
	timer := time.NewTimer(delay)
	defer timer.Stop()
	select {
	case <-ctx.Done():
		return false
	case <-timer.C:
		return true
	}
}

func reflectorPresence(record reflectionRecord, forceOffline bool) []byte {
	var status reflectedStatus
	if err := json.Unmarshal(record.Payload, &status); err != nil {
		return nil
	}
	t, _ := time.Parse(time.RFC3339Nano, status.Time)
	if forceOffline || (status.Status == "online" && time.Since(t) > 3*time.Minute) {
		status.Status = "offline"
		status.Time = time.Now().UTC().Format(time.RFC3339Nano)
	}
	payload, _ := json.Marshal(status)
	return payload
}

func reflectDestination(ctx context.Context, cfg ReflectorConfig, destination ReflectorDestination,
	q *reflectionQueue, signer *mastTokenSource, logger *slog.Logger) error {
	base := "meshcore/" + cfg.IATA + "/" + cfg.Identity
	var client mqtt.Client
	var renewAt, issuedAt time.Time
	var lastOnline bool
	index, failures, published := 0, 0, uint64(0)
	defer func() {
		if client != nil {
			if status, ok := q.status(); ok && client.IsConnectionOpen() {
				client.Publish(base+"/status", 1, true, reflectorPresence(status, true)).WaitTimeout(ioTimeout)
			}
			client.Disconnect(100)
		}
	}()
	for ctx.Err() == nil {
		status, haveStatus := q.status()
		if !haveStatus {
			if !reflectorPause(ctx, time.Second) {
				break
			}
			continue
		}
		now := time.Now()
		if client == nil || !client.IsConnectionOpen() || !now.Before(renewAt) || now.UnixNano() < issuedAt.UnixNano() {
			if client != nil {
				client.Disconnect(0)
			}
			address := destination.URLs[index]
			u, _ := url.Parse(address)
			password, renewal, err := signer.token(ctx, u.Hostname(), logger)
			if err == nil {
				opts := mqtt.NewClientOptions().AddBroker(address).SetClientID(cfg.Identity).
					SetUsername("v1_"+cfg.Identity).SetPassword(password).
					SetTLSConfig(&tls.Config{MinVersion: tls.VersionTLS12}).
					SetCleanSession(true).SetAutoReconnect(false).SetConnectRetry(false).
					SetConnectTimeout(ioTimeout).SetWriteTimeout(ioTimeout).
					SetKeepAlive(30*time.Second).SetPingTimeout(ioTimeout).
					SetWill(base+"/status", string(reflectorPresence(status, true)), 1, true)
				client = mqtt.NewClient(opts)
				if !waitToken(ctx, client.Connect()) {
					err = errors.New("MQTT CONNECT rejected or timed out; check observer authorization and WSS endpoint")
				} else if !waitToken(ctx, client.Publish(base+"/status", 1, true, reflectorPresence(status, false))) {
					err = errors.New("MQTT status PUBACK unavailable; retained status remains queued")
				}
			}
			if err != nil {
				if ctx.Err() != nil {
					break
				}
				if client != nil {
					client.Disconnect(0)
					client = nil
				}
				logger.Warn("observer destination unavailable", "destination", destination.Name, "broker", u.Hostname(), "error", err)
				index = (index + 1) % len(destination.URLs)
				failures++
				if !reflectorPause(ctx, time.Duration(min(failures, 12))*5*time.Second) {
					break
				}
				continue
			}
			renewAt, issuedAt, failures = renewal, time.Now(), 0
			lastOnline = bytes.Contains(reflectorPresence(status, false), []byte(`"status":"online"`))
			if err := q.acknowledge("status.json", destination.Name, status); err != nil {
				return err
			}
			logger.Info("observer destination connected; retained status acknowledged", "destination", destination.Name, "broker", u.Hostname())
		}
		presence := reflectorPresence(status, false)
		online := bytes.Contains(presence, []byte(`"status":"online"`))
		if online != lastOnline {
			if !waitToken(ctx, client.Publish(base+"/status", 1, true, presence)) {
				client.Disconnect(0)
				continue
			}
			lastOnline = online
		}
		name, record := q.next(destination.Name)
		if name == "" {
			if !reflectorPause(ctx, time.Second) {
				break
			}
			continue
		}
		payload := []byte(record.Payload)
		if name == "status.json" {
			payload = reflectorPresence(record, false)
		}
		if !waitToken(ctx, client.Publish(record.Topic, 1, name == "status.json", payload)) {
			client.Disconnect(0)
			continue
		}
		if err := q.acknowledge(name, destination.Name, record); err != nil {
			return fmt.Errorf("saving destination acknowledgement: %w", err)
		}
		if name != "status.json" {
			published++
			if published == 1 || published%100 == 0 {
				logger.Info("observer RF packets acknowledged", "destination", destination.Name, "packets", published)
			}
		}
	}
	return nil
}

func RunReflector(ctx context.Context, cfg ReflectorConfig, logger *slog.Logger) error {
	if err := cfg.Validate(); err != nil {
		return err
	}
	if logger == nil {
		logger = slog.Default()
	}
	username, password := os.Getenv(cfg.UsernameEnv), os.Getenv(cfg.PasswordEnv)
	if username == "" || password == "" {
		return errors.New("reflector source credentials are missing from the configured environment variables")
	}
	q, lock, err := openReflectionQueue(cfg)
	if err != nil {
		return err
	}
	defer lock.Close()
	ctx, cancel := context.WithCancel(ctx)
	defer cancel()
	signing := &mastTokenSource{cfg: cfg, client: &http.Client{
		Timeout: 10 * time.Second, CheckRedirect: func(*http.Request, []*http.Request) error { return http.ErrUseLastResponse },
	}}
	failures := make(chan error, len(cfg.Destinations)+1)
	var workers sync.WaitGroup
	defer workers.Wait()
	defer cancel()
	for _, destination := range cfg.Destinations {
		workers.Add(1)
		go func() {
			defer workers.Done()
			if err := reflectDestination(ctx, cfg, destination, q, signing, logger); err != nil {
				failures <- err
			}
		}()
	}
	opts := mqtt.NewClientOptions().AddBroker(cfg.SourceURL).SetClientID("reflector-" + cfg.Identity).
		SetUsername(username).SetPassword(password).SetTLSConfig(&tls.Config{MinVersion: tls.VersionTLS12}).
		SetCleanSession(true).SetAutoReconnect(true).SetConnectRetry(false).
		SetConnectTimeout(ioTimeout).SetWriteTimeout(ioTimeout).
		SetKeepAlive(20 * time.Second).SetPingTimeout(ioTimeout)
	opts.SetConnectionLostHandler(func(_ mqtt.Client, _ error) {
		logger.Warn("local observer MQTT connection lost; reconnecting")
	})
	opts.SetOnConnectHandler(func(client mqtt.Client) {
		base := "meshcore/" + cfg.IATA + "/" + cfg.Identity
		token := client.SubscribeMultiple(map[string]byte{base + "/packets": 1, base + "/status": 1}, func(_ mqtt.Client, message mqtt.Message) {
			if err := q.enqueue(message.Topic(), message.Payload()); err != nil {
				// Payloads can contain private fields: log the condition, never their bytes.
				logger.Error("observer reception rejected or not saved", "topic", message.Topic(), "error", err)
				if errors.Is(err, state.ErrCommitIndeterminate) {
					select {
					case failures <- err:
					default:
					}
				}
			}
		})
		go func() {
			if !waitToken(ctx, token) {
				select {
				case failures <- errors.New("local observer subscription not acknowledged"):
				case <-ctx.Done():
				}
			} else {
				logger.Info("local observer feed subscribed", "topic", base)
			}
		}()
	})
	client := mqtt.NewClient(opts)
	if !waitToken(ctx, client.Connect()) {
		client.Disconnect(0)
		return errors.New("local observer MQTT connection failed; check source URL and credentials")
	}
	defer client.Disconnect(100)
	select {
	case <-ctx.Done():
		return nil
	case err := <-failures:
		return err
	}
}
