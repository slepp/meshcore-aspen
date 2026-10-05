"""Bounded, durable companion mailbox. No RF configuration or automatic resend."""

import asyncio
from collections import deque
import hashlib
import json
import os
from pathlib import Path
import re
import sqlite3
import stat
import time
import uuid

from meshcore import EventType, MeshCore
from meshcore.tcp_cx import TCPConnection

KEY = re.compile(r"[0-9a-f]{64}")
PREFIX = re.compile(r"[0-9a-f]{12}")
MAX_INBOX = 512
MAX_DEDUP = 2048
MAX_SENDS = 256


def private_file(path):
    fd = os.open(path, os.O_RDONLY | os.O_NOFOLLOW)
    info = os.fstat(fd)
    if not stat.S_ISREG(info.st_mode) or info.st_uid != os.getuid() or info.st_mode & 0o077:
        os.close(fd)
        raise ValueError("Configuration must be an owner-only regular file")
    with os.fdopen(fd) as stream:
        data = stream.read(16385)
    if len(data) > 16384:
        raise ValueError("Configuration exceeds 16 KiB")
    return json.loads(data)


def load_config(path):
    config = private_file(path)
    if not isinstance(config, dict) or not KEY.fullmatch(str(config.get("target", ""))):
        raise ValueError("Configuration requires target: 64 lowercase hex digits")
    feeds = config.get("feeds")
    if not isinstance(feeds, list) or not 1 <= len(feeds) <= 8:
        raise ValueError("Configuration requires 1–8 companion feeds")
    names, endpoints = set(), set()
    for feed in feeds:
        if not isinstance(feed, dict):
            raise ValueError("Each feed must be an object")
        name, host, port = feed.get("name"), feed.get("host"), feed.get("port")
        if not isinstance(name, str) or not re.fullmatch(r"[A-Za-z0-9_-]{1,64}", name):
            raise ValueError("Feed name must be a short identifier")
        if not isinstance(host, str) or not host or len(host) > 253:
            raise ValueError(f"Feed {name} requires a host")
        if type(port) is not int or not 1 <= port <= 65535:
            raise ValueError(f"Feed {name} requires port 1–65535")
        if not KEY.fullmatch(str(feed.get("public_key", ""))):
            raise ValueError(f"Feed {name} requires its pinned full public_key")
        if name in names or (host, port) in endpoints:
            raise ValueError("Duplicate feed name or endpoint")
        names.add(name)
        endpoints.add((host, port))
    if config.get("default_feed", feeds[0]["name"]) not in names:
        raise ValueError("default_feed is not configured")
    return config


