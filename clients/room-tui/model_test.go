package main

import (
	"context"
	"path/filepath"
	"strings"
	"testing"

	tea "charm.land/bubbletea/v2"
	"charm.land/lipgloss/v2"
)

func fixtureModel(t *testing.T) (*fixtureServer, *model) {
	t.Helper()
	f, client, store, saved, session := fixtureClient(t)
	ctx, cancel := context.WithCancel(context.Background())
	t.Cleanup(cancel)
	m := newModel(ctx, cancel, client, store, saved)
	m.loginMode = "account"
	m.rooms = []*roomState{{info: roomInfo{"A", "Harbor", strings.Repeat("11", 32)}, session: &session,
		messages: make(map[int64]message), profiles: make(map[string]profile), status: "Connected"}}
	saved.Rooms["A"] = savedRoom{}
	m.choose(0)
	return f, m
}

func TestRadioDeliveryUpdatesDoNotConfirmPostsOrChangeContent(t *testing.T) {
	_, m := fixtureModel(t)
	room := m.active()
	msg := message{Seq: 1, Timestamp: 100, OriginAlias: "A", Author: strings.Repeat("aa", 32),
		Text: "hello", RF: &radioDelivery{Recipients: 1, Sent: 1, Attempts: 4, Retrying: 1, Exhausted: 1}}
	if err := m.merge(room, []message{msg}, nil, false); err != nil {
		t.Fatal(err)
	}
	if label := radioDeliveryLabel(msg.RF); !strings.Contains(label, "RF ACK 0/1") ||
		!strings.Contains(label, "awaiting ACK") || !strings.Contains(label, "retry budget exhausted") {
		t.Fatal(label)
	}
	room.busy, room.rfBusy = true, true
	msg.RF = &radioDelivery{Recipients: 1, Acknowledged: 1, Attempts: 4}
	m.Update(roomResult{Alias: "A", Generation: room.generation, Kind: "rf",
		Page: historyPage{Messages: []message{msg}}})
	if !room.busy || room.rfBusy || len(room.messages) != 1 ||
		!strings.Contains(radioDeliveryLabel(room.messages[1].RF), "RF ACK 1/1") {
		t.Fatal("RF refresh changed post admission or lost status")
	}
	msg.RF = &radioDelivery{Recipients: 1, Sent: 1, Acknowledged: 1}
	if validateMessage(msg) == nil {
		t.Fatal("inconsistent RF counts accepted")
	}
	if !strings.Contains(radioDeliveryLabel(nil), "unavailable") {
		t.Fatal("missing RF evidence shown as success")
	}
}

func TestModelSavesBeforePostAndRecoversUncertainty(t *testing.T) {
	f, m := fixtureModel(t)
	m.composer.SetValue("saved before send")
	f.mutex.Lock()
	f.drop = true
	f.mutex.Unlock()
	command := m.send(m.active(), false)
	if command == nil {
		t.Fatal("send was not prepared")
	}
	saved, err := m.store.load()
	if err != nil || saved.Rooms["A"].Pending == nil || saved.Rooms["A"].Pending.Author != m.saved.PublicKey {
		t.Fatalf("pending post not persisted before HTTP: %v", err)
	}
	id := saved.Rooms["A"].Pending.ID
	m.Update(command())
	if m.saved.Rooms["A"].Pending == nil || m.saved.Rooms["A"].Pending.ID != id {
		t.Fatal("uncertain post was lost")
	}
	retry := m.send(m.active(), true)
	if retry == nil {
		t.Fatal("retry not prepared")
	}
	m.Update(retry())
	if m.saved.Rooms["A"].Pending != nil || m.composer.Value() != "" {
		t.Fatal("confirmed post did not clear pending work")
	}
	loaded, err := m.store.load()
	if err != nil || loaded.Rooms["A"].Pending != nil || len(m.active().messages) != 1 {
		t.Fatal("confirmed result did not persist")
	}
}

