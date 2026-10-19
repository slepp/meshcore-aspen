#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Maintain an ESP32S3 application or request one explicit node restart."""
import argparse
from contextlib import contextmanager
import hashlib
import json
import os
from pathlib import Path
import re
import stat
import sys
import tempfile
import time
import urllib.request
import urllib.error

sys.path.insert(0, str(Path(__file__).resolve().parents[2]))
from firmware.esp32.esp_update_manifest import sign
from tools import node_backup
from tools.hardware.admin import checked, private_file, read_file, write_new_file

DEFAULT_COMMANDS = ("bot aliases", "bot policy", "role-path", "get autoadvert", "setup status")
RECEIPT_FORMAT = "meshcore-esp32-maintenance-v1"
COMMAND_DELAY = .6


def paced_command(client, text):
    time.sleep(COMMAND_DELAY)
    return checked(client, text)


def digest(raw):
    return hashlib.sha256(raw).hexdigest()


@contextmanager
def receipt_lock(path):
    try:
        import fcntl
    except ImportError:
        raise ValueError("Firmware updates require a POSIX host; on Windows, run in WSL") from None
    # Keep a separate inode: atomic receipt replacement must not release this lock.
    fd = os.open(str(path) + ".lock", os.O_CREAT | os.O_RDWR | os.O_NOFOLLOW, 0o600)
    try:
        info = os.fstat(fd)
        if (not stat.S_ISREG(info.st_mode) or info.st_uid != os.geteuid() or
                info.st_mode & 0o077 or info.st_nlink != 1):
            raise ValueError("Maintenance receipt lock must be an owner-only regular file with one link")
        try:
            fcntl.flock(fd, fcntl.LOCK_EX | fcntl.LOCK_NB)
        except BlockingIOError:
            raise ValueError("Maintenance receipt is already in use; wait for its current operation") from None
        yield
    finally:
        os.close(fd)


def save_receipt(path, record):
    """Replace a private receipt atomically, including intent before any uncertain write."""
    private_file(path, 256 * 1024)
    fd, temporary = tempfile.mkstemp(prefix=".maintenance-", dir=path.parent)
    try:
        with os.fdopen(fd, "wb") as stream:
            stream.write((json.dumps(record, indent=2) + "\n").encode())
            stream.flush()
            os.fsync(stream.fileno())
        os.replace(temporary, path)
        directory = os.open(path.parent, os.O_RDONLY | os.O_DIRECTORY)
        try:
            os.fsync(directory)
        finally:
            os.close(directory)
    finally:
        Path(temporary).unlink(missing_ok=True)


def response_failure(error, client):
    result = {"type": type(error).__name__}
    if isinstance(error, urllib.error.HTTPError):
        result["status"] = error.code
        try:
            value = json.loads(error.read(4096))
            message = value.get("error", "") if isinstance(value, dict) else ""
            if isinstance(message, str):
                result["error"] = "".join(c for c in message if c.isprintable())[:512].replace(client.token, "[session]")
        except (OSError, ValueError):
            pass
    return result


def json_request(client, route, body=None, headers=None):
    fields = {"X-Mast-Session": client.token, "X-Host-Admin": client.token,
              "X-Host-Intent": "admin-v1", **(headers or {})}
    request = urllib.request.Request(client.url + route, data=body, headers=fields)
    with client.opener.open(request, timeout=200 if body else 15) as response:
        raw = response.read(65537)
    if len(raw) > 65536:
        raise ValueError("Radio status response exceeds 64 KiB")
    result = json.loads(raw)
    if not isinstance(result, dict):
        raise ValueError("Radio status must be a JSON object")
    return result


def running(status, sha256, size=None):
    return (status.get("running_sha256") == sha256 and
            (size is None or status.get("running_size") == size) and
            not status.get("running_hash_error"))


def roles_ready(roles):
    # A role disabled by its saved profile stays disabled; every other role must be ready.
    return bool(roles) and all(role.get("ready") or role.get("state") == "disabled" for role in roles)


def unready_roles(roles):
    """Role name, state and fault text of every role that is neither ready nor disabled."""
    return [{"role": str(role.get("role", ""))[:32], "state": str(role.get("state", ""))[:32],
             "fault": str(role.get("fault") or "")[:96]}
            for role in roles or [] if not (role.get("ready") or role.get("state") == "disabled")]


