package main

import (
	"context"
	"crypto/ed25519"
	"encoding/hex"
	"encoding/json"
	"fmt"
	"net/http"
	"net/http/httptest"
	"strings"
	"sync"
	"testing"
	"time"

	"github.com/gorilla/websocket"
)

type fixtureServer struct {
	server    *httptest.Server
	mutex     sync.Mutex
	author    string
	messages  map[string]message
	next      int64
	posts     int
	fail      int
	drop      bool
	socket    bool
	conns     []*websocket.Conn
	connected chan *websocket.Conn
}

func newFixtureServer(t *testing.T) *fixtureServer {
	t.Helper()
	f := &fixtureServer{messages: make(map[string]message), connected: make(chan *websocket.Conn, 8)}
	f.server = httptest.NewServer(http.HandlerFunc(f.serve))
	t.Cleanup(func() {
		f.mutex.Lock()
		for _, conn := range f.conns {
			conn.Close()
		}
		f.mutex.Unlock()
		f.server.Close()
	})
	return f
}

func respond(w http.ResponseWriter, value any) {
	w.Header().Set("Content-Type", "application/json")
	json.NewEncoder(w).Encode(value)
}

func (f *fixtureServer) serve(w http.ResponseWriter, r *http.Request) {
	if r.Header.Get("Origin") != f.server.URL {
		w.WriteHeader(403)
		respond(w, map[string]string{"error": "wrong origin"})
		return
	}
	if r.URL.Path == "/v1/web/rooms" {
		respond(w, roomListing{Rooms: []roomInfo{{"A", "Harbor", strings.Repeat("11", 32)}},
			MaxPostBytes: 151, LoginMode: "account", DeviceProtocol: deviceProtocol})
		return
	}
	var body map[string]string
	if r.Method == "POST" {
		if r.Header.Get("Content-Type") != "application/json" || json.NewDecoder(r.Body).Decode(&body) != nil {
			w.WriteHeader(415)
			respond(w, map[string]string{"error": "bad JSON body"})
			return
		}
	}
	endpoint := r.URL.Path[strings.LastIndex(r.URL.Path, "/")+1:]
	if endpoint == "challenge" {
		nonce := strings.Repeat("ab", 32)
		respond(w, map[string]any{"nonce": nonce, "expires": time.Now().Unix() + 120, "protocol": deviceProtocol,
			"message": strings.Join([]string{deviceProtocol, f.server.URL, "A", body["username"], body["publicKey"], nonce}, "\n")})
		return
	}
	if endpoint == "login" {
		pub, _ := hex.DecodeString(body["publicKey"])
		sig, _ := hex.DecodeString(body["signature"])
		text := strings.Join([]string{deviceProtocol, f.server.URL, "A", body["username"], body["publicKey"], body["nonce"]}, "\n")
		if len(pub) != ed25519.PublicKeySize || body["username"] != "alice" || body["password"] != "fixture password only" ||
			body["nonce"] != strings.Repeat("ab", 32) || !ed25519.Verify(pub, []byte(text), sig) {
			w.WriteHeader(403)
			respond(w, map[string]string{"error": "bad device login"})
			return
		}
		f.mutex.Lock()
		f.author = body["publicKey"]
		author := f.author
		f.mutex.Unlock()
		http.SetCookie(w, &http.Cookie{Name: "aspen_room_A", Value: strings.Repeat("cd", 32), Path: "/v1/web/rooms/A/",
			Secure: true, HttpOnly: true, MaxAge: 2592000, SameSite: http.SameSiteStrictMode})
		respond(w, session{Author: author, Name: "Alice", Username: "alice", Expires: time.Now().Unix() + 2592000, MaxPostBytes: 151})
		return
	}
	cookie, err := r.Cookie("aspen_room_A")
	if err != nil || cookie.Value != strings.Repeat("cd", 32) {
		w.WriteHeader(401)
		respond(w, map[string]string{"error": "sign in again"})
		return
	}
	f.mutex.Lock()
	author := f.author
	f.mutex.Unlock()
	switch endpoint {
	case "session":
		respond(w, session{Author: author, Name: "Alice", Username: "alice", Expires: time.Now().Unix() + 2592000, MaxPostBytes: 151})
	case "history":
		f.mutex.Lock()
		messages := make([]message, 0, len(f.messages))
		var after int64
		fmt.Sscan(r.URL.Query().Get("after"), &after)
		for seq := int64(1); seq <= f.next; seq++ {
			for _, m := range f.messages {
				if m.Seq == seq && m.Seq > after {
					messages = append(messages, m)
				}
			}
		}
		f.mutex.Unlock()
		respond(w, historyPage{Messages: messages})
	case "posts":
		f.mutex.Lock()
		f.posts++
		if f.fail != 0 {
			status := f.fail
			f.mutex.Unlock()
			w.WriteHeader(status)
			respond(w, map[string]string{"error": "fixture post rejected"})
			return
		}
		m, duplicate := f.messages[body["id"]]
		if !duplicate {
			f.next++
			name := "Alice"
			m = message{Seq: f.next, Timestamp: uint32(time.Now().Unix()), OriginAlias: "A", Author: author,
				Text: "Alice: " + body["text"], WebName: &name}
			f.messages[body["id"]] = m
		}
		drop := f.drop
		f.drop = false
		f.mutex.Unlock()
		if drop {
			conn, _, err := w.(http.Hijacker).Hijack()
			if err == nil {
				conn.Close()
			}
			return
		}
		respond(w, postResult{m, duplicate})
	case "logout":
		http.SetCookie(w, &http.Cookie{Name: "aspen_room_A", Path: "/v1/web/rooms/A/", MaxAge: -1,
			Secure: true, HttpOnly: true, SameSite: http.SameSiteStrictMode})
		respond(w, map[string]bool{"left": true})
	case "socket":
		upgrader := websocket.Upgrader{Subprotocols: []string{socketProtocol}}
		conn, err := upgrader.Upgrade(w, r, nil)
		if err != nil {
			return
		}
		f.mutex.Lock()
		f.conns = append(f.conns, conn)
		f.mutex.Unlock()
		conn.WriteJSON(map[string]any{"type": "ready", "version": 1, "alias": "A", "name": "Harbor", "publicKey": strings.Repeat("11", 32)})
		f.connected <- conn
		for {
			if _, _, err := conn.ReadMessage(); err != nil {
				return
			}
		}
	default:
		w.WriteHeader(404)
		respond(w, map[string]string{"error": "unknown route"})
	}
}

