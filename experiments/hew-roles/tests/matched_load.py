"""Finite Go/Hew workload on one shared KISS fixture; no physical radio."""
import argparse
from contextlib import ExitStack
import hashlib
import json
import math
import os
from pathlib import Path
import queue
import shutil
import socket
import struct
import subprocess
import sys
import threading
import time

sys.path.insert(0, str(Path(__file__).resolve().parent))
from base_service import Source, PROFILE
from event_performance import Program, children, fetch, get_http, private, process, profiler
from parity import ROOT, packet, parse, public, seal, secret
from service_demo import Emulator, RunningService, addressed, advert, body
sys.path.insert(0, str(ROOT))
from migrate_go import identity_public, migrate, write_private


def airtime(length):
    frequency, bandwidth, spreading, coding, power, factor, cad, threshold = struct.unpack(
        "<IIBBBfBh", PROFILE)
    symbol = (2 ** spreading) * 1000 / bandwidth
    low_rate = symbol >= 16
    payload = 8 + max(0, math.ceil((8 * length - 4 * spreading + 44) /
                                  (4 * (spreading - 2 * low_rate))) * coding)
    return math.ceil((8 + 4.25 + payload) * symbol)


class Connection(Source):
    def __init__(self, modem, connection, epoch):
        self.modem, self.epoch = modem, epoch
        self.ports = {0}
        self.announced = {}
        self.write_lock = threading.Lock()
        self.hold = self.hold_all = self.drift = self.malformed_hello = False
        self.identity_root = None
        self.policy_mode = "ok"
        self.policy_before_apply = None
        self.policy_requests = queue.Queue()
        self.policy_counts = {}
        self.all_submissions = []
        self.submissions = queue.Queue()
        super().__init__(connection=connection, port=modem.port)

    def send(self, port, command, value, connection=None, metadata=b"\xf9\x0c\xb7"):
        Emulator.send(self, port, command, value, connection, metadata)

    def event(self, port, generation, job, state, connection, wait=0, duration=0):
        self.send(port, 6, b"\xfa\1" + struct.pack("<II", generation, job) +
                  bytes([state, 0]) + struct.pack("<III", wait, duration, duration), connection)

    def handle(self, value):
        port, command = value[0] >> 4, value[0] & 15
        if command != 6 or len(value) < 2:
            raise AssertionError("shared fixture requires a hardware request")
        operation, data = value[1], value[2:]
        generation = 100 * self.epoch + port + 1
        self.controls.append((self.epoch, port, operation, data.hex()))
        if operation == 32:
            self.ports.add(port)
        if operation == 37:
            if port != 0 or data not in (b"\1", b"\1\1"):
                raise AssertionError("invalid shared-source capacity request")
            reply = b"\1\4\5" + (b"\4\1" if len(data) == 2 else b"")
        elif operation == 34:
            if data != b"\1\0":
                raise AssertionError("matched workload attempted a PHY change")
            reply = b"\1\0" + struct.pack("<I", 17) + PROFILE
        elif operation == 38:
            if (len(data) != 34 or data[0] != 1 or data[1] > 3 or
                    data[2:] != self.modem.role_keys[data[1]]):
                raise AssertionError("invalid shared-source role announcement")
            self.announced[port] = data[1]
            reply = b"\1\0" + struct.pack("<I", generation) + b"\0\0"
        elif operation == 15:
            if len(data) != 1:
                raise AssertionError("invalid shared-source airtime length")
            reply = struct.pack("<I", airtime(data[0]))
        elif operation == 23:
            if data or port != 0:
                raise AssertionError("heartbeat must be a read-only port-zero request")
            reply = b""
        elif operation == 36:
            if data != b"\1":
                raise AssertionError("invalid shared-source statistics version")
            self.announced[port] = 2
            with self.modem.condition:
                done = [item for item in self.modem.completed]
                owned = [item for item in done if item["source"] == self.epoch and item["port"] == port]
                total_time = sum(item["airtime_ms"] for item in done)
                owned_time = sum(item["airtime_ms"] for item in owned)
                count = self.modem.received
            reply = b"\1" + struct.pack("<9I", 17, count, total_time, len(done), 0,
                                        count, owned_time, len(owned), 0) + b"\0\0"
        elif operation == 33:
            if len(data) < 19 or data[0] != 1:
                raise AssertionError("invalid shared-source submission")
            actual_generation, identifier = struct.unpack_from("<II", data, 1)
            if actual_generation != generation:
                raise AssertionError("submission belongs to another source generation")
            delay, expiry = struct.unpack_from("<II", data, 10)
            if expiry != 0:
                raise AssertionError("unexpected workload expiry")
            raw = data[18:]
            item = {"source": self.epoch, "port": port, "generation": generation,
                    "job": identifier, "priority": data[9], "delay": delay,
                    "raw": raw, "airtime_ms": airtime(len(raw)),
                    "submitted": time.monotonic(), "connection": self}
            self.all_submissions.append(item)
            self.event(port, generation, identifier, 1, self.connection)
            self.modem.enqueue(item)
            return
        elif operation in (19, 16, 13, 20, 18):
            if data or port != 0:
                raise AssertionError("telemetry request must be read-only port zero")
            reply = {19: struct.pack("<H", 3900), 16: struct.pack("<h", -117),
                     13: struct.pack("<b", -73), 20: struct.pack("<h", 251),
                     18: struct.pack("<III", self.modem.received, len(self.modem.completed), 0)}[operation]
        else:
            Emulator.handle(self, value, self.connection, self.epoch)
            return
        self.send(port, 6, bytes([operation | 128]) + reply)

    def receive(self, raw, snr=12, rssi=-73):
        for port in sorted(self.ports):
            self.send(port, 0, raw)


