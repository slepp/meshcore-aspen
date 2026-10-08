package main

import (
	"bytes"
	"context"
	"crypto/ed25519"
	"encoding/hex"
	"encoding/json"
	"errors"
	"fmt"
	"io"
	"net/http"
	"net/http/cookiejar"
	"net/url"
	"regexp"
	"strings"
	"sync"
	"time"
	"unicode/utf8"

	"github.com/gorilla/websocket"
)

const deviceProtocol = "aspen-room.device.v1"
const socketProtocol = "aspen-room.web.v1"
const maxSequence = 1<<53 - 1

var usernamePattern = regexp.MustCompile(`^[a-z0-9][a-z0-9_-]{2,63}$`)

type roomInfo struct {
	ID        string `json:"id"`
	Name      string `json:"name"`
	PublicKey string `json:"publicKey"`
}

type roomListing struct {
	Rooms          []roomInfo `json:"rooms"`
	MaxPostBytes   int        `json:"maxPostBytes"`
	LoginMode      string     `json:"loginMode"`
	DeviceProtocol string     `json:"deviceProtocol"`
}

type session struct {
	Author       string `json:"author"`
	Name         string `json:"name"`
	Username     string `json:"username"`
	Expires      int64  `json:"expires"`
	MaxPostBytes int    `json:"maxPostBytes"`
}

type message struct {
	Seq             int64   `json:"seq"`
	Timestamp       uint32  `json:"timestamp"`
	OriginAlias     string  `json:"originAlias"`
	Author          string  `json:"author"`
	ClientTimestamp uint32  `json:"clientTimestamp"`
	Text            string  `json:"text"`
	WebName         *string `json:"webName"`
}

type profile struct {
	PublicKey  string `json:"publicKey"`
	Name       string `json:"name"`
	AdvertType int    `json:"advertType"`
	Timestamp  int64  `json:"timestamp"`
	Source     string `json:"source"`
}

type historyPage struct {
	Messages []message `json:"messages"`
	Profiles []profile `json:"profiles"`
	More     bool      `json:"more"`
	Floor    int64     `json:"floor"`
}

type postResult struct {
	Message   message `json:"message"`
	Duplicate bool    `json:"duplicate"`
}

type apiError struct {
	Status int
	Text   string
}

func (e *apiError) Error() string { return e.Text }

func statusIs(err error, statuses ...int) bool {
	var api *apiError
	if !errors.As(err, &api) {
		return false
	}
	for _, status := range statuses {
		if api.Status == status {
			return true
		}
	}
	return false
}

type roomClient struct {
	origin  string
	base    *url.URL
	http    *http.Client
	cookieM sync.Mutex
	cookies map[string]*http.Cookie
}

func newClient(raw string) (*roomClient, error) {
	u, err := url.Parse(raw)
	if err != nil || u.Host == "" || u.User != nil || u.RawQuery != "" || u.Fragment != "" ||
		(u.Path != "" && u.Path != "/") || u.Opaque != "" {
		return nil, errors.New("use a service origin without credentials, path, query or fragment")
	}
	u.Scheme = strings.ToLower(u.Scheme)
	u.Host = strings.ToLower(u.Host)
	if strings.HasSuffix(u.Host, ":") {
		return nil, errors.New("service origin has an empty port")
	}
	if u.Scheme != "https" && (u.Scheme != "http" ||
		(u.Hostname() != "localhost" && u.Hostname() != "127.0.0.1" && u.Hostname() != "::1")) {
		return nil, errors.New("room login requires HTTPS; HTTP is allowed only on localhost")
	}
	u.Path = ""
	u.RawPath = ""
	if u.Scheme == "https" && u.Port() == "443" || u.Scheme == "http" && u.Port() == "80" {
		u.Host = u.Hostname()
		if strings.Contains(u.Host, ":") {
			u.Host = "[" + u.Host + "]"
		}
	}
	jar, err := cookiejar.New(nil)
	if err != nil {
		return nil, err
	}
	return &roomClient{origin: u.String(), base: u, cookies: make(map[string]*http.Cookie),
		http: &http.Client{Jar: jar, Timeout: 20 * time.Second, CheckRedirect: func(*http.Request, []*http.Request) error {
			return errors.New("room service redirected the request; use its final HTTPS origin")
		}}}, nil
}