def radio_state(client, state=None):
    if state is None:
        state = json_request(client, "/api/status")
    roles = state.get("roles", [])
    if not roles_ready(roles):
        raise ValueError("Radio roles are not all ready; inspect /api/status")
    identities = {role["role"]: {"name": role["name"], "public_key": role["public_key"],
                                 "ready": bool(role.get("ready"))}
                  for role in roles}
    if len(identities) != len(roles) or any(
            not re.fullmatch(r"[0-9a-fA-F]{64}", role["public_key"] or "")
            for role in identities.values() if role["ready"]):
        raise ValueError("Radio public identities are missing or duplicated")
    profile = {key: value for key, value in state["profile"].items() if key != "generation"}
    return {"identities": identities, "profile": profile}


def retained(client, commands):
    result = {command: paced_command(client, command) for command in commands}
    for prefix in ("source", "source wasm"):
        status = paced_command(client, prefix + " status")
        match = re.match(r"gen=(\d+) active=([0-3]) prev=([0-3]) size=(\d+) upload=none ", status)
        hashed = paced_command(client, prefix + " hash")
        parsed = re.fullmatch(r"SHA256 ([0-9a-f]{64}) gen=(\d+)", hashed)
        if not match or not parsed or match[1] != parsed[2]:
            raise ValueError(prefix + " selection unavailable or upload active; inspect source status")
        result[prefix] = {"generation": int(match[1]), "active": int(match[2]), "previous": int(match[3])}
        if match[2] != "3":
            result[prefix].update(size=int(match[4]), sha256=parsed[1])
    for slot in range(2):
        result[f"packet {slot}"] = packet_state(client, slot)
    for index in range(8):
        command = f"bot membership {index}"
        result[command] = paced_command(client, command)
    for kind in ("access", "thread"):
        for context in ("dm", "native", *range(8)):
            offset = 0
            while True:
                command = f"bot {kind} {context} list {offset}"
                response = paced_command(client, command)
                result[command] = response
                match = re.search(r"\bnext=(\d+)$", response)
                if not match:
                    raise ValueError("Policy pagination response missing next: " + command)
                following = int(match[1])
                if following == offset:
                    break
                if not offset < following <= 64:
                    raise ValueError("Policy pagination did not advance: " + command)
                offset = following
    return result


def packet_state(client, slot):
    prefix = f"packet {slot}"
    status = paced_command(client, prefix + " status")
    fields = dict(re.findall(r"([a-z]+)=([^ ;]+)", status))
    if (fields.get("sealed") != "0" or fields.get("runtime") not in ("none", "wasm", "lua") or
            fields.get("saved") not in ("0", "1") or fields.get("live") != fields.get("saved")):
        raise ValueError(f"Packet slot {slot} saved/live state differs or is sealed; inspect packet {slot} status")
    return {"enabled": fields["saved"] == "1", "runtime": fields["runtime"],
            "sha256": paced_command(client, prefix + " hash"),
            "budget": paced_command(client, prefix + " budget")}


def sampler_state(client, sampler):
    state = packet_state(client, sampler["slot"])
    if state["sha256"] != sampler["sha256"] or state["runtime"] != "wasm":
        raise ValueError("Packet sampler hash differs or runtime is not Wasm; inspect slot before maintenance")
    return {key: state[key] for key in ("enabled", "budget")}


def no_pending_identities(client):
    for role in ("repeater", "room", "companion", "management", "bot"):
        time.sleep(COMMAND_DELAY)
        if client.command(f"key {role} pending") != "Error: identity or pending key unavailable":
            raise ValueError("Pending identity must be resolved before application maintenance: " + role)


def restore_sampler(client, record, receipt):
    if not record.get("sampler_restore_pending"):
        return
    sampler = record["sampler"]
    # Verify the pinned program and budget again before touching its enable flag.
    actual = sampler_state(client, sampler)
    if actual["budget"] != sampler["budget"]:
        raise ValueError("Packet sampler budget changed; restoration remains pending")
    if actual["enabled"] != sampler["enabled"]:
        paced_command(client, f'packet {sampler["slot"]} enable ' + ("on" if sampler["enabled"] else "off"))
    if sampler_state(client, sampler) != {key: sampler[key] for key in ("enabled", "budget")}:
        raise ValueError("Packet sampler enable readback differs; restoration remains pending")
    record["sampler_restore_pending"] = False
    save_receipt(receipt, record)


