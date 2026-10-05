"""Foreground daemon and single-request clients for the shared local mailbox."""

import argparse
import asyncio
import fcntl
import json
import os
from pathlib import Path
import signal
import sqlite3
import sys

from .link import Feed, Store, load_config
import stat

MAX_REQUEST = 16384


async def request(socket, operation, args):
    reader, writer = await asyncio.wait_for(asyncio.open_unix_connection(socket, limit=262144), 3)
    try:
        writer.write((json.dumps({"operation": operation, "args": args}) + "\n").encode())
        await writer.drain()
        line = await asyncio.wait_for(reader.readline(), 60)
        if not line:
            raise ConnectionError("Listener closed without a response")
        return json.loads(line)
    finally:
        writer.close()
        await writer.wait_closed()


async def serve(config_path, state_path, attached=False):
    config = load_config(config_path)
    directory = Path(state_path)
    directory.mkdir(mode=0o700, parents=True, exist_ok=True)
    info = directory.lstat()
    if not stat.S_ISDIR(info.st_mode) or info.st_uid != os.getuid() or info.st_mode & 0o077:
        raise ValueError("State directory must be owner-only (0700)")
    lock_fd = os.open(directory / "listener.lock", os.O_CREAT | os.O_RDWR | os.O_NOFOLLOW, 0o600)
    try:
        fcntl.flock(lock_fd, fcntl.LOCK_EX | fcntl.LOCK_NB)
    except BlockingIOError:
        os.close(lock_fd)
        raise ValueError("A listener already owns this state directory")
    store = Store(state_path)
    socket = store.directory / "link.sock"
    stop = asyncio.Event()
    loop = asyncio.get_running_loop()
    for sig in (signal.SIGTERM, signal.SIGINT):
        loop.add_signal_handler(sig, stop.set)

    def emit(event):
        sys.stdout.write(json.dumps(event) + "\n")
        sys.stdout.flush()

    feeds = {row["name"]: Feed(row, config["target"], store, emit) for row in config["feeds"]}
    default_feed = config.get("default_feed", config["feeds"][0]["name"])
    clients = set()

    async def dispatch(operation, args):
        if not isinstance(args, dict):
            raise ValueError("Request args must be an object")
        if operation == "status":
            return {"feeds": [f.status() for f in feeds.values()],
                    "notification_cursor": store.notification_cursor(),
                    "recent_sends": store.ledger(),
                    "bounds": {"inbox": 512, "dedup": 2048, "sends": 256}}
        if operation == "inbox":
            return store.inbox(args.get("cursor", 0), args.get("limit", 50))
        if operation == "notifications":
            return store.inbox(store.notification_cursor(), 20)
        if operation == "advance":
            store.advance(args.get("cursor"))
            return {"cursor": store.notification_cursor()}
        if operation == "send":
            name = args.get("feed") or default_feed
            if not isinstance(name, str) or name not in feeds:
                raise ValueError("Unknown feed; inspect status")
            return await feeds[name].send(args.get("text"))
        raise ValueError("Unknown operation")

    async def client(reader, writer):
        task = asyncio.current_task()
        if len(clients) >= 16:
            writer.close()
            await writer.wait_closed()
            return
        clients.add(task)
        try:
            line = await asyncio.wait_for(reader.readline(), 3)
            if not line or len(line) > MAX_REQUEST:
                raise ValueError("Request is empty or exceeds 16 KiB")
            payload = json.loads(line)
            if not isinstance(payload, dict):
                raise ValueError("Request must be an object")
            result = await dispatch(payload.get("operation"), payload.get("args", {}))
            reply = {"ok": True, "result": result}
        except (ValueError, TypeError, OSError, sqlite3.Error, asyncio.TimeoutError) as error:
            reply = {"ok": False, "error": f"{type(error).__name__}: {error}"}
        try:
            writer.write((json.dumps(reply) + "\n").encode())
            await writer.drain()
        except (OSError, ConnectionError):
            emit({"event": "error", "error": "Local client disconnected before response"})
        finally:
            writer.close()
            await writer.wait_closed()
            clients.discard(task)

    if socket.exists():
        socket.unlink()
    server = await asyncio.start_unix_server(client, path=socket, limit=MAX_REQUEST + 1)
    os.chmod(socket, 0o600)
    tasks = [asyncio.create_task(f.run(stop), name=name) for name, f in feeds.items()]
    def feed_done(task):
        if not task.cancelled() and task.exception() is not None:
            feeds[task.get_name()].fail(f"Feed listener failed: {type(task.exception()).__name__}")
            emit({"event": "error", "error": f"Feed {task.get_name()} listener failed: {type(task.exception()).__name__}"})
    for task in tasks:
        task.add_done_callback(feed_done)
    pipe = None
    if attached:
        class ParentPipe(asyncio.Protocol):
            def connection_lost(self, error):
                stop.set()
        pipe, _ = await loop.connect_read_pipe(ParentPipe, sys.stdin)
    emit({"event": "ready", "socket": str(socket)})
    try:
        await stop.wait()
    finally:
        server.close()
        await server.wait_closed()
        for task in clients:
            task.cancel()
        await asyncio.gather(*clients, return_exceptions=True)
        for task in tasks:
            task.cancel()
        results = await asyncio.gather(*tasks, return_exceptions=True)
        for result in results:
            if isinstance(result, Exception):
                emit({"event": "error", "error": f"Feed task stopped: {type(result).__name__}"})
        if pipe:
            pipe.close()
        socket.unlink(missing_ok=True)
        store.db.close()
        os.close(lock_fd)


def main():
    os.umask(0o077)
    parser = argparse.ArgumentParser(description="Directed messages through persistent companion listeners")
    parser.add_argument("--state", required=True, help="Private listener state directory (0700)")
    sub = parser.add_subparsers(dest="command", required=True)
    daemon = sub.add_parser("serve", help="Run listeners in the foreground; no automatic resend")
    daemon.add_argument("--config", required=True, help="Private endpoint/identity configuration (0600)")
    daemon.add_argument("--attached", action="store_true", help="Exit when extension stdin closes")
    sub.add_parser("status")
    inbox = sub.add_parser("inbox", help="Read retained messages without changing notification cursor")
    inbox.add_argument("--cursor", type=int, default=0)
    inbox.add_argument("--limit", type=int, default=50)
    send = sub.add_parser("send", help="Send once to the pinned target; uncertain sends are never retried")
    send.add_argument("text")
    send.add_argument("--feed")
    args = parser.parse_args()
    try:
        if args.command == "serve":
            asyncio.run(serve(args.config, args.state, args.attached))
        else:
            params = ({"text": args.text, "feed": args.feed} if args.command == "send"
                      else {"cursor": args.cursor, "limit": args.limit} if args.command == "inbox" else {})
            reply = asyncio.run(request(str(Path(args.state) / "link.sock"), args.command, params))
            print(json.dumps(reply, ensure_ascii=True))
            if not reply["ok"]:
                sys.exit(1)
    except (ValueError, OSError, ConnectionError, asyncio.TimeoutError) as error:
        print(f"MeshCore listener: {type(error).__name__}: {error}", file=sys.stderr)
        sys.exit(1)


if __name__ == "__main__":
    main()
