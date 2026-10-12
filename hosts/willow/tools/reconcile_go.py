#!/usr/bin/env python3
"""Reconcile a frozen imported Willow tree into a NEW private Go candidate."""
import argparse
import base64
import copy
from contextlib import ExitStack, contextmanager
import fcntl
import hashlib
import json
import math
import os
from pathlib import Path
import struct
import region_state

from migrate_go import (BASE_FIELDS, OBSERVER_FIELDS, ROLE_OPTIONS, document, fail,
                        hashes, inventory, native_state, private, raw_bytes,
                        region_key, write_private, observer_authority, owner_ledger, admin_clock)
from cryptography.hazmat.primitives.asymmetric.ed25519 import Ed25519PrivateKey
from cryptography.hazmat.primitives.serialization import Encoding, PublicFormat

ROLES = {"relay": ("repeater", 2), "room": ("room", 3), "bot": ("bot", 1),
         "observer": ("observer", None)}


def encoded(value):
    return json.dumps(value, ensure_ascii=True, sort_keys=True, indent=2).encode()+b"\n"


def unique_fields(value, where):
    if not isinstance(value, dict):
        fail(f"{where}: expected an object")
    names = [key.lower() for key in value]
    if len(names) != len(set(names)):
        fail(f"{where}: ambiguous field casing")


def settings(raw, strict=False):
    result = {}
    for number, line in enumerate(raw.decode("utf-8").splitlines(), 1):
        if not line:
            continue
        key, sep, value = line.partition("=")
        if not sep:
            fail(f"Willow config line {number}: expected key=value")
        if strict and key in result:
            fail(f"Willow config line {number}: duplicate key {key}")
        if "\0" in line:
            fail(f"Willow config line {number}: {key} contains a NUL byte")
        prefix, _, suffix = key.partition(".")
        if key not in BASE_FIELDS and not (prefix in ("relay", "room") and suffix in ROLE_OPTIONS) and not (prefix == "observer" and suffix in OBSERVER_FIELDS):
            fail(f"Willow config line {number}: unknown key {key}")
        if suffix == "preference_profile" and value not in ("native-preferences", "durable-host-preferences"):
            fail(f"Willow config line {number}: {key} must be native-preferences or durable-host-preferences")
        result[key] = value
    return result


def identity(files, role):
    if role == "observer":
        material, public, raw = observer_authority(files)
        state = document(raw, "observer state") if raw is not None else None
        envelope = document(files["observer/identity-state.json"], "observer envelope") if "observer/identity-state.json" in files else None
        return material, public, state, envelope
    seed = files.get(role+"/identity.seed", b"")
    if len(seed) != 32:
        fail(f"{role}: a retained matching seed is required")
    expanded = bytearray(hashlib.sha512(seed).digest())
    expanded[0] &= 248
    expanded[31] = (expanded[31]&63)|64
    envelope = None
    if role+"/identity-state.json" in files:
        envelope = document(files[role+"/identity-state.json"], role+" envelope")
        unique_fields(envelope, role+" envelope")
        if envelope.get("version") != 1 or envelope.get("pending_identity") is not None:
            fail(f"{role}: pending or unsupported identity envelope")
        if raw_bytes(envelope.get("identity"), 64, role+" identity") != expanded:
            fail(f"{role}: active envelope identity differs from retained seed")
        if envelope.get("document") != "state.json":
            fail(f"{role}: unsupported identity envelope document")
        state = envelope.get("state")
    else:
        if role+"/identity.expanded" in files and files[role+"/identity.expanded"] != expanded:
            fail(f"{role}: active expanded identity differs from retained seed")
        state = document(files[role+"/state.json"], role+" state") if role+"/state.json" in files else None
    public = Ed25519PrivateKey.from_private_bytes(seed).public_key().public_bytes(Encoding.Raw, PublicFormat.Raw)
    return seed, public, state, envelope


class Reader:
    def __init__(self, data, where):
        self.data, self.at, self.where = data, 0, where

    def take(self, size):
        if size < 0 or self.at+size > len(self.data):
            fail(self.where+": truncated snapshot")
        value = self.data[self.at:self.at+size]
        self.at += size
        return value

    def number(self, size=1):
        return int.from_bytes(self.take(size), "little")

    def flag(self):
        value = self.number()
        if value > 1:
            fail(self.where+": invalid boolean")
        return bool(value)

    def finish(self):
        if self.at != len(self.data):
            fail(self.where+": unknown trailing snapshot data")


