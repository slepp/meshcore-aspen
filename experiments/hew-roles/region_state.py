"""Named-region records shared by offline staging and Go reconciliation."""
import copy
import hashlib
import struct


def key(value):
    if isinstance(value, list):
        if any(type(byte) is not int or not 0 <= byte <= 255 for byte in value):
            raise ValueError("region key contains an invalid byte")
        value = bytes(value)
    if not isinstance(value, bytes) or len(value) != 16:
        raise ValueError("region key must contain 16 bytes")
    return value


def normalized(name):
    return name[1:] if name.startswith("#") else name


def entry_keys(entry):
    if entry["name"].startswith("$"):
        return [key(value) for value in entry.get("keys") or []]
    name = entry["name"]
    return [hashlib.sha256((name if name.startswith("#") else "#"+name).encode()).digest()[:16]]


def validate(table):
    entries = table["entries"]
    if not isinstance(entries, list) or len(entries) > 32:
        raise ValueError("region table exceeds native capacity 32")
    for field, maximum in (("wildcard", 255), ("home", 65535), ("next", 65536),
                           ("default_id", 65535), ("discovery", 0xffffffff)):
        if type(table[field]) is not int or not 0 <= table[field] <= maximum:
            raise ValueError("invalid region metadata")
    if table["next"] == 0 or type(table["managed"]) is not bool:
        raise ValueError("invalid region metadata")
    if not isinstance(table["default_name"], str) or len(table["default_name"].encode()) > 30 or "\0" in table["default_name"]:
        raise ValueError("default scope name must be at most 30 bytes without NUL")
    for field in ("default_key", "legacy_home"):
        if table[field] != b"":
            key(table[field])
    if len(table["legacy_keys"]) > 128:
        raise ValueError("too many legacy region keys")
    for raw in table["legacy_keys"]:
        key(raw)
    identifiers, names = {}, set()
    for entry in entries:
        if set(entry)-{"id", "parent", "flags", "name", "keys"}:
            raise ValueError("unsupported region field")
        for field, minimum, maximum in (("id", 1, 65535), ("parent", 0, 65535), ("flags", 0, 255)):
            value = entry.get(field, 0)
            if type(value) is not int or not minimum <= value <= maximum:
                raise ValueError("invalid region identity, name or key count")
        name = entry["name"]
        if not isinstance(name, str) or not 1 <= len(name.encode()) <= 30 or len(entry.get("keys") or []) > 4:
            raise ValueError("invalid region identity, name or key count")
        if any(not (byte in (35, 36, 45) or 48 <= byte <= 57 or byte >= 65) for byte in name.encode()):
            raise ValueError("invalid native region name character")
        for raw in entry.get("keys") or []:
            key(raw)
        if entry["id"] in identifiers or normalized(name) in names:
            raise ValueError("duplicate region identity or name")
        identifiers[entry["id"]] = entry
        names.add(normalized(name))
    for entry in entries:
        parent, seen = entry.get("parent", 0), {entry["id"]}
        while parent:
            if parent not in identifiers or parent in seen:
                raise ValueError("region parent is missing or cyclic")
            seen.add(parent)
            parent = identifiers[parent].get("parent", 0)
    return table


def from_go(state):
    preferences = state["Preferences"]
    entries = copy.deepcopy(preferences.get("regions") or [])
    for entry in entries:
        entry.setdefault("parent", 0)
        entry.setdefault("flags", 0)
        entry["keys"] = [key(value) for value in entry.get("keys") or []]
    default = preferences.get("default_scope") or {}
    if set(default)-{"name", "key"}:
        raise ValueError("unsupported default scope field")
    raw = key(default.get("key", [0]*16))
    return validate({
        "entries": entries, "wildcard": preferences.get("wildcard_flags", 0),
        "home": state.get("HomeRegion", 0), "next": max(state.get("NextRegionID", 1) or 1,
            max((entry["id"] for entry in entries), default=0)+1),
        "default_id": state.get("DefaultRegion", 0), "managed": state.get("ManagedDefaultRegion", False),
        "default_name": default.get("name", ""), "default_key": raw if any(raw) else b"",
        "discovery": state.get("DiscoveryModified", 0), "legacy_keys": [], "legacy_home": b"",
    })


def encode(table):
    validate(table)
    out = bytearray(b"NGR1"+struct.pack("<BHIHBI", table["wildcard"], table["home"],
        table["next"], table["default_id"], table["managed"], table["discovery"]))
    name = table["default_name"].encode()
    out += bytes([len(name)])+name+(table["default_key"] or bytes(16))+(table["legacy_home"] or bytes(16))
    out += bytes([len(table["legacy_keys"])])+b"".join(table["legacy_keys"])+bytes([len(table["entries"])])
    for entry in table["entries"]:
        name = entry["name"].encode()
        keys = [key(raw) for raw in entry.get("keys") or []]
        out += struct.pack("<HHBB", entry["id"], entry.get("parent", 0), entry.get("flags", 0), len(name))
        out += name+bytes([len(keys)])+b"".join(keys)
    return bytes(out)