func (c *roomClient) endpoint(alias, path string) string {
	return c.origin + "/v1/web/rooms/" + alias + "/" + path
}

func (c *roomClient) cookieForJar(cookie *http.Cookie) *http.Cookie {
	copy := *cookie
	if c.base.Scheme == "http" {
		copy.Secure = false // The Worker deliberately permits local HTTP development.
	}
	copy.MaxAge = 0
	return &copy
}

func validateCookie(cookie *http.Cookie) error {
	if cookie == nil || !strings.HasPrefix(cookie.Name, "aspen_room_") ||
		!aliasPattern.MatchString(strings.TrimPrefix(cookie.Name, "aspen_room_")) ||
		cookie.Domain != "" || !cookie.Secure || !cookie.HttpOnly ||
		cookie.Path != "/v1/web/rooms/"+strings.TrimPrefix(cookie.Name, "aspen_room_")+"/" ||
		(cookie.MaxAge >= 0 && cookie.Value != "" && !publicKeyPattern.MatchString(cookie.Value)) {
		return errors.New("room service returned an invalid scoped session cookie")
	}
	return nil
}

func (c *roomClient) restoreCookies(cookies []*http.Cookie) error {
	c.cookieM.Lock()
	defer c.cookieM.Unlock()
	for _, cookie := range cookies {
		if err := validateCookie(cookie); err != nil {
			return err
		}
		if cookie.MaxAge < 0 || cookie.Value == "" || !cookie.Expires.After(time.Now()) {
			continue
		}
		copy := *cookie
		c.cookies[copy.Name] = &copy
		c.http.Jar.SetCookies(c.base, []*http.Cookie{c.cookieForJar(&copy)})
	}
	return nil
}

func (c *roomClient) cookieSnapshot() []*http.Cookie {
	c.cookieM.Lock()
	defer c.cookieM.Unlock()
	result := make([]*http.Cookie, 0, len(c.cookies))
	for _, cookie := range c.cookies {
		if cookie.Expires.After(time.Now()) {
			copy := *cookie
			result = append(result, &copy)
		}
	}
	return result
}

func (c *roomClient) request(ctx context.Context, target string, body any, result any) error {
	var data []byte
	var err error
	method := http.MethodGet
	if body != nil {
		method = http.MethodPost
		data, err = json.Marshal(body)
		if err != nil {
			return err
		}
		defer clear(data)
	}
	req, err := http.NewRequestWithContext(ctx, method, target, bytes.NewReader(data))
	if err != nil {
		return err
	}
	req.Header.Set("Origin", c.origin)
	req.Header.Set("User-Agent", "aspen-room-tui/1")
	if body != nil {
		req.Header.Set("Content-Type", "application/json")
	}
	response, err := c.http.Do(req)
	if err != nil {
		return err
	}
	defer response.Body.Close()
	c.cookieM.Lock()
	for _, cookie := range response.Cookies() {
		if err := validateCookie(cookie); err != nil {
			c.cookieM.Unlock()
			return err
		}
		if cookie.MaxAge < 0 || cookie.Value == "" {
			delete(c.cookies, cookie.Name)
			c.http.Jar.SetCookies(c.base, []*http.Cookie{cookie})
		} else {
			if cookie.MaxAge > 0 {
				cookie.Expires = time.Now().Add(time.Duration(cookie.MaxAge) * time.Second)
			}
			cookie.MaxAge = 0
			c.cookies[cookie.Name] = cookie
			c.http.Jar.SetCookies(c.base, []*http.Cookie{c.cookieForJar(cookie)})
		}
	}
	c.cookieM.Unlock()
	raw, err := io.ReadAll(io.LimitReader(response.Body, 256*1024+1))
	if err != nil {
		return err
	}
	if len(raw) > 256*1024 {
		return errors.New("room response exceeds 256 KiB")
	}
	if response.StatusCode < 200 || response.StatusCode >= 300 {
		var failure struct {
			Error string `json:"error"`
		}
		if err := json.Unmarshal(raw, &failure); err != nil || failure.Error == "" {
			return &apiError{response.StatusCode, fmt.Sprintf("Room request failed (%d); response was not a JSON error", response.StatusCode)}
		}
		return &apiError{response.StatusCode, failure.Error}
	}
	if err := json.Unmarshal(raw, result); err != nil {
		return errors.New("room service returned invalid JSON")
	}
	return nil
}

