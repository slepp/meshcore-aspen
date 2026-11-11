package main

import (
	"context"
	"errors"
	"fmt"
	"os"
	"slices"
	"strings"
	"time"
	"unicode"

	"charm.land/bubbles/v2/key"
	"charm.land/bubbles/v2/textarea"
	"charm.land/bubbles/v2/textinput"
	"charm.land/bubbles/v2/viewport"
	tea "charm.land/bubbletea/v2"
	"charm.land/lipgloss/v2"
	"github.com/charmbracelet/x/ansi"
	qrcode "github.com/skip2/go-qrcode"
)

type listingResult struct {
	Listing roomListing
	Err     error
}

type roomResult struct {
	Alias      string
	Generation int
	Kind       string
	Session    session
	Page       historyPage
	Post       postResult
	Username   string
	Err        error
}

type roomState struct {
	info       roomInfo
	session    *session
	messages   map[int64]message
	profiles   map[string]profile
	cursor     int64
	more       bool
	unread     int
	generation int
	stop       context.CancelFunc
	status     string
	busy       bool
	rfBusy     bool
}

type radioTick struct{}
type linkResult struct {
	Link     deviceLink
	Approved bool
	Alias    string
	Started  bool
	Err      error
}
type linkTick struct{ Code string }

func radioRefreshTick() tea.Cmd {
	return tea.Tick(5*time.Second, func(time.Time) tea.Msg { return radioTick{} })
}

type model struct {
	ctx          context.Context
	cancel       context.CancelFunc
	client       *roomClient
	store        *stateStore
	saved        *savedState
	events       chan streamEvent
	rooms        []*roomState
	selected     int
	focus        int
	width        int
	height       int
	loginMode    string
	name         textinput.Model
	password     textinput.Model
	composer     textarea.Model
	history      viewport.Model
	notice       string
	dark         bool
	noColor      bool
	quitting     bool
	quitWarned   bool
	exitErr      error
	handoff      *deviceLink
	handoffQR    string
	handoffBusy  bool
	help         bool
	details      bool
	leaveConfirm bool
}

func newModel(ctx context.Context, cancel context.CancelFunc, client *roomClient, store *stateStore, saved *savedState) *model {
	name, password := textinput.New(), textinput.New()
	name.Prompt, name.Placeholder, name.CharLimit = "> ", "Username", 64
	password.Prompt, password.Placeholder, password.CharLimit = "> ", "Password", 256
	password.EchoMode = textinput.EchoPassword
	password.EchoCharacter = '*'
	composer := textarea.New()
	composer.Prompt, composer.Placeholder, composer.CharLimit = "> ", "Message this room", 2048
	composer.ShowLineNumbers = false
	composer.KeyMap.InsertNewline = key.NewBinding(key.WithKeys("alt+enter", "shift+enter", "ctrl+j"))
	composer.SetHeight(3)
	m := &model{ctx: ctx, cancel: cancel, client: client, store: store, saved: saved,
		events: make(chan streamEvent, 128), width: 100, height: 30, dark: true,
		name: name, password: password, composer: composer, history: viewport.New()}
	_, m.noColor = os.LookupEnv("NO_COLOR")
	m.setStyles()
	m.resize()
	return m
}

func (m *model) Init() tea.Cmd {
	return tea.Batch(m.loadListing(), m.waitEvent(), tea.RequestBackgroundColor, radioRefreshTick())
}

func (m *model) loadListing() tea.Cmd {
	return func() tea.Msg {
		listing, err := m.client.listing(m.ctx)
		return listingResult{listing, err}
	}
}

func (m *model) waitEvent() tea.Cmd {
	return func() tea.Msg {
		select {
		case event := <-m.events:
			return event
		case <-m.ctx.Done():
			return nil
		}
	}
}

func (m *model) active() *roomState {
	if len(m.rooms) == 0 {
		return nil
	}
	return m.rooms[m.selected]
}

func (m *model) find(alias string) *roomState {
	for _, room := range m.rooms {
		if room.info.ID == alias {
			return room
		}
	}
	return nil
}

func (m *model) persist() bool {
	m.saved.Cookies = m.client.cookieSnapshot()
	if err := m.store.save(m.saved); err != nil {
		m.notice = "Private state could not be saved: " + err.Error()
		return false
	}
	m.quitWarned = false
	return true
}