class SharedFixture:
    def __init__(self):
        self.listener = socket.socket()
        self.listener.bind(("127.0.0.1", 0))
        self.listener.listen(4)
        self.listener.settimeout(.1)
        self.port = self.listener.getsockname()[1]
        self.sources = []
        self.condition = threading.Condition()
        self.channel = threading.RLock()
        self.pending = []
        self.completed = []
        self.responses = queue.Queue()
        self.received = self.max_queue = 0
        self.peer_acks = 0
        self.error = None
        self.role_keys = [public(seed) for seed in (1, 4, 5, 3)]
        self.stop = False
        self.acceptor = threading.Thread(target=self.accept, daemon=True)
        self.transmitter = threading.Thread(target=self.transmit, daemon=True)
        self.acceptor.start()
        self.transmitter.start()

    def active(self):
        return [source for source in self.sources if source.thread.is_alive()]

    def accept(self):
        while not self.stop:
            try:
                connection, _ = self.listener.accept()
            except socket.timeout:
                continue
            except OSError:
                return
            if len(self.active()) >= 4:
                connection.close()
            else:
                self.sources.append(Connection(self, connection, len(self.sources) + 1))

    def enqueue(self, item):
        with self.condition:
            self.pending.append(item)
            self.max_queue = max(self.max_queue, len(self.pending))
            if len(self.pending) > 64:
                raise AssertionError("finite workload exceeded fixture queue budget")
            self.condition.notify_all()

    def transmit(self):
        try:
            self.transmit_loop()
        except BaseException as error:
            self.error = error

    def transmit_loop(self):
        while True:
            with self.condition:
                while not self.pending and not self.stop:
                    self.condition.wait(.1)
                if self.stop:
                    return
                now = time.monotonic()
                ready = [item for item in self.pending if item["submitted"] + item["delay"] / 1000 <= now]
                if not ready:
                    self.condition.wait(.001)
                    continue
                item = min(ready, key=lambda job: (job["priority"], job["submitted"]))
                self.pending.remove(item)
            with self.channel:
                item["queue_wait_ms"] = math.ceil((time.monotonic() - item["submitted"]) * 1000)
                time.sleep(item["airtime_ms"] / 1000)
                item["terminal"] = 2
                item["completed"] = time.monotonic()
                item["connection"].event(item["port"], item["generation"], item["job"], 2,
                                         item["connection"].connection,
                                         item["queue_wait_ms"], item["airtime_ms"])
                with self.condition:
                    self.completed.append(item)
                self.responses.put(item)
                decoded = parse(item["raw"])
                if decoded[0] == 2 and decoded[4][:1] == public(2)[:1]:
                    for seed in (1, 4, 5):
                        if decoded[4][1:2] == public(seed)[:1]:
                            plain = body(item["raw"], seed, 2).rstrip(b"\0")
                            proof = hashlib.sha256(plain + public(seed)).digest()[:4]
                            self.peer_acks += 1
                            self.receive(packet(3, proof))
                            break

    def receive(self, raw):
        with self.channel:
            time.sleep(airtime(len(raw)) / 1000)
            self.received += 1
            for source in self.active():
                source.receive(raw)
        time.sleep(0)

    def check(self):
        if self.error:
            raise self.error
        for source in self.sources:
            if source.error:
                raise source.error

    def quiet(self):
        now = time.monotonic()
        delay = max([max(0, item["submitted"] + item["delay"] / 1000 - now)
                     for item in self.pending] or [0])
        budget = delay + sum(item["airtime_ms"] for item in self.pending) / 1000 + 3
        if budget > 30:
            raise RuntimeError("shared fixture queued work exceeds the finite 30 s completion budget")
        deadline = now + budget
        while time.monotonic() < deadline:
            self.check()
            if not self.pending and all(len(source.all_submissions) == sum(
                    item["source"] == source.epoch for item in self.completed) for source in self.sources):
                return
            time.sleep(.025)
        raise RuntimeError("shared fixture retained a nonterminal transmission: " + str({
            "pending": [(item["source"], item["port"], item["delay"], item["priority"])
                        for item in self.pending],
            "counts": [(source.epoch, len(source.all_submissions), sum(
                item["source"] == source.epoch for item in self.completed)) for source in self.sources],
            "transmitter_alive": self.transmitter.is_alive()}))

    def reply(self, wanted, timeout=12):
        deadline = time.monotonic() + timeout
        replies = {}
        while time.monotonic() < deadline:
            self.check()
            try:
                item = self.responses.get(timeout=.1)
            except queue.Empty:
                continue
            if parse(item["raw"])[0] == 2:
                for role, seed in (("repeater", 1), ("room", 4), ("bot", 5)):
                    if parse(item["raw"])[4][1:2] != public(seed)[:1]:
                        continue
                    try:
                        text = body(item["raw"], seed, 2)[5:].rstrip(b"\0").decode()
                    except (ValueError, UnicodeDecodeError):
                        continue
                    for key, expected in list(wanted.items()):
                        if key[0] == role and expected in text:
                            replies[f"{role}:{key[1]}"] = text
                            del wanted[key]
                            break
            if not wanted:
                return replies
        raise RuntimeError(f"shared fixture replies incomplete: {sorted(wanted)}")

    def close(self):
        self.stop = True
        self.listener.close()
        with self.condition:
            self.condition.notify_all()
        self.acceptor.join(3)
        self.transmitter.join(3)
        for source in self.sources:
            source.close()