func (c *roomClient) listing(ctx context.Context) (roomListing, error) {
	var listing roomListing
	err := c.request(ctx, c.origin+"/v1/web/rooms", nil, &listing)
	if err != nil {
		return listing, err
	}
	if listing.MaxPostBytes != 151 || len(listing.Rooms) > 256 ||
		(listing.LoginMode != "account" && listing.LoginMode != "room-password") ||
		(listing.LoginMode == "account" && listing.DeviceProtocol != deviceProtocol) {
		return listing, errors.New("room service uses an unsupported login protocol or message limit")
	}
	seen := make(map[string]bool)
	for _, room := range listing.Rooms {
		if !aliasPattern.MatchString(room.ID) || !publicKeyPattern.MatchString(room.PublicKey) ||
			room.Name == "" || !utf8.ValidString(room.Name) || len(room.Name) > 31 || seen[room.ID] {
			return listing, errors.New("room service returned an invalid or duplicate room")
		}
		seen[room.ID] = true
	}
	return listing, nil
}

func validateSession(s session) error {
	if !publicKeyPattern.MatchString(s.Author) || !validName(s.Name) || s.MaxPostBytes != 151 ||
		s.Expires <= time.Now().Unix() {
		return errors.New("room service returned an invalid or expired session")
	}
	return nil
}

func (c *roomClient) getSession(ctx context.Context, alias string) (session, error) {
	var s session
	if err := c.request(ctx, c.endpoint(alias, "session"), nil, &s); err != nil {
		return s, err
	}
	return s, validateSession(s)
}

func (c *roomClient) login(ctx context.Context, alias, user, password, mode string, saved *savedState) (session, error) {
	var s session
	var body any
	if mode == "account" {
		if !usernamePattern.MatchString(user) {
			return s, errors.New("use the lowercase username assigned by your room operator")
		}
		var challenge struct {
			Nonce    string `json:"nonce"`
			Message  string `json:"message"`
			Protocol string `json:"protocol"`
			Expires  int64  `json:"expires"`
		}
		if err := c.request(ctx, c.endpoint(alias, "challenge"), map[string]string{"username": user, "publicKey": saved.PublicKey}, &challenge); err != nil {
			return s, err
		}
		text := strings.Join([]string{deviceProtocol, c.origin, alias, user, saved.PublicKey, challenge.Nonce}, "\n")
		if challenge.Protocol != deviceProtocol || !publicKeyPattern.MatchString(challenge.Nonce) ||
			challenge.Expires <= time.Now().Unix() || challenge.Message != text || len(text) > 512 {
			return s, errors.New("device login challenge is expired or belongs to a different service, room or account")
		}
		key := saved.privateKey()
		signature := ed25519.Sign(key, []byte(text))
		clear(key)
		body = map[string]string{"username": user, "password": password, "publicKey": saved.PublicKey,
			"nonce": challenge.Nonce, "signature": hex.EncodeToString(signature)}
	} else {
		if !validName(user) {
			return s, errors.New("use a display name of 1..24 UTF-8 bytes without colons or control characters")
		}
		body = map[string]string{"identity": saved.LegacyIdentity, "name": user, "password": password}
	}
	if err := c.request(ctx, c.endpoint(alias, "login"), body, &s); err != nil {
		return s, err
	}
	if err := validateSession(s); err != nil {
		return s, err
	}
	if mode == "account" && (s.Author != saved.PublicKey || s.Username != user) {
		return s, errors.New("room session does not match this device key and username")
	}
	return s, nil
}

func (c *roomClient) history(ctx context.Context, alias, query string) (historyPage, error) {
	var page historyPage
	err := c.request(ctx, c.endpoint(alias, "history")+query, nil, &page)
	if err != nil {
		return page, err
	}
	if page.Floor < 0 || page.Floor > maxSequence || len(page.Messages) > 100 || len(page.Profiles) > 100 {
		return page, errors.New("room history returned an invalid page")
	}
	var last int64
	for _, message := range page.Messages {
		if err := validateMessage(message); err != nil {
			return page, err
		}
		if message.Seq <= last || message.Seq <= page.Floor {
			return page, errors.New("room history is not in sequence order")
		}
		last = message.Seq
	}
	for _, profile := range page.Profiles {
		if err := validateProfile(profile); err != nil {
			return page, err
		}
	}
	return page, nil
}