func (m *model) sessionCommand(room *roomState) tea.Cmd {
	alias, generation := room.info.ID, room.generation
	return func() tea.Msg {
		session, err := m.client.getSession(m.ctx, alias)
		return roomResult{Alias: alias, Generation: generation, Kind: "session", Session: session, Err: err}
	}
}

func (m *model) historyCommand(room *roomState, kind, query string) tea.Cmd {
	room.busy = true
	alias, generation := room.info.ID, room.generation
	return func() tea.Msg {
		page, err := m.client.history(m.ctx, alias, query)
		return roomResult{Alias: alias, Generation: generation, Kind: kind, Page: page, Err: err}
	}
}

func (m *model) startStream(room *roomState) {
	if room.stop != nil {
		room.stop()
	}
	room.generation++
	room.rfBusy = false
	ctx, stop := context.WithCancel(m.ctx)
	room.stop, room.status = stop, "Connecting"
	go m.client.stream(ctx, room.info, room.generation, room.cursor, m.events)
}

func (m *model) requireLogin(room *roomState) tea.Cmd {
	if room.stop != nil {
		room.stop()
		room.stop = nil
	}
	room.generation++
	room.rfBusy = false
	room.session, room.status, room.busy = nil, "Not joined", false
	if room == m.active() {
		m.focus = 2
		return m.setFocus()
	}
	return nil
}

func (m *model) choose(index int) tea.Cmd {
	if len(m.rooms) == 0 {
		return nil
	}
	m.leaveConfirm = false
	m.selected = (index + len(m.rooms)) % len(m.rooms)
	room := m.active()
	room.unread = 0
	m.saved.Selected = room.info.ID
	m.password.SetValue("")
	if m.loginMode == "account" || m.loginMode == "passkey" {
		m.name.SetValue(m.saved.Username)
	} else {
		m.name.SetValue(m.saved.Name)
	}
	m.composer.SetValue(m.saved.Rooms[room.info.ID].Draft)
	if m.focus == 3 && room.session != nil {
		m.focus = 2
	}
	m.persist()
	m.refreshHistory(true)
	return m.setFocus()
}

func (m *model) merge(room *roomState, messages []message, profiles []profile, live bool) error {
	for _, p := range profiles {
		if err := validateProfile(p); err != nil {
			return err
		}
		old, exists := room.profiles[p.PublicKey]
		if !exists || p.Timestamp > old.Timestamp {
			room.profiles[p.PublicKey] = p
		}
	}
	for _, incoming := range messages {
		if err := validateMessage(incoming); err != nil {
			return err
		}
		old, exists := room.messages[incoming.Seq]
		if exists && (old.Text != incoming.Text || old.Author != incoming.Author || old.Timestamp != incoming.Timestamp ||
			old.OriginAlias != incoming.OriginAlias || old.ClientTimestamp != incoming.ClientTimestamp ||
			(old.WebName == nil) != (incoming.WebName == nil) || old.WebName != nil && *old.WebName != *incoming.WebName) {
			return fmt.Errorf("room returned conflicting content at sequence %d", incoming.Seq)
		}
		room.messages[incoming.Seq] = incoming
		if !exists && live && (room != m.active() || !m.history.AtBottom()) {
			room.unread++
		}
	}
	for {
		if _, found := room.messages[room.cursor+1]; !found {
			break
		}
		room.cursor++
	}
	if room == m.active() {
		m.refreshHistory(false)
	}
	return nil
}

func clean(text string) string {
	return strings.Map(func(r rune) rune {
		if r != '\n' && unicode.IsControl(r) {
			return -1
		}
		return r
	}, ansi.Strip(text))
}

func authorLabel(message message, profiles map[string]profile) string {
	if message.WebName != nil {
		return *message.WebName
	}
	if profile, exists := profiles[message.Author]; exists {
		return profile.Name
	}
	return message.Author[:8]
}

func messageBody(message message) string {
	if message.WebName != nil {
		return strings.TrimPrefix(message.Text, *message.WebName+": ")
	}
	return message.Text
}

