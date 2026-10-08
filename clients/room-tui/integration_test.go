package main

import (
	"context"
	"errors"
	"io"
	"net/http"
	"os"
	"testing"
	"time"
)

type lostPostResponse struct {
	base  http.RoundTripper
	posts int
	drop  bool
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