class Store:
    def __init__(self, directory):
        self.directory = Path(directory)
        self.directory.mkdir(mode=0o700, parents=True, exist_ok=True)
        info = self.directory.lstat()
        if not stat.S_ISDIR(info.st_mode) or info.st_uid != os.getuid() or info.st_mode & 0o077:
            raise ValueError("State directory must be owner-only (0700)")
        path = self.directory / "mailbox.sqlite"
        fd = os.open(path, os.O_CREAT | os.O_RDWR | os.O_NOFOLLOW, 0o600)
        info = os.fstat(fd)
        os.close(fd)
        if not stat.S_ISREG(info.st_mode) or info.st_uid != os.getuid() or info.st_mode & 0o077:
            raise ValueError("Mailbox must be owner-only")
        self.db = sqlite3.connect(path)
        self.db.row_factory = sqlite3.Row
        self.db.executescript("""
            PRAGMA journal_mode=DELETE;
            PRAGMA max_page_count=2048;
            CREATE TABLE IF NOT EXISTS inbox(
                id INTEGER PRIMARY KEY AUTOINCREMENT, digest TEXT UNIQUE,
                body TEXT NOT NULL);
            CREATE TABLE IF NOT EXISTS dedup(
                id INTEGER PRIMARY KEY AUTOINCREMENT, digest TEXT UNIQUE);
            CREATE TABLE IF NOT EXISTS sends(
                id INTEGER PRIMARY KEY AUTOINCREMENT, token TEXT UNIQUE,
                body TEXT NOT NULL);
            CREATE TABLE IF NOT EXISTS meta(key TEXT PRIMARY KEY, value INTEGER);
            INSERT OR IGNORE INTO meta VALUES('notification_cursor', 0);
        """)
        for row in self.db.execute("SELECT token,body FROM sends").fetchall():
            body = json.loads(row["body"])
            if body["outcome"] == "admitted":
                body.update(outcome="uncertain", reason="listener restarted during send")
                self.update_send(row["token"], body)
        self.db.commit()

    def put(self, body):
        # Prefix is the actual source identifier supplied by the companion wire
        # protocol; a guessed full key must never affect cross-feed dedup.
        fingerprint = [body["sender_prefix"], body["sender_timestamp"],
                       body["txt_type"], body["text"]]
        digest = hashlib.sha256(json.dumps(fingerprint, ensure_ascii=True).encode()).hexdigest()
        with self.db:
            if self.db.execute("SELECT 1 FROM dedup WHERE digest=?", (digest,)).fetchone():
                return None
            self.db.execute("INSERT INTO dedup(digest) VALUES(?)", (digest,))
            cur = self.db.execute("INSERT INTO inbox(digest,body) VALUES(?,?)",
                                  (digest, json.dumps(body)))
            self.db.execute("DELETE FROM inbox WHERE id <= "
                            "(SELECT COALESCE(MAX(id),0)-? FROM inbox)", (MAX_INBOX,))
            self.db.execute("DELETE FROM dedup WHERE id <= "
                            "(SELECT COALESCE(MAX(id),0)-? FROM dedup)", (MAX_DEDUP,))
            return cur.lastrowid

    def inbox(self, cursor=0, limit=50):
        if type(cursor) is not int or cursor < 0 or type(limit) is not int or not 1 <= limit <= 100:
            raise ValueError("cursor must be nonnegative; limit must be 1–100")
        bounds = self.db.execute("SELECT MIN(id), MAX(id) FROM inbox").fetchone()
        rows = self.db.execute("SELECT id,body FROM inbox WHERE id>? ORDER BY id LIMIT ?",
                               (cursor, limit)).fetchall()
        return {"messages": [dict(json.loads(r["body"]), cursor=r["id"]) for r in rows],
                "cursor": rows[-1]["id"] if rows else cursor,
                "oldest_cursor": bounds[0], "latest_cursor": bounds[1],
                "gap": bounds[0] is not None and cursor < bounds[0] - 1}

    def notification_cursor(self):
        return self.db.execute("SELECT value FROM meta WHERE key='notification_cursor'").fetchone()[0]

    def advance(self, cursor):
        if type(cursor) is not int or cursor < 0:
            raise ValueError("Invalid notification cursor")
        latest = self.db.execute("SELECT COALESCE(MAX(id),0) FROM inbox").fetchone()[0]
        if cursor > latest:
            raise ValueError("Notification cursor exceeds mailbox")
        with self.db:
            self.db.execute("UPDATE meta SET value=MAX(value,?) WHERE key='notification_cursor'", (cursor,))

    def add_send(self, body):
        token = uuid.uuid4().hex
        with self.db:
            self.db.execute("INSERT INTO sends(token,body) VALUES(?,?)", (token, json.dumps(body)))
            self.db.execute("DELETE FROM sends WHERE id <= "
                            "(SELECT COALESCE(MAX(id),0)-? FROM sends)", (MAX_SENDS,))
        return token

    def update_send(self, token, body):
        with self.db:
            self.db.execute("UPDATE sends SET body=? WHERE token=?", (json.dumps(body), token))

    def ledger(self):
        return [dict(json.loads(r["body"]), token=r["token"])
                for r in self.db.execute("SELECT token,body FROM sends ORDER BY id DESC LIMIT 20")]


def resolve(prefix, contacts):
    if not isinstance(prefix, str) or not PREFIX.fullmatch(prefix):
        raise ValueError("Direct message has invalid sender prefix")
    candidates = {c.get("public_key") for c in contacts.values()
                  if isinstance(c, dict) and KEY.fullmatch(str(c.get("public_key", "")))
                  and c["public_key"].startswith(prefix)}
    return next(iter(candidates)) if len(candidates) == 1 else None, (
        "unique_contact" if len(candidates) == 1 else "ambiguous" if candidates else "unresolved")


