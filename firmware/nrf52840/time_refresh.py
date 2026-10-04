#!/usr/bin/env python3
"""Refresh Pine's trusted UTC through its existing encrypted administrator CLI."""
import argparse
import logging
import re
import signal
import subprocess
import threading
import time

import sys
from pathlib import Path
sys.path.insert(0, str(Path(__file__).resolve().parents[2]))
sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "esp32"))
from tools.hardware import admin as mast_cli

LOG = logging.getLogger("pine-time")
REFRESH_SECONDS = 1800
RETRY_SECONDS = 30


def refresh(client, now):
    if not 1715770351 <= now <= 4102444800:
        raise ValueError("Host UTC is outside Pine's supported range")
    mast_cli.checked(client, "bot time " + str(now))
    reply = mast_cli.checked(client, "bot time")
    if not re.search(r"\btrusted=1\b", reply):
        raise ValueError("Pine did not confirm trusted UTC: " + reply)
    return reply


def synchronized():
    result = subprocess.run(
        ["timedatectl", "show", "--property=NTPSynchronized", "--value"],
        check=True, capture_output=True, text=True, timeout=5)
    if result.stdout.strip() != "yes":
        raise ValueError("Host UTC is not NTP-synchronized; Pine clock was not changed")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--gateway", required=True, help="Reachable shared modem hostname or IP")
    parser.add_argument("--port", type=int, default=8001)
    parser.add_argument("--seed-file", type=Path, required=True,
                        help="Private seed for an authorized Pine administrator")
    parser.add_argument("--target-key", required=True, help="Pine repeater's full public key")
    parser.add_argument("--password-file", type=Path,
                        help="Private administrator password; omit for trusted-key login")
    parser.add_argument("--once", action="store_true", help="Refresh once and exit")
    parser.add_argument("--allow-unsynchronized-clock", action="store_true",
                        help="Operator assertion that host UTC is correct without NTP confirmation")
    args = parser.parse_args()
    if not 1 <= args.port <= 65535:
        parser.error("--port must be 1..65535")
    logging.basicConfig(level=logging.INFO, format="%(levelname)s %(message)s")
    stop = threading.Event()
    for signum in (signal.SIGINT, signal.SIGTERM):
        signal.signal(signum, lambda _signum, _frame: stop.set())
    password = "" if args.password_file is None else mast_cli.private_file(args.password_file, 15).decode("ascii")
    while not stop.is_set():
        client = None
        delay = RETRY_SECONDS
        try:
            if not args.allow_unsynchronized_clock:
                synchronized()
            client = mast_cli.NativeClient(
                args.gateway, args.port, args.seed_file, args.target_key, password,
                timeout=20, tagged=False)
            reply = refresh(client, int(time.time()))
            LOG.info("Pine trusted UTC refreshed: %s", reply)
            delay = REFRESH_SECONDS
        except (OSError, ValueError, TimeoutError, subprocess.SubprocessError) as error:
            LOG.error("Pine UTC refresh failed: %s", error)
            if args.once:
                return 1
        finally:
            if client is not None:
                client.close()
        if args.once:
            return 0
        stop.wait(delay)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