func radioDeliveryLabel(rf *radioDelivery) string {
	if rf == nil {
		return "Saved | RF status unavailable"
	}
	if rf.Recipients == 0 {
		return "Saved | no current RF delivery"
	}
	parts := []string{fmt.Sprintf("Saved | RF ACK %d/%d", rf.Acknowledged, rf.Recipients)}
	for _, item := range []struct {
		count int64
		label string
	}{
		{rf.Queued, "queued"}, {rf.Sent, "transmitted, awaiting ACK"},
		{rf.Uncertain, "transmission uncertain"}, {rf.Failed, "transmission failed"},
		{rf.Paused, "paused"}, {rf.Retrying, "retrying"}, {rf.Exhausted, "retry budget exhausted"},
	} {
		if item.count != 0 {
			parts = append(parts, fmt.Sprintf("%d %s", item.count, item.label))
		}
	}
	if rf.Attempts > 1 {
		parts = append(parts, fmt.Sprintf("up to %d dispatch attempts", rf.Attempts))
	}
	if rf.NextRetryAt != nil {
		parts = append(parts, "retry "+time.UnixMilli(*rf.NextRetryAt).Local().Format("15:04:05"))
	}
	return strings.Join(parts, " | ")
}

func (m *model) refreshHistory(bottom bool) {
	room := m.active()
	if room == nil {
		return
	}
	follow := bottom || m.history.AtBottom()
	keys := make([]int64, 0, len(room.messages))
	for seq := range room.messages {
		keys = append(keys, seq)
	}
	slices.Sort(keys)
	var text strings.Builder
	day := ""
	for _, seq := range keys {
		message := room.messages[seq]
		source := "RADIO"
		if message.WebName != nil {
			source = "IP"
		}
		stamp := time.Unix(int64(message.Timestamp), 0).Local()
		if date := stamp.Format("Mon, 02 Jan 2006"); date != day {
			day = date
			text.WriteString(m.muted().Render(day) + "\n")
		}
		meta := fmt.Sprintf("%s  %s [%s]  %s",
			stamp.Format("15:04"),
			clean(authorLabel(message, room.profiles)), message.Author[:8], source)
		if m.details {
			meta += fmt.Sprintf("  #%d", message.Seq)
		}
		text.WriteString(m.accent().Render(ansi.Wrap(meta, m.history.Width(), "")))
		text.WriteByte('\n')
		text.WriteString(ansi.Wrap(clean(messageBody(message)), m.history.Width(), ""))
		text.WriteByte('\n')
		if m.details || message.WebName != nil {
			label := radioDeliveryLabel(message.RF)
			if !m.details && message.RF != nil && message.RF.Recipients == 0 {
				label = "Saved"
			}
			text.WriteString(m.muted().Render(ansi.Wrap(label, m.history.Width(), "")) + "\n")
		}
		text.WriteByte('\n')
	}
	if len(keys) == 0 {
		text.WriteString(ansi.Wrap("No messages yet. Write a message to start the conversation.", m.history.Width(), ""))
	}
	m.history.SetContent(text.String())
	if follow {
		room.unread = 0
		m.history.GotoBottom()
	}
}

func (m *model) login(room *roomState) tea.Cmd {
	if room.busy || m.loginMode == "passkey" && (m.handoff != nil || m.handoffBusy) {
		return nil
	}
	m.handoffBusy = m.loginMode == "passkey"
	user, password := m.name.Value(), m.password.Value()
	m.password.SetValue("")
	identity := savedState{Seed: m.saved.Seed, PublicKey: m.saved.PublicKey, LegacyIdentity: m.saved.LegacyIdentity}
	alias, generation, mode := room.info.ID, room.generation, m.loginMode
	room.busy, room.status = true, "Signing in"
	return func() tea.Msg {
		session, err := m.client.login(m.ctx, alias, user, password, mode, &identity)
		if mode == "passkey" && statusIs(err, 401) {
			link, linkErr := m.client.beginLink(m.ctx, &identity)
			return linkResult{Link: link, Alias: alias, Started: true, Err: linkErr}
		}
		return roomResult{Alias: alias, Generation: generation, Kind: "login", Session: session, Username: user, Err: err}
	}
}

func linkWait(code string) tea.Cmd {
	return tea.Tick(3*time.Second, func(time.Time) tea.Msg { return linkTick{Code: code} })
}

