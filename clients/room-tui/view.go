package main

import (
	"fmt"
	"strings"
	"time"

	tea "charm.land/bubbletea/v2"
	"charm.land/lipgloss/v2"
	"github.com/charmbracelet/x/ansi"
)

func (m *model) muted() lipgloss.Style {
	style := lipgloss.NewStyle()
	if !m.noColor {
		colour := "#52606d"
		if m.dark {
			colour = "#a7adb5"
		}
		style = style.Foreground(lipgloss.Color(colour))
	}
	return style
}

func (m *model) helpView() string {
	lines := []string{
		"Aspen Rooms · Keyboard help", "",
		"Enter          Send message / sign in",
		"Alt+Enter      New line (also Ctrl+J)",
		"Tab / Shift+Tab Switch pane",
		"Alt+Up / Down  Switch room (Ctrl+P/N)",
		"Ctrl+Z         Suspend; fg resumes",
		"PgUp / PgDn    Scroll conversation",
		"Ctrl+Home / End Oldest / latest loaded",
		"Ctrl+O         Load earlier messages",
		"Ctrl+R         Retry pending post / connect",
		"Ctrl+X         Leave room / cancel sign-in",
		"Ctrl+L         Redraw screen",
		"F2             Toggle delivery details",
		"Ctrl+Q / C     Save and quit", "",
		"Esc / F1       Return to conversation",
	}
	for i, line := range lines {
		lines[i] = ansi.Truncate(line, m.width, "…")
	}
	return strings.Join(lines, "\n")
}

// signInView leaves the footer available even when a QR code will not fit.
func (m *model) signInView(width, height int) string {
	var lines []string
	if m.loginMode == "passkey" {
		if m.handoff == nil {
			lines = []string{"Sign in through your browser passkey.", "", "Enter resumes this device or creates", "a five-minute approval request.", "", "Approve it in Devices in your browser."}
		} else {
			link := m.handoff
			lines = []string{"Approve in Devices in your browser.", "Code: " + link.Code, link.URL,
				"Requesting device key:", link.PublicKey,
				fmt.Sprintf("Expires in %d seconds.", max(0, link.Expires-time.Now().Unix())),
				"Compare the full key before approving.", "Ctrl+X cancels this request."}
			text := ansi.Hardwrap(strings.Join(lines, "\n"), width, true)
			if lipgloss.Width(m.handoffQR) <= width && lipgloss.Height(text)+lipgloss.Height(m.handoffQR)+1 <= height {
				return text + "\n" + m.handoffQR
			}
		}
	} else {
		userLabel, passwordLabel := "Username", "Account password"
		if m.loginMode != "account" {
			userLabel, passwordLabel = "Display name", "Room password"
		}
		lines = []string{userLabel, m.name.View(), "", passwordLabel, m.password.View(), "", "Enter: next field / sign in"}
	}
	return ansi.Hardwrap(strings.Join(lines, "\n"), width, true)
}