def artifact(args):
    seed = private_file(args.key_file, 32)
    image = read_file(args.image, 0x330000)
    signed = sign(image, args.target, seed)
    raw = read_file(args.build_manifest, 1024 * 1024)
    metadata = json.loads(raw)
    applications = [item for item in metadata.get("files", []) if item.get("name") == args.image.name]
    if (len(applications) != 1 or applications[0].get("sha256") != signed["sha256"] or
            applications[0].get("bytes") != len(image)):
        raise ValueError("Application differs from build manifest; no upload")
    backup = private_file(args.backup, node_backup.FILE_LIMIT)
    if args.allow_unsaved_state and not 0 <= time.time() - args.backup.stat().st_mtime <= 3600:
        raise ValueError("Unsaved-state recovery requires an encrypted backup downloaded within the last hour")
    _, files = node_backup.entries(node_backup.decrypt(backup, seed))
    return image, signed, {"path": str(args.backup.resolve()), "sha256": digest(backup),
                            "bytes": len(backup), "records": len(files)}, digest(raw)


def finish(client, record, receipt, connect, deadline, interval):
    signed = record["application"]
    candidate = client
    try:
        status = json_request(client, "/admin/update")
        record["last_update"] = status
        save_receipt(receipt, record)
        if (status.get("state") == "verified" or not running(status, signed["sha256"], signed["size"])) and not record.get("reboot_attempted"):
            if not running(status, record["expected_current_sha256"]):
                raise ValueError("Running application changed since preflight; no reboot")
            if (status.get("state") != "verified" or not status.get("reboot_ready") or
                    status.get("sha256") != signed["sha256"] or status.get("received") != signed["size"]):
                raise ValueError("Upload is not verified; no reboot or upload retry; inspect update status")
            record["reboot_attempted"] = True
            save_receipt(receipt, record)
            try:
                record["reboot_response"] = json_request(client, "/admin/update/reboot", b"")
            except (OSError, ValueError) as error:
                record["reboot_response_unavailable"] = type(error).__name__
            save_receipt(receipt, record)
            candidate.close()
            candidate = None
        # Reuse the authenticated session unless reboot or a failed connection invalidates it.
        while True:
            try:
                if candidate is None:
                    candidate = connect()
                status = json_request(candidate, "/admin/update")
                if running(status, signed["sha256"], signed["size"]):
                    dashboard = json_request(candidate, "/api/status")
                    # Keep the new image's own role faults: a failed candidate reverts to the old image.
                    failed = unready_roles(dashboard.get("roles"))
                    if failed and failed != record.get("unready_roles"):
                        record["unready_roles"] = failed
                        save_receipt(receipt, record)
                if (running(status, signed["sha256"], signed["size"]) and
                        status.get("state") == "idle" and status.get("boot_health") == "healthy"):
                    if roles_ready(dashboard.get("roles")):
                        after = radio_state(candidate, dashboard)
                        restore_sampler(candidate, record, receipt)
                        actual = retained(candidate, record["commands"])
                        record.update(after=after, retained_after=actual, after_update=status)
                        changed = [key for key in record["retained_before"]
                                   if record["retained_before"][key] != actual.get(key)]
                        if after["identities"] != record["before"]["identities"]:
                            changed.append("role identities")
                        if after["profile"] != record["before"]["profile"]:
                            changed.append("radio profile")
                        if changed:
                            raise ValueError("Changed after update: " + ", ".join(changed))
                        record["complete"] = True
                        save_receipt(receipt, record)
                        return
            except OSError:
                if candidate is not None:
                    candidate.close()
                    candidate = None
            if time.monotonic() >= deadline:
                raise TimeoutError("Intended application has not returned healthy; resume this receipt without replaying upload/reboot")
            time.sleep(interval)
    finally:
        if candidate is not None:
            candidate.close()