def decode(data):
    if not isinstance(data, bytes) or not 53 <= len(data) <= 8192:
        raise ValueError("invalid region record size")
    at = 0

    def take(size):
        nonlocal at
        if size < 0 or at+size > len(data):
            raise ValueError("truncated region record")
        value = data[at:at+size]
        at += size
        return value

    def number(size=1):
        return int.from_bytes(take(size), "little")

    if take(4) != b"NGR1":
        raise ValueError("unknown region record version")
    table = dict(zip(("wildcard", "home", "next", "default_id", "managed", "discovery"),
                     (number(size) for size in (1, 2, 4, 2, 1, 4))))
    if table["managed"] > 1:
        raise ValueError("invalid region managed-default flag")
    table["managed"] = bool(table["managed"])
    table["default_name"] = take(number()).decode("utf-8")
    for field in ("default_key", "legacy_home"):
        raw = take(16)
        table[field] = raw if any(raw) else b""
    table["legacy_keys"] = [take(16) for _ in range(number())]
    table["entries"] = []
    for _ in range(number()):
        entry = {"id": number(2), "parent": number(2), "flags": number(), "name": take(number()).decode("utf-8")}
        entry["keys"] = [take(16) for _ in range(number())]
        table["entries"].append(entry)
    if at != len(data):
        raise ValueError("unknown trailing region record data")
    return validate(table)


def apply(state, table):
    """Replace committed named metadata; resolve only retained legacy raw keys."""
    validate(table)
    preferences = state["Preferences"]
    entries = copy.deepcopy(table["entries"])
    next_id = table["next"]

    def locate(raw):
        nonlocal next_id
        for entry in entries:
            if raw in entry_keys(entry):
                return entry
        if len(entries) >= 32 or next_id > 65535:
            raise ValueError("Go region capacity exhausted")
        name = "$willow-"+raw.hex()[:16]
        if any(entry["name"] == name for entry in entries):
            raise ValueError("generated region name collides with retained name")
        entry = {"id": next_id, "parent": 0, "flags": 1, "name": name, "keys": [raw]}
        entries.append(entry)
        next_id += 1
        return entry

    for raw in table["legacy_keys"]:
        entry = locate(raw)
        entry["flags"] &= ~1
    home = table["home"]
    if table["legacy_home"]:
        home = locate(table["legacy_home"])["id"]
    for entry in entries:
        entry["keys"] = [list(key(raw)) for raw in entry.get("keys") or []]
        if not entry["keys"]:
            entry.pop("keys")
    preferences["regions"] = entries
    preferences["wildcard_flags"] = table["wildcard"]
    preferences["default_scope"] = {"name": table["default_name"], "key": list(table["default_key"] or bytes(16))}
    state.update(HomeRegion=home, NextRegionID=next_id, DefaultRegion=table["default_id"],
                 ManagedDefaultRegion=table["managed"], DiscoveryModified=table["discovery"])


def configured(table, values, prefix):
    """Apply edited raw aliases without replacing later named-region writes."""
    if prefix+"region_state" not in values:
        return table
    baseline = decode(bytes.fromhex(values[prefix+"region_state"]))
    if baseline["managed"]:
        entry = next((entry for entry in baseline["entries"] if entry["id"] == baseline["default_id"]), None)
        available = entry_keys(entry) if entry else []
        baseline["default_key"] = available[0] if available else b""
    home_entry = next((entry for entry in baseline["entries"] if entry["id"] == baseline["home"]), None)
    home_keys = entry_keys(home_entry) if home_entry else []
    home_key = home_keys[0] if home_keys else baseline["legacy_home"]
    permitted = [raw for entry in baseline["entries"] if not entry.get("flags", 0)&1 for raw in entry_keys(entry)]
    permitted += baseline["legacy_keys"]
    updated = copy.deepcopy(table)

    def locate(raw):
        for entry in updated["entries"]:
            if raw in entry_keys(entry):
                return entry
        if len(updated["entries"]) >= 32 or updated["next"] > 65535:
            raise ValueError("region table full")
        name = "$willow-"+raw.hex()[:16]
        if any(entry["name"] == name for entry in updated["entries"]):
            raise ValueError("generated region name collides with retained name")
        entry = {"id": updated["next"], "parent": 0, "flags": 1, "name": name, "keys": [key(raw)]}
        updated["entries"].append(entry)
        updated["next"] += 1
        return entry

    if prefix+"regions" in values:
        wanted = [key(bytes.fromhex(text)) for text in values[prefix+"regions"].split(",")] if values[prefix+"regions"] else []
        if wanted != permitted:
            entries = []
            for raw in wanted:
                entry = locate(raw)
                if entry not in entries:
                    entries.append(entry)
            denied = [entry for entry in updated["entries"] if entry not in entries]
            for entry in entries:
                entry["flags"] &= ~1
            for entry in denied:
                entry["flags"] |= 1
            updated["entries"] = entries+denied
            updated["legacy_keys"] = []
    for setting, original in (("home", home_key), ("default_scope", baseline["default_key"])):
        if prefix+setting not in values:
            continue
        raw = bytes.fromhex(values[prefix+setting])
        if raw:
            key(raw)
        if raw == original:
            continue
        entry = locate(raw) if raw else None
        if setting == "home":
            updated["home"] = entry["id"] if entry else 0
            updated["legacy_home"] = b""
        else:
            updated["default_id"] = entry["id"] if entry else 0
            updated["default_name"] = entry["name"] if entry else ""
            updated["default_key"] = raw
            updated["managed"] = not entry or raw == entry_keys(entry)[0]
    if prefix+"wildcard" in values:
        value = values[prefix+"wildcard"]
        if value not in ("0", "1"):
            raise ValueError("wildcard must be 0 or 1")
        allowed = value == "1"
        if allowed != (baseline["wildcard"]&1 == 0):
            updated["wildcard"] = (updated["wildcard"]&~1) | (0 if allowed else 1)
    return validate(updated)
