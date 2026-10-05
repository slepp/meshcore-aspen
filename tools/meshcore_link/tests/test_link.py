import asyncio
import json
import os
from pathlib import Path
import shutil
from types import SimpleNamespace
import unittest
import uuid
from unittest.mock import patch

from meshcore import EventType

from tools.meshcore_link.link import Feed, Store, load_config, resolve
from tools.meshcore_link.__main__ import request, serve

TARGET = "ab" * 32
BASE = "cd" * 32
OTHER = "12" * 32


def event(kind, payload):
    return SimpleNamespace(type=kind, payload=payload)


class Subscription:
    def __init__(self, mc, kind, callback):
        self.mc, self.kind, self.callback = mc, kind, callback

    def unsubscribe(self):
        self.mc.callbacks[self.kind].remove(self.callback)


class FakeCore:
    def __init__(self, behavior="early"):
        self.contacts = {TARGET: {"public_key": TARGET}}
        self.self_info = {"name": "Example-Base", "public_key": BASE}
        self.callbacks = {}
        self.commands = self
        self.behavior = behavior
        self.sent = 0
        self.closed = False
        self.fetch_started = False

    def subscribe(self, kind, callback):
        self.callbacks.setdefault(kind, []).append(callback)
        return Subscription(self, kind, callback)

    async def emit(self, kind, payload):
        for callback in list(self.callbacks.get(kind, [])):
            result = callback(event(kind, payload))
            if asyncio.iscoroutine(result):
                await result

    async def send_msg(self, destination, text):
        self.sent += 1
        if self.behavior == "timeout":
            await asyncio.sleep(60)
        if self.behavior == "early":
            await self.emit(EventType.ACK, {"code": "aabbccdd"})
        if self.behavior == "wrong":
            await self.emit(EventType.ACK, {"code": "11223344"})
        if self.behavior == "disconnect":
            await self.emit(EventType.DISCONNECTED, {})
        if self.behavior == "error":
            return event(EventType.ERROR, {"code": 4})
        if self.behavior == "no_response":
            return event(EventType.ERROR, {"reason": "no_event_received"})
        return event(EventType.MSG_SENT, {"expected_ack": bytes.fromhex("aabbccdd")})

    async def get_contacts(self):
        return event(EventType.CONTACTS, self.contacts)

    async def get_time(self):
        return event(EventType.CURRENT_TIME, {})

    async def get_msg(self):
        return event(EventType.NO_MORE_MSGS, {})

    async def start_auto_message_fetching(self):
        self.fetch_started = True

    async def disconnect(self):
        self.closed = True


