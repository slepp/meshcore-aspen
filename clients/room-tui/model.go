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

	"charm.land/bubbles/v2/textarea"
	"charm.land/bubbles/v2/textinput"
	"charm.land/bubbles/v2/viewport"
	tea "charm.land/bubbletea/v2"
	"charm.land/lipgloss/v2"
	"github.com/charmbracelet/x/ansi"
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
}

type model struct {
	ctx        context.Context
	cancel     context.CancelFunc
	client     *roomClient
	store      *stateStore
	saved      *savedState
	events     chan streamEvent
	rooms      []*roomState
	selected   int
	focus      int
	width      int
	height     int
	loginMode  string
	name       textinput.Model
	password   textinput.Model
	composer   textarea.Model
	history    viewport.Model
	notice     string
	dark       bool
	noColor    bool
	quitting   bool
	quitWarned bool
	exitErr    error
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
	return tea.Batch(m.loadListing(), m.waitEvent(), tea.RequestBackgroundColor)
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
	m.selected = (index + len(m.rooms)) % len(m.rooms)
	room := m.active()
	room.unread = 0
	m.saved.Selected = room.info.ID
	m.password.SetValue("")
	if m.loginMode == "account" {
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
		if !exists && live && room != m.active() {
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
	for _, seq := range keys {
		message := room.messages[seq]
		source := "RADIO"
		if message.WebName != nil {
			source = "IP"
		}
		meta := fmt.Sprintf("%s  %s [%s]  %s  #%d",
			time.Unix(int64(message.Timestamp), 0).Local().Format("Jan 02 15:04"),
			clean(authorLabel(message, room.profiles)), message.Author[:8], source, message.Seq)
		text.WriteString(m.accent().Render(ansi.Hardwrap(meta, m.history.Width(), true)))
		text.WriteByte('\n')
		text.WriteString(ansi.Hardwrap(clean(messageBody(message)), m.history.Width(), true))
		text.WriteString("\n\n")
	}
	if len(keys) == 0 {
		text.WriteString("No messages loaded. IP and radio participants share this conversation.")
	}
	m.history.SetContent(text.String())
	if follow {
		m.history.GotoBottom()
	}
}

func (m *model) login(room *roomState) tea.Cmd {
	if room.busy {
		return nil
	}
	user, password := m.name.Value(), m.password.Value()
	m.password.SetValue("")
	identity := savedState{Seed: m.saved.Seed, PublicKey: m.saved.PublicKey, LegacyIdentity: m.saved.LegacyIdentity}
	alias, generation, mode := room.info.ID, room.generation, m.loginMode
	room.busy, room.status = true, "Signing in"
	return func() tea.Msg {
		session, err := m.client.login(m.ctx, alias, user, password, mode, &identity)
		return roomResult{Alias: alias, Generation: generation, Kind: "login", Session: session, Username: user, Err: err}
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
	room.busy = false
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
		} else {
			room.status = "Request failed; Ctrl+R retries"
		}
		if statusIs(result.Err, 401) {
			return m.requireLogin(room)
		}
		return nil
	}
	switch result.Kind {
	case "session", "login":
		if m.loginMode == "account" && result.Session.Author != m.saved.PublicKey {
			m.notice = "Saved session belongs to a different device. Ctrl+L leaves it; keep the original device state."
			room.session = &result.Session
			return nil
		}
		room.session = &result.Session
		m.saved.Name = result.Session.Name
		if m.loginMode == "account" {
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
			m.history.GotoTop()
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
		switch value.String() {
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
		case "ctrl+n":
			return m, m.choose(m.selected + 1)
		case "ctrl+p":
			return m, m.choose(m.selected - 1)
		case "tab", "shift+tab":
			count, delta := 3, 1
			if room == nil || room.session == nil {
				count = 4
			}
			if value.String() == "shift+tab" {
				delta = -1
			}
			m.focus = (m.focus + delta + count) % count
			return m, m.setFocus()
		case "esc":
			m.notice = ""
			m.focus = 0
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
		case "ctrl+l":
			if room != nil && room.session != nil && !room.busy {
				room.busy = true
				alias, generation := room.info.ID, room.generation
				return m, func() tea.Msg {
					return roomResult{Alias: alias, Generation: generation, Kind: "logout", Err: m.client.logout(m.ctx, alias)}
				}
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
		if room.session == nil && value.String() == "enter" {
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
	var command tea.Cmd
	if m.focus == 1 && room != nil && room.session != nil {
		m.history, command = m.history.Update(msg)
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
	} else if room != nil && room.session == nil {
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
	m.history.SetHeight(max(3, m.height-14))
}

func (m *model) View() tea.View {
	view := tea.NewView("")
	view.AltScreen = true
	if m.width < 40 || m.height < 18 {
		view.SetContent("Aspen Rooms\nResize: 40 columns x 18 rows\nCtrl+Q saves and quits.")
		return view
	}
	room := m.active()
	if room == nil {
		text := "Aspen Rooms\n" + clean(m.client.origin) + "\n\nNo rooms loaded.\n" + clean(m.notice) + "\n\nCtrl+R reloads   Ctrl+Q quits"
		view.SetContent(ansi.Hardwrap(text, m.width, true))
		return view
	}
	title := fmt.Sprintf("# %s  |  %s", clean(room.info.Name), clean(room.status))
	title = ansi.Truncate(title, m.contentWidth(), "...")
	var body strings.Builder
	body.WriteString(m.accent().Render(title))
	body.WriteString("\n" + ansi.Truncate(clean(m.client.origin), m.contentWidth(), "...") + "\n")
	if room.session == nil {
		userLabel, passwordLabel := "Username", "Account password"
		if m.loginMode != "account" {
			userLabel, passwordLabel = "Display name", "Room password"
		}
		body.WriteString("\nJoin the shared IP / radio conversation.\n\n" + userLabel + "\n" + m.name.View() + "\n\n" +
			passwordLabel + "\n" + m.password.View() + "\n\nEnter signs in. Private keys stay on this device.\n")
	} else {
		header := fmt.Sprintf("%s [%s]  |  History %d%%", clean(room.session.Name), room.session.Author[:8], int(m.history.ScrollPercent()*100))
		if m.focus == 1 {
			header += " [focus]"
		}
		if room.more {
			header += "  |  Ctrl+O earlier"
		}
		body.WriteString(ansi.Truncate(header, m.contentWidth(), "...") + "\n" + m.history.View() + "\n")
		work := m.saved.Rooms[room.info.ID]
		if work.Pending != nil {
			body.WriteString("Send uncertain. Ctrl+R checks the saved post ID.\n")
		} else {
			label := fmt.Sprintf("Compose (%d / 151 bytes) Ctrl+S sends", len(room.session.Name+": "+m.composer.Value()))
			if m.focus == 2 {
				label = "[focus] " + label
			}
			body.WriteString(ansi.Truncate(label, m.contentWidth(), "...") + "\n")
		}
		body.WriteString(m.composer.View() + "\n")
	}
	notice := clean(m.notice)
	if notice == "" {
		notice = "Saved means stored in the room; it does not confirm RF reception."
	}
	body.WriteString(ansi.Truncate(strings.ReplaceAll(notice, "\n", " "), m.contentWidth(), "...") + "\n")
	body.WriteString("Tab focus | Ctrl+N/P room | Ctrl+Q quit\n")
	body.WriteString("Ctrl: S send, R check, O old, L leave")
	content := lipgloss.NewStyle().Width(m.contentWidth()).MaxHeight(m.height - 2).Render(body.String())
	var channels strings.Builder
	channels.WriteString("CHANNELS")
	if m.focus == 0 {
		channels.WriteString(" [focus]")
	}
	channels.WriteString("\n\n")
	for i, candidate := range m.rooms {
		mark, state := "  ", "locked"
		if i == m.selected {
			mark = "> "
		}
		if candidate.session != nil {
			state = "joined"
		}
		line := fmt.Sprintf("%s%s", mark, clean(candidate.info.Name))
		if candidate.unread != 0 {
			line += fmt.Sprintf(" (%d)", candidate.unread)
		}
		channels.WriteString(ansi.Truncate(line, 21, "...") + "\n  " + state + "\n")
	}
	if m.width >= 80 {
		sidebar := lipgloss.NewStyle().Width(22).MaxHeight(m.height - 2).Render(channels.String())
		content = lipgloss.JoinHorizontal(lipgloss.Top, sidebar, "  ", content)
	} else {
		focus := "Tab: channels/history/input"
		if m.focus == 0 {
			focus = "CHANNELS [focus]  Up/Down selects"
		}
		content = m.accent().Render(focus) + "\n" + content
	}
	view.SetContent(content)
	return view
}
