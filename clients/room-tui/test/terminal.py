#!/usr/bin/env python3
"""Exercise the real terminal client against the local Worker using a PTY."""
import fcntl
import hashlib
import json
import os
from pathlib import Path
import pty
import re
import select
import signal
import stat
import struct
import subprocess
import sys
import termios
import time
import urllib.request

binary, origin, directory = sys.argv[1:4]
login_only = len(sys.argv) > 4 and sys.argv[4] == "--login-only"
username, password, alias = "alice", "fixture password only", "A"
if login_only:
    username, password_file, alias = sys.argv[5:]
    with os.fdopen(os.open(password_file, os.O_RDONLY | os.O_NOFOLLOW), "r", encoding="utf-8") as file:
        info = os.fstat(file.fileno())
        assert stat.S_ISREG(info.st_mode) and info.st_uid == os.getuid() and info.st_mode & 0o077 == 0
        password = file.read().removesuffix("\n")
use_default_state = directory == "-"
if use_default_state:
    config = os.environ.get("XDG_CONFIG_HOME", str(Path.home() / ".config"))
    directory = str(Path(config) / "aspen-rooms" / hashlib.sha256(origin.encode()).hexdigest()[:16])
state_path = Path(directory) / "state.json"
process = None
master = None
output = bytearray()
screen = []
row = column = 0
terminal_pending = ""


def render(chunk):
    global row, column, terminal_pending, screen
    terminal_pending += chunk.decode("utf-8", errors="replace")
    while terminal_pending:
        if terminal_pending.startswith("\x1b"):
            match = re.match(r"\x1b\[[0-?]*[ -/]*[@-~]|\x1b\].*?(?:\x07|\x1b\\)", terminal_pending)
            if not match:
                if len(terminal_pending) == 1 or terminal_pending[1] in "[]":
                    return
                terminal_pending = terminal_pending[2:]
                continue
            escape = match.group()
            terminal_pending = terminal_pending[len(escape):]
            if not escape.startswith("\x1b[") or not re.fullmatch(r"[0-9;]*", escape[2:-1]):
                continue
            values = [int(v or "0") for v in escape[2:-1].split(";")]
            n, action = values[0] or 1, escape[-1]
            if action in "Hf":
                row, column = n - 1, (values[1] or 1) - 1 if len(values) > 1 else 0
            elif action == "d":
                row = n - 1
            elif action == "G":
                column = n - 1
            elif action == "A":
                row = max(0, row - n)
            elif action == "B":
                row = min(len(screen) - 1, row + n)
            elif action == "C":
                column += n
            elif action == "D":
                column = max(0, column - n)
            elif action == "J" and values[0] in (2, 3):
                screen = [[" "] * 200 for _ in range(100)]
            elif action == "K":
                if values[0] == 2:
                    screen[row] = [" "] * 200
                elif values[0] == 0:
                    screen[row][column:] = [" "] * (200 - column)
            elif action == "X":
                screen[row][column:column+n] = [" "] * n
            elif action == "L":
                screen[row:row] = [[" "] * 200 for _ in range(n)]
                screen = screen[:100]
            continue
        char, terminal_pending = terminal_pending[0], terminal_pending[1:]
        if char == "\r":
            column = 0
        elif char == "\n":
            row = min(99, row + 1)
        elif char == "\b":
            column = max(0, column - 1)
        elif char >= " " and row < 100 and column < 200:
            screen[row][column] = char
            column += 1


def visible():
    return "\n".join("".join(line) for line in screen)


def size(columns, rows):
    fcntl.ioctl(master, termios.TIOCSWINSZ, struct.pack("HHHH", rows, columns, 0, 0))
    os.kill(process.pid, signal.SIGWINCH)


def start():
    global process, master, output, screen, row, column, terminal_pending
    master, slave = pty.openpty()
    fcntl.ioctl(slave, termios.TIOCSWINSZ, struct.pack("HHHH", 30, 100, 0, 0))
    process = subprocess.Popen(
        [binary, "--service", origin] + ([] if use_default_state else ["--state-dir", directory]),
        stdin=slave, stdout=slave, stderr=slave,
        env={**os.environ, "TERM": "xterm-256color", "NO_COLOR": "1"},
        start_new_session=True,
    )
    os.close(slave)
    output = bytearray()
    screen, row, column, terminal_pending = [[" "] * 200 for _ in range(100)], 0, 0, ""


