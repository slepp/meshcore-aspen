package main

import (
	"bytes"
	"context"
	"crypto/sha256"
	"encoding/base64"
	"encoding/binary"
	"encoding/hex"
	"encoding/json"
	"errors"
	"io"
	"net/http"
	"os"
	"testing"
	"time"

	"github.com/gorilla/websocket"
)

type lostPostResponse struct {
	base  http.RoundTripper
	posts int
	drop  bool
}

type workerTransmit struct {
	Type       string `json:"type"`
	DispatchID string `json:"dispatchId"`
	Packet     string `json:"packet"`
	DelayMS    int    `json:"delayMs"`
}

func workerRadioOperation(t *testing.T, client *roomClient, ctx context.Context, operation any) bool {
	t.Helper()
	data, err := json.Marshal(operation)
	if err != nil {
		t.Fatal(err)
	}
	request, err := http.NewRequestWithContext(ctx, "POST", client.origin+"/v1/aliases/A/operations", bytes.NewReader(data))
	if err != nil {
		t.Fatal(err)
	}
	request.Header.Set("Authorization", "Bearer one")
	response, err := client.http.Do(request)
	if err != nil {
		t.Fatal(err)
	}
	defer response.Body.Close()
	var result struct {
		Accepted bool `json:"accepted"`
	}
	if response.StatusCode != 200 || json.NewDecoder(response.Body).Decode(&result) != nil {
		t.Fatalf("local fixture radio operation failed (%d)", response.StatusCode)
	}
	return result.Accepted
}

func acknowledgeWorkerMessage(t *testing.T, client *roomClient, ctx context.Context, m message, reader string) {
	t.Helper()
	public, err := hex.DecodeString(reader)
	if err != nil {
		t.Fatal(err)
	}
	author, err := hex.DecodeString(m.Author)
	if err != nil {
		t.Fatal(err)
	}
	body := make([]byte, 9)
	binary.LittleEndian.PutUint32(body, m.Timestamp)
	copy(body[5:], author[:4])
	body = append(body, []byte(m.Text)...)
	body = append(body, public...)
	accepted := 0
	for attempt := byte(0); attempt < 4; attempt++ {
		body[4] = 8 | attempt
		hash := sha256.Sum256(body)
		packet := append([]byte{13, 0x80}, hash[:4]...)
		if workerRadioOperation(t, client, ctx, map[string]string{"op": "rf", "packet": base64.StdEncoding.EncodeToString(packet)}) {
			accepted++
		}
	}
	if accepted != 1 {
		t.Fatalf("canonical native message ACK accepted %d variants", accepted)
	}
}

func TestActualWorkerRetryAlarm(t *testing.T) {
	origin := os.Getenv("ASPEN_ROOM_TEST_ORIGIN")
	if origin == "" {
		t.Skip("run node test/worker.mjs for the timed local Worker check")
	}
	client, err := newClient(origin)
	if err != nil {
		t.Fatal(err)
	}
	if client.base.Scheme != "http" || (client.base.Hostname() != "localhost" && client.base.Hostname() != "127.0.0.1") {
		t.Fatal("public RF fixtures are restricted to the local Worker")
	}
	ctx, cancel := context.WithTimeout(context.Background(), 50*time.Second)
	defer cancel()
	_, saved := testState(t, origin)
	if _, err := client.login(ctx, "A", "alice", "fixture password only", "account", saved); err != nil {
		t.Fatal(err)
	}
	raw, err := os.ReadFile("../../services/shared-room/test/native-fixtures.json")
	if err != nil {
		t.Fatal(err)
	}
	var fixture struct {
		ReaderLogin string `json:"readerLogin"`
		Reader      struct {
			PublicKey string `json:"publicKey"`
		} `json:"reader"`
	}
	if err := json.Unmarshal(raw, &fixture); err != nil {
		t.Fatal(err)
	}
	socketURL := *client.base
	socketURL.Scheme, socketURL.Path = "ws", "/v1/aliases/A/socket"
	socket, response, err := websocket.DefaultDialer.DialContext(ctx, socketURL.String(), http.Header{"Authorization": {"Bearer one"}})
	if response != nil && response.Body != nil {
		response.Body.Close()
	}
	if err != nil {
		t.Fatal(err)
	}
	defer socket.Close()
	socket.SetReadDeadline(time.Now().Add(45 * time.Second))
	var ready struct {
		Type    string `json:"type"`
		Version int    `json:"version"`
	}
	if err := socket.ReadJSON(&ready); err != nil || ready.Type != "ready" || ready.Version != 2 {
		t.Fatalf("fixture frontend ready: %+v %v", ready, err)
	}
	page, err := client.history(ctx, "A", "")
	if err != nil || page.More {
		t.Fatalf("fixture history: %v", err)
	}
	if !workerRadioOperation(t, client, ctx, map[string]string{"op": "rf", "packet": fixture.ReaderLogin}) {
		t.Fatal("native reader login rejected")
	}
	next := func() workerTransmit {
		t.Helper()
		var tx workerTransmit
		if err := socket.ReadJSON(&tx); err != nil {
			t.Fatal(err)
		}
		if tx.Type != "transmit" || tx.DispatchID == "" || tx.Packet == "" || tx.DelayMS != 1500 {
			t.Fatalf("invalid native history dispatch: %+v", tx)
		}
		return tx
	}
	for _, m := range page.Messages {
		next()
		acknowledgeWorkerMessage(t, client, ctx, m, fixture.Reader.PublicKey)
	}
	id, err := newPostID()
	if err != nil {
		t.Fatal(err)
	}
	result, err := client.post(ctx, "A", pendingPost{id, "Real alarm RF retry check", saved.PublicKey})
	if err != nil {
		t.Fatal(err)
	}
	first := next()
	started := time.Now()
	if !workerRadioOperation(t, client, ctx, map[string]string{"op": "txReceipt", "dispatchId": first.DispatchID, "outcome": "sent"}) {
		t.Fatal("fixture TX receipt rejected")
	}
	retry := next()
	elapsed := time.Since(started)
	if elapsed < 28*time.Second || elapsed > 40*time.Second || retry.DispatchID == first.DispatchID || retry.Packet == first.Packet {
		t.Fatalf("real alarm retry: %s, reused dispatch %v, reused packet %v", elapsed,
			retry.DispatchID == first.DispatchID, retry.Packet == first.Packet)
	}
	acknowledgeWorkerMessage(t, client, ctx, result.Message, fixture.Reader.PublicKey)
	t.Logf("Real Worker alarm retried the same canonical post after %s without another client request", elapsed.Round(time.Millisecond))
}

