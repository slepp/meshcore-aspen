#!/usr/bin/env python3
"""Run/check Willow, rebind frozen state, or contact its owner-only control socket."""
import argparse
import hashlib
import json
import os
import socket
import stat
import struct
import sys
import time
import uuid
from pathlib import Path

from build_worker import HERE, verify
from migrate_go import private, write_private, observer_config, identity_public
from reconcile_go import settings
from release_identity import VERSION

OWNER_PHASES = ("pending-admission", "admitted", "queued", "local-tx-confirmed",
                "native-rf-ack", "failed", "unknown")
OWNER_REASONS = ("none", "invalid-text", "fresh-direct-contact-required", "native-capacity-or-radio-unavailable",
                 "native-clock-unavailable", "native-airtime-or-admission-denied", "native-tx-failed-or-uncertain",
                 "native-ack-deadline-expired", "worker-modem-or-host-interrupted")
OWNER_ERRORS = {1: "malformed-owner-request", 2: "native-worker-or-modem-unavailable",
                3: "owner-request-ledger-full", 4: "request-id-content-conflict",
                6: "inbox-generation-changed", 7: "request-id-not-recorded",
                8: "owner-command-outcome-unknown; inspect affected state before another write",
                9: "owner-admin-request-ids-exhausted",
                10: "host-monitoring-sample-unavailable-or-stale"}


def owner_rpc(root, request):
    private(root, True)
    admin = len(request) >= 2 and request[1] in (4, 5)
    path = root/("admin.sock" if admin else "owner.sock")
    info = path.lstat()
    if not stat.S_ISSOCK(info.st_mode) or info.st_uid != os.geteuid() or stat.S_IMODE(info.st_mode) != 0o600:
        raise ValueError("owner control socket must be an owned mode-0600 Unix socket")
    with socket.socket(socket.AF_UNIX, socket.SOCK_SEQPACKET) as connection:
        connection.settimeout(10 if admin else 4)
        connection.connect(str(path))
        _, uid, _ = struct.unpack("3i", connection.getsockopt(socket.SOL_SOCKET, socket.SO_PEERCRED, 12))
        if uid != os.geteuid():
            raise ValueError("owner control server UID mismatch")
        connection.sendall(request)
        response, _, flags, _ = connection.recvmsg(65536 if len(request) >= 2 and request[1] == 6 else 4096)
    if flags & socket.MSG_TRUNC or len(response) < 2 or response[0] != 1:
        raise ValueError("invalid owner control response")
    if response[1]:
        raise ValueError(OWNER_ERRORS.get(response[1], "owner-control-error"))
    return response[2:]


def owner_status(root, identifier, operation=2, body=b""):
    data = owner_rpc(root, bytes([1, operation])+identifier+body)
    if len(data) != 19 or data[:16] != identifier or data[16] >= len(OWNER_PHASES) or data[17] > 7:
        raise ValueError("invalid owner send status")
    return {"request_id": identifier.hex(), "phase": OWNER_PHASES[data[16]],
            "admitted": bool(data[17]&4), "local_tx_confirmed": bool(data[17]&1),
            "native_rf_ack": bool(data[17]&2),
            "reason": OWNER_REASONS[data[18]] if data[18] < len(OWNER_REASONS) else "native-error"}


def owner_inbox(root, cursor="0:0"):
    generation, after = (int(part) for part in cursor.split(":"))
    if min(generation, after) < 0 or max(generation, after) > 0x7fffffffffffffff:
        raise ValueError("invalid inbox cursor")
    data = owner_rpc(root, b"\1\3"+struct.pack("<QQ", generation, after))
    if len(data) < 17:
        raise ValueError("invalid owner inbox response")
    generation, dropped = struct.unpack_from("<QQ", data)
    messages, at = [], 17
    for _ in range(data[16]):
        if len(data) < at+9:
            raise ValueError("truncated owner inbox entry")
        sequence = struct.unpack_from("<Q", data, at)[0]
        size = data[at+8]; at += 9
        body = data[at:at+size]; at += size
        if len(body) != size or size < 37:
            raise ValueError("invalid owner inbox message")
        messages.append({"sequence": sequence, "sender": body[:32].hex(),
                         "timestamp": struct.unpack_from("<I", body, 32)[0],
                         "message": body[36:].decode("ascii")})
        after = sequence
    if at != len(data):
        raise ValueError("trailing owner inbox data")
    return {"cursor": f"{generation}:{after}", "dropped": dropped, "messages": messages}