class Feed:
    def __init__(self, config, target, store, notify, factory=None):
        self.config, self.target, self.store, self.notify = config, target, store, notify
        self.factory = factory
        self.mc = None
        self.state = "starting"
        self.error = None
        self.actual = None
        self.lock = asyncio.Lock()
        self.command_lock = asyncio.Lock()
        self.down = asyncio.Event()
        self.generation = 0
        self.last_received = None

    def fail(self, condition):
        self.error = condition
        self.state = "reconnecting"
        self.down.set()

    async def incoming(self, event):
        try:
            p = event.payload
            if not isinstance(p, dict):
                raise ValueError("Direct message payload must be an object")
            text, timestamp, txt_type = p.get("text"), p.get("sender_timestamp"), p.get("txt_type")
            if not isinstance(text, str) or len(text.encode()) > 1024:
                raise ValueError("Direct message text is invalid or exceeds 1024 bytes")
            if type(timestamp) is not int or not 0 <= timestamp <= 0xffffffff or txt_type not in (0, 1, 2):
                raise ValueError("Direct message timestamp or type is invalid")
            key, resolution = resolve(p.get("pubkey_prefix"), self.mc.contacts)
            body = {"feed": self.config["name"], "receiver_key": self.config["public_key"],
                    "sender_prefix": p["pubkey_prefix"], "sender_key": key,
                    "resolution": resolution, "pinned_target": key == self.target,
                    "text": text, "sender_timestamp": timestamp, "txt_type": txt_type,
                    "received_at": time.time(), "trust": "untrusted_radio_data"}
            cursor = self.store.put(body)
            self.last_received = time.time()
            if cursor is not None:
                self.notify({"event": "inbox", "cursor": cursor})
        except (ValueError, TypeError, AttributeError, sqlite3.Error, OSError) as error:
            self.fail(f"Incoming direct message failed: {type(error).__name__}: {error}")

    async def run(self, stop):
        backoff = 1
        while not stop.is_set():
            self.down.clear()
            self.state = "connecting"
            try:
                if self.factory is None:
                    # Retain the object before connecting so a cancelled
                    # handshake cannot leave an unowned companion slot open.
                    self.mc = MeshCore(TCPConnection(self.config["host"], self.config["port"]),
                                       default_timeout=5, auto_reconnect=False, only_error=True)
                    result = await asyncio.wait_for(self.mc.connect(), 12)
                    if result is None:
                        raise ConnectionError("Companion handshake failed")
                else:
                    self.mc = await asyncio.wait_for(self.factory(
                        self.config["host"], self.config["port"], default_timeout=5,
                        auto_reconnect=False, only_error=True), 12)
                if self.mc is None:
                    raise ConnectionError("Companion did not respond")
                info = self.mc.self_info
                name = info.get("name")
                self.actual = {"name": name, "public_key": info.get("public_key")}
                if name != self.config["name"] or info.get("public_key") != self.config["public_key"]:
                    raise ValueError("Companion name/public key differs from pinned feed")
                if hasattr(self.mc.commands, "send"):
                    raw_send = self.mc.commands.send
                    async def serialized_send(*args, _raw_send=raw_send, **kwargs):
                        async with self.command_lock:
                            return await _raw_send(*args, **kwargs)
                    # SDK command ERROR responses are not request-tagged.
                    # Prevent auto-fetch responses from crossing submissions.
                    self.mc.commands.send = serialized_send
                self.mc.subscribe(EventType.DISCONNECTED,
                                  lambda event: self.fail("Companion TCP disconnected"))
                self.mc.subscribe(EventType.ERROR,
                                  lambda event: self.fail("Companion command returned ERROR; connection will refresh"))
                result = await asyncio.wait_for(self.mc.commands.get_contacts(), 12)
                if result.type != EventType.CONTACTS:
                    raise ConnectionError("Companion contact retrieval failed")
                self.mc.subscribe(EventType.CONTACT_MSG_RECV, self.incoming)
                await asyncio.wait_for(self.mc.start_auto_message_fetching(), 8)
                self.generation += 1
                self.state, self.error = "connected", None
                backoff = 1
                while not stop.is_set() and not self.down.is_set():
                    waits = [asyncio.create_task(self.down.wait()), asyncio.create_task(stop.wait())]
                    try:
                        done, _ = await asyncio.wait(waits, timeout=15, return_when=asyncio.FIRST_COMPLETED)
                    finally:
                        for task in waits:
                            task.cancel()
                        await asyncio.gather(*waits, return_exceptions=True)
                    if not done:
                        async with self.lock:
                            result = await asyncio.wait_for(self.mc.commands.get_time(), 8)
                            if result.type == EventType.CURRENT_TIME:
                                # Also recover missed MESSAGES_WAITING pushes.
                                fetched = await asyncio.wait_for(self.mc.commands.get_msg(), 8)
                                if fetched.type == EventType.ERROR:
                                    raise ConnectionError("Companion mailbox fetch failed")
                        if result.type != EventType.CURRENT_TIME:
                            raise ConnectionError("Companion heartbeat failed")
            except (OSError, ValueError, ConnectionError, asyncio.TimeoutError, RuntimeError) as error:
                self.fail(f"{type(error).__name__}: {error}")
            finally:
                self.state = "reconnecting"
                if self.mc is not None:
                    try:
                        await asyncio.wait_for(self.mc.disconnect(), 5)
                    except (OSError, asyncio.TimeoutError, RuntimeError) as error:
                        self.error = f"Companion cleanup failed: {type(error).__name__}"
                    self.mc = None
            try:
                await asyncio.wait_for(stop.wait(), backoff)
            except asyncio.TimeoutError:
                pass
            backoff = min(backoff * 2, 30)
        self.state = "stopped"

    async def send(self, text, ack_timeout=30):
        if not isinstance(text, str) or not text.strip() or len(text.encode()) > 160:
            raise ValueError("Message must contain 1–160 UTF-8 bytes")
        if self.lock.locked():
            raise ValueError("Feed is busy; send was not admitted")
        async with self.lock:
            mc = self.mc
            if self.state != "connected" or mc is None:
                raise ValueError("Feed is unavailable; send was not admitted")
            try:
                contacts = await asyncio.wait_for(mc.commands.get_contacts(), 12)
            except (OSError, RuntimeError, asyncio.TimeoutError) as error:
                self.fail(f"Target contact refresh failed: {type(error).__name__}")
                raise ValueError("Contact refresh failed; send was not admitted") from error
            if contacts.type != EventType.CONTACTS or self.down.is_set() or self.mc is not mc:
                raise ValueError("Contact refresh or connection failed; send was not admitted")
            key, _ = resolve(self.target[:12], mc.contacts)
            if key != self.target:
                raise ValueError("Pinned target missing or prefix collision; send was not admitted")
            body = {"feed": self.config["name"], "target": self.target, "text": text,
                    "outcome": "admitted", "tx": "unknown", "ack": False,
                    "created_at": time.time()}
            token = self.store.add_send(body)
            early = deque(maxlen=128)
            ack = asyncio.Event()
            expected = None
            generation = self.generation

            async def on_ack(event):
                code = event.payload.get("code") if isinstance(event.payload, dict) else None
                if not isinstance(code, str) or not re.fullmatch(r"[0-9a-f]{8}", code):
                    self.fail("Malformed companion ACK code")
                    return
                early.append(code)
                if code == expected:
                    ack.set()

            subscription = mc.subscribe(EventType.ACK, on_ack)
            try:
                result = await asyncio.wait_for(mc.commands.send_msg(self.target, text), 8)
                if result.type == EventType.ERROR:
                    # A timeout or transport error does not establish rejection.
                    reason = result.payload.get("reason", "")
                    if "code" in result.payload and reason not in ("timeout", "no_event_received"):
                        body.update(outcome="rejected", reason="Companion rejected message")
                    else:
                        body.update(outcome="uncertain", reason="No submission response")
                elif result.type == EventType.MSG_SENT:
                    raw = result.payload.get("expected_ack")
                    if not isinstance(raw, bytes) or len(raw) != 4:
                        raise ValueError("Submission response has invalid expected_ack")
                    expected = raw.hex()
                    body.update(tx="submitted", expected_ack=expected)
                    self.store.update_send(token, body)
                    if expected in early:
                        ack.set()
                    ack_task = asyncio.create_task(ack.wait())
                    down_task = asyncio.create_task(self.down.wait())
                    try:
                        await asyncio.wait([ack_task, down_task], timeout=ack_timeout,
                                           return_when=asyncio.FIRST_COMPLETED)
                        if ack.is_set() and self.generation == generation:
                            body.update(outcome="acknowledged", ack=True, tx="confirmed_by_ack")
                        else:
                            body.update(outcome="uncertain", reason="Disconnected or ACK deadline expired")
                    finally:
                        for task in (ack_task, down_task):
                            task.cancel()
                        await asyncio.gather(ack_task, down_task, return_exceptions=True)
                else:
                    raise ValueError("Unexpected submission event")
            except asyncio.CancelledError:
                body.update(outcome="uncertain", reason="Listener stopped during send")
                raise
            except (OSError, ValueError, RuntimeError, asyncio.TimeoutError) as error:
                body.update(outcome="uncertain", reason=f"Submission failed: {type(error).__name__}")
                self.fail(f"Message submission failed: {type(error).__name__}")
            finally:
                subscription.unsubscribe()
                self.store.update_send(token, body)
            return dict(body, token=token)

    def status(self):
        return {"feed": self.config["name"], "endpoint": f'{self.config["host"]}:{self.config["port"]}',
                "state": self.state, "error": self.error, "actual": self.actual,
                "generation": self.generation, "last_received": self.last_received,
                "busy": self.lock.locked()}