func (c *roomClient) post(ctx context.Context, alias string, post pendingPost) (postResult, error) {
	var result postResult
	err := c.request(ctx, c.endpoint(alias, "posts"), map[string]string{"id": post.ID, "text": post.Text}, &result)
	if err != nil {
		return result, err
	}
	if err := validateMessage(result.Message); err != nil {
		return result, err
	}
	if result.Message.Author != post.Author || result.Message.WebName == nil ||
		result.Message.Text != *result.Message.WebName+": "+post.Text {
		return result, errors.New("saved post response does not match the pending author's message; retain it and check again")
	}
	return result, nil
}

func (c *roomClient) logout(ctx context.Context, alias string) error {
	var result struct {
		Left bool `json:"left"`
	}
	if err := c.request(ctx, c.endpoint(alias, "logout"), struct{}{}, &result); err != nil {
		return err
	}
	if !result.Left {
		return errors.New("room logout was not confirmed")
	}
	return nil
}

func validName(name string) bool {
	return name != "" && len(name) <= 24 && utf8.ValidString(name) && name == strings.TrimSpace(name) &&
		!strings.ContainsAny(name, ":") && strings.IndexFunc(name, func(r rune) bool { return r < 32 || r >= 127 && r <= 159 }) < 0
}

func validateMessage(m message) error {
	if m.Seq < 1 || m.Seq > maxSequence || !publicKeyPattern.MatchString(m.Author) ||
		!aliasPattern.MatchString(m.OriginAlias) || m.Text == "" || len(m.Text) > 151 || !utf8.ValidString(m.Text) ||
		(m.WebName != nil && !validName(*m.WebName)) {
		return errors.New("room returned an invalid message")
	}
	return nil
}

func validateProfile(p profile) error {
	if !publicKeyPattern.MatchString(p.PublicKey) || p.Name == "" || len(p.Name) > 31 || !utf8.ValidString(p.Name) ||
		strings.IndexFunc(p.Name, func(r rune) bool { return r < 32 || r >= 127 && r <= 159 }) >= 0 ||
		p.AdvertType < 1 || p.AdvertType > 4 || p.Timestamp < 0 || p.Timestamp > 0xffffffff ||
		(p.Source != "radio" && p.Source != "desktop") {
		return errors.New("room returned an invalid participant profile")
	}
	return nil
}

type streamEvent struct {
	Alias      string
	Generation int
	Status     string
	Messages   []message
	Profiles   []profile
	Err        error
}

func emit(ctx context.Context, events chan<- streamEvent, event streamEvent) bool {
	select {
	case events <- event:
		return true
	case <-ctx.Done():
		return false
	}
}

func (c *roomClient) stream(ctx context.Context, room roomInfo, generation int, cursor int64, events chan<- streamEvent) {
	delay := time.Second
	for ctx.Err() == nil {
		err := c.readStream(ctx, room, generation, &cursor, events)
		if ctx.Err() != nil {
			return
		}
		if statusIs(err, 401, 403) {
			emit(ctx, events, streamEvent{Alias: room.ID, Generation: generation, Status: "Sign in again", Err: err})
			return
		}
		if !emit(ctx, events, streamEvent{Alias: room.ID, Generation: generation,
			Status: "Reconnecting", Err: fmt.Errorf("room connection closed; reconnecting in %s: %w", delay, err)}) {
			return
		}
		timer := time.NewTimer(delay)
		select {
		case <-ctx.Done():
			timer.Stop()
			return
		case <-timer.C:
		}
		delay = min(30*time.Second, delay*2)
	}
}

func (c *roomClient) catchUp(ctx context.Context, room roomInfo, generation int, cursor *int64, events chan<- streamEvent) error {
	for {
		page, err := c.history(ctx, room.ID, fmt.Sprintf("?after=%d", *cursor))
		if err != nil {
			return err
		}
		next := max(*cursor, page.Floor)
		if len(page.Messages) != 0 {
			next = max(next, page.Messages[len(page.Messages)-1].Seq)
		}
		if !emit(ctx, events, streamEvent{Alias: room.ID, Generation: generation, Messages: page.Messages, Profiles: page.Profiles}) {
			return ctx.Err()
		}
		if page.More && next <= *cursor {
			return errors.New("room catch-up page made no cursor progress")
		}
		*cursor = next
		if !page.More {
			return nil
		}
	}
}