def packet_log(root, role):
    private(root, True)
    if role not in ("repeater", "room"):
        raise ValueError("packet-log requires --role repeater or room")
    path = root/("relay" if role == "repeater" else role)
    path = path.with_name(path.name+".state.packet.log")
    try:
        fd = os.open(path, os.O_RDONLY|os.O_NOFOLLOW|os.O_CLOEXEC|os.O_NONBLOCK)
    except FileNotFoundError:
        return b""
    with os.fdopen(fd, "rb") as source:
        info = os.fstat(source.fileno())
        if not stat.S_ISREG(info.st_mode) or info.st_uid != os.geteuid() or info.st_mode&0o077 or info.st_nlink != 1 or info.st_size > 4194304:
            raise ValueError("packet log must be an owned private regular file no larger than 4 MiB")
        data = source.read(4194305)
        if len(data) > 4194304:
            raise ValueError("packet log grew beyond 4 MiB while reading")
        return data


def check(root, verify_worker=True):
    private(root,True)
    private(root/"config")
    config=settings((root/"config").read_bytes())
    observing = observer_config(config)
    if observing:
        observer_path = root/("observer.expanded" if (root/"observer.expanded").exists() else "observer.seed")
        private(observer_path)
        material = observer_path.read_bytes()
        if len(material) != (64 if observer_path.suffix == ".expanded" else 32):
            raise ValueError("observer: invalid identity file length")
        identity_public(material)
    if verify_worker and config.get("worker","-")!="-": verify(config["worker"])
    for role in ("relay","room","bot"):
        private(root/(role+".seed"))
        if (root/(role+".seed")).stat().st_size!=32:
            raise ValueError(f"{role}: invalid seed length")
    if config.get("imported")=="1":
        private(root/"migration.json")
        record=json.loads((root/"migration.json").read_text())
        if record.get("version")!=1 or record.get("mode")!="staged":
            raise ValueError("migration record is incomplete")
        roles = ("relay", "room", "bot") + (("observer",) if "observer" in record["roles"] else ())
        for role in roles:
            identity_format = record["roles"][role].get("identity_format", "seed") if role == "observer" else "seed"
            if identity_format not in ("seed", "expanded"):
                raise ValueError("observer: unknown imported identity format")
            identity_path = root/(role+"."+identity_format)
            private(identity_path)
            material = identity_path.read_bytes()
            if len(material) != (64 if identity_format == "expanded" else 32):
                raise ValueError(role+": invalid imported identity length")
            if role == "observer" and identity_format == "seed" and (root/"observer.expanded").exists():
                raise ValueError("observer: unexpected expanded identity overrides imported seed")
            public=identity_public(material)
            if public.hex()!=record["roles"][role]["public_key"]:
                raise ValueError(f"{role}: imported identity changed; restore the original seed")
        marker=root/"willow.started"
        if not marker.exists():
            for relative,expected in record["output_files"].items():
                candidate=Path(relative)
                if candidate.is_absolute() or ".." in candidate.parts:
                    raise ValueError("invalid migration output path")
                target=root/candidate
                private(target)
                if hashlib.sha256(target.read_bytes()).hexdigest()!=expected:
                    raise ValueError(f"migration output changed before first startup: {relative}")
        else:
            private(marker)
    return config