def pump():
    if select.select([master], [], [], .1)[0]:
        try:
            chunk = os.read(master, 65536)
        except OSError:
            chunk = b""
        output.extend(chunk)
        render(chunk)
        # Reply to background, cursor, device-attribute and keyboard queries.
        for query, reply in (
            (b"\x1b]11;?\x07", b"\x1b]11;rgb:0000/0000/0000\x07"),
            (b"\x1b]11;?\x1b\\", b"\x1b]11;rgb:0000/0000/0000\x1b\\"),
            (b"\x1b[6n", b"\x1b[1;1R"),
            (b"\x1b[c", b"\x1b[?1;2c"),
            (b"\x1b[>c", b"\x1b[>0;0;0c"),
            (b"\x1b[?u", b"\x1b[?0u"),
        ):
            if query in chunk:
                os.write(master, reply)


def until(check, label, timeout=15):
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        pump()
        if check():
            return
        if process.poll() is not None:
            raise AssertionError(f"Client exited during {label}: {bytes(output)!r}")
    raise AssertionError(f"Timed out: {label}: {bytes(output)!r}")


def state():
    return json.loads(state_path.read_text())


def send(keys):
    os.write(master, keys)


def quit_client():
    send(b"\x11")
    until(lambda: process.poll() is not None, "clean quit")
    assert process.returncode == 0
    os.close(master)


try:
    start()
    until(lambda: b"Not joined" in output and b"Username" in output, "login screen")
    send(("\r" + username + "\r").encode())
    for _ in range(5):
        pump()
    send((password + "\r").encode())
    until(lambda: "Connected" in visible() and state()["username"] == username, "account login")
    assert password.encode() not in output, "password was echoed"
    public_key = state()["publicKey"]
    if login_only:
        assert state()["selected"] == alias
        cookie = next(c for c in state()["cookies"] if c["Name"] == "aspen_room_" + alias)
        request = urllib.request.Request(
            origin + "/v1/web/rooms/" + alias + "/session",
            headers={"Origin": origin, "Cookie": cookie["Name"] + "=" + cookie["Value"],
                     "User-Agent": "aspen-room-tui/1"},
        )
        with urllib.request.urlopen(request) as response:
            session = json.load(response)
        assert session["author"] == public_key and session["username"] == username
        quit_client()
        assert password not in state_path.read_text()
        start()
        until(lambda: "Connected" in visible(), "live session restoration")
        assert state()["publicKey"] == public_key
        quit_client()
        print("PASS: read-only live account login, device author and default-state session restoration; no posts sent.")
        sys.exit(0)
    send(b"PTY shared conversation")
    until(lambda: state()["rooms"]["A"]["draft"] == "PTY shared conversation", "durable draft")
    send(b"\x13")
    until(lambda: state()["rooms"]["A"]["draft"] == "" and
          "PTY shared conversation" in visible() and "Saved to shared history" in visible(), "confirmed post")
    saved = state()
    cookie = next(c for c in saved["cookies"] if c["Name"] == "aspen_room_A")
    request = urllib.request.Request(
        origin + "/v1/web/rooms/A/history",
        headers={"Origin": origin, "Cookie": cookie["Name"] + "=" + cookie["Value"]},
    )
    with urllib.request.urlopen(request) as response:
        history = json.load(response)
    assert sum(m["author"] == public_key and m["text"] == "Alice: PTY shared conversation"
               for m in history["messages"]) == 1
    send(b"Draft after restart")
    until(lambda: state()["rooms"]["A"]["draft"] == "Draft after restart", "restart draft")
    size(60, 24)
    for _ in range(10):
        pump()
    assert process.poll() is None, "narrow resize exited"
    size(35, 15)
    until(lambda: "40" in visible() and "18" in visible(), "minimum-size warning")
    size(100, 30)
    quit_client()
    assert state_path.stat().st_mode & 0o777 == 0o600
    assert Path(directory).stat().st_mode & 0o777 == 0o700
    assert "fixture password only" not in state_path.read_text()
    start()
    until(lambda: "Connected" in visible() and "Draft after restart" in visible(), "session/draft restoration")
    assert state()["publicKey"] == public_key
    quit_client()
    print("PASS: PTY masked login, keyboard send, canonical post, private draft/key/cookie persistence and resize.")
finally:
    if process is not None and process.poll() is None:
        process.terminate()
        try:
            process.wait(timeout=5)
        except subprocess.TimeoutExpired:
            process.kill()
            process.wait()