func (c *roomClient) readStream(ctx context.Context, room roomInfo, generation int, cursor *int64, events chan<- streamEvent) error {
	target, _ := url.Parse(c.endpoint(room.ID, fmt.Sprintf("socket?since=%d", *cursor)))
	cookieURL := *target
	if c.base.Scheme == "https" {
		target.Scheme = "wss"
	} else {
		target.Scheme = "ws"
	}
	headers := http.Header{"Origin": {c.origin}, "User-Agent": {"aspen-room-tui/1"}}
	for _, cookie := range c.http.Jar.Cookies(&cookieURL) {
		headers.Add("Cookie", cookie.String())
	}
	dialer := websocket.Dialer{HandshakeTimeout: 20 * time.Second, Subprotocols: []string{socketProtocol}}
	conn, response, err := dialer.DialContext(ctx, target.String(), headers)
	if err != nil {
		if response != nil {
			response.Body.Close()
			if response.StatusCode == 401 || response.StatusCode == 403 {
				return &apiError{response.StatusCode, "Room session expired; sign in again"}
			}
		}
		return err
	}
	defer conn.Close()
	if conn.Subprotocol() != socketProtocol {
		return errors.New("room socket negotiated a different protocol")
	}
	conn.SetReadLimit(4096)
	if err := conn.SetReadDeadline(time.Now().Add(90 * time.Second)); err != nil {
		return err
	}
	conn.SetPongHandler(func(string) error { return conn.SetReadDeadline(time.Now().Add(90 * time.Second)) })
	done := make(chan struct{})
	defer close(done)
	go func() {
		ticker := time.NewTicker(30 * time.Second)
		defer ticker.Stop()
		for {
			select {
			case <-ctx.Done():
				conn.Close()
				return
			case <-done:
				return
			case <-ticker.C:
				if err := conn.WriteControl(websocket.PingMessage, nil, time.Now().Add(5*time.Second)); err != nil {
					conn.Close()
					return
				}
			}
		}
	}()
	ready := false
	for {
		var event struct {
			Type      string  `json:"type"`
			Version   int     `json:"version"`
			Alias     string  `json:"alias"`
			PublicKey string  `json:"publicKey"`
			Name      string  `json:"name"`
			Message   message `json:"message"`
			Profile   profile `json:"profile"`
			Error     string  `json:"error"`
		}
		if err := conn.ReadJSON(&event); err != nil {
			if websocket.IsCloseError(err, websocket.ClosePolicyViolation) {
				return &apiError{401, "Room session ended; sign in again"}
			}
			return err
		}
		if !ready && event.Type != "ready" {
			return errors.New("room socket sent content before its identity")
		}
		switch event.Type {
		case "ready":
			if ready || event.Version != 1 || event.Alias != room.ID || event.PublicKey != room.PublicKey || event.Name != room.Name {
				return errors.New("room socket returned a different radio identity")
			}
			ready = true
			if err := c.catchUp(ctx, room, generation, cursor, events); err != nil {
				return err
			}
			if !emit(ctx, events, streamEvent{Alias: room.ID, Generation: generation, Status: "Connected"}) {
				return ctx.Err()
			}
		case "message":
			if err := validateMessage(event.Message); err != nil {
				return err
			}
			*cursor = max(*cursor, event.Message.Seq)
			if !emit(ctx, events, streamEvent{Alias: room.ID, Generation: generation, Messages: []message{event.Message}}) {
				return ctx.Err()
			}
		case "profile":
			if err := validateProfile(event.Profile); err != nil {
				return err
			}
			if !emit(ctx, events, streamEvent{Alias: room.ID, Generation: generation, Profiles: []profile{event.Profile}}) {
				return ctx.Err()
			}
		case "catchup":
			if err := c.catchUp(ctx, room, generation, cursor, events); err != nil {
				return err
			}
			if err := conn.WriteJSON(map[string]any{"op": "sync", "since": *cursor}); err != nil {
				return err
			}
		case "error":
			return errors.New("room socket: " + event.Error)
		default:
			return errors.New("room socket sent an unknown event")
		}
	}
}