func TestModelDoesNotSendWithoutPrivatePersistenceOrAsAnotherAuthor(t *testing.T) {
	f, m := fixtureModel(t)
	m.composer.SetValue("do not lose this")
	directory := m.store.directory
	m.store.directory = filepath.Join(directory, "missing")
	if command := m.send(m.active(), false); command != nil {
		t.Fatal("send started without durable state")
	}
	m.store.directory = directory
	f.mutex.Lock()
	posts := f.posts
	f.mutex.Unlock()
	if posts != 0 {
		t.Fatal("HTTP was submitted after save failed")
	}
	work := m.saved.Rooms["A"]
	work.Pending.Author = strings.Repeat("ee", 32)
	m.saved.Rooms["A"] = work
	if command := m.send(m.active(), true); command != nil || !strings.Contains(m.notice, "another identity") {
		t.Fatal("uncertain post was reauthored")
	}
}

func TestModelNativeBudgetDefinitiveRejectionAndFullKeyNames(t *testing.T) {
	f, m := fixtureModel(t)
	m.composer.SetValue(strings.Repeat("🙂", 37))
	if m.send(m.active(), false) != nil {
		t.Fatal("oversized UTF-8 post accepted")
	}
	m.composer.SetValue(strings.Repeat("🙂", 36))
	f.mutex.Lock()
	f.fail = 400
	f.mutex.Unlock()
	command := m.send(m.active(), false)
	if command == nil {
		t.Fatal("exact 151-byte post rejected")
	}
	m.Update(command())
	if m.saved.Rooms["A"].Pending != nil || m.saved.Rooms["A"].Draft != strings.Repeat("🙂", 36) {
		t.Fatal("definitive rejection lost the editable draft")
	}
	a, b := strings.Repeat("aa", 32), strings.Repeat("aa", 4)+strings.Repeat("bb", 28)
	profiles := map[string]profile{a: {PublicKey: a, Name: "Same", Source: "radio"}, b: {PublicKey: b, Name: "Other", Source: "radio"}}
	if authorLabel(message{Author: a}, profiles) != "Same" || authorLabel(message{Author: b}, profiles) != "Other" {
		t.Fatal("short-prefix collision changed names")
	}
	if clean("\x1b]52;c;hidden\x07visible\x1b[31m red\x1b[0m\b") != "visible red" {
		t.Fatal("remote terminal control sequence survived")
	}
}

func TestModelKeyboardResizeAndNoColor(t *testing.T) {
	t.Setenv("NO_COLOR", "1")
	_, m := fixtureModel(t)
	m.focus = 0
	m.Update(tea.KeyPressMsg{Code: tea.KeyTab})
	if m.focus != 1 {
		t.Fatal("Tab did not select history")
	}
	m.Update(tea.KeyPressMsg{Code: tea.KeyTab})
	if m.focus != 2 || !m.composer.Focused() {
		t.Fatal("Tab did not select compose")
	}
	for _, size := range [][2]int{{100, 30}, {80, 24}, {50, 24}, {40, 18}, {39, 12}, {35, 15}} {
		m.Update(tea.WindowSizeMsg{Width: size[0], Height: size[1]})
		view := m.View().Content
		if size[0] < 40 {
			if !strings.Contains(view, "Resize: 40 columns x 18 rows") {
				t.Fatal("tiny terminal warning missing")
			}
			continue
		}
		if lipgloss.Width(view) > size[0] || lipgloss.Height(view) > size[1] {
			t.Errorf("view overflows %dx%d: %dx%d", size[0], size[1], lipgloss.Width(view), lipgloss.Height(view))
		}
		if strings.Contains(view, "\x1b[38;") || strings.Contains(view, "\x1b[48;") {
			t.Error("NO_COLOR emitted foreground/background colors")
		}
	}
	m.active().session = nil
	m.focus = 2
	m.Update(tea.KeyPressMsg{Code: tea.KeyEnter})
	if m.focus != 3 || !m.password.Focused() {
		t.Fatal("Enter did not move login focus to masked password")
	}
	m.password.SetValue("fixture password only")
	if strings.Contains(m.View().Content, "fixture password only") {
		t.Fatal("password appeared in the terminal")
	}
}
