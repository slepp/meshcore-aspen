# SPDX-License-Identifier: Apache-2.0
import hashlib
import os
from pathlib import Path
import re
import select
import shutil
import struct
import subprocess
import time
import unittest
import zlib
import ssl
import threading
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer


ROOT = Path(__file__).resolve().parents[2]
WORKER = Path(os.environ["BOT_NATIVE_WORKER"])
STATE = ROOT / ".tmp" / "native-worker-integration-state"


def frame(tag, body=b""):
    return struct.pack("<I", 1 + len(body)) + bytes((tag,)) + body


def receive(stream, timeout=12):
    def exact(length):
        output = bytearray()
        deadline = time.monotonic() + timeout
        while len(output) < length:
            wait = deadline - time.monotonic()
            if wait <= 0 or not select.select([stream], [], [], wait)[0]:
                raise AssertionError("native worker frame timeout")
            part = os.read(stream.fileno(), length - len(output))
            if not part:
                raise AssertionError("native worker stdout closed")
            output.extend(part)
        return output

    length, = struct.unpack("<I", exact(4))
    assert 1 <= length <= 4096
    payload = exact(length)
    return payload[0], bytes(payload[1:])


class WorkerProcessTest(unittest.TestCase):
    def setUp(self):
        STATE.mkdir(mode=0o700)
        (STATE / "nvs").mkdir(mode=0o700)
        (STATE / "spiffs").mkdir(mode=0o700)
        self.addCleanup(lambda: shutil.rmtree(STATE))
        expanded = bytearray(hashlib.sha512(b"native-worker-integration").digest())
        expanded[0] &= 248
        expanded[31] = (expanded[31] & 63) | 64
        self.identity = bytes(expanded)
        self.hello = frame(1, struct.pack("<H64sIIBBBh", 1, self.identity, 915000000,
                                          125000, 7, 5, 18, -120) +
                           struct.pack("<256I", *([0] + [10] * 255)))

    def start(self, dormant=False):
        process = subprocess.Popen([str(WORKER), str(STATE)] + (["--dormant"] if dormant else []), stdin=subprocess.PIPE,
                                   stdout=subprocess.PIPE, stderr=subprocess.PIPE)
        def cleanup():
            if not process.stdin.closed:
                process.stdin.close()
            process.wait(timeout=8)
            process.stdout.close()
            process.stderr.close()
        self.addCleanup(cleanup)
        return process

    def test_dormant_candidate_never_transmits_before_explicit_activation(self):
        process = self.start(dormant=True)
        process.stdin.write(self.hello)
        process.stdin.flush()
        key = None
        for _ in range(35):
            tag, body = receive(process.stdout)
            self.assertNotEqual(tag, 0x81, "candidate transmitted before identity commit")
            if tag == 0x86:
                key = body[:32]
            elif tag == 0x82:
                self.assertEqual(body[:32], key)
                break
            else:
                self.assertEqual(tag, 0x83)
        else:
            self.fail("dormant candidate never became READY")
        process.stdin.write(frame(6))
        process.stdin.flush()
        activated = advertised = False
        for _ in range(10):
            tag, body = receive(process.stdout)
            if tag == 0x87:
                self.assertEqual(body, b"\x01")
                activated = True
            elif tag == 0x81:
                # TX_REQUEST header (13), packet header/path (2), advert key.
                packet = body[13:]
                self.assertEqual(packet[0] & 3, 2)
                self.assertEqual(packet[1], 0)
                self.assertEqual(packet[2:34], key)
                advertised = True
            else:
                self.assertEqual(tag, 0x83)
            if activated and advertised:
                break
        self.assertTrue(activated and advertised)
        self.stop(process)

    def ready(self, process):
        process.stdin.write(self.hello)
        process.stdin.flush()
        ready = None
        startup_advert = False
        for _ in range(35):
            tag, body = receive(process.stdout)
            if tag == 0x81:
                token, = struct.unpack_from("<I", body)
                process.stdin.write(frame(3, struct.pack("<IBBIIIB", token, 1, 0, 0, 0, 10, 0)))
                process.stdin.write(frame(3, struct.pack("<IBBIIIB", token, 2, 0, 0, 10, 10, 1)))
                process.stdin.flush()
                startup_advert = True
            elif tag == 0x82:
                self.assertEqual(body[-1], 1)
                ready = body[:32]
            else:
                self.assertIn(tag, (0x83, 0x86))
            if ready is not None and startup_advert:
                return ready
        self.fail("durable source did not become ready and advertise")

    def admin(self, process, command, request_id=17, faulted=False):
        process.stdin.write(frame(4, struct.pack("<I", request_id) + command.encode("ascii")))
        process.stdin.flush()
        for _ in range(35):
            tag, body = receive(process.stdout)
            if tag == 0x85:
                self.assertGreaterEqual(len(body), 4)
                self.assertLessEqual(len(body), 260)
                self.assertEqual(struct.unpack_from("<I", body)[0], request_id)
                return body[4:].decode("ascii")
            if faulted:
                self.assertEqual(tag, 0x83, "faulted source transmitted or became ready")
                self.assertEqual(body[:2], b"\x00\x01")
            else:
                self.assertIn(tag, (0x81, 0x83, 0x86))
        self.fail("admin receipt missing")

    def active(self, process, slot):
        for _ in range(35):
            status = self.admin(process, "source status")
            if f"active={slot}" in status and "durably saved and active" in status:
                return status
            time.sleep(0.06)
        self.fail("durable source did not become active: " + status)

    def upload(self, process, data, upload_id="0123456789abcdef"):
        digest = hashlib.sha256(data).hexdigest()
        self.assertEqual(self.admin(process, f"source begin {upload_id} {len(data)} {digest}"),
                         f"ACK {upload_id} next=0")
        for index, offset in enumerate(range(0, len(data), 48)):
            self.assertEqual(self.admin(process, f"source chunk {upload_id} {index} "
                                               f"{data[offset:offset + 48].hex()}"),
                             f"ACK {upload_id} next={index + 1}")
        self.assertIn("Accepted verification", self.admin(process, f"source commit {upload_id}"))
        return digest

    def bot_values(self, process, key):
        self.assertTrue(self.admin(process, "data export kv bot " + "0" * 64).startswith("PENDING"))
        for _ in range(80):
            status = self.admin(process, "data status")
            if status.startswith("EXPORTED "):
                break
            self.assertFalse(status.startswith(("Error:", "REJECTED", "UNKNOWN")), status)
            time.sleep(0.01)
        else:
            self.fail("bot KV export did not complete: " + status)
        _, digest, export_id = status.split()
        data = bytearray()
        for index in range(51):
            reply = self.admin(process, f"data read {export_id} {index}")
            self.assertTrue(reply.startswith("DATA "), reply)
            data.extend(bytes.fromhex(reply[5:]))
        self.assertEqual(len(data), 2422)
        self.assertEqual(hashlib.sha256(data).hexdigest(), digest)
        self.assertEqual(data[:4], b"BKD\x01")
        self.assertEqual(data[4:36], key)
        self.assertEqual(data[37:69], b"\0" * 32)
        self.assertEqual(hashlib.sha256(data[:-32]).digest(), data[-32:])
        values = {}
        for index in range(data[69]):
            offset = 70 + index * 290
            name = bytes(data[offset:offset + 33]).split(b"\0", 1)[0].decode()
            value = bytes(data[offset + 33:offset + 290]).split(b"\0", 1)[0].decode()
            values[name] = value
        return values

    def wait_bot_value(self, process, key, name):
        deadline = time.monotonic() + 5
        while time.monotonic() < deadline:
            values = self.bot_values(process, key)
            if name in values:
                return values
            time.sleep(0.02)
        self.fail(f"startup handler did not write {name}: {values}; " +
                  self.admin(process, "status"))

    def native_status(self, process):
        return {key: int(value) for key, value in re.findall(
            r"(\w+)=(\d+)", self.admin(process, "status"))}

    def stop(self, process):
        process.stdin.close()
        self.assertEqual(process.wait(timeout=8), 0, process.stderr.read().decode())

    def test_source_durable_upload_restart_rollback_remove(self):
        source = (b"function custom(text) reply('Hosted:'..text) end "
                  b"command('custom','text:text:150','Hosted')")
        first = self.start()
        key = self.ready(first)
        self.assertIn("active=3", self.admin(first, "source status"))
        self.assertIn("https", self.admin(first, "source api package"))
        digest = self.upload(first, source)
        self.assertIn("active=0", self.active(first, 0))
        self.assertIn(digest, self.admin(first, "source hash"))
        self.assertIn("Accepted live retry", self.admin(first, "source retry"))
        self.active(first, 0)
        self.stop(first)

        second = self.start()
        self.assertEqual(self.ready(second), key)
        self.assertIn("active=0", self.admin(second, "source status"))
        self.assertIn(digest, self.admin(second, "source hash"))
        self.assertIn("Accepted verification", self.admin(second, "source rollback"))
        self.active(second, 3)
        self.assertIn("active=3", self.admin(second, "source status"))
        self.upload(second, source, "fedcba9876543210")
        self.active(second, 1)
        self.assertIn("Accepted verification", self.admin(second, "source remove"))
        self.active(second, 3)
        self.stop(second)

        third = self.start()
        self.assertEqual(self.ready(third), key)
        self.assertIn("active=3", self.admin(third, "source status"))
        self.stop(third)

    def test_rejected_candidate_preserves_deployment_and_reports_runtime_epoch(self):
        process = self.start()
        key = self.ready(process)
        self.assertIn("Saved/applied", self.admin(process, "shared on"))
        self.assertIn("Saved/applied", self.admin(process, "events 1"))
        source = (b"function custom() return 'old code' end "
                  b"function _started() local old=kv.get('boot','bot') "
                  b"kv.put('boot',old and 'replayed' or 'first','bot') end "
                  b"events.on('startup','_started')")
        self.upload(process, source)
        before = self.active(process, 0)
        before_hash = self.admin(process, "source hash")
        self.assertEqual(self.wait_bot_value(process, key, "boot")["boot"], "first")
        before_runtime = self.native_status(process)
        self.upload(process, b"function broken(", "fedcba9876543210")
        deadline = time.monotonic() + 5
        while time.monotonic() < deadline:
            rejected = self.admin(process, "source status")
            if "Error:" in rejected:
                break
            time.sleep(0.01)
        else:
            self.fail("candidate rejection was not reported: " + rejected)
        self.assertEqual(re.match(r"gen=\d+ active=\d+ prev=\d+", rejected).group(),
                         re.match(r"gen=\d+ active=\d+ prev=\d+", before).group())
        self.assertEqual(self.admin(process, "source hash"), before_hash)
        time.sleep(1.1)  # Allow the existing one-second event admission gate to expire.
        after_runtime = self.native_status(process)
        self.assertEqual(after_runtime["ready"], 1)
        if "runtime_epoch" in before_runtime:
            self.assertNotEqual(after_runtime["runtime_epoch"], before_runtime["runtime_epoch"])
            self.assertEqual(after_runtime["source_generation"], before_runtime["source_generation"])
        self.assertEqual(after_runtime["queued"], before_runtime["queued"])
        self.assertEqual(after_runtime["completed"], before_runtime["completed"])
        self.assertEqual(self.bot_values(process, key)["boot"], "first")
        self.stop(process)

    def test_real_tls_streamed_package_hash_failure_and_cancellation_preserve_source(self):
        certificate, private_key = STATE / "tls.crt", STATE / "tls.key"
        subprocess.run(["openssl", "req", "-x509", "-newkey", "rsa:2048", "-nodes",
                        "-days", "1", "-subj", "/CN=localhost",
                        "-addext", "subjectAltName=DNS:localhost",
                        "-keyout", str(private_key), "-out", str(certificate)],
                       check=True, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        token = "t" * 32
        source = (b"--@meshcore-bot/1;name=network;version=1.0.0;runtime=lua-5.5.1;"
                  b"api=named-commands-v1;caps=none;schema=none;rollback=none\n"
                  b"function fetched() return 'Fetched' end\n")
        source += b"--" + b"x" * (4096 - len(source) - 3) + b"\n"
        source = getattr(self, "package_fixture", source)
        digest = hashlib.sha256(source).hexdigest()
        state = {"body": source, "requests": 0, "hold": False, "response_errors": []}
        started, release = threading.Event(), threading.Event()

        class PackageServer(BaseHTTPRequestHandler):
            protocol_version = "HTTP/1.1"
            def log_message(self, *args):
                pass
            def do_GET(self):
                state["requests"] += 1
                if self.path != "/package" or self.headers.get("Authorization") != "Bearer " + token:
                    self.send_error(403)
                    return
                body = state["body"]
                started.set()
                if state["hold"]:
                    release.wait(5)
                try:
                    self.send_response(200)
                    self.send_header("Content-Type", getattr(owner, "package_mime", "application/x-lua"))
                    self.send_header("Content-Length", str(len(body)))
                    self.end_headers()
                    self.wfile.write(body)
                except (OSError, ssl.SSLError) as error:
                    state["response_errors"].append(type(error).__name__)

        owner = self
        server = ThreadingHTTPServer(("127.0.0.1", 0), PackageServer)
        tls = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
        tls.load_cert_chain(certificate, private_key)
        server.socket = tls.wrap_socket(server.socket, server_side=True)
        serving = threading.Thread(target=server.serve_forever)
        serving.start()
        def cleanup():
            release.set()
            server.shutdown()
            server.server_close()
            serving.join()
        self.addCleanup(cleanup)
        first = self.start()
        key = self.ready(first)
        self.assertIn("staged", self.admin(first,
            f"https endpoint package 127.0.0.1 localhost {server.server_port} /package get"))
        for field, data in (("ca", certificate.read_bytes()), ("token", token.encode())):
            for offset in range(0, len(data), 40):
                self.assertNotIn("Error:", self.admin(first, f"https {field} package {data[offset:offset+40].hex()}"))
        self.assertNotIn("Error:", self.admin(first, "https commit"))
        self.assertNotIn("Error:", self.admin(first, "home on"))
        self.assertIn("Accepted", self.admin(first, f"source fetch package {digest}"))
        self.active(first, 0)
        installed = self.admin(first, "source hash")
        self.assertIn(digest, installed)
        self.assertIn("Accepted", self.admin(first, "source fetch package " + "00" * 32))
        for _ in range(80):
            status = self.admin(first, "source status")
            if "SHA-256 did not match" in status:
                break
            time.sleep(.02)
        else:
            self.fail(status)
        self.assertEqual(self.admin(first, "source hash"), installed)
        self.assertEqual(state["requests"], 2)
        self.stop(first)

        second = self.start()
        self.assertEqual(self.ready(second), key)
        self.assertEqual(self.admin(second, "source hash"), installed)
        started.clear()
        state["hold"] = True
        self.assertIn("Accepted", self.admin(second, f"source fetch package {digest}"))
        self.assertTrue(started.wait(3))
        self.assertIn("active=0", self.admin(second, "source status"))
        self.assertIn("cancellation", self.admin(second, "source cancel"))
        for _ in range(80):
            status = self.admin(second, "source status")
            if "package fetch cancelled; active source retained" in status:
                break
            time.sleep(.02)
        else:
            self.fail(status)
        self.assertNotIn("Error:", status)
        self.assertIn("remote GET outcome unknown", status)
        self.assertEqual(self.admin(second, "source hash"), installed)
        self.assertEqual(state["requests"], 3, "cancelled package request replayed")
        release.set()
        time.sleep(.1)
        self.assertEqual(self.admin(second, "source hash"), installed)
        selected = bytearray()
        for index in range((len(source) + 47) // 48):
            chunk = self.admin(second, f"source read {index}")
            self.assertTrue(chunk.startswith("DATA "), chunk)
            selected.extend(bytes.fromhex(chunk[5:]))
        self.assertEqual(selected, source, "late canceled response changed selected package bytes")
        self.stop(second)

        third = self.start()
        self.assertEqual(self.ready(third), key)
        self.assertEqual(self.admin(third, "source hash"), installed)
        self.stop(third)

    def test_private_admin_name_channel_and_rejected_identity(self):
        first = self.start()
        self.ready(first)
        for invalid in ("https endpoint bad", "https token demo zz", "https commit"):
            self.assertTrue(self.admin(first, invalid).startswith("Error: "))
        self.assertEqual(self.admin(first, "name Host Mast", 29),
                         "Saved and applied bot name; identity unchanged")
        self.assertEqual(self.admin(first, "advert.zerohop"),
                         "Bot zero-hop advert queued; verify RF")
        self.assertIn("rate limited (1 minute)", self.admin(first, "advert.zerohop"))
        self.assertIn("reboot required",
                      self.admin(first, "channel 546573744368616e6e656c 00112233445566778899aabbccddeeff"))
        self.assertEqual(self.admin(first, "airtime status"),
                         "Bot airtime saved=360 ms/min; reboot applies")
        self.assertIn("saved=0 live=0", self.admin(first, "adaptive"))
        self.assertIn("reboot required", self.admin(first, "adaptive on"))
        self.assertIn("saved=1 live=0", self.admin(first, "adaptive"))
        self.assertIn("Error:", self.admin(first, "adaptive everyone"))
        self.assertIn("saved=0 live=0", self.admin(first, "discovery"))
        self.assertIn("Saved and applied", self.admin(first, "discovery on"))
        self.assertIn("saved=1 live=1", self.admin(first, "discovery status"))
        self.assertIn("Error:", self.admin(first, "discovery everyone"))
        for invalid in ("airtime 0", "airtime 3601", "airtime +360", "airtime 360oops"):
            self.assertEqual(self.admin(first, invalid), "Error: bot airtime requires 360..3600 ms/min")
        self.assertEqual(self.admin(first, "airtime 1200"), "Saved bot airtime; reboot required")
        self.assertEqual(self.admin(first, "airtime status"),
                         "Bot airtime saved=1200 ms/min; reboot applies")
        self.assertIn("Go owner socket", self.admin(first, "identity rotate"))
        self.assertIn("Error:", self.admin(first, "name Bad:Name"))
        self.assertIn("Error:", self.admin(first, "channel 00 00000000000000000000000000000000"))
        self.assertIn("Error:", self.admin(first, "channel 540054 00112233445566778899aabbccddeeff"))
        self.stop(first)
        second = self.start()
        self.ready(second)
        self.assertIn("saved=1 live=1", self.admin(second, "adaptive"))
        self.assertIn("reboot required", self.admin(second, "adaptive off"))
        self.assertIn("saved=1 live=1", self.admin(second, "discovery"))
        self.assertIn("Saved and applied", self.admin(second, "discovery off"))
        self.assertIn("saved=0 live=0", self.admin(second, "discovery"))
        self.assertEqual(self.admin(second, "airtime status"),
                         "Bot airtime saved=1200 ms/min; reboot applies")
        self.assertIn("reboot required", self.admin(second, "channel off"))
        self.stop(second)

    def test_corrupt_adaptive_flag_boots_static_and_owner_repairs(self):
        first = self.start()
        key = self.ready(first)
        self.assertIn("reboot required", self.admin(first, "adaptive on"))
        self.stop(first)
        snapshot = STATE / "nvs" / "nvs.snapshot"
        contents = bytearray(snapshot.read_bytes())
        marker = b"BAA\x01\x01"
        self.assertEqual(contents.count(marker), 1)
        contents[contents.index(marker) + 2] = ord("D")
        contents[-4:] = struct.pack("<I", zlib.crc32(contents[:-4]))
        snapshot.write_bytes(contents)
        second = self.start()
        self.assertEqual(self.ready(second), key)
        for _ in range(2):
            self.assertIn("live=0 policy-fault=1", self.admin(second, "adaptive"))
        self.assertIn("reboot required", self.admin(second, "adaptive off"))
        self.assertIn("saved=0 live=0", self.admin(second, "adaptive"))
        self.stop(second)
        third = self.start()
        self.assertEqual(self.ready(third), key)
        self.assertIn("saved=0 live=0", self.admin(third, "adaptive"))
        self.stop(third)

    def test_help_grants_reply_is_bounded_and_worker_stays_ready(self):
        process = self.start()
        self.ready(process)
        policy = self.admin(process, "policy", request_id=18)
        reply = self.admin(process, "help grants", request_id=19)
        self.assertLessEqual(len(reply.encode("ascii")), 256)
        self.assertIn("MASK 0..31", reply)
        self.assertIn("16 scheduled", reply)
        self.assertIn("Defaults off", reply)
        self.assertIn("packages never authorize scripts", reply)
        self.assertEqual(self.admin(process, "policy", request_id=20), policy)
        self.assertIn("ready=1 jobs=0", self.admin(process, "status", request_id=21))
        self.stop(process)

    def test_private_native_grants_validate_apply_and_restart(self):
        first = self.start()
        key = self.ready(first)
        for command in ("shared", "reminders", "events"):
            self.assertIn("saved=0 applied=0", self.admin(first, command))
            self.assertIn(command, self.admin(first, "help"))
        self.assertIn("clock=", self.admin(first, "reminders"))
        self.assertIn("policy", self.admin(first, "help"))
        self.assertIn("clock", self.admin(first, "help"))
        for field in ("https=", "scheduler=", "uncertainty-us=", "scheduler-limit-us=2000000", "reason="):
            self.assertIn(field, self.admin(first, "clock"))
        self.assertIn("packages never authorize scripts", self.admin(first, "help grants"))
        self.assertIn("use help", self.admin(first, "not-a-native-command"))
        self.assertEqual(self.admin(first, "policy"), self.admin(first, "source api grants"))
        self.assertIn("ready=1 jobs=0", self.admin(first, "status"))
        for command in ("source api storage", "source api reminders", "source api board"):
            self.assertIn("saved=0 applied=0", self.admin(first, command))
        self.assertIn("clock=", self.admin(first, "source api reminders"))
        self.assertIn("autonomous=0", self.admin(first, "source api reminders"))
        self.assertIn("autonomous-reminders=0", self.admin(first, "source api storage"))
        self.assertIn("subscribed=0", self.admin(first, "source api events"))
        for capability in ("kv", "reminders", "events"):
            self.assertIn(capability, self.admin(first, "source api package"))

        invalid = ("shared ", "shared yes", "shared on extra", "shared off ", "shared ON",
                   "reminders ", "reminders yes", "reminders on extra", "reminders off ", "reminders ON",
                   "events ",
                   "events -1", "events +1", "events 32", "events 256", "events 4294967296",
                   "events 1x", "events 1 extra", "events 1 ", "events on", "events off")
        for command in invalid:
            before = self.admin(first, "policy")
            self.assertTrue(self.admin(first, command).startswith("Error:"), command)
            self.assertEqual(self.admin(first, "policy"), before, command)
        self.assertIn("Saved/applied", self.admin(first, "shared on"))
        self.assertIn("Saved/applied", self.admin(first, "reminders on"))
        self.assertIn("Saved/applied", self.admin(first, "events 15"))
        self.assertIn("saved=1 applied=1", self.admin(first, "shared status"))
        self.assertIn("saved=1 applied=1", self.admin(first, "reminders status"))
        # The granted mask is independent of which handlers the active source declares.
        self.assertIn("saved=15 applied=15 subscribed=0", self.admin(first, "events status"))
        self.assertIn("subscribed=0", self.admin(first, "source api events"))
        self.assertIn("saved=15 applied=15 subscribed=0", self.admin(first, "source api events"))
        for command in invalid:
            before = self.admin(first, "policy")
            self.assertTrue(self.admin(first, command).startswith("Error:"), command)
            self.assertEqual(self.admin(first, "policy"), before, command)
        self.stop(first)

        second = self.start()
        self.assertEqual(self.ready(second), key)
        self.assertIn("saved=1 applied=1", self.admin(second, "shared"))
        self.assertIn("saved=1 applied=1", self.admin(second, "reminders"))
        self.assertIn("saved=15 applied=15 subscribed=0", self.admin(second, "events"))
        self.assertIn("Saved/applied", self.admin(second, "shared off"))
        self.assertIn("Saved/applied", self.admin(second, "reminders off"))
        self.assertIn("Saved/applied", self.admin(second, "events 0"))
        self.assertIn("personal reminders unchanged", self.admin(second, "cancel"))
        self.stop(second)

        third = self.start()
        self.assertEqual(self.ready(third), key)
        for command in ("shared", "reminders", "events"):
            self.assertIn("saved=0 applied=0", self.admin(third, command))
        self.stop(third)

    def test_native_event_shared_grants_and_cancel_fence_yielding_writes(self):
        process = self.start()
        key = self.ready(process)
        self.assertIn("Saved/applied", self.admin(process, "shared on"))

        def install(marker):
            source = (f"function started() kv.put('{marker}','ran','bot') "
                      f"sleep(1500) kv.put('{marker}-done','ran','bot') end "
                      "events.on('startup','started')").encode()
            slot = int(re.search(r"active=(\d+)", self.admin(process, "source status"))[1])
            self.upload(process, source)
            self.active(process, 0 if slot == 3 else (slot + 1) % 3)

        install("shared")
        self.assertEqual(self.bot_values(process, key), {})
        self.assertEqual(self.native_status(process)["queued"], 0)
        self.assertIn("Saved/applied", self.admin(process, "events 1"))
        self.wait_bot_value(process, key, "shared")
        self.assertIn("Saved/applied", self.admin(process, "shared off"))
        time.sleep(1.7)
        self.assertEqual(self.bot_values(process, key), {"shared": "ran"})
        self.assertGreaterEqual(self.native_status(process)["failed"], 1)

        install("denied")
        time.sleep(0.15)
        self.assertEqual(self.bot_values(process, key), {"shared": "ran"})
        self.assertGreaterEqual(self.native_status(process)["failed"], 2)
        self.assertIn("Saved/applied", self.admin(process, "events 0"))
        self.assertIn("Saved/applied", self.admin(process, "shared on"))
        install("events")
        self.assertEqual(self.bot_values(process, key), {"shared": "ran"})
        self.assertIn("Saved/applied", self.admin(process, "events 1"))
        self.wait_bot_value(process, key, "events")
        self.assertIn("Saved/applied", self.admin(process, "events 0"))
        time.sleep(1.7)
        self.assertNotIn("events-done", self.bot_values(process, key))
        # Regrant does not replay a startup handler already admitted in this source generation.
        self.assertIn("Saved/applied", self.admin(process, "events 1"))
        before = self.native_status(process)["queued"]
        time.sleep(0.15)
        self.assertEqual(self.native_status(process)["queued"], before)

        install("cancel")
        self.wait_bot_value(process, key, "cancel")
        self.assertIn("Cancelled running", self.admin(process, "cancel"))
        time.sleep(1.7)
        self.assertNotIn("cancel-done", self.bot_values(process, key))
        self.assertIn("saved=1 applied=1", self.admin(process, "events"))

        install("complete")
        self.wait_bot_value(process, key, "complete")
        for invalid in ("shared off extra", "reminders off extra", "events 32"):
            self.assertTrue(self.admin(process, invalid).startswith("Error:"), invalid)
        self.wait_bot_value(process, key, "complete-done")
        self.assertGreaterEqual(self.native_status(process)["completed"], 1)
        self.assertIn("subscribed=1", self.admin(process, "source api events"))
        self.stop(process)

    def test_interrupted_source_upload_resumes_on_restart(self):
        source = (b"function custom(text) reply('Resumed:'..text) end "
                  b"command('custom','text:text:150','Resumed')")
        upload_id = "abababababababab"
        digest = hashlib.sha256(source).hexdigest()
        first = self.start()
        self.ready(first)
        self.assertEqual(self.admin(first, f"source begin {upload_id} {len(source)} {digest}"),
                         f"ACK {upload_id} next=0")
        self.assertEqual(self.admin(first, f"source chunk {upload_id} 0 {source[:48].hex()}"),
                         f"ACK {upload_id} next=1")
        self.stop(first)
        second = self.start()
        self.ready(second)
        self.assertEqual(self.admin(second, f"source begin {upload_id} {len(source)} {digest}"),
                         f"ACK {upload_id} next=1")
        self.assertEqual(self.admin(second, f"source chunk {upload_id} 1 {source[48:].hex()}"),
                         f"ACK {upload_id} next=2")
        self.assertIn("Accepted verification", self.admin(second, f"source commit {upload_id}"))
        self.active(second, 0)
        self.assertIn(digest, self.admin(second, "source hash"))
        self.stop(second)

    def test_missing_source_emits_no_advert_until_recovered(self):
        first = self.start()
        key = self.ready(first)
        bundled_hash = self.admin(first, "source hash").split()[1]
        self.upload(first, b"function custom() reply('active') end")
        self.active(first, 0)
        self.stop(first)
        (STATE / "spiffs" / "spiffs.a.lua").unlink()

        second = self.start()
        second.stdin.write(self.hello)
        second.stdin.flush()
        management = None
        for _ in range(4):
            tag, body = receive(second.stdout, timeout=3)
            self.assertIn(tag, (0x86, 0x83), "missing source transmitted or became ready")
            if tag == 0x86:
                management = body
            if tag == 0x83:
                self.assertEqual(body[:2], b"\x00\x01")
                break
        else:
            self.fail("missing source did not publish faulted status")
        self.assertIsNotNone(management)
        self.assertEqual(management[:32], key)
        for _ in range(2):
            tag, body = receive(second.stdout, timeout=3)
            self.assertEqual(tag, 0x83, "missing source advertised or became ready while awaiting recovery")
            self.assertEqual(body[:2], b"\x00\x01")
        deadline = time.monotonic() + 9
        while True:
            status = self.admin(second, "source status", faulted=True)
            metadata, separator, outcome = status.partition("; ")
            self.assertTrue(separator, status)
            self.assertIn(" active=0 ", metadata)
            self.assertTrue(outcome.startswith("Error: package source file unavailable or size changed; "), status)
            if outcome == "Error: package source file unavailable or size changed; use source retry; startup blocked":
                break
            self.assertEqual(outcome, "Error: package source file unavailable or size changed; live retry pending")
            self.assertLess(time.monotonic(), deadline, "missing-source retries did not terminate")
            time.sleep(0.05)
        self.assertTrue(self.admin(second, "advert.zerohop", faulted=True).startswith("Error:"))
        self.assertEqual(self.admin(second, "source remove"),
                         "Accepted verification; source status reports durable activation outcome")
        advert = ready = False
        for _ in range(35):
            tag, body = receive(second.stdout)
            if tag == 0x81:
                advert = True
            elif tag == 0x82:
                self.assertEqual(body[:32], key)
                ready = True
                break
            else:
                self.assertEqual(tag, 0x83)
        self.assertTrue(advert, "recovered bot did not advertise after source activation")
        self.assertTrue(ready)
        self.active(second, 3)
        self.assertTrue(self.admin(second, "source hash").startswith(f"SHA256 {bundled_hash} gen="))
        self.stop(second)

    def test_partial_frame_eof_is_fatal(self):
        for fragment in (b"\x14\x00", struct.pack("<I", 20) + b"\x03\x01\x00"):
            with self.subTest(fragment=fragment):
                process = self.start()
                self.ready(process)
                process.stdin.write(fragment)
                process.stdin.close()
                self.assertNotEqual(process.wait(timeout=8), 0)
                self.assertTrue(process.stdout.read().endswith(frame(0x84, b"\x05")))

    def test_malformed_admin_frame_has_no_receipt(self):
        for command in (b"", b"source status\x00", b"source status\n", b"a" * 161):
            with self.subTest(command=command):
                process = self.start()
                self.ready(process)
                process.stdin.write(frame(4, struct.pack("<I", 44) + command))
                process.stdin.flush()
                self.assertNotEqual(process.wait(timeout=8), 0)
                frames = []
                while True:
                    tag, body = receive(process.stdout) if select.select(
                        [process.stdout], [], [], 1)[0] else (None, b"")
                    if tag is None:
                        break
                    frames.append((tag, body))
                    if tag == 0x84:
                        break
                self.assertEqual(frames[-1], (0x84, b"\x05"))
                self.assertNotIn(0x85, [tag for tag, _ in frames])

    def test_real_worker_ready_and_terminal_tx(self):
        process = self.start()
        process.stdin.write(self.hello)
        process.stdin.flush()
        ready = None
        token = None
        for _ in range(10):
            tag, body = receive(process.stdout)
            if tag == 0x81:
                self.assertIsNone(token)
                self.assertGreaterEqual(len(body), 14)
                token, = struct.unpack_from("<I", body)
                self.assertLessEqual(body[4], 7)
                process.stdin.write(frame(3, struct.pack("<IBBIIIB", token, 1, 0, 0, 0, 10, 0)))
                process.stdin.write(frame(3, struct.pack("<IBBIIIB", token, 2, 0, 0, 10, 10, 1)))
                process.stdin.flush()
            elif tag == 0x82:
                ready = body
                break
            else:
                self.assertIn(tag, (0x83, 0x86))
        self.assertEqual(len(ready), 33)
        self.assertEqual(ready[-1], 1)
        self.assertNotEqual(ready[:32], b"\0" * 32)
        self.assertIsNotNone(token)
        process.stdin.write(frame(2, struct.pack("<ffB", -72.0, 10.0, 0) + b"\x00"))
        process.stdin.flush()
        time.sleep(0.06)
        self.assertIsNone(process.poll())
        process.stdin.close()
        self.assertEqual(process.wait(timeout=8), 0, process.stderr.read().decode())

    def test_mast_rejection_after_provisional_acceptance_and_unmeasured_rx(self):
        process = self.start()
        process.stdin.write(self.hello)
        process.stdin.flush()
        token = None
        for _ in range(12):
            tag, body = receive(process.stdout)
            if tag == 0x81:
                token, = struct.unpack_from("<I", body)
                process.stdin.write(frame(3, struct.pack("<IBBIIIB", token, 1, 0, 0, 0, 10, 0)))
                process.stdin.write(frame(3, struct.pack("<IBBIIIB", token, 0, 3, 0, 0, 10, 0)))
                process.stdin.flush()
            elif tag == 0x82:
                break
        self.assertIsNotNone(token)
        process.stdin.write(frame(2, struct.pack("<ffB", 0.0, 0.0, 2) + b"\x00"))
        process.stdin.flush()
        self.assertIn("path_hash_mode=2 width=3", self.admin(process, "path status"))
        self.assertIsNone(process.poll(), process.stderr.read() if process.poll() else "")
        self.stop(process)

    def test_hello_is_identity_authority_and_rejects_malformed_length(self):
        first = self.start()
        first.stdin.write(self.hello)
        first.stdin.flush()
        for _ in range(10):
            tag, _ = receive(first.stdout)
            if tag == 0x82:
                break
        first.stdin.close()
        self.assertEqual(first.wait(timeout=8), 0, first.stderr.read().decode())

        second = self.start()
        changed = bytearray(self.hello)
        changed[7] ^= 0x08  # Within the expanded private identity.
        second.stdin.write(changed)
        second.stdin.flush()
        for _ in range(10):
            tag, _ = receive(second.stdout)
            if tag == 0x82:
                break
        else:
            self.fail("new Go HELLO identity did not reach READY")
        self.stop(second)

        third = self.start()
        third.stdin.write(struct.pack("<I", 4097))
        third.stdin.flush()
        self.assertNotEqual(third.wait(timeout=8), 0)
        self.assertEqual(receive(third.stdout), (0x84, b"\x05"))
        self.assertEqual(third.stdout.read(), b"")


if __name__ == "__main__":
    unittest.main()