def preferences(r):
    start = r.at
    version = r.number()
    if version not in (1, 2, 3):
        fail(r.where+": unsupported preference version")
    texts = [r.take(r.number(2)).decode("utf-8") for _ in range(4)]
    name, guest, admin, owner = texts
    if not 1 <= len(name.encode()) <= 31 or len(guest.encode()) > 63 or len(admin.encode()) > 63 or len(owner.encode()) > 119 or any("\0" in text for text in texts):
        fail(r.where+": invalid preference text")
    width, flags, acks, loop, flood, unscoped, adverts, local, flood_time = [r.number(4) for _ in range(9)]
    delay, direct = struct.unpack("<ff", r.take(8))
    latitude, longitude, offset, location = struct.unpack("<ddqB", r.take(25)) if version >= 2 else (0, 0, 0, 0)
    rxdelay, factor = struct.unpack("<ff", r.take(8)) if version == 3 else (0, 1)
    preference_profile = r.number() if version == 3 else 0
    if preference_profile > 1:
        fail(r.where+": invalid source-factor preference profile")
    if not math.isfinite(rxdelay) or not 0 <= rxdelay <= 20 or not math.isfinite(factor) or factor < 0:
        fail(r.where+": invalid receive delay or source airtime factor")
    if any(not math.isfinite(value) or abs(value) > limit for value, limit in ((latitude, 90), (longitude, 180))) or location not in (0, 2):
        fail(r.where+": invalid saved advert location")
    if not 54 <= r.at-start <= 364 or not 1 <= width <= 3 or flags > 3 or acks > 1 or loop > 3 or max(flood, unscoped, adverts) > 64:
        fail(r.where+": invalid preference scalars")
    if local > 30600 or flood_time > 918000 or any(not math.isfinite(v) or not 0 <= v <= 2 for v in (delay, direct)):
        fail(r.where+": preference exceeds Go storage bounds")
    return {"Name": name, "guest": guest, "admin": admin, "AllowReadOnly": bool(flags&2),
            "MultiACKs": acks, "Latitude": latitude, "Longitude": longitude,
            "RTCOffset": offset, "AdvertLocation": location,
            **({"PreferenceProfile": "durable-host-preferences" if preference_profile else "native-preferences"} if version == 3 else {}),
            "Preferences": {
                "owner_info": owner, "path_hash_mode": width-1, "repeat": bool(flags&1),
                "loop": loop, "flood_max_hops": flood, "unscoped_max_hops": unscoped,
                "advert_max_hops": adverts, "local_advert_seconds": local,
                "flood_advert_seconds": flood_time, "txdelay": delay, "direct_txdelay": direct,
                "rxdelay": rxdelay, "airtime_factor": factor}}


def snapshot(data, public, kind, where):
    if not 51 <= len(data) <= 65536:
        fail(where+": invalid snapshot size")
    r = Reader(data, where)
    version = r.take(4)
    if version not in (b"HEW2", b"HEW3", b"HEW4", b"HEW5", b"HEW6"):
        fail(where+": unknown snapshot version")
    if r.take(32) != public:
        fail(where+": snapshot identity differs from retained seed")
    result = dict(zip(("Clock", "Posted", "Pushed"), (r.number(4) for _ in range(3))))
    count = r.number()
    if count > 20 or r.number() != kind:
        fail(where+": invalid member count or role")
    members = {}
    for _ in range(count):
        key = r.take(32)
        if key.hex() in members:
            fail(where+": duplicate member")
        member = {"Key": list(key), "Permissions": r.number(), "LastTimestamp": r.number(4),
                  "SyncSince": r.number(4), "KnownPath": r.flag(), "PathLength": r.number()}
        path = r.take(r.number())
        length = member["PathLength"]
        if length >= 192 or len(path) > 64 or len(path) != ((length>>6)+1)*(length&63):
            fail(where+": invalid member path")
        member.update(Path=base64.b64encode(path).decode() if path else None, Attempt=0)
        members[key.hex()] = member
    note_count = r.number()
    if note_count:
        fail(where+": all-Hew notes have no Go-native representation; native worker required")
    history = version == b"HEW3"
    if version in (b"HEW4", b"HEW5", b"HEW6"):
        history = r.flag()
        for member in members.values():
            member["Attempt"] = r.number()
    posts = []
    if history:
        count = r.number()
        if count > 32:
            fail(where+": too many retained posts")
        previous = 0
        for _ in range(count):
            author, stamp = r.take(32), r.number(4)
            text = r.take(r.number())
            if not 1 <= len(text) <= 151 or b"\0" in text or not previous < stamp <= result["Clock"]:
                fail(where+": invalid retained history")
            post = {"Author": list(author), "Timestamp": stamp}
            try:
                post["Text"] = text.decode("utf-8")
            except UnicodeDecodeError:
                post.update(Text="", RawText=base64.b64encode(text).decode())
            posts.append(post)
            previous = stamp
    result.update(Members=members, MemberOrder=list(members), History=posts,
                  Retention="durable-replay" if history else "native")
    if version == b"HEW6":
        saved = Reader(r.take(r.number(2)), where+" preferences")
        result["settings"] = preferences(saved)
        saved.finish()
        result["region_table"] = region_state.decode(r.take(r.number(2)))
    else:
        result["settings"] = preferences(r) if version == b"HEW5" else None
        result["region_table"] = None
    r.finish()
    return result