class LinkTests(unittest.IsolatedAsyncioTestCase):
    def setUp(self):
        self.root = Path("tools/meshcore_link/tests") / (".work-" + uuid.uuid4().hex)
        self.root.mkdir(mode=0o700)
        self.store = Store(self.root / "state")
        self.config = {"name": "Example-Base", "host": "localhost", "port": 5000,
                       "public_key": BASE}
        self.notifications = []
        self.feed = Feed(self.config, TARGET, self.store, self.notifications.append)
        self.feed.mc = FakeCore()
        self.feed.state = "connected"
        self.feed.generation = 1

    def tearDown(self):
        self.store.db.close()
        shutil.rmtree(self.root)

    def message(self, text="hello", prefix=TARGET[:12], timestamp=42):
        return event(EventType.CONTACT_MSG_RECV,
                     {"pubkey_prefix": prefix, "text": text, "sender_timestamp": timestamp,
                      "txt_type": 0})

    async def test_ack_before_submit_response(self):
        result = await self.feed.send("hello", ack_timeout=.02)
        self.assertEqual(result["outcome"], "acknowledged")
        self.assertEqual(result["expected_ack"], "aabbccdd")
        self.assertEqual(result["tx"], "confirmed_by_ack")
        self.assertEqual(self.feed.mc.callbacks[EventType.ACK], [])

    async def test_wrong_ack_timeout_never_replays(self):
        self.feed.mc.behavior = "wrong"
        result = await self.feed.send("hello", ack_timeout=.01)
        self.assertEqual(result["outcome"], "uncertain")
        self.assertEqual(result["tx"], "submitted")
        await asyncio.sleep(.02)
        self.assertEqual(self.feed.mc.sent, 1)

    async def test_rejection_distinct_from_missing_response(self):
        self.feed.mc.behavior = "error"
        result = await self.feed.send("hello", ack_timeout=.01)
        self.assertEqual(result["outcome"], "rejected")
        self.feed.mc.behavior = "no_response"
        result = await self.feed.send("hello", ack_timeout=.01)
        self.assertEqual(result["outcome"], "uncertain")

    async def test_admitted_ledger_and_cancelled_submission(self):
        self.feed.mc.behavior = "timeout"
        sending = asyncio.create_task(self.feed.send("one"))
        await asyncio.sleep(.01)
        self.assertEqual(self.store.ledger()[0]["outcome"], "admitted")
        sending.cancel()
        with self.assertRaises(asyncio.CancelledError):
            await sending
        self.assertEqual(self.store.ledger()[0]["outcome"], "uncertain")
        self.assertEqual(self.feed.mc.sent, 1)

    async def test_cross_feed_dedup_and_cursor(self):
        await self.feed.incoming(self.message())
        second = Feed(dict(self.config, name="Another-Base", public_key=OTHER),
                      TARGET, self.store, self.notifications.append)
        second.mc = FakeCore()
        await second.incoming(self.message())
        box = self.store.inbox()
        self.assertEqual(len(box["messages"]), 1)
        self.assertEqual(len(self.notifications), 1)
        msg = box["messages"][0]
        self.assertEqual(msg["feed"], "Example-Base")
        self.assertEqual(msg["sender_key"], TARGET)
        self.assertEqual(msg["trust"], "untrusted_radio_data")
        self.assertEqual(self.store.inbox(box["cursor"])["messages"], [])
        self.store.advance(box["cursor"])
        self.assertEqual(self.store.notification_cursor(), box["cursor"])

    async def test_unresolved_ambiguous_and_malformed(self):
        collision = TARGET[:12] + "ff" * 26
        self.feed.mc.contacts[collision] = {"public_key": collision}
        await self.feed.incoming(self.message())
        body = self.store.inbox()["messages"][0]
        self.assertEqual(body["resolution"], "ambiguous")
        self.assertIsNone(body["sender_key"])
        self.assertFalse(body["pinned_target"])
        await self.feed.incoming(self.message(prefix=OTHER[:12]))
        self.assertEqual(self.store.inbox()["messages"][-1]["resolution"], "unresolved")
        with self.assertRaisesRegex(ValueError, "collision"):
            await self.feed.send("no")
        await self.feed.incoming(self.message(prefix="bad"))
        self.assertIn("invalid sender prefix", self.feed.error)

    async def test_limits_and_durable_restart(self):
        for i in range(2100):
            await self.feed.incoming(self.message(text=str(i), timestamp=i))
        self.assertEqual(self.store.db.execute("SELECT COUNT(*) FROM inbox").fetchone()[0], 512)
        self.assertEqual(self.store.db.execute("SELECT COUNT(*) FROM dedup").fetchone()[0], 2048)
        self.assertTrue(self.store.inbox()["gap"])
        for i in range(300):
            self.store.add_send({"outcome": "admitted", "text": str(i)})
        self.assertEqual(self.store.db.execute("SELECT COUNT(*) FROM sends").fetchone()[0], 256)
        self.store.advance(2100)
        self.store.db.close()
        self.store = Store(self.root / "state")
        self.feed.store = self.store
        self.assertEqual(self.store.notification_cursor(), 2100)
        self.assertEqual(self.store.ledger()[0]["outcome"], "uncertain")
        self.assertEqual(self.store.inbox(2099)["messages"][0]["text"], "2099")

    async def test_reconnect_pending_send_no_replay(self):
        made = []
        async def factory(*args, **kwargs):
            core = FakeCore("disconnect" if not made else "early")
            made.append(core)
            return core
        feed = Feed(self.config, TARGET, self.store, self.notifications.append, factory)
        stop = asyncio.Event()
        running = asyncio.create_task(feed.run(stop))
        for _ in range(100):
            if feed.state == "connected":
                break
            await asyncio.sleep(.01)
        self.assertTrue(made[0].fetch_started)
        result = await feed.send("one", ack_timeout=.2)
        self.assertEqual(result["outcome"], "uncertain")
        await asyncio.sleep(1.1)
        self.assertEqual(feed.state, "connected")
        self.assertEqual(len(made), 2)
        self.assertEqual(made[0].sent, 1)
        self.assertEqual(made[1].sent, 0)
        stop.set()
        await asyncio.wait_for(running, 2)
        self.assertTrue(all(core.closed for core in made))

    async def test_wrong_base_pin_never_fetches(self):
        core = FakeCore()
        core.self_info["public_key"] = OTHER
        async def factory(*args, **kwargs):
            return core
        feed = Feed(self.config, TARGET, self.store, self.notifications.append, factory)
        stop = asyncio.Event()
        task = asyncio.create_task(feed.run(stop))
        await asyncio.sleep(.03)
        self.assertIn("differs from pinned", feed.error)
        self.assertFalse(core.fetch_started)
        self.assertTrue(core.closed)
        stop.set()
        await task

    async def test_invalid_configs_and_private_permissions(self):
        path = self.root / "config.json"
        good = {"target": TARGET, "feeds": [self.config]}
        for bad in [None, {}, {"target": TARGET, "feeds": []},
                    dict(good, default_feed="unknown"),
                    dict(good, feeds=[self.config, self.config]),
                    dict(good, feeds=[dict(self.config, port=True)]),
                    dict(good, feeds=[dict(self.config, public_key="ab")])]:
            path.write_text(json.dumps(bad))
            path.chmod(0o600)
            with self.assertRaises(ValueError):
                load_config(path)
        path.write_text(json.dumps(good))
        self.assertEqual(load_config(path), good)
        path.chmod(0o644)
        with self.assertRaisesRegex(ValueError, "owner-only"):
            load_config(path)

    async def test_inbox_validation_and_send_size(self):
        for cursor, limit in [(-1, 1), (0, 101), (True, 50)]:
            with self.assertRaises(ValueError):
                self.store.inbox(cursor, limit)
        for text in ["", " " * 5, "é" * 81]:
            with self.assertRaises(ValueError):
                await self.feed.send(text)
        self.assertEqual(self.feed.mc.sent, 0)

    async def test_busy_feed_not_admitted(self):
        async with self.feed.lock:
            with self.assertRaisesRegex(ValueError, "busy"):
                await self.feed.send("no")
        self.assertEqual(self.store.ledger(), [])

    async def test_serializes_sdk_commands_and_retains_connection(self):
        core = FakeCore()
        active = 0
        peak = 0
        async def raw_send(*args, **kwargs):
            nonlocal active, peak
            active += 1
            peak = max(peak, active)
            await asyncio.sleep(.02)
            active -= 1
        core.send = raw_send
        async def factory(*args, **kwargs):
            return core
        feed = Feed(self.config, TARGET, self.store, self.notifications.append, factory)
        stop = asyncio.Event()
        running = asyncio.create_task(feed.run(stop))
        for _ in range(100):
            if feed.state == "connected":
                break
            await asyncio.sleep(.01)
        await asyncio.gather(core.send(), core.send())
        self.assertEqual(peak, 1)
        self.assertIs(feed.mc, core)
        stop.set()
        await running

    async def test_local_rpc_status_inbox_invalid_and_single_owner(self):
        config_path = self.root / "config.json"
        config_path.write_text(json.dumps({"target": TARGET, "feeds": [self.config]}))
        config_path.chmod(0o600)
        state = self.root / "rpc-state"
        async def fake_connect(core):
            core.self_info = {"name": "Example-Base", "public_key": BASE}
            core.contacts = {TARGET: {"public_key": TARGET}}
            return event(EventType.SELF_INFO, core.self_info)
        core = FakeCore()
        core.connect = lambda: fake_connect(core)
        with patch("tools.meshcore_link.link.MeshCore", return_value=core):
            running = asyncio.create_task(serve(config_path, state))
            try:
                for _ in range(100):
                    if (state / "link.sock").exists():
                        break
                    await asyncio.sleep(.01)
                await asyncio.sleep(.03)
                reply = await request(str(state / "link.sock"), "status", {})
                self.assertTrue(reply["ok"])
                self.assertEqual(reply["result"]["feeds"][0]["state"], "connected")
                self.assertTrue((await request(str(state / "link.sock"), "inbox", {}))["ok"])
                self.assertFalse((await request(str(state / "link.sock"), "inbox", {"cursor": -1}))["ok"])
                self.assertFalse((await request(str(state / "link.sock"), "no_such_op", {}))["ok"])
                with self.assertRaisesRegex(ValueError, "already owns"):
                    await serve(config_path, state)
            finally:
                running.cancel()
                with self.assertRaises(asyncio.CancelledError):
                    await running
        self.assertTrue(core.closed)
        self.assertFalse((state / "link.sock").exists())


if __name__ == "__main__":
    unittest.main()