func fixtureClient(t *testing.T) (*fixtureServer, *roomClient, *stateStore, *savedState, session) {
	t.Helper()
	f := newFixtureServer(t)
	client, err := newClient(f.server.URL)
	if err != nil {
		t.Fatal(err)
	}
	store, saved := testState(t, client.origin)
	session, err := client.login(context.Background(), "A", "alice", "fixture password only", "account", saved)
	if err != nil {
		t.Fatal(err)
	}
	return f, client, store, saved, session
}

func TestClientProofCookiesAndNoRedirects(t *testing.T) {
	_, client, _, saved, s := fixtureClient(t)
	if s.Author != saved.PublicKey || len(client.cookieSnapshot()) != 1 {
		t.Fatal("device key/cookie missing")
	}
	if _, err := client.getSession(context.Background(), "A"); err != nil {
		t.Fatal(err)
	}
	restarted, err := newClient(client.origin)
	if err != nil {
		t.Fatal(err)
	}
	if err := restarted.restoreCookies(client.cookieSnapshot()); err != nil {
		t.Fatal(err)
	}
	if _, err := restarted.getSession(context.Background(), "A"); err != nil {
		t.Fatal("session did not survive restart:", err)
	}
	if err := restarted.logout(context.Background(), "A"); err != nil {
		t.Fatal(err)
	}
	if len(restarted.cookieSnapshot()) != 0 {
		t.Fatal("logout cookie remained")
	}
	if _, err := restarted.getSession(context.Background(), "A"); !statusIs(err, 401) {
		t.Fatalf("logout still authenticated: %v", err)
	}
	for _, target := range []string{"http://example.test", "https://user:pass@room.test", "https://room.test/path",
		"https://room.test?password=secret", "file:///tmp/room", "https://room.test#secret"} {
		if _, err := newClient(target); err == nil {
			t.Errorf("unsafe origin accepted: %s", target)
		}
	}
	normalized, err := newClient("HTTPS://ROOM.TEST:443/")
	if err != nil || normalized.origin != "https://room.test" {
		t.Fatalf("origin did not match browser normalization: %v", err)
	}
	redirect := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		http.Redirect(w, r, "https://other.test", 307)
	}))
	defer redirect.Close()
	redirectClient, _ := newClient(redirect.URL)
	var output any
	if err := redirectClient.request(context.Background(), redirect.URL, map[string]string{"password": "fixture"}, &output); err == nil {
		t.Fatal("credential redirect accepted")
	}
}