func (m *model) View() tea.View {
	view := tea.NewView("")
	view.AltScreen = true
	if m.width < 40 || m.height < 18 {
		view.SetContent("Aspen Rooms\nResize: 40 columns x 18 rows\nCtrl+Q saves and quits.")
		return view
	}
	if m.help {
		view.SetContent(m.helpView())
		return view
	}
	room := m.active()
	if room == nil {
		text := "Aspen Rooms\n" + clean(m.client.origin) + "\n\nNo rooms loaded.\n" + clean(m.notice) + "\n\nCtrl+R reloads · F1 help · Ctrl+Q quits"
		view.SetContent(ansi.Hardwrap(text, m.width, true))
		return view
	}
	width := m.contentWidth()
	line := func(text string) string { return ansi.Truncate(text, width, "…") }
	var body strings.Builder
	body.WriteString(m.accent().Render(line("# "+clean(room.info.Name)+" · "+clean(room.status))) + "\n")
	if room.session == nil {
		body.WriteString(m.muted().Render(line(clean(m.client.origin))) + "\n\n")
		body.WriteString(lipgloss.NewStyle().Height(m.height - 6).MaxHeight(m.height - 6).Render(m.signInView(width, m.height-6)))
		body.WriteByte('\n')
	} else {
		position := "Live"
		if !m.history.AtBottom() {
			position = fmt.Sprintf("History %d%% · Ctrl+End: latest", int(m.history.ScrollPercent()*100))
		}
		if room.unread > 0 {
			position = fmt.Sprintf("%d new · Ctrl+End: latest", room.unread)
		}
		if room.more && m.history.AtTop() {
			position = "Ctrl+O: load earlier messages"
		}
		if m.focus == 1 {
			position = "[History] " + position
		}
		body.WriteString(m.muted().Render(line(position)) + "\n" + m.history.View() + "\n")
		work := m.saved.Rooms[room.info.ID]
		budget := len(room.session.Name + ": " + m.composer.Value())
		label := fmt.Sprintf("Message · %d/151 bytes · %s", budget, clean(room.session.Name))
		if budget > 151 {
			label = fmt.Sprintf("Message · %d/151 bytes · too long", budget)
		}
		if m.focus == 2 {
			label = "> " + label
		}
		if work.Pending != nil {
			label = "Send uncertain · Ctrl+R: check saved post"
			if room.busy {
				label = "Sending message…"
			}
		}
		body.WriteString(m.accent().Render(line(label)) + "\n" + m.composer.View() + "\n")
	}
	notice := clean(m.notice)
	if m.leaveConfirm {
		notice = "Leave room? Enter: yes · Esc: cancel"
	}
	if notice == "" {
		notice = "Enter: send · Alt+Enter: new line · PgUp: history"
	}
	body.WriteString(line(strings.ReplaceAll(notice, "\n", " ")) + "\n")
	footer := "F1: help · Ctrl+Q: quit · Tab: pane"
	if width >= 55 {
		footer += " · Alt+↑/↓: room"
	}
	body.WriteString(m.muted().Render(line(footer)))
	content := lipgloss.NewStyle().Width(width).Render(body.String())
	if m.width >= 80 {
		var channels strings.Builder
		heading := "ROOMS"
		if m.focus == 0 {
			heading = "[ROOMS]"
		}
		channels.WriteString(m.accent().Render(heading) + "\n\n")
		// Keep the selected room visible in a long channel list.
		capacity := max(1, (m.height-4)/2)
		start := max(0, m.selected-capacity+1)
		if start > 0 {
			channels.WriteString("↑ More rooms\n")
		}
		for i := start; i < min(len(m.rooms), start+capacity); i++ {
			candidate := m.rooms[i]
			mark, state := "  ", "sign in"
			if i == m.selected {
				mark = "> "
			}
			if candidate.session != nil {
				state = "joined"
			}
			suffix := ""
			if candidate.unread != 0 {
				suffix = fmt.Sprintf(" (%d)", candidate.unread)
			}
			name := mark + ansi.Truncate(clean(candidate.info.Name), max(1, 21-len(mark)-len(suffix)), "…") + suffix
			if i == m.selected {
				name = m.accent().Render(name)
			}
			channels.WriteString(name + "\n  " + m.muted().Render(state) + "\n")
		}
		if start+capacity < len(m.rooms) {
			channels.WriteString("↓ More rooms")
		}
		sidebar := lipgloss.NewStyle().Width(22).MaxHeight(m.height).Render(channels.String())
		content = lipgloss.JoinHorizontal(lipgloss.Top, sidebar, "  ", content)
	} else {
		heading := fmt.Sprintf("Room %d/%d", m.selected+1, len(m.rooms))
		if m.focus == 0 {
			heading += " [Rooms] ↑/↓ select · Enter: chat"
		}
		content = m.accent().Render(ansi.Truncate(heading, m.width, "…")) + "\n" + content
	}
	view.SetContent(content)
	return view
}