def regions(state, cfg, role):
    prefs = state["Preferences"]
    old = prefs.get("regions") or []
    keys, by_id = {}, {}
    for entry in old:
        unique_fields(entry, role+" region")
        key = region_key({k: v for k, v in entry.items() if k in ("id", "parent", "flags", "name", "keys")})
        identifier = entry["id"]
        if key.hex() in keys or identifier in by_id:
            fail(role+": ambiguous retained region key or ID")
        keys[key.hex()] = entry
        by_id[identifier] = entry
    next_id = max(state.get("NextRegionID", 1), max(by_id, default=0)+1)

    def lookup(text):
        nonlocal next_id
        if not text:
            return None
        raw = bytes.fromhex(text)
        if len(raw) != 16 or not any(raw):
            fail(role+": region key must be 16 nonzero-as-a-whole bytes; Go treats an all-zero scope as disabled")
        text = raw.hex()
        if text not in keys:
            if next_id > 65535 or len(old) >= 32:
                fail(role+": Go region capacity exhausted")
            entry = {"id": next_id, "parent": 0, "flags": 1,
                     "name": "$willow-"+text[:16], "keys": [list(raw)]}
            if any(item["name"] == entry["name"] for item in old):
                fail(role+": generated region name collides with retained name")
            old.append(entry)
            keys[text] = entry
            by_id[next_id] = entry
            next_id += 1
        return keys[text]

    prefix = role+"."
    permitted = cfg.get(prefix+"regions")
    if permitted is not None:
        wanted = [lookup(key) for key in permitted.split(",")] if permitted else []
        if len(wanted) > 8 or len({e["id"] for e in wanted}) != len(wanted):
            fail(role+": invalid permitted region list")
        allowed = {e["id"] for e in wanted}
        for entry in old:
            entry["flags"] = (entry.get("flags", 0)&~1) | (0 if entry["id"] in allowed else 1)
        # Retain denied entries and relative metadata; permitted matching order follows Willow.
        iterator = iter(wanted)
        old = [next(iterator) if e["id"] in allowed else e for e in old]
    if prefix+"home" in cfg:
        entry = lookup(cfg[prefix+"home"])
        state["HomeRegion"] = entry["id"] if entry else 0
    if prefix+"default_scope" in cfg:
        value = cfg[prefix+"default_scope"]
        original = prefs.get("default_scope") or {}
        original_key = raw_bytes(original.get("key", [0]*16), 16, role+" default scope")
        entry = lookup(value)
        if value != (original_key.hex() if any(original_key) else ""):
            replacement = copy.deepcopy(original)
            replacement.update(name=entry["name"] if entry else "", key=list(bytes.fromhex(value)) if value else [0]*16)
            prefs["default_scope"] = replacement
        if state.get("ManagedDefaultRegion") or value != (original_key.hex() if any(original_key) else ""):
            state["ManagedDefaultRegion"] = True
            state["DefaultRegion"] = entry["id"] if entry else 0
    if prefix+"wildcard" in cfg:
        value = cfg[prefix+"wildcard"]
        if value not in ("0", "1"):
            fail(role+": invalid wildcard setting")
        prefs["wildcard_flags"] = (prefs.get("wildcard_flags", 0)&~1) | (0 if value == "1" else 1)
    prefs["regions"] = old
    state["NextRegionID"] = next_id