def available():
    with socket.socket() as listener:
        listener.bind(("127.0.0.1", 0))
        return listener.getsockname()[1]


def measure(kind, root, modem, go_binary, broker_port, bursts):
    with ExitStack() as cleanup:
        original = root / "go"
        subprocess.run(["go", "test", "../../internal/roles", "-run",
                        "^TestWillowMigrationFixture$", "-count=1"], cwd=ROOT,
                       env=os.environ | {"TMPDIR": str(ROOT / "build"),
                                         "MESHCORE_WILLOW_FIXTURE": str(original)},
                       check=True, capture_output=True, timeout=60)
        origin = RunningService(root=root / "origin")
        try:
            origin.stop_process()
            shutil.copytree(origin.root / "native", original / "bot/native")
        finally:
            origin.close()
        (original / "observer").mkdir(mode=0o700, exist_ok=True)
        write_private(original / "observer/identity.seed", bytes([3]) * 32)
        runtime = root / "runtime"
        runtime.mkdir(mode=0o700)
        environment = os.environ | {"HEW_WORKERS": "4", "HEW_PPROF": "auto",
                                     "HEW_OBSERVE": "1", "XDG_RUNTIME_DIR": str(runtime),
                                     "MESHCORE_MATCHED_ADMIN": "admin"}
        status_port = available()
        settings = root / "settings"
        write_private(settings, (
            f"address=127.0.0.1\nport={modem.port}\nprofile={PROFILE.hex()}\n"
            "password=room\nadmin=admin\nbot_home=\nbot_default=\n"
            "relay.name=Birch-repeater\nroom.name=Birch-room\n"
            f"observer.enabled=1\nobserver.url=tcp://127.0.0.1:{broker_port}\n"
            "observer.format=internal-v1\nobserver.topic_prefix=matched\n").encode())
        state = root / "h"
        migrate(original, settings, state)
        observer = state / ("observer.expanded" if (state / "observer.expanded").exists()
                            else "observer.seed")
        modem.role_keys = [public(1), public(4), public(5), identity_public(observer.read_bytes())]
        if kind == "go":
            config = root / "config.json"
            private(config, json.dumps({
                "radio_address": f"127.0.0.1:{modem.port}", "radio_session": "per_role",
                "radio_client_capacity": 4, "phy_authority": "modem", "require_parity": True,
                "radio": {"FreqHz": 912525000, "BwHz": 250000, "SF": 7, "CR": 5},
                "tx_power": 2, "phy_profile": {"airtime_factor": 1, "cad_enabled": True},
                "state_dir": str(original), "enabled_roles": ["repeater", "room", "bot", "observer"],
                "bot_runtime": "native_lua", "bot_native_worker": str(ROOT / "build/native-worker"),
                "bot_listen": "not-a-listener", "status_listen": f"127.0.0.1:{status_port}",
                "room_password": "room", "admin_password_env": "MESHCORE_MATCHED_ADMIN",
                "mqtt": {"url": f"tcp://127.0.0.1:{broker_port}", "broker_listen": "",
                         "topic_prefix": "matched",
                         "format": "internal-v1"}
            }).encode())
            owner = subprocess.Popen([str(go_binary), "-config", str(config)],
                stdout=(root / "application.log").open("wb"), stderr=subprocess.STDOUT,
                env=environment)
            cleanup.callback(lambda: owner.wait(timeout=15))
            cleanup.callback(owner.terminate)
            deadline = time.monotonic() + 30
            while time.monotonic() < deadline:
                if owner.poll() is not None:
                    raise RuntimeError("Go failed same-fixture startup; inspect application.log")
                try:
                    code, ready = get_http(status_port, "/readyz")
                    if code == 200 and ready["ready"]:
                        break
                except (OSError, ValueError):
                    pass
                time.sleep(.05)
            else:
                raise RuntimeError("Go did not become ready on matched fixture")
            code, health = get_http(status_port, "/status")
            if code != 200 or any(health[role].get("state") != "running" or not
                    health[role].get("radio_connected") for role in ("repeater", "room", "bot", "observer")):
                raise RuntimeError("Go four-role health failed same-fixture compatibility")
            if len(modem.active()) != 4:
                raise RuntimeError("Go/per_role did not admit exactly four physical sources")
            profile_socket = None
        else:
            program = Program(ROOT / "build/hew-host-release", state, environment)
            cleanup.callback(program.close)
            program.wait("ONLINE ")
            program.wait("BOT_READY ")
            owner = program.proc
            profile_socket = profiler(runtime, owner.pid)
            if len(modem.active()) != 1:
                raise RuntimeError("Hew did not admit one four-port source")
        worker = children(owner.pid)
        if len(worker) != 1:
            raise RuntimeError("matched workload requires one unchanged native worker")
        worker = next(iter(worker))
        stamp = int(time.time()) - 100
        modem.receive(packet(4, parse(advert(2))[4], route=2, width=3))
        for recipient in (1, 4):
            modem.receive(packet(7, public(recipient)[:1] + public(2) +
                seal(secret(2, public(recipient)), struct.pack("<II", stamp, 0) + b"admin\0"), route=2))
        time.sleep(.5)
        modem.quiet()

        def burst(start, count):
            wanted = {}
            for index in range(start, start + count):
                for role, recipient in (("repeater", 1), ("room", 4), ("bot", 5)):
                    text = f"!calc 10000 + {index}" if role == "bot" else f"{index:02x}|get name"
                    wanted[(role, index)] = str(10000 + index) if role == "bot" else f"{index:02x}|"
                    raw = addressed(2, recipient, 2, struct.pack("<IB", stamp + index + 1,
                                     0 if role == "bot" else 4) + text.encode(), route=2)
                    modem.receive(raw)
            replies = modem.reply(wanted)
            modem.quiet()
            return replies

        burst(0, 4)

        def capture():
            result = {"host": process(owner.pid), "native": process(worker)}
            if profile_socket:
                result["host"].update(
                    heap_bytes=fetch(profile_socket, "/api/memory")["bytes_live"],
                    actors=len(fetch(profile_socket, "/api/actors")),
                    messages=fetch(profile_socket, "/api/metrics")["messages_sent"])
            return result

        first = capture()
        offset = len(modem.completed)
        peer_acks = modem.peer_acks
        began = time.monotonic()
        replies = {}
        for number in range(bursts):
            replies.update(burst(4 + 8 * number, 8))
        seconds = time.monotonic() - began
        last = capture()
        if children(owner.pid) != {worker}:
            raise RuntimeError("native worker changed during matched workload")
        completed = modem.completed[offset:]
        digest = {
            "compatible_same_fixture": True, "physical_sources": len(modem.active()),
            "binary_sha256": {name: hashlib.sha256(Path(value["executable"]).read_bytes()).hexdigest()
                              for name, value in first.items()},
            "requests": bursts * 24, "request_count_per_role": bursts * 8,
            "reply_payload_sha256": hashlib.sha256(json.dumps(replies, sort_keys=True).encode()).hexdigest(),
            "reply_payloads": replies,
            "seconds": seconds, "before": first, "after": last,
            "cpu_percent_one_core": sum(
                100 * (last[name]["cpu_ticks"] - first[name]["cpu_ticks"]) /
                os.sysconf("SC_CLK_TCK") / seconds for name in first),
            "terminal_count": len(completed),
            "emulated_recipient_acks": modem.peer_acks - peer_acks,
            "modeled_reported_rf_airtime_ms": sum(item["airtime_ms"] for item in completed),
            "max_queue_wait_ms": max(item["queue_wait_ms"] for item in completed),
            "max_observed_fixture_queue": modem.max_queue,
            "terminal_states": sorted(set(item["terminal"] for item in completed)),
            "completed_transmissions_per_role": {
                role: sum(next(source for source in modem.sources if source.epoch == item["source"]).
                          announced.get(item["port"]) == index for item in completed)
                for index, role in enumerate(("repeater", "room", "bot", "observer"))},
            "native_memory": "RSS/arena proxy, not live allocator bytes"}
        if profile_socket:
            time.sleep(3)
            settled = capture()
            digest["settled"] = settled
            if settled["host"]["heap_bytes"] - first["host"]["heap_bytes"] > 131072:
                raise RuntimeError("Hew finite workload live heap grew over 128 KiB")
            if settled["host"]["actors"] - first["host"]["actors"] > 2:
                raise RuntimeError("Hew finite workload actors grew over two")
            if settled["host"]["fds"] > first["host"]["fds"]:
                raise RuntimeError("Hew finite workload descriptor growth")
        return digest


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--go-binary", type=Path, required=True)
    parser.add_argument("--report", type=Path, required=True)
    parser.add_argument("--bursts", type=int, default=3)
    args = parser.parse_args()
    if not 1 <= args.bursts <= 8:
        raise ValueError("finite workload requires one to eight 24-request bursts")
    os.umask(0o077)
    root = Path(os.environ.get("MESHCORE_NATIVE_TEST_STATE_ROOT", ROOT.parents[1])) / f".ml-{os.getpid()}"
    root.mkdir(mode=0o700)
    report = {"rf_commands": 0, "hew_workers": 4, "fixture": "shared KISS source with ordinary and virtual ports",
              "airtime": "half-duplex modeled LoRa SF7/BW250kHz/CR4:5 from actual RX/TX packet lengths; not measured RF airtime",
              "recipient_ack": "emulated peer generates native proofs after receipt; not a physical RF ACK",
              "queue_budget": 64, "maximum_capacity_claim": False}
    try:
        with ExitStack() as cleanup:
            modem = SharedFixture()
            cleanup.callback(modem.close)
            broker_port = available()
            config = root / "broker"
            private(config, f"host=127.0.0.1\nport={broker_port}\n".encode())
            broker = Program(ROOT / "build/hew-broker-release", config, os.environ | {"HEW_WORKERS": "4"})
            cleanup.callback(broker.close)
            broker.wait("MQTT_BROKER_READY ")
            for kind in ("go", "hew"):
                place = root / kind
                place.mkdir(mode=0o700)
                report[kind] = measure(kind, place, modem, args.go_binary.resolve(),
                                       broker_port, args.bursts)
                deadline = time.monotonic() + 5
                while modem.active() and time.monotonic() < deadline:
                    time.sleep(.05)
                if modem.active():
                    raise RuntimeError("prior role owners still occupy the shared fixture")
                modem.check()
            if report["go"]["reply_payload_sha256"] != report["hew"]["reply_payload_sha256"]:
                raise RuntimeError("Go/Hew authenticated reply payloads differ on the shared fixture")
            report["accepted"] = True
    except BaseException as error:
        report["failure"] = str(error)
        raise
    finally:
        args.report.parent.mkdir(parents=True, exist_ok=True)
        private(args.report, (json.dumps(report, indent=2) + "\n").encode())
        if report.get("accepted"):
            shutil.rmtree(root)
        else:
            print("Failed private fixture retained:", root, flush=True)
    print(json.dumps({"accepted": True,
                      "go_requests": report["go"]["requests"], "hew_requests": report["hew"]["requests"],
                      "go_cpu": report["go"]["cpu_percent_one_core"],
                      "hew_cpu": report["hew"]["cpu_percent_one_core"]}))


if __name__ == "__main__":
    main()