func (r *lostPostResponse) RoundTrip(req *http.Request) (*http.Response, error) {
	response, err := r.base.RoundTrip(req)
	if req.URL.Path != "/v1/web/rooms/A/posts" {
		return response, err
	}
	r.posts++
	if err == nil && r.drop {
		r.drop = false
		_, readErr := io.Copy(io.Discard, response.Body)
		response.Body.Close()
		if readErr != nil {
			return nil, readErr
		}
		return nil, errors.New("fixture lost response after Worker commit")
	}
	return response, err
}

func TestActualWorker(t *testing.T) {
	origin := os.Getenv("ASPEN_ROOM_TEST_ORIGIN")
	if origin == "" {
		t.Skip("run node test/worker.mjs for the local Worker and PTY checks")
	}
	ctx, cancel := context.WithTimeout(context.Background(), 30*time.Second)
	defer cancel()
	client, err := newClient(origin)
	if err != nil {
		t.Fatal(err)
	}
	listing, err := client.listing(ctx)
	if err != nil || listing.LoginMode != "account" || len(listing.Rooms) != 2 {
		t.Fatalf("listing: %+v %v", listing, err)
	}
	store, saved := testState(t, origin)
	login, err := client.login(ctx, "A", "alice", "fixture password only", "account", saved)
	if err != nil || login.Author != saved.PublicKey || login.Name != "Alice" {
		t.Fatalf("native verification of Go device signature: %+v %v", login, err)
	}
	transport := &lostPostResponse{base: http.DefaultTransport, drop: true}
	client.http.Transport = transport
	id, err := newPostID()
	if err != nil {
		t.Fatal(err)
	}
	pending := pendingPost{id, "Go Worker integration", saved.PublicKey}
	if _, err := client.post(ctx, "A", pending); err == nil || transport.posts != 1 {
		t.Fatalf("lost response was retried or accepted: %v (%d attempts)", err, transport.posts)
	}
	work := savedRoom{Draft: pending.Text, Pending: &pending}
	saved.Rooms["A"], saved.Username, saved.Cookies = work, "alice", client.cookieSnapshot()
	if err := store.save(saved); err != nil {
		t.Fatal(err)
	}
	reloaded, err := store.load()
	if err != nil || reloaded.PublicKey != saved.PublicKey || reloaded.Rooms["A"].Pending.ID != id {
		t.Fatalf("uncertain post did not persist: %v", err)
	}
	restarted, err := newClient(origin)
	if err != nil {
		t.Fatal(err)
	}
	if err := restarted.restoreCookies(reloaded.Cookies); err != nil {
		t.Fatal(err)
	}
	if _, err := restarted.getSession(ctx, "A"); err != nil {
		t.Fatal("persisted session:", err)
	}
	result, err := restarted.post(ctx, "A", *reloaded.Rooms["A"].Pending)
	if err != nil || !result.Duplicate || result.Message.Author != saved.PublicKey {
		t.Fatalf("explicit same-ID retry: %+v %v", result, err)
	}
	page, err := restarted.history(ctx, "B", "")
	if !statusIs(err, 401) {
		t.Fatalf("unjoined alias was accessible: %+v %v", page, err)
	}
	if _, err := restarted.login(ctx, "B", "alice", "fixture password only", "account", saved); err != nil {
		t.Fatal(err)
	}
	page, err = restarted.history(ctx, "B", "")
	if err != nil {
		t.Fatal(err)
	}
	copies, named := 0, false
	for _, m := range page.Messages {
		if m.Author == saved.PublicKey && m.Text == "Alice: "+pending.Text {
			copies++
		}
	}
	for _, p := range page.Profiles {
		if p.PublicKey == saved.PublicKey && p.Name == "Alice" && p.Source == "desktop" {
			named = true
		}
	}
	if copies != 1 || !named {
		t.Fatalf("shared history: %d copies, desktop profile %v", copies, named)
	}
	events := make(chan streamEvent, 128)
	streamCtx, stop := context.WithCancel(ctx)
	defer stop()
	go restarted.stream(streamCtx, listing.Rooms[0], 1, 0, events)
	for {
		select {
		case event := <-events:
			if event.Err != nil {
				t.Fatal(event.Err)
			}
			if event.Status == "Connected" {
				return
			}
		case <-ctx.Done():
			t.Fatal("actual Worker socket did not connect")
		}
	}
}