def merge_state(original, current, cfg, role, runtime=None):
    state = copy.deepcopy(original)
    unique_fields(state, role+" Go state")
    if state.get("Version") not in (1, 2) or not isinstance(state.get("Preferences"), dict):
        fail(role+": unsupported original Go state")
    prefs = state["Preferences"]
    unique_fields(prefs, role+" Go preferences")
    old_members = state.get("Members") or {}
    old_posts = {p["Timestamp"]: p for p in state.get("History") or []}
    fresh = copy.deepcopy(current)
    applied = fresh.pop("settings")
    named = fresh.pop("region_table", None)
    for key, member in fresh["Members"].items():
        retained = copy.deepcopy(old_members.get(key, {}))
        unique_fields(retained, role+" member")
        retained.update(member)
        fresh["Members"][key] = retained
    for i, post in enumerate(fresh["History"]):
        retained = copy.deepcopy(old_posts.get(post["Timestamp"], {}))
        if retained and retained.get("Author") != post["Author"]:
            fail(role+": retained post timestamp changed author")
        retained.pop("RawText", None)
        retained.update(post)
        fresh["History"][i] = retained
    if current["Retention"] == "native" and state.get("History"):
        fail(role+": volatile snapshot cannot replace retained Go history")
    if role == "room" and current["Retention"] == "native" and any(
            current[key] > original.get(key, 0) for key in ("Posted", "Clock")):
        fail("room: new posts used native retention and are absent from the snapshot; cannot recover volatile history")
    if any(fresh[key] < original.get(key, 0) for key in ("Clock", "Posted", "Pushed")):
        fail(role+": persisted clock/counter regressed; cannot infer reset versus wrap")
    state.update(fresh)
    state["Retention"] = "durable-replay"
    state["Version"] = 2
    prefix = role+"."
    state["Name"] = cfg.get(prefix+"name", "Willow-"+role.title())
    width = int(cfg.get("width", "3"))
    if not 1 <= width <= 3:
        fail(role+": invalid configured path width")
    prefs["path_hash_mode"] = width-1
    for source, target in (("repeat", "repeat"), ("loop", "loop"), ("flood_max", "flood_max_hops"),
                           ("unscoped_max", "unscoped_max_hops"), ("advert_max", "advert_max_hops"),
                           ("local_advert_seconds", "local_advert_seconds"),
                           ("flood_advert_seconds", "flood_advert_seconds")):
        if prefix+source in cfg:
            value = int(cfg[prefix+source])
            maximum = {"repeat": 1, "loop": 3, "local_advert_seconds": 30600,
                       "flood_advert_seconds": 918000}.get(source, 64)
            if not 0 <= value <= maximum:
                fail(role+": configured "+source+" exceeds Go storage bounds")
            prefs[target] = bool(value) if source == "repeat" else value
    for source, target in (("txdelay_milli", "txdelay"), ("direct_txdelay_milli", "direct_txdelay")):
        if prefix+source in cfg:
            value = int(cfg[prefix+source])
            if not 0 <= value <= 2000:
                fail(role+": invalid configured "+source)
            prefs[target] = struct.unpack("<f", struct.pack("<f", value/1000))[0]
    credentials = {name: cfg.get(prefix+name, cfg.get(name, "")) for name in ("password", "admin")}
    if applied:
        prefs.update(applied["Preferences"])
        for key in ("Name", "AllowReadOnly", "MultiACKs", "Latitude", "Longitude", "RTCOffset", "AdvertLocation", "PreferenceProfile"):
            if key in applied:
                state[key] = applied[key]
        if prefix+"preference_profile" in cfg:
            state["PreferenceProfile"] = cfg[prefix+"preference_profile"]
        credentials = {"password": applied["guest"], "admin": applied["admin"]}
    for key, text, raw in (("password", "GuestPasswordOverride", "GuestPasswordBytes"),
                           ("admin", "AdminPasswordOverride", "AdminPasswordBytes")):
        value = credentials[key]
        if len(value.encode()) > 63 or "\0" in value:
            fail(role+": invalid management password")
        state.pop(raw, None)
        if len(value.encode()) <= 15:
            state[text] = value
        else:
            runtime_key = "room_password" if key == "password" else "admin_password"
            if runtime is None or (runtime_key in runtime and runtime[runtime_key] != value):
                fail(role+": different long per-role passwords cannot share Go runtime credentials")
            runtime[runtime_key] = value
            state[text] = None
    if not 1 <= len(state["Name"].encode()) <= 31 or "\0" in state["Name"]:
        fail(role+": invalid configured name")
    if named is not None:
        region_state.apply(state, region_state.configured(named, cfg, prefix))
    elif prefix+"region_state" in cfg:
        baseline = region_state.decode(bytes.fromhex(cfg[prefix+"region_state"]))
        region_state.apply(state, region_state.configured(baseline, cfg, prefix))
    else:
        regions(state, cfg, role)
    # Go saves immediately on load. Native retention would delete returned members
    # and replay stamps there, so the candidate must retain the complete snapshot.
    return state


