"""Compare two isolated six-process fleets; no lab services or RF are used."""
import argparse
from collections import deque
from contextlib import ExitStack
import hashlib
import http.client
import json
import os
from pathlib import Path
import re
import socket
import subprocess
import tempfile
import threading
import time
from unittest.mock import patch

from base_service import SharedModem, saved_state, transfer, PROFILE
from combined_service import unused_port
from observer_service import ObserverService
from parity import ROOT, public
import service_demo


class UnixHTTP(http.client.HTTPConnection):
    def __init__(self, path):
        super().__init__("localhost", timeout=4)
        self.path = str(path)

    def connect(self):
        self.sock = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        self.sock.settimeout(self.timeout)
        self.sock.connect(self.path)


def fetch(socket_path, endpoint):
    connection = UnixHTTP(socket_path)
    try:
        connection.request("GET", endpoint)
        response = connection.getresponse()
        if response.status != 200:
            raise RuntimeError(f"profiler {endpoint}: HTTP {response.status}")
        value = json.loads(response.read())
        if value.get("schema_version") != "v0.5" or "data" not in value:
            raise ValueError("profiler response requires schema v0.5 and data")
        return value["data"]
    finally:
        connection.close()


def process(pid):
    root = Path("/proc") / str(pid)
    fields = (root / "stat").read_text().rsplit(")", 1)[1].split()
    status = dict(line.split(":", 1) for line in (root / "status").read_text().splitlines())
    return {"pid": pid, "start_ticks": int(fields[19]),
            "cpu_ticks": int(fields[11]) + int(fields[12]),
            "rss_kib": int(status["VmRSS"].split()[0]),
            "threads": int(status["Threads"]),
            "executable": str((root / "exe").resolve())}


def children(pid):
    result = set()
    for task in (Path("/proc") / str(pid) / "task").iterdir():
        result.update(int(value) for value in (task / "children").read_text().split())
    return result


def private(path, data):
    path.write_bytes(data)
    path.chmod(0o600)