def finish_restart(client, record, receipt, connect, deadline, interval):
    try:
        target = record["expected_target"]
        while True:
            try:
                if client is None:
                    client = connect()
                status = json_request(client, "/admin/update")
                dashboard = json_request(client, "/api/status")
                uptime = dashboard.get("uptime_ms")
                boot_id = status.get("boot_id")
                restarted = (isinstance(boot_id, str) and
                             re.fullmatch(r"[0-9a-f]{16}", boot_id) and
                             boot_id != record["before_update"]["boot_id"])
                record.update(last_update=status, last_uptime_ms=uptime)
                if (restarted and running(status, target["sha256"], target["size"]) and
                        status.get("state") == "idle" and status.get("boot_health") == "healthy" and
                        roles_ready(dashboard.get("roles"))):
                    record.update(complete=True, after_update=status, after_uptime_ms=uptime)
                    save_receipt(receipt, record)
                    return True
            except OSError:
                if client is not None:
                    client.close()
                    client = None
            if time.monotonic() >= deadline:
                raise TimeoutError("Node restart has not returned healthy; resume this receipt without repeating the restart")
            time.sleep(interval)
    finally:
        if client is not None:
            client.close()
        save_receipt(receipt, record)


def restart_locked(args, record=None):
    password = private_file(args.password_file, 256)
    connect = lambda: node_backup.HttpClient(args.url, password)
    client = None
    try:
        if args.action == "force-restart":
            if not re.fullmatch(r"[0-9a-f]{64}", args.expected_current_sha256):
                raise ValueError("Expected current application requires a full lowercase SHA256")
            record = {"format": RECEIPT_FORMAT, "kind": "force-restart", "url": args.url,
                      "complete": False, "expected_current_sha256": args.expected_current_sha256}
            write_new_file(args.receipt, b"{}\n", 0o600)
            save_receipt(args.receipt, record)
            client = connect()
            status = json_request(client, "/admin/update")
            if status.get("force_restart_supported") is not True:
                raise ValueError("Radio does not support forced node restart; no restart request sent")
            if not isinstance(status.get("boot_id"), str) or not re.fullmatch(r"[0-9a-f]{16}", status["boot_id"]):
                raise ValueError("Node boot identifier unavailable; no restart request sent")
            if not running(status, args.expected_current_sha256):
                raise ValueError("Running application differs; no restart request sent")
            if status.get("state") not in ("idle", "failed", "verified") or status.get("force_restart_pending"):
                raise ValueError("Application transfer or node restart active; inspect status")
            target = {"sha256": status["running_sha256"], "size": status.get("running_size")}
            if status.get("state") == "verified":
                target = {"sha256": status.get("sha256"), "size": status.get("size")}
                if (not status.get("reboot_ready") or status.get("received") != target["size"] or
                        not isinstance(target["sha256"], str) or
                        not re.fullmatch(r"[0-9a-f]{64}", target["sha256"])):
                    raise ValueError("Selected application is not verified; no restart request sent")
            if not isinstance(target["size"], int) or isinstance(target["size"], bool) or target["size"] <= 0:
                raise ValueError("Expected application size unavailable; no restart request sent")
            record.update(before_update=status, expected_target=target,
                          restart_attempted=True)
            print("Warning: forced node restart skips saving; recent unsaved contacts, messages and settings may be lost.", file=sys.stderr)
            save_receipt(args.receipt, record)
            try:
                record["restart_response"] = json_request(client, "/admin/restart/force", b"")
            except (OSError, ValueError) as error:
                record["restart_failure"] = response_failure(error, client)
                save_receipt(args.receipt, record)
                if isinstance(error, urllib.error.HTTPError):
                    raise ValueError(f'Forced restart HTTP {error.code}: {record["restart_failure"].get("error", "inspect status")}') from None
            save_receipt(args.receipt, record)
        else:
            if record.get("format") != RECEIPT_FORMAT or record.get("url") != args.url:
                raise ValueError("Maintenance receipt format or radio URL differs")
            if not record.get("restart_attempted"):
                raise ValueError("Receipt has no restart attempt; start with a new receipt")
            record["complete"] = False
            save_receipt(args.receipt, record)
            client = connect()
        previous_client, client = client, None
        return finish_restart(previous_client, record, args.receipt, connect,
                              time.monotonic() + args.wait, args.poll_interval)
    finally:
        if client is not None:
            client.close()


def maintain(args):
    with receipt_lock(args.receipt):
        return maintain_locked(args)