@contextmanager
def frozen(path, lock_name):
    path = Path(path).absolute()
    private(path, True)
    path = path.resolve(strict=True)
    handle = None
    if (path/lock_name).exists():
        private(path/lock_name)
        handle = (path/lock_name).open("rb")
        try:
            fcntl.flock(handle, fcntl.LOCK_EX|fcntl.LOCK_NB)
        except BlockingIOError:
            handle.close()
            fail(f"{path}: locked; stop the writer and freeze a complete snapshot")
    try:
        yield path
    finally:
        if handle:
            handle.close()


def reconcile(source, destination, go_source=None):
    with ExitStack() as stack:
        source = stack.enter_context(frozen(source, "service.lock"))
        files = inventory(source, retained=("rollback-go",))
        record = document(files.get("migration.json", b""), "migration record")
        if record.get("version") != 1 or record.get("mode") != "staged":
            fail("source requires its completed Go-to-Willow migration record")
        base = {name[12:]: raw for name, raw in files.items() if name.startswith("rollback-go/")}
        if hashes(base) != record.get("source_files"):
            fail("rollback-go differs from the preserved migration source")
        cfg = settings(files.get("config", b""), strict=True)
        if cfg.get("imported") != "1" or cfg.get("worker", "-") == "-":
            fail("reconciliation requires imported state and the native bot")
        output = dict(base)
        if go_source is not None:
            go_source = stack.enter_context(frozen(go_source, ".lock"))
            output = inventory(go_source, record.get("retained_snapshot_directories", ()))
            for target in record["roles"]:
                if target not in ROLES:
                    fail("migration record contains an unknown role")
                role = ROLES[target][0]+"/"
                if {n: b for n, b in output.items() if n.startswith(role)} != {n: b for n, b in base.items() if n.startswith(role)}:
                    fail(role+": current Go role changed since handover; cannot overwrite competing writes")
        baseline = hashes(output)
        recognized = {"config", "migration.json", "service.lock", "willow.started", "roles.initialized",
                      "advert-times", "observer.initialized", "observer.expanded", "owner.state", "admin.clock"}
        recognized |= {role+suffix for role in ROLES for suffix in (".seed", ".state", ".state.packet.log")}
        for name in files:
            if not name.startswith(("rollback-go/", "native/")) and name not in recognized:
                fail(name+": unknown Willow file; freeze/recover the source before reconciliation")
            if name.endswith(".pending"):
                fail(name+": unfinished transaction; recover with the owning service before freezing")
        publics = {}
        roles = {}
        runtime = {}
        for target, info in record["roles"].items():
            if target not in ROLES:
                fail("migration record contains an unknown role")
            role, kind = ROLES[target]
            seed, public, original, envelope = identity(base, role)
            identity_format = "expanded" if target == "observer" and len(seed) == 64 else "seed"
            if target == "observer" and info.get("identity_format", "seed") != identity_format:
                fail("observer: imported identity format differs from its active Go authority")
            if files.get(target+"."+identity_format) != seed or info.get("public_key") != public.hex():
                fail(target+": retained identity changed after migration")
            if public in publics.values():
                fail(target+": role identity collision")
            publics[target] = public
            if kind is None:
                if "observer.initialized" in files and files["observer.initialized"] != public:
                    fail("observer.initialized differs from its retained identity")
                continue
            blob = files.get(target+".state")
            if blob is None:
                if "willow.started" in files or target != "bot":
                    fail(target+": committed snapshot missing; do not reconcile a quarantined role")
                continue
            current = snapshot(blob, public, kind, target)
            if target == "bot":
                if current["Members"] or current["History"] or any(current[k] for k in ("Clock", "Posted", "Pushed")):
                    fail("bot.state contains all-Hew activity; native-only reconciliation cannot discard it")
                continue
            if original is None or original.get("Identity") != public.hex() or original.get("Room") != (kind == 3):
                fail(role+": retained Go state identity/role mismatch")
            merged = merge_state(original, current, cfg, target, runtime)
            log = files.get(target+".state.packet.log")
            output.pop(role+"/packet.log", None)
            if log is not None:
                if len(log) > 4194304: fail(role+": packet log exceeds 4 MiB")
                output[role+"/packet.log"] = log
            if envelope is not None:
                envelope["state"] = merged
                output[role+"/identity-state.json"] = encoded(envelope)
            else:
                output[role+"/state.json"] = encoded(merged)
            roles[role] = {"members": len(merged["Members"]), "history": len(merged["History"]),
                           "identity": public.hex(), "retention": merged["Retention"],
                           "previous_retention": original.get("Retention", "native")}
        if not {"relay", "room", "bot"}.issubset(publics):
            fail("migration record is missing a production role")
        if "roles.initialized" in files and files["roles.initialized"] != b"RLS1"+publics["relay"]+publics["room"]+publics["bot"]:
            fail("roles.initialized does not match the migrated identities")
        native_files = {"bot/"+name: raw for name, raw in files.items() if name.startswith("native/")}
        native, native_info = native_state(native_files, {})
        output = {name: raw for name, raw in output.items() if not name.startswith("bot/native/")}
        output.update({"bot/native/"+name: raw for name, raw in native.items()})
        if "owner.state" in files:
            output["willow-owner.state"] = owner_ledger(files["owner.state"])
        if "admin.clock" in files:
            output["willow-admin.clock"] = admin_clock(files["admin.clock"])
        previous = output.pop("willow-reconciliation.json", None)
        if previous is not None:
            output["willow-reconciliation-history/"+hashlib.sha256(previous).hexdigest()+".json"] = previous
        report = {"version": 1, "mode": "reconciled", "source": str(source), "roles": roles,
                  "native": native_info, "willow_files": hashes(files), "go_files": baseline,
                  "output_files": hashes(output), "required_go_runtime": runtime,
                  "independent_go_source": str(go_source) if go_source else None}
        destination = Path(destination).absolute()
        destination = destination.parent.resolve(strict=True)/destination.name
        for tree in (source, go_source):
            if tree is not None and (destination.is_relative_to(tree) or tree.is_relative_to(destination)):
                fail("destination must be NEW and disjoint from both input snapshots")
        if os.path.lexists(destination):
            fail("destination must be NEW")
        def unchanged():
            if hashes(inventory(source, retained=("rollback-go",))) != hashes(files):
                fail("Willow source changed during reconciliation; candidate must not be run")
            if go_source is not None and hashes(inventory(go_source, record.get("retained_snapshot_directories", ()))) != baseline:
                fail("Go source changed during reconciliation; candidate must not be run")
        unchanged()
        destination.mkdir(mode=0o700)
        for name, raw in output.items():
            write_private(destination/name, raw)
        unchanged()
        for directory in sorted([p for p in destination.rglob("*") if p.is_dir()], reverse=True)+[destination]:
            fd = os.open(directory, os.O_RDONLY|os.O_DIRECTORY)
            try:
                os.fsync(fd)
            finally:
                os.close(fd)
        write_private(destination/"willow-reconciliation.json", encoded(report))
        for directory in (destination, destination.parent):
            fd = os.open(directory, os.O_RDONLY|os.O_DIRECTORY)
            try:
                os.fsync(fd)
            finally:
                os.close(fd)
        return report


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--source", required=True, type=Path, help="complete stopped/frozen imported Willow tree")
    parser.add_argument("--destination", required=True, type=Path, help="NEW private Go candidate directory")
    parser.add_argument("--go-source", type=Path, help="stopped/frozen current Go tree; preserve newer independent Base state")
    args = parser.parse_args()
    try:
        report = reconcile(args.source, args.destination, args.go_source)
        print(json.dumps({"mode": report["mode"], "roles": report["roles"], "native": report["native"],
                          "required_go_runtime_keys": sorted(report["required_go_runtime"])}, indent=2))
    except (OSError, ValueError, KeyError, TypeError, IndexError, struct.error) as error:
        raise SystemExit(f"Willow reconciliation refused: {error}")


if __name__ == "__main__":
    main()