func (m *model) pollLink(link deviceLink) tea.Cmd {
	return func() tea.Msg {
		approved, err := m.client.pollLink(m.ctx, link)
		return linkResult{Link: link, Approved: approved, Err: err}
	}
}

func (m *model) send(room *roomState, retry bool) tea.Cmd {
	if room.session == nil || room.busy {
		return nil
	}
	work := m.saved.Rooms[room.info.ID]
	if work.Pending == nil {
		if retry {
			return nil
		}
		text := m.composer.Value()
		if strings.TrimSpace(text) == "" || len(room.session.Name+": "+text) > 151 {
			m.notice = "Message must contain text and fit 151 UTF-8 bytes including the display name."
			return nil
		}
		id, err := newPostID()
		if err != nil {
			m.notice = "Could not create a post ID: " + err.Error()
			return nil
		}
		work.Draft, work.Pending = text, &pendingPost{ID: id, Text: text, Author: room.session.Author}
		m.saved.Rooms[room.info.ID] = work
	} else if !retry {
		m.notice = "A post is still uncertain. Ctrl+R checks or retries its saved ID."
		return nil
	}
	if work.Pending.Author != room.session.Author {
		m.notice = "Pending post belongs to another identity. Restore its original login; no new copy was sent."
		return nil
	}
	if !m.persist() {
		return nil
	}
	post, alias, generation := *work.Pending, room.info.ID, room.generation
	room.busy, room.status = true, "Saving to room"
	return func() tea.Msg {
		result, err := m.client.post(m.ctx, alias, post)
		return roomResult{Alias: alias, Generation: generation, Kind: "post", Post: result, Err: err}
	}
}

func (m *model) handleResult(result roomResult) tea.Cmd {
	room := m.find(result.Alias)
	if room == nil || room.generation != result.Generation {
		return nil
	}
	if result.Kind == "rf" {
		room.rfBusy = false
	} else {
		room.busy = false
	}
	if result.Err != nil {
		if result.Kind == "session" && statusIs(result.Err, 401) {
			room.status = "Not joined"
			return nil
		}
		m.notice = clean(room.info.Name) + ": " + result.Err.Error()
		if result.Kind == "post" {
			room.status = "Send uncertain; Ctrl+R checks"
			if statusIs(result.Err, 400, 413, 415) {
				old := m.saved.Rooms[room.info.ID]
				next := old
				next.Pending = nil
				m.saved.Rooms[room.info.ID] = next
				if !m.persist() {
					m.saved.Rooms[room.info.ID] = old
				}
				room.status = "Post rejected; edit the draft"
			}
		} else if result.Kind != "rf" {
			room.status = "Request failed; Ctrl+R retries"
		}
		if statusIs(result.Err, 401) {
			return m.requireLogin(room)
		}
		return nil
	}
	switch result.Kind {
	case "rf":
		known := make([]message, 0, len(result.Page.Messages))
		for _, incoming := range result.Page.Messages {
			if _, exists := room.messages[incoming.Seq]; exists {
				known = append(known, incoming)
			}
		}
		if err := m.merge(room, known, result.Page.Profiles, false); err != nil {
			m.notice = "RF delivery status: " + err.Error()
		}
	case "session", "login":
		if (m.loginMode == "account" || m.loginMode == "passkey") && result.Session.Author != m.saved.PublicKey {
			m.notice = "Saved session belongs to a different device. Ctrl+X leaves it; keep the original device state."
			room.session = &result.Session
			return nil
		}
		room.session = &result.Session
		m.saved.Name = result.Session.Name
		if m.loginMode == "account" || m.loginMode == "passkey" {
			m.saved.Username = result.Session.Username
		}
		m.persist()
		var focus tea.Cmd
		if room == m.active() {
			m.focus = 2
			focus = m.setFocus()
		}
		return tea.Batch(focus, m.historyCommand(room, "initial", ""))
	case "initial", "older":
		oldLines, oldOffset := m.history.TotalLineCount(), m.history.YOffset()
		if err := m.merge(room, result.Page.Messages, result.Page.Profiles, false); err != nil {
			m.notice = room.info.Name + ": " + err.Error()
			return nil
		}
		room.more = result.Page.More
		if result.Kind == "initial" {
			room.cursor = result.Page.Floor
			if len(result.Page.Messages) != 0 {
				room.cursor = result.Page.Messages[len(result.Page.Messages)-1].Seq
			}
			m.startStream(room)
			if room == m.active() {
				m.refreshHistory(true)
			}
		} else if room == m.active() {
			m.history.SetYOffset(oldOffset + m.history.TotalLineCount() - oldLines)
		}
	case "post":
		if err := m.merge(room, []message{result.Post.Message}, nil, false); err != nil {
			m.notice = err.Error()
			return nil
		}
		old := m.saved.Rooms[room.info.ID]
		m.saved.Rooms[room.info.ID] = savedRoom{}
		if !m.persist() {
			m.saved.Rooms[room.info.ID] = old
			m.notice = "Post was saved, but its pending state could not be cleared. Ctrl+R checks the same ID."
		} else if room == m.active() {
			m.composer.SetValue("")
		}
		room.status = "Saved to shared history"
		if room == m.active() {
			m.refreshHistory(true)
		}
	case "logout":
		focus := m.requireLogin(room)
		room.messages = make(map[int64]message)
		room.cursor, room.more = 0, false
		m.persist()
		m.refreshHistory(true)
		return focus
	}
	return nil
}