func TestClientRetainsOnePostAfterLostResponse(t *testing.T) {
	f, client, _, saved, _ := fixtureClient(t)
	id, _ := newPostID()
	pending := pendingPost{id, "one copy", saved.PublicKey}
	f.mutex.Lock()
	f.drop = true
	f.mutex.Unlock()
	if _, err := client.post(context.Background(), "A", pending); err == nil {
		t.Fatal("dropped response was reported successful")
	}
	f.mutex.Lock()
	attempts := f.posts
	f.mutex.Unlock()
	if attempts != 1 {
		t.Fatal("client automatically retried a POST")
	}
	retry, err := client.post(context.Background(), "A", pending)
	if err != nil || !retry.Duplicate || retry.Message.Seq != 1 {
		t.Fatalf("retry: %+v %v", retry, err)
	}
}

func TestStreamReconnectsCatchUpAndCancels(t *testing.T) {
	f, client, _, saved, _ := fixtureClient(t)
	id, _ := newPostID()
	first, err := client.post(context.Background(), "A", pendingPost{id, "first", saved.PublicKey})
	if err != nil {
		t.Fatal(err)
	}
	ctx, cancel := context.WithCancel(context.Background())
	defer cancel()
	events := make(chan streamEvent, 128)
	stopped := make(chan struct{})
	go func() {
		client.stream(ctx, roomInfo{"A", "Harbor", strings.Repeat("11", 32)}, 3, 0, events)
		close(stopped)
	}()
	next := func() streamEvent {
		t.Helper()
		select {
		case event := <-events:
			return event
		case <-time.After(5 * time.Second):
			t.Fatal("missing stream event")
			return streamEvent{}
		}
	}
	if event := next(); len(event.Messages) != 1 || event.Messages[0].Seq != first.Message.Seq {
		t.Fatalf("catch-up: %+v", event)
	}
	if event := next(); event.Status != "Connected" || event.Generation != 3 {
		t.Fatalf("ready: %+v", event)
	}
	conn := <-f.connected
	conn.Close()
	if event := next(); event.Status != "Reconnecting" || event.Err == nil {
		t.Fatalf("reconnect status: %+v", event)
	}
	nextID, _ := newPostID()
	if _, err := client.post(context.Background(), "A", pendingPost{nextID, "second", saved.PublicKey}); err != nil {
		t.Fatal(err)
	}
	if event := next(); len(event.Messages) != 1 || event.Messages[0].Seq != 2 {
		t.Fatalf("reconnected catch-up: %+v", event)
	}
	if event := next(); event.Status != "Connected" {
		t.Fatalf("reconnected ready: %+v", event)
	}
	select {
	case <-f.connected:
	case <-time.After(time.Second):
		t.Fatal("missing second connection")
	}
	cancel()
	select {
	case <-stopped:
	case <-time.After(time.Second):
		t.Fatal("stream did not stop after cancellation")
	}
}