def maintain_locked(args):
    if not 1 <= args.wait <= 600 or not .1 <= args.poll_interval <= 30:
        raise ValueError("Wait must be 1..600 seconds; poll interval 0.1..30 seconds")
    if args.action == "force-restart":
        return restart_locked(args)
    if args.action == "resume":
        saved = json.loads(private_file(args.receipt, 256 * 1024))
        if saved.get("kind") == "force-restart":
            return restart_locked(args, saved)
    password = private_file(args.password_file, 256)
    connect = lambda: node_backup.HttpClient(args.url, password)
    client = None
    record = None
    receipt_owned = False
    try:
        if args.action == "update":
            image, signed, backup, build_hash = artifact(args)
            if not re.fullmatch(r"[0-9a-f]{64}", args.expected_current_sha256):
                raise ValueError("Expected current application requires a full lowercase SHA256")
            if (args.sampler_slot is None) != (args.sampler_sha256 is None):
                raise ValueError("Provide both sampler slot and exact SHA256, or neither")
            if args.sampler_slot is not None and (not 0 <= args.sampler_slot < 2 or
                    not re.fullmatch(r"[0-9a-f]{64}", args.sampler_sha256)):
                raise ValueError("Sampler requires slot 0 or 1 and exact lowercase SHA256")
            record = {"format": RECEIPT_FORMAT, "url": args.url, "complete": False,
                      "application": signed, "backup": backup, "build_manifest_sha256": build_hash,
                      "expected_current_sha256": args.expected_current_sha256,
                      "commands": list(DEFAULT_COMMANDS) + args.retain_command,
                      "allow_unsaved_state": args.allow_unsaved_state}
            write_new_file(args.receipt, b"{}\n", 0o600)
            receipt_owned = True
            save_receipt(args.receipt, record)
            client = connect()
            status = json_request(client, "/admin/update")
            if (not running(status, args.expected_current_sha256) or
                    status.get("state") not in ("idle", "failed") or status.get("boot_health") != "healthy"):
                raise ValueError("Running application differs, is unhealthy or update active; no upload")
            if args.allow_unsaved_state and status.get("unsaved_state_supported") is not True:
                raise ValueError("Radio does not support unsaved-state application recovery; no upload")
            no_pending_identities(client)
            record.update(before_update=status, before=radio_state(client),
                          retained_before=retained(client, record["commands"]))
            if args.sampler_slot is not None:
                sampler = {"slot": args.sampler_slot, "sha256": args.sampler_sha256}
                sampler.update(sampler_state(client, sampler))
                record["sampler"] = sampler
            save_receipt(args.receipt, record)
            if record.get("sampler", {}).get("enabled"):
                record["sampler_restore_pending"] = True
                save_receipt(args.receipt, record)
                paced_command(client, f"packet {args.sampler_slot} enable off")
                paused = sampler_state(client, record["sampler"])
                if paused["enabled"] or paused["budget"] != record["sampler"]["budget"]:
                    raise ValueError("Packet sampler did not pause; no upload")
            try:
                flushed = paced_command(client, "bot contacts flush")
            except ValueError:
                if not args.allow_unsaved_state:
                    raise
                flushed = ""
            contacts_saved = bool(re.fullmatch(r"Saved learned contacts; dirty=0(?: .*)?", flushed))
            if not contacts_saved and not args.allow_unsaved_state:
                raise ValueError("Learned contacts were not flushed; no upload")
            record["contacts_flushed"] = contacts_saved
            if args.allow_unsaved_state:
                print("Warning: unsaved-state application recovery selected; recent unsaved state may be lost.", file=sys.stderr)
            record["upload_attempted"] = True
            save_receipt(args.receipt, record)
            try:
                record["upload_response"] = json_request(client, "/admin/update", image, {
                    "Content-Type": "application/octet-stream", "X-Mast-Target": signed["target"],
                    "X-Mast-SHA256": signed["sha256"], "X-Mast-Signature": signed["signature"],
                    **({"X-Mast-Allow-Unsaved-State": "1"} if args.allow_unsaved_state else {})})
            except (OSError, ValueError) as error:
                record["upload_response_unavailable"] = type(error).__name__
                record["upload_failure"] = response_failure(error, client)
                if record["upload_failure"].get("status"):
                    print(f'Application upload HTTP {error.code}: {record["upload_failure"].get("error", "inspect update status")}', file=sys.stderr)
            save_receipt(args.receipt, record)
        else:
            record = json.loads(private_file(args.receipt, 256 * 1024))
            if record.get("format") != RECEIPT_FORMAT or record.get("url") != args.url:
                raise ValueError("Maintenance receipt format or radio URL differs")
            receipt_owned = True
            record["complete"] = False
            save_receipt(args.receipt, record)
            client = connect()
            if not record.get("upload_attempted"):
                status = json_request(client, "/admin/update")
                if not running(status, record["expected_current_sha256"]) or status.get("state") not in ("idle", "failed"):
                    raise ValueError("Running application changed or update is active; sampler restoration needs inspection")
                restore_sampler(client, record, args.receipt)
                raise ValueError("This receipt has no upload attempt; start an update with a new receipt")
        previous_client, client = client, None
        finish(previous_client, record, args.receipt, connect,
               time.monotonic() + args.wait, args.poll_interval)
    finally:
        try:
            if receipt_owned and record.get("sampler_restore_pending"):
                if client is None:
                    client = connect()
                status = json_request(client, "/admin/update")
                if status.get("state") in ("idle", "failed") and (
                        running(status, record["expected_current_sha256"]) or
                        running(status, record["application"]["sha256"], record["application"]["size"])):
                    restore_sampler(client, record, args.receipt)
        except (OSError, ValueError) as error:
            record["sampler_restore_error"] = str(error)
            print("Packet sampler restoration remains pending; inspect the maintenance receipt", file=sys.stderr)
        finally:
            if client is not None:
                client.close()
            if receipt_owned:
                save_receipt(args.receipt, record)


