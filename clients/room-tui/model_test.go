package main

import (
	"context"
	"fmt"
	"path/filepath"
	"strings"
	"testing"

	tea "charm.land/bubbletea/v2"
	"charm.land/lipgloss/v2"
	"github.com/charmbracelet/x/ansi"
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
	m.Update(tea.WindowSizeMsg{Width: 80, Height: 24})
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

func press(m *model, code rune, mod tea.KeyMod) tea.Cmd {
	_, cmd := m.Update(tea.KeyPressMsg{Code: code, Mod: mod})
	return cmd
}

func TestChatKeysAndSafeLeave(t *testing.T) {
	_, m := fixtureModel(t)
	m.focus = 2
	m.setFocus()
	m.composer.SetValue("first")
	press(m, tea.KeyEnter, tea.ModAlt)
	if m.composer.Value() != "first\n" {
		t.Fatalf("Alt+Enter: %q", m.composer.Value())
	}
	m.composer.SetValue("hello")
	if cmd := press(m, tea.KeyEnter, 0); cmd == nil || m.saved.Rooms["A"].Pending == nil {
		t.Fatal("Enter did not prepare a persisted post")
	}
	m.active().busy = false
	if cmd := press(m, 'l', tea.ModCtrl); cmd == nil || m.leaveConfirm || m.active().session == nil {
		t.Fatal("Ctrl+L must redraw without leaving")
	}
	press(m, 'x', tea.ModCtrl)
	if !m.leaveConfirm {
		t.Fatal("leave confirmation missing")
	}
	press(m, tea.KeyEscape, 0)
	if m.leaveConfirm || m.active().session == nil {
		t.Fatal("cancel left the room")
	}
	press(m, 'x', tea.ModCtrl)
	if cmd := press(m, tea.KeyEnter, 0); cmd == nil || !m.active().busy {
		t.Fatal("confirmed leave did not start")
	}
}

func TestReadingHistoryWhileTypingAndPrepending(t *testing.T) {
	_, m := fixtureModel(t)
	m.focus = 2
	m.composer.SetValue("keep my draft")
	var messages []message
	for i := int64(20); i < 50; i++ {
		messages = append(messages, message{Seq: i, Timestamp: uint32(100 + i), OriginAlias: "A", Author: strings.Repeat("aa", 32), Text: "line"})
	}
	room := m.active()
	if err := m.merge(room, messages, nil, false); err != nil {
		t.Fatal(err)
	}
	m.refreshHistory(true)
	press(m, tea.KeyPgUp, 0)
	if m.history.AtBottom() || m.focus != 2 || m.composer.Value() != "keep my draft" {
		t.Fatal("paging disturbed compose or failed to scroll")
	}
	before := m.history.View()
	incoming := message{Seq: 50, Timestamp: 150, OriginAlias: "A", Author: strings.Repeat("aa", 32), Text: "new arrival"}
	if err := m.merge(room, []message{incoming}, nil, true); err != nil {
		t.Fatal(err)
	}
	if room.unread != 1 || m.history.View() != before {
		t.Fatal("live message moved history or failed to mark unread")
	}
	older := message{Seq: 19, Timestamp: 119, OriginAlias: "A", Author: strings.Repeat("aa", 32), Text: "earlier"}
	m.handleResult(roomResult{Alias: "A", Generation: room.generation, Kind: "older", Page: historyPage{Messages: []message{older}}})
	if m.history.View() != before {
		t.Fatal("older page changed the visible conversation")
	}
	press(m, tea.KeyEnd, tea.ModCtrl)
	if !m.history.AtBottom() || room.unread != 0 {
		t.Fatal("latest did not resume live view")
	}
}

func TestLayoutKeepsComposerHelpAndSelectedRoomVisible(t *testing.T) {
	_, m := fixtureModel(t)
	for i := 0; i < 30; i++ {
		m.rooms = append(m.rooms, &roomState{info: roomInfo{ID: fmt.Sprint(i), Name: fmt.Sprintf("Room %d", i)}, messages: map[int64]message{}, profiles: map[string]profile{}})
	}
	for _, size := range [][2]int{{120, 40}, {80, 24}, {60, 24}, {40, 18}} {
		m.Update(tea.WindowSizeMsg{Width: size[0], Height: size[1]})
		view := m.View().Content
		for _, label := range []string{"151 bytes", "F1: help", "Enter: send"} {
			if !strings.Contains(view, label) {
				t.Errorf("%dx%d missing %q", size[0], size[1], label)
			}
		}
		if lipgloss.Height(view) > size[1] || lipgloss.Width(view) > size[0] {
			t.Fatalf("overflow %dx%d", size[0], size[1])
		}
		press(m, tea.KeyF1, 0)
		if !strings.Contains(m.View().Content, "Keyboard help") {
			t.Fatal("help missing")
		}
		press(m, tea.KeyEscape, 0)
	}
	m.Update(tea.WindowSizeMsg{Width: 80, Height: 24})
	m.choose(len(m.rooms) - 1)
	if !strings.Contains(m.View().Content, "> Room 29") {
		t.Fatal("selected room scrolled out of sidebar")
	}
}

func TestDeviceApprovalFitsNarrowTerminal(t *testing.T) {
	_, m := fixtureModel(t)
	m.active().session = nil
	m.loginMode = "passkey"
	m.handoff = &deviceLink{Code: strings.Repeat("A", 24), PublicKey: strings.Repeat("ab", 32), URL: "https://aspen.ve6slp.ca/devices?code=" + strings.Repeat("A", 24)}
	for _, size := range [][2]int{{40, 18}, {80, 24}, {120, 40}} {
		m.Update(tea.WindowSizeMsg{Width: size[0], Height: size[1]})
		view := m.View().Content
		plain := ansi.Strip(view)
		// A full public key must be readable even when it wraps over multiple lines.
		compact := strings.ReplaceAll(strings.ReplaceAll(plain, " ", ""), "\n", "")
		if !strings.Contains(compact, m.handoff.PublicKey) || !strings.Contains(plain, "Ctrl+X cancels") || !strings.Contains(plain, "F1: help") {
			t.Fatalf("approval controls/key clipped at %dx%d:\n%s", size[0], size[1], plain)
		}
		if lipgloss.Height(view) > size[1] || lipgloss.Width(view) > size[0] {
			t.Fatalf("approval overflows %dx%d", size[0], size[1])
		}
	}
}

func TestHiddenComposerDoesNotSendOrEdit(t *testing.T) {
	_, m := fixtureModel(t)
	m.focus = 2
	m.composer.SetValue("keep this")
	m.Update(tea.WindowSizeMsg{Width: 39, Height: 17})
	if cmd := press(m, tea.KeyEnter, 0); cmd != nil || m.saved.Rooms["A"].Pending != nil {
		t.Fatal("hidden composer submitted a post")
	}
	m.Update(tea.PasteMsg{Content: "hidden paste"})
	if m.composer.Value() != "keep this" {
		t.Fatal("hidden paste changed draft")
	}
	m.Update(tea.WindowSizeMsg{Width: 80, Height: 24})
	press(m, tea.KeyF1, 0)
	m.Update(tea.PasteMsg{Content: "hidden paste"})
	if m.composer.Value() != "keep this" {
		t.Fatal("help overlay changed draft")
	}
}