func (m *model) Update(msg tea.Msg) (tea.Model, tea.Cmd) {
	switch value := msg.(type) {
	case linkTick:
		if m.handoff != nil && m.handoff.Code == value.Code {
			return m, m.pollLink(*m.handoff)
		}
		return m, nil
	case linkResult:
		if value.Started {
			m.handoffBusy = false
			if room := m.find(value.Alias); room != nil {
				room.busy = false
				room.status = "Waiting for device approval"
			}
		} else if m.handoff == nil || value.Link.Code != m.handoff.Code {
			return m, nil
		}
		if value.Err != nil {
			m.handoff, m.handoffQR = nil, ""
			m.notice = "Device sign-in: " + value.Err.Error()
			return m, nil
		}
		if value.Approved {
			m.handoff, m.handoffQR = nil, ""
			if !m.persist() {
				return m, nil
			}
			if room := m.active(); room != nil && room.session == nil {
				return m, m.login(room)
			}
			return m, nil
		}
		if value.Started {
			qr, err := qrcode.New(value.Link.URL, qrcode.Medium)
			if err != nil {
				m.notice = "Could not render device approval QR code: " + err.Error()
				return m, nil
			}
			m.handoff, m.handoffQR = &value.Link, qr.ToSmallString(false)
		}
		return m, linkWait(value.Link.Code)
	case radioTick:
		var refresh tea.Cmd
		if room := m.active(); room != nil && room.session != nil && room.stop != nil && !room.busy && !room.rfBusy {
			room.rfBusy = true
			alias, generation, cursor := room.info.ID, room.generation, room.cursor
			refresh = func() tea.Msg {
				page, err := m.client.history(m.ctx, alias, fmt.Sprintf("?before=%d", cursor+1))
				return roomResult{Alias: alias, Generation: generation, Kind: "rf", Page: page, Err: err}
			}
		}
		return m, tea.Batch(radioRefreshTick(), refresh)
	case tea.WindowSizeMsg:
		m.width, m.height = value.Width, value.Height
		m.resize()
		m.refreshHistory(false)
	case tea.BackgroundColorMsg:
		m.dark = value.IsDark()
		m.setStyles()
		m.refreshHistory(false)
	case listingResult:
		if value.Err != nil {
			m.notice = "Could not open room service: " + value.Err.Error() + ". Ctrl+R retries."
			return m, nil
		}
		m.loginMode = value.Listing.LoginMode
		var commands []tea.Cmd
		m.rooms = nil
		for _, info := range value.Listing.Rooms {
			room := &roomState{info: info, messages: make(map[int64]message), profiles: make(map[string]profile),
				status: "Checking saved login", busy: true}
			m.rooms = append(m.rooms, room)
			commands = append(commands, m.sessionCommand(room))
			if _, exists := m.saved.Rooms[info.ID]; !exists {
				m.saved.Rooms[info.ID] = savedRoom{}
			}
		}
		m.selected = 0
		for i, room := range m.rooms {
			if room.info.ID == m.saved.Selected {
				m.selected = i
			}
		}
		commands = append(commands, m.choose(m.selected))
		return m, tea.Batch(commands...)
	case roomResult:
		if value.Kind == "login" {
			m.handoffBusy = false
		}
		return m, m.handleResult(value)
	case streamEvent:
		room := m.find(value.Alias)
		var focus tea.Cmd
		if room != nil && room.generation == value.Generation {
			if value.Err != nil {
				room.status = value.Status
				m.notice = clean(room.info.Name) + ": " + value.Err.Error()
				if statusIs(value.Err, 401, 403) {
					focus = m.requireLogin(room)
				}
			} else {
				if err := m.merge(room, value.Messages, value.Profiles, true); err != nil {
					m.notice = clean(room.info.Name) + ": " + err.Error()
					if room.stop != nil {
						room.stop()
					}
					room.status = "Connection stopped; Ctrl+R reconnects"
				} else if value.Status != "" {
					room.status = value.Status
				}
			}
		}
		return m, tea.Batch(focus, m.waitEvent())
	case tea.KeyPressMsg:
		room := m.active()
		if value.String() == "ctrl+z" {
			if m.persist() {
				return m, tea.Suspend
			}
			return m, nil
		}
		if (m.width < 40 || m.height < 18) && value.String() != "ctrl+q" && value.String() != "ctrl+c" {
			return m, nil
		}
		if m.help && value.String() != "ctrl+q" && value.String() != "ctrl+c" {
			if value.String() == "esc" || value.String() == "f1" || value.String() == "ctrl+g" {
				m.help = false
			}
			return m, nil
		}
		if m.leaveConfirm {
			m.leaveConfirm = false
			if value.String() == "enter" && room != nil && room.session != nil && !room.busy {
				room.busy = true
				alias, generation := room.info.ID, room.generation
				return m, func() tea.Msg {
					return roomResult{Alias: alias, Generation: generation, Kind: "logout", Err: m.client.logout(m.ctx, alias)}
				}
			}
			if value.String() != "ctrl+q" && value.String() != "ctrl+c" {
				return m, nil
			}
		}
		switch value.String() {
		case "f1", "ctrl+g":
			m.help = true
			return m, nil
		case "f2":
			m.details = !m.details
			m.refreshHistory(false)
			return m, nil
		case "pgup", "pgdown", "ctrl+home", "ctrl+end":
			if room != nil && room.session != nil {
				switch value.String() {
				case "pgup":
					m.history.HalfPageUp()
				case "pgdown":
					m.history.HalfPageDown()
				case "ctrl+home":
					m.history.GotoTop()
				case "ctrl+end":
					m.history.GotoBottom()
				}
			}
			if room != nil && m.history.AtBottom() {
				room.unread = 0
			}
			return m, nil
		case "ctrl+l":
			return m, tea.ClearScreen
		case "ctrl+q", "ctrl+c":
			if !m.persist() {
				if !m.quitWarned {
					m.quitWarned = true
					m.notice += ". Ctrl+Q again quits without saving the latest changes."
					return m, nil
				}
				m.exitErr = errors.New("quit after a state-save failure; latest changes were not saved")
			}
			m.quitting = true
			m.cancel()
			return m, tea.Quit
		case "ctrl+n", "alt+down":
			return m, m.choose(m.selected + 1)
		case "ctrl+p", "alt+up":
			return m, m.choose(m.selected - 1)
		case "tab", "shift+tab":
			count, delta := 3, 1
			if (room == nil || room.session == nil) && m.loginMode != "passkey" {
				count = 4
			}
			if value.String() == "shift+tab" {
				delta = -1
			}
			m.focus = (m.focus + delta + count) % count
			return m, m.setFocus()
		case "esc":
			m.notice = ""
			m.focus = 2
			return m, m.setFocus()
		case "ctrl+r":
			if room == nil {
				return m, m.loadListing()
			}
			if room.busy {
				return m, nil
			}
			if room.session == nil {
				m.focus = 2
				return m, m.setFocus()
			}
			if m.saved.Rooms[room.info.ID].Pending != nil {
				return m, m.send(room, true)
			}
			m.startStream(room)
			return m, nil
		case "ctrl+s":
			if room != nil {
				return m, m.send(room, false)
			}
			return m, nil
		case "ctrl+o":
			if room != nil && room.session != nil && !room.busy && room.more && len(room.messages) != 0 {
				first := int64(maxSequence)
				for seq := range room.messages {
					first = min(first, seq)
				}
				return m, m.historyCommand(room, "older", fmt.Sprintf("?before=%d", first))
			}
			return m, nil
		case "ctrl+x":
			if m.handoff != nil {
				m.handoff, m.handoffQR = nil, ""
				m.notice = "Device approval request cancelled on this TUI; its code expires after five minutes."
				return m, nil
			}
			if room != nil && room.session != nil && !room.busy {
				m.leaveConfirm = true
			}
			return m, nil
		}
		if room == nil {
			return m, nil
		}
		if m.focus == 0 {
			switch value.String() {
			case "down", "right":
				return m, m.choose(m.selected + 1)
			case "up", "left":
				return m, m.choose(m.selected - 1)
			case "enter":
				m.focus = 2
				return m, m.setFocus()
			}
			return m, nil
		}
		if room.session != nil && m.focus == 2 && value.String() == "enter" {
			return m, m.send(room, false)
		}
		if room.session == nil && value.String() == "enter" {
			if m.loginMode == "passkey" {
				return m, m.login(room)
			}
			if m.focus == 2 {
				m.focus = 3
				return m, m.setFocus()
			}
			if m.focus == 3 {
				return m, m.login(room)
			}
		}
	}
	room := m.active()
	if m.help || m.leaveConfirm || m.width < 40 || m.height < 18 {
		return m, nil
	}
	var command tea.Cmd
	if m.focus == 1 && room != nil && room.session != nil {
		m.history, command = m.history.Update(msg)
		if m.history.AtBottom() {
			room.unread = 0
		}
	} else if m.focus == 2 && room != nil && room.session != nil {
		if !room.busy && m.saved.Rooms[room.info.ID].Pending == nil {
			m.composer, command = m.composer.Update(msg)
			work := m.saved.Rooms[room.info.ID]
			if work.Draft != m.composer.Value() {
				work.Draft = m.composer.Value()
				m.saved.Rooms[room.info.ID] = work
				m.persist()
			}
		}
	} else if room != nil && room.session == nil && m.loginMode != "passkey" {
		if m.focus == 2 {
			m.name, command = m.name.Update(msg)
		} else if m.focus == 3 {
			m.password, command = m.password.Update(msg)
		}
	}
	return m, command
}