class Program:
    def __init__(self, binary, config, environment):
        self.lines, self.errors = deque(maxlen=100), deque(maxlen=100)
        self.proc = subprocess.Popen([str(binary), str(config)], env=environment,
                                     stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
        self.readers = [threading.Thread(target=self.capture, args=(source, destination), daemon=True)
                        for source, destination in ((self.proc.stdout, self.lines),
                                                    (self.proc.stderr, self.errors))]
        for reader in self.readers:
            reader.start()

    @staticmethod
    def capture(source, destination):
        for line in source:
            destination.append(line.strip())

    def wait(self, marker):
        end = time.monotonic() + 30
        while time.monotonic() < end:
            for line in list(self.lines):
                if line.startswith(marker):
                    return line
            if self.proc.poll() is not None:
                raise RuntimeError(f"{marker}: process exited: {list(self.lines)} {list(self.errors)}")
            time.sleep(.02)
        raise TimeoutError(f"{marker}: startup deadline: {list(self.lines)} {list(self.errors)}")

    def close(self):
        if self.proc.poll() is None:
            self.proc.terminate()
            try:
                self.proc.wait(timeout=15)
            except subprocess.TimeoutExpired:
                self.proc.kill()
                self.proc.wait()
                raise RuntimeError("isolated performance process did not shut down")
        for reader in self.readers:
            reader.join(2)
        self.proc.stdout.close()
        self.proc.stderr.close()
        if self.proc.returncode != 0:
            raise RuntimeError(f"isolated process exit={self.proc.returncode}: {list(self.errors)}")


def get_http(port, path):
    connection = http.client.HTTPConnection("127.0.0.1", port, timeout=5)
    try:
        connection.request("GET", path)
        response = connection.getresponse()
        return response.status, json.loads(response.read())
    finally:
        connection.close()


def profiler(runtime, pid):
    end = time.monotonic() + 5
    while time.monotonic() < end:
        found = [path for path in (runtime / "hew-profilers").glob("*.sock")
                 if re.search(rf"(?<!\d){pid}(?!\d)", path.name)]
        if len(found) == 1:
            fetch(found[0], "/api/metrics")
            return found[0]
        if len(found) > 1:
            raise RuntimeError(f"multiple profiler sockets for isolated PID {pid}")
        time.sleep(.05)
    raise RuntimeError(f"no private profiler socket for PID {pid} in {runtime}")


def slope(rows, label, field):
    pairs = [(row["elapsed"], row["processes"][label][field]) for row in rows]
    mean_x = sum(x for x, _ in pairs) / len(pairs)
    mean_y = sum(y for _, y in pairs) / len(pairs)
    return sum((x - mean_x) * (y - mean_y) for x, y in pairs) / sum(
        (x - mean_x) ** 2 for x, _ in pairs)


def measure(binary_directory, warmup, seconds, synchronous_reference_broker=False):
    with tempfile.TemporaryDirectory(prefix="mc-events-", dir="/tmp") as temporary, ExitStack() as cleanup:
        root = Path(temporary)
        runtime = root / "runtime"
        runtime.mkdir(mode=0o700)
        environment = os.environ | {"HEW_WORKERS": "4", "HEW_PPROF": "auto",
                                     "HEW_OBSERVE": "1", "XDG_RUNTIME_DIR": str(runtime)}
        programs = {}

        def launch(label, binary, config, marker):
            owner = Program(binary_directory / binary, config, environment)
            cleanup.callback(owner.close)
            programs[label] = owner.proc
            return owner.wait(marker)

        broker_port = unused_port()
        broker_config = root / "broker.conf"
        private(broker_config, f"host=127.0.0.1\nport={broker_port}\n".encode())
        launch("broker", "hew-broker-release", broker_config, "MQTT_BROKER_READY ")
        with patch.dict(os.environ, environment), patch.object(service_demo, "WORKER", binary_directory / "native-worker"), \
                patch.dict(os.environ, {"MESHCORE_HEW_HOST": str(binary_directory / "hew-host-release")}):
            host = ObserverService(root=root / "host", extra_config=(
                f"observer.enabled=1\nobserver.url=tcp://127.0.0.1:{broker_port}\n"
                "observer.format=internal-v1\nobserver.topic_prefix=event-performance\n"))
        cleanup.callback(host.close)
        programs["host"] = host.proc
        modem = SharedModem()
        cleanup.callback(modem.close)
        health_ports = []
        for index, seed in enumerate((17, 18)):
            frozen = root / f"frozen-{index}"
            frozen.mkdir(mode=0o700)
            document = saved_state()
            document["PublicKey"] = list(public(seed))
            document["Name"] = ("Synthetic Base", "Synthetic Secondary")[index]
            document["Preferences"]["flood_advert_seconds"] = 0
            private(frozen / "identity.seed", bytes([seed]) * 32)
            private(frozen / "companion.json", json.dumps(document).encode())
            directory = root / f"base-{index}"
            transfer(frozen, directory, "stage")
            health_port = unused_port()
            health_ports.append(health_port)
            config = root / f"base-{index}.json"
            private(config, json.dumps({"state_dir": str(directory),
                "radio_address": f"127.0.0.1:{modem.port}", "companion_listen": "127.0.0.1:0",
                "status_listen": f"127.0.0.1:{health_port}",
                "status_role": ("companion", "bot_companion")[index],
                "required_profile": PROFILE.hex(), "enable_factory_reset": False,
                "enable_key_export": False, "enable_key_import": False}).encode())
            launch(("base", "secondary")[index], "hew-base-release", config, "BASE_LISTEN ")
        dashboard_config = root / "dashboard.conf"
        private(dashboard_config, (f"port=0\nstate={host.root}\n"
            f"page={ROOT.parents[1] / 'internal/app/admin_page.html'}\n"
            f"base_port={health_ports[0]}\nbot_companion_port={health_ports[1]}\n"
            "password_env=MESHCORE_EVENT_PERFORMANCE_PASSWORD\n").encode())
        environment.update(MESHCORE_HOST_ADMIN_HTTP="1",
                           MESHCORE_EVENT_PERFORMANCE_PASSWORD="synthetic-password")
        line = launch("dashboard", "hew-dashboard-release", dashboard_config, "DASHBOARD_LISTEN ")
        dashboard_port = int(line.split("port=")[1].split()[0])
        end = time.monotonic() + 20
        while time.monotonic() < end:
            code, ready = get_http(dashboard_port, "/readyz")
            if code == 200 and ready["mqtt_connection_checked"]:
                break
            time.sleep(.1)
        else:
            raise RuntimeError("isolated six-identity fleet did not become ready")
        code, status = get_http(dashboard_port, "/status")
        expected = {"repeater": 1, "room": 4, "bot": 5, "observer": 3,
                    "companion": 17, "bot_companion": 18}
        if code != 200 or set(status) != set(expected) or any(
                status[role]["public_key"] != public(seed).hex() for role, seed in expected.items()):
            raise RuntimeError("isolated fleet identities do not match the shared fixture")
        worker_ids = children(host.proc.pid)
        if len(worker_ids) != 1:
            raise RuntimeError(f"expected one isolated native worker, got {worker_ids}")
        worker = worker_ids.pop()
        sockets = {label: profiler(runtime, owner.pid) for label, owner in programs.items()
                   if not (synchronous_reference_broker and label == "broker")}
        pids = {label: owner.pid for label, owner in programs.items()} | {"native": worker}
        print(f"FLEET_READY binaries={binary_directory} processes={len(pids)}", flush=True)
        time.sleep(warmup)
        began, rows = time.monotonic(), []
        while True:
            elapsed = time.monotonic() - began
            values = {}
            for label, pid in pids.items():
                value = process(pid)
                if label in sockets:
                    value["heap_bytes"] = fetch(sockets[label], "/api/memory")["bytes_live"]
                    value["messages"] = fetch(sockets[label], "/api/metrics")["messages_sent"]
                    value["actors"] = len(fetch(sockets[label], "/api/actors"))
                values[label] = value
            rows.append({"elapsed": elapsed, "processes": values})
            if elapsed >= seconds:
                break
            code, ready = get_http(dashboard_port, "/readyz")
            if code != 200 or not ready["mqtt_connection_checked"]:
                raise RuntimeError("fleet readiness changed during the performance interval")
            modem_errors = [source.error for source in modem.sources if source.error]
            if modem_errors or host.emulator.error:
                raise RuntimeError(f"isolated modem failed: {modem_errors} {host.emulator.error}")
            time.sleep(min(5, seconds - elapsed))
        if children(host.proc.pid) != {worker}:
            raise RuntimeError("native worker incarnation changed during performance measurement")
        interval = rows[-1]["elapsed"] - rows[0]["elapsed"]
        hz = os.sysconf("SC_CLK_TCK")
        summary = {}
        for label in pids:
            first, last = (row["processes"][label] for row in (rows[0], rows[-1]))
            if first["start_ticks"] != last["start_ticks"]:
                raise RuntimeError(f"{label} process incarnation changed")
            digest = hashlib.sha256(Path(last["executable"]).read_bytes()).hexdigest()
            item = {"binary_sha256": digest, "threads": last["threads"],
                    "cpu_percent_one_core": 100 * (last["cpu_ticks"] - first["cpu_ticks"]) / hz / interval,
                    "rss_kib_first": first["rss_kib"], "rss_kib_last": last["rss_kib"]}
            if label in sockets:
                item.update(heap_bytes_per_second=slope(rows, label, "heap_bytes"),
                            heap_growth_bytes=last["heap_bytes"] - first["heap_bytes"],
                            actor_growth=last["actors"] - first["actors"],
                            messages_per_second=(last["messages"] - first["messages"]) / interval)
            summary[label] = item
        return {"seconds": interval, "warmup_seconds": warmup, "processes": summary,
                "unprofiled": {"broker": "reference broker has no scheduler/profiler startup"}
                if synchronous_reference_broker else {},
                "cpu_percent_one_core": sum(value["cpu_percent_one_core"] for value in summary.values()),
                "samples": rows}


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--reference", type=Path, required=True, help="accepted reference build directory")
    parser.add_argument("--candidate", type=Path, default=ROOT / "build")
    parser.add_argument("--report", type=Path, required=True)
    parser.add_argument("--seconds", type=int, default=180)
    parser.add_argument("--warmup", type=int, default=30)
    parser.add_argument("--synchronous-reference-broker", action="store_true",
                        help="reference broker predates actors and cannot start the runtime profiler")
    args = parser.parse_args()
    if args.seconds < 60 or args.warmup < 15:
        raise ValueError("performance intervals require at least 60 s and 15 s warmup")
    os.umask(0o077)
    result = {"rf_commands": 0, "fixture": "six identities, 167 contacts and 256 retained messages per Base",
              "hew_workers": 4, "profiling": "same private v0.5 profiler settings on both fleets",
              "reference": measure(args.reference.resolve(), args.warmup, args.seconds,
                                   args.synchronous_reference_broker),
              "candidate": measure(args.candidate.resolve(), args.warmup, args.seconds)}
    old, new = result["reference"], result["candidate"]
    result["cpu_reduction_fraction"] = 1 - new["cpu_percent_one_core"] / old["cpu_percent_one_core"]
    previous_messages = sum(item.get("messages_per_second", 0) for item in old["processes"].values())
    current_messages = sum(item.get("messages_per_second", 0) for item in new["processes"].values())
    result["actor_message_reduction_fraction"] = 1 - current_messages / previous_messages
    failures = []
    if result["cpu_reduction_fraction"] < .5:
        failures.append("six-process CPU did not fall by at least 50%")
    if result["actor_message_reduction_fraction"] < .8:
        failures.append("profiled actor message rate did not fall by at least 80%")
    for label, item in new["processes"].items():
        if label == "native":
            continue
        if item["heap_bytes_per_second"] > 256 or item["heap_growth_bytes"] > 131072:
            failures.append(f"{label}: sustained live heap exceeds 256 B/s or 128 KiB interval growth")
        if item["actor_growth"] > 2:
            failures.append(f"{label}: actor count grew by more than two")
        if item["messages_per_second"] > 500:
            failures.append(f"{label}: idle actor traffic exceeds 500 messages/s")
    result["failures"] = failures
    args.report.parent.mkdir(parents=True, exist_ok=True)
    private(args.report, (json.dumps(result, indent=2) + "\n").encode())
    print(json.dumps({key: result[key] for key in (
        "cpu_reduction_fraction", "actor_message_reduction_fraction", "failures")}))
    if failures:
        raise SystemExit(1)


if __name__ == "__main__":
    main()