def rebind(source, destination):
    from migrate_go import document, hashes, inventory, owner_ledger, admin_clock
    from reconcile_go import frozen

    worker = verify()
    with frozen(source, "service.lock") as source:
        check(source, verify_worker=False)
        files = inventory(source, retained=("rollback-go",))
        if any(name.endswith(".pending") for name in files):
            raise ValueError("frozen state contains an unfinished transaction; recover with its owning version first")
        config = settings(files["config"], strict=True)
        if config.get("imported") != "1" or config.get("worker", "-") == "-":
            raise ValueError("rebind requires imported native-worker state")
        record = document(files["migration.json"], "migration record")
        originals = {name[12:]: raw for name, raw in files.items() if name.startswith("rollback-go/")}
        if hashes(originals) != record.get("source_files"):
            raise ValueError("rollback-go differs from its migration source")
        if "owner.state" in files:
            owner_ledger(files["owner.state"])
        if "admin.clock" in files:
            admin_clock(files["admin.clock"])
        old_worker = config["worker"]
        config["worker"] = worker["worker"]
        output = dict(files)
        output["config"] = "".join(key+"="+value+"\n" for key, value in sorted(config.items())).encode()
        record["rebind"] = {
            "version": 1, "source": str(source),
            "source_fingerprint": hashlib.sha256(json.dumps(hashes(files), sort_keys=True).encode()).hexdigest(),
            "previous_worker": old_worker, "previous_worker_sha256": record.get("worker_sha256"),
            "worker": worker["worker"], "worker_sha256": worker["worker_sha256"]}
        record["worker_sha256"] = worker["worker_sha256"]
        record["output_files"]["config"] = hashlib.sha256(output["config"]).hexdigest()
        encoded = json.dumps(record, indent=2, sort_keys=True).encode()+b"\n"
        if len(encoded) > 65536:
            raise ValueError("rebound migration record exceeds 64 KiB")
        destination = destination.parent.resolve(strict=True)/destination.name
        if os.path.lexists(destination) or destination.is_relative_to(source) or source.is_relative_to(destination):
            raise ValueError("rebind destination must be NEW and disjoint from the frozen source")
        destination.mkdir(mode=0o700)
        for directory in sorted(p.relative_to(source) for p in source.rglob("*") if p.is_dir()):
            (destination/directory).mkdir(mode=0o700, parents=True, exist_ok=True)
        for name, raw in output.items():
            if name != "migration.json":
                write_private(destination/name, raw)
        if hashes(inventory(source, retained=("rollback-go",))) != hashes(files):
            raise ValueError("source changed during rebinding; candidate is incomplete and must not run")
        for directory in sorted((p for p in destination.rglob("*") if p.is_dir()), reverse=True)+[destination]:
            fd = os.open(directory, os.O_RDONLY|os.O_DIRECTORY)
            try: os.fsync(fd)
            finally: os.close(fd)
        write_private(destination/"migration.json", encoded)
        for directory in (destination, destination.parent):
            fd = os.open(directory, os.O_RDONLY|os.O_DIRECTORY)
            try: os.fsync(fd)
            finally: os.close(fd)
        check(destination)
        return {"state": str(destination), "worker": worker["worker"],
                "worker_sha256": worker["worker_sha256"], "mode": "rebound"}


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--version", action="version", version="Willow " + VERSION)
    parser.add_argument("operation",choices=("check","run","rebind","send","status","inbox","command","health","packet-log"))
    parser.add_argument("--state",type=Path,required=True)
    parser.add_argument("--source",type=Path,help="rebind: complete stopped/frozen imported state; --state must be NEW")
    parser.add_argument("--release",action="store_true",help="run the optimized build/hew-host-release executable")
    parser.add_argument("--request-id", help="required nonzero UUID/32 hex digits; never automatically replayed")
    parser.add_argument("--to", help="full 32-byte recipient public key in hex; requires a fresh native direct contact")
    parser.add_argument("--message-file", type=Path, help="private ASCII message file; omit to read stdin")
    parser.add_argument("--wait", type=float, default=0, help="poll send status for at most this many seconds")
    parser.add_argument("--after", default="0:0", help="inbox generation:sequence cursor")
    parser.add_argument("--role", choices=("bot", "repeater", "room"), default="bot",
                        help="command: local owner role; command is read from stdin or --message-file")
    args=parser.parse_args()
    root=args.state.absolute()
    try:
        if args.operation == "packet-log":
            sys.stdout.buffer.write(packet_log(root, args.role))
            return
        if args.operation == "rebind":
            if args.source is None:
                raise ValueError("rebind requires --source FROZEN_STATE and --state NEW_STATE")
            print(json.dumps(rebind(args.source, root), indent=2))
            return
        if args.operation == "inbox":
            print(json.dumps(owner_inbox(root, args.after), indent=2))
            return
        if args.operation == "health":
            print(json.dumps({"roles": json.loads(owner_rpc(root, b"\x01\x06\x00")),
                              "readiness": json.loads(owner_rpc(root, b"\x01\x06\x01"))}, indent=2))
            return
        if args.operation == "command":
            if args.message_file:
                private(args.message_file, max_bytes=161)
                command = args.message_file.read_bytes()
            else:
                command = sys.stdin.buffer.read(162)
            command = command.removesuffix(b"\n")
            if not 1 <= len(command) <= 160 or any(c < 32 or c > 126 for c in command):
                raise ValueError("owner command requires 1..160 printable ASCII bytes")
            prefix = b"\x01\x04" if args.role == "bot" else bytes((1, 5, 2 if args.role == "repeater" else 3))
            reply = owner_rpc(root, prefix+command).decode("utf-8", errors="strict")
            print(json.dumps({"role": args.role, "reply": reply}, indent=2))
            if reply.startswith(("Error:", "Error,", "Error ", "Err ", "Err-")):
                raise SystemExit(2)
            return
        if args.operation in ("send", "status"):
            if not args.request_id or not 0 <= args.wait <= 120:
                raise ValueError("send/status requires --request-id and --wait in 0..120")
            identifier = uuid.UUID(args.request_id).bytes
            if identifier == bytes(16):
                raise ValueError("request ID must be nonzero")
            if args.operation == "send":
                recipient = bytes.fromhex(args.to or "")
                if len(recipient) != 32:
                    raise ValueError("send requires --to FULL_PUBLIC_KEY")
                if args.message_file:
                    private(args.message_file, max_bytes=163)
                    text = args.message_file.read_bytes()
                else:
                    text = sys.stdin.buffer.read(164)
                text = text.removesuffix(b"\n")
                if not 1 <= len(text) <= 162 or any(c < 32 or c > 126 for c in text):
                    raise ValueError("native DM requires 1..162 printable ASCII bytes")
                result = owner_status(root, identifier, 1, recipient+text)
            else:
                result = owner_status(root, identifier)
            end = time.monotonic()+args.wait
            while result["phase"] not in ("native-rf-ack", "failed", "unknown") and time.monotonic() < end:
                time.sleep(.15)
                result = owner_status(root, identifier)
            print(json.dumps(result, indent=2))
            if result["phase"] in ("failed", "unknown") or (args.wait and result["phase"] != "native-rf-ack"):
                raise SystemExit(2)
            return
        config=check(root)
        if args.operation=="check":
            print("Willow state and worker checks passed; no modem connection opened")
            return
        binary=HERE/"build"/("hew-host-release" if args.release else "hew-host")
        if not binary.is_file() or not os.access(binary,os.X_OK):
            raise ValueError(f"service executable unavailable; run make build/{binary.name}")
        marker=root/"willow.started"
        if config.get("imported")=="1" and not marker.exists():
            write_private(marker,b"Willow first-start migration inputs checked\n")
            fd=os.open(root,os.O_RDONLY|os.O_DIRECTORY)
            try: os.fsync(fd)
            finally: os.close(fd)
        os.execv(binary,[str(binary),str(root)])
    except (OSError,ValueError,KeyError) as error:
        detail = "; if send may have been submitted, query status with the SAME request ID; do not send a new ID" if args.operation == "send" else (
            "; owner command outcome may be unknown; inspect affected state before another write" if args.operation == "command" else "")
        raise SystemExit(f"Willow {args.operation} refused: {error}{detail}")


if __name__=="__main__": main()