func (m *model) setFocus() tea.Cmd {
	m.name.Blur()
	m.password.Blur()
	m.composer.Blur()
	room := m.active()
	if room == nil {
		return nil
	}
	if room.session == nil {
		if m.loginMode == "passkey" {
			return nil
		}
		if m.focus == 2 {
			return m.name.Focus()
		}
		if m.focus == 3 {
			return m.password.Focus()
		}
	} else if m.focus == 2 {
		return m.composer.Focus()
	}
	return nil
}

func (m *model) accent() lipgloss.Style {
	style := lipgloss.NewStyle().Bold(true)
	if !m.noColor {
		color := "#146a37"
		if m.dark {
			color = "#8adcaf"
		}
		style = style.Foreground(lipgloss.Color(color))
	}
	return style
}

func (m *model) setStyles() {
	text := textinput.DefaultStyles(m.dark)
	area := textarea.DefaultStyles(m.dark)
	if m.noColor {
		text, area = textinput.Styles{}, textarea.Styles{}
	}
	m.name.SetStyles(text)
	m.password.SetStyles(text)
	m.composer.SetStyles(area)
}

func (m *model) contentWidth() int {
	width := m.width
	if width >= 80 {
		width -= 24
	}
	return max(20, width-2)
}

func (m *model) resize() {
	width := m.contentWidth()
	m.name.SetWidth(width - 2)
	m.password.SetWidth(width - 2)
	m.composer.SetWidth(width)
	m.history.SetWidth(width)
	overhead := 8
	if m.width < 80 {
		overhead++
	}
	m.history.SetHeight(max(3, m.height-overhead))
}