def arguments(parser):
    sub = parser.add_subparsers(dest="maintenance_action", required=True)
    update = sub.add_parser("update", help="Verify backup and image, upload once, restart once, compare saved settings")
    restart = sub.add_parser("force-restart", help="Restart the node once without saving; recent unsaved state may be lost")
    resume = sub.add_parser("resume", help="Finish an earlier attempt; never repeat an upload or attempted restart")
    status = sub.add_parser("status", help="Read current application SHA256 and update/boot state")
    for command in (update, resume, restart):
        command.add_argument("--receipt", type=Path, required=True, help="Private JSON receipt (new path for update or force-restart)")
        command.add_argument("--wait", type=float, default=240, help="Seconds to wait for a healthy application")
        command.add_argument("--poll-interval", type=float, default=3, help=argparse.SUPPRESS)
    update.add_argument("--image", type=Path, required=True)
    update.add_argument("--build-manifest", type=Path, required=True)
    update.add_argument("--key-file", type=Path, required=True, help="Existing 32-byte operator signing seed (0600)")
    update.add_argument("--backup", type=Path, required=True, help="Encrypted node backup (0600), authenticated before upload")
    update.add_argument("--expected-current-sha256", required=True)
    restart.add_argument("--expected-current-sha256", required=True)
    update.add_argument("--board", default="xiao-esp32s3")
    update.add_argument("--retain-command", action="append", default=[], help="Additional read-only stable setting to compare")
    update.add_argument("--sampler-slot", type=int, help="Known read-only Wasm sampler to pause for flash writes")
    update.add_argument("--sampler-sha256", help="Exact hash of that read-only sampler")
    update.add_argument("--allow-unsaved-state", action="store_true",
                        help="Recover with one signed application update despite failed state saves; recent unsaved state may be lost (backup must be less than one hour old)")


def run(args, parser):
    if not args.web or not args.password_file or args.gateway or args.unix_socket or args.willow_socket:
        parser.error("firmware operations require --web and --password-file")
    args.url = args.web
    args.action = args.maintenance_action
    if args.action == "update":
        args.target = args.board
    try:
        if args.action == "status":
            client = node_backup.HttpClient(args.url, private_file(args.password_file, 256))
            try:
                print(json.dumps(json_request(client, "/admin/update"), indent=2))
            finally:
                client.close()
            return
        restarted = maintain(args)
    except (OSError, ValueError, KeyError) as error:
        receipt_help = f"Inspect receipt {args.receipt} before any retry.\n" if args.action != "status" else ""
        parser.exit(2, f"Application maintenance stopped: {error}\n" + receipt_help)
    if restarted:
        print(f"Node restarted; intended application and roles healthy. Receipt: {args.receipt}")
        return
    print(f"Application healthy. Identities, radio profile, selected programs and settings match. Receipt: {args.receipt}")


if __name__ == "__main__":
    raise SystemExit("Use python3 tools/admin.py firmware --help")
