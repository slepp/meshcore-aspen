#!/usr/bin/env python3
"""One-way, offline Birch -> Willow staging. Never opens a radio or starts Go."""
import argparse
import base64
import ctypes
import fcntl
import hashlib
import json
import os
from pathlib import Path
import stat
import struct
import ipaddress
import math
import zlib
from urllib.parse import urlsplit

from cryptography.hazmat.primitives.asymmetric.ed25519 import Ed25519PrivateKey
from cryptography.hazmat.primitives.serialization import Encoding, PublicFormat
from build_worker import WORKER, verify
import region_state

ROLE_FIELDS = set("""Version Retention PreferenceProfile Preferences Name AllowReadOnly MultiACKs
Latitude Longitude AdvertLocation RTCOffset HomeRegion NextRegionID DefaultRegion ManagedDefaultRegion
DiscoveryModified AdminPasswordOverride GuestPasswordOverride AdminPasswordBytes GuestPasswordBytes
Identity Room Clock Members MemberOrder History Posted Pushed""".split())
PREFERENCE_FIELDS = set("""version path_hash_mode rxdelay txdelay direct_txdelay airtime_factor repeat
local_advert_seconds flood_advert_seconds flood_max_hops unscoped_max_hops advert_max_hops loop
default_scope wildcard_flags regions owner_info""".split())
MEMBER_FIELDS = set("Key Permissions LastTimestamp SyncSince Path PathLength KnownPath Attempt".split())
BASE_FIELDS = set("""address port profile worker password admin width region channel run_ms native_airtime
bot_home bot_default bot_name channel_name native_setup imported room_public""".split())
ROLE_OPTIONS = set("""name password admin retention public evict home regions default_scope wildcard repeat loop flood_max unscoped_max
advert_max txdelay_milli direct_txdelay_milli local_advert_seconds flood_advert_seconds region_state preference_profile""".split())
COMPATIBILITY_ROLES = {"base","observer","companion","bot_companion"}
OBSERVER_FIELDS = set("""enabled url format client_id topic_prefix username password queue_size
iata origin model firmware_version radio audience ca_file packet_filter""".split())


def observer_config(values):
    enabled = values.get("observer.enabled", "0")
    if enabled not in ("0", "1"): fail("observer.enabled must be 0 or 1")
    format = values.get("observer.format", "internal-v1")
    if format not in ("internal-v1", "observer-v1", "capture-v1"):
        fail("observer.format must be internal-v1, observer-v1 or capture-v1")
    if format != "internal-v1":
        iata = values.get("observer.iata", "")
        if len(iata) != 3 or any(c < "A" or c > "Z" for c in iata):
            fail("observer.iata requires three uppercase ASCII letters")
    if not 1 <= int(values.get("observer.queue_size", "256")) <= 4096:
        fail("observer.queue_size must be 1..4096")
    mask = values.get("observer.packet_filter", "")
    if mask and (not mask.isdecimal() or not 0 <= int(mask) <= 65535):
        fail("observer.packet_filter must be an empty value or decimal uint16 mask")
    prefix = values.get("observer.topic_prefix", "meshcore")
    if any(c in prefix for c in "+#\0") or len(prefix.encode()) > 65000:
        fail("observer.topic_prefix is invalid")
    for field in ("client_id", "username", "password"):
        value = values.get("observer." + field, "")
        if len(value.encode()) > 65535 or (field != "password" and "\0" in value):
            fail("observer." + field + " exceeds MQTT wire limits")
    if values.get("observer.password") and not values.get("observer.username"):
        fail("observer.password requires observer.username")
    if values.get("observer.audience") and (format == "internal-v1" or
            values.get("observer.username") or values.get("observer.password")):
        fail("observer.audience requires observer-v1/capture-v1 and no static credentials")
    if enabled == "1":
        url = urlsplit(values.get("observer.url", ""))
        if url.scheme not in ("tcp", "tls", "ssl", "ws", "wss") or not url.hostname or url.username is not None or url.password is not None or url.query or url.fragment or (url.scheme not in ("ws", "wss") and url.path not in ("", "/")):
            fail("observer.url requires an explicit MQTT endpoint without URL credentials")
        if url.port is not None and not 1 <= url.port <= 65535:
            fail("observer.url port must be 1..65535")
        if url.scheme in ("tcp", "tls", "ssl") and url.port is None:
            fail("observer.url requires an explicit TCP/TLS port")
        if url.hostname not in ("localhost", "127.0.0.1", "::1", "mqtt-meshcore-1.ve6slp.ca"):
            fail("observer.url destination must be localhost or mqtt-meshcore-1.ve6slp.ca")
        ca = values.get("observer.ca_file", "")
        if ca and not Path(ca).is_absolute(): fail("observer.ca_file must be absolute")
    return enabled == "1"


def fail(message):
    raise ValueError(message)


def private(path, directory=False, max_bytes=32*1024*1024):
    path = Path(path)
    info = path.lstat()
    valid = stat.S_ISDIR(info.st_mode) if directory else stat.S_ISREG(info.st_mode)
    if not valid or info.st_uid != os.geteuid() or info.st_mode & 0o077:
        fail(f"{path}: expected an owned private {'directory' if directory else 'regular file'}")
    if not directory and (info.st_nlink != 1 or info.st_size > max_bytes):
        fail(f"{path}: multiple links or excessive file size")
    return info


def retained_directories(names):
    result=set(names)
    for name in result:
        if not name or name in (".","..") or "/" in name or "\\" in name or name.startswith(".") or name in COMPATIBILITY_ROLES|{"repeater","room","bot"}:
            fail("retain-directory must name a separate snapshot directory, not an active/compatibility role or path")
    return result


def inventory(root, retained=()):
    private(root, True)
    retained=retained_directories(retained)
    for name in retained:
        private(root/name,True)
    files = {}
    total = 0
    for path in sorted(root.rglob("*")):
        info = path.lstat()
        if stat.S_ISDIR(info.st_mode):
            private(path, True)
        else:
            top=path.relative_to(root).parts[0]
            private(path,max_bytes=(128 if top in retained else 32)*1024*1024)
            data = path.read_bytes()
            total += len(data)
            if total > 128*1024*1024:
                fail("snapshot exceeds the 128 MiB migration bound")
            files[str(path.relative_to(root))] = data
    return files


def hashes(files):
    return {name: hashlib.sha256(data).hexdigest() for name,data in sorted(files.items())}


def owner_ledger(data):
    if len(data) < 13 or data[:4] != b"OWN1":
        fail("owner request ledger: unsupported or truncated header")
    generation = int.from_bytes(data[4:12], "little")
    count = data[12]
    if not 1 <= generation < 0x7ffffffffffffffe or count > 128 or len(data) != 13+51*count:
        fail("owner request ledger: invalid generation, count or length")
    identifiers = set()
    for at in range(13, len(data), 51):
        identifier = data[at:at+16]
        phase, flags, reason = data[at+48:at+51]
        if identifier == bytes(16) or identifier in identifiers or phase > 6 or flags > 7 or reason > 8:
            fail("owner request ledger: invalid or duplicate request")
        identifiers.add(identifier)
    return data


def admin_clock(data):
    if (len(data) != 8 or data[:4] != b"ADM1"
            or not 0x40000000 <= int.from_bytes(data[4:], "little") <= 0x7fffffff):
        fail("owner admin request clock: invalid header, length or counter")
    return data


def document(raw, where):
    def unique(pairs):
        result = {}
        for key,value in pairs:
            if key in result: fail(f"{where}: duplicate JSON field {key}")
            result[key] = value
        return result
    value = json.loads(raw, object_pairs_hook=unique)
    if not isinstance(value, dict): fail(f"{where}: expected JSON object")
    return value


def known(value, fields, where):
    unknown = set(value)-fields
    if unknown: fail(f"{where}: unsupported fields {', '.join(sorted(unknown))}; source retained")


def u32(value, where):
    if type(value) is not int or not 0 <= value <= 0xffffffff:
        fail(f"{where}: exceeds Willow's unsigned 32-bit field")
    return value


def raw_bytes(value, size, where):
    try:
        result = base64.b64decode(value, validate=True) if isinstance(value,str) else bytes(value)
    except (ValueError, TypeError):
        fail(f"{where}: invalid byte encoding")
    if size is not None and len(result) != size: fail(f"{where}: expected {size} bytes")
    return result


def config_file(path):
    private(path)
    values = {}
    for line in Path(path).read_text().splitlines():
        if not line: continue
        key,sep,value = line.partition("=")
        if not sep or key in values: fail(f"configuration: malformed/duplicate setting {key}")
        if key not in BASE_FIELDS and not (key.startswith("observer.") and key[9:] in OBSERVER_FIELDS) and not any(
                key.startswith(role+".") and key[len(role)+1:] in ROLE_OPTIONS for role in ("relay","room")):
            fail(f"configuration: unsupported setting {key}")
        values[key] = value
    for field in ("address","port","profile","bot_home","bot_default"):
        if field not in values: fail(f"configuration: explicitly supply {field}, empty when intentionally disabled")
    if len(bytes.fromhex(values["profile"])) != 18: fail("configuration: profile must contain 18 bytes")
    if ipaddress.ip_address(values["address"]).version != 4 or not 1<=int(values["port"])<=65535:
        fail("configuration: numeric IPv4 address and port 1..65535 required")
    frequency,bandwidth,sf,cr,power,factor,cad,threshold=struct.unpack("<IIBBBfBh",bytes.fromhex(values["profile"]))
    if not 150000000<=frequency<=960000000 or bandwidth not in (7800,7810,10400,10420,15600,15630,20800,20830,31250,41700,62500,125000,250000,500000) or not 5<=sf<=12 or not 5<=cr<=8 or power>30 or not math.isfinite(factor) or factor<0 or cad>1:
        fail("configuration: invalid required shared-modem PHY readback")
    if values.get("width","3") != "3": fail("Willow transition requires three-byte ordinary paths")
    if values.get("worker",str(WORKER)) != str(WORKER): fail("configuration: use this worktree's verified native worker")
    if values.get("region",""): fail("configuration: use per-role regions/default_scope, not legacy region")
    values.update(worker=str(WORKER), width="3", native_setup="preserve", imported="1")
    observer_config(values)
    return values


def identity_public(material):
    if len(material) == 32:
        return Ed25519PrivateKey.from_private_bytes(material).public_key().public_bytes(Encoding.Raw,PublicFormat.Raw)
    if len(material) != 64 or material[0]&7 or material[31]&0xc0 != 0x40:
        fail("expanded identity requires 64 bytes with a clamped native scalar")
    sodium = ctypes.CDLL("libsodium.so.23")
    multiply = sodium.crypto_scalarmult_ed25519_base_noclamp
    multiply.argtypes = (ctypes.c_void_p, ctypes.c_void_p)
    multiply.restype = ctypes.c_int
    public = ctypes.create_string_buffer(32)
    if multiply(public, material[:32]) != 0 or public.raw[0] in (0, 255):
        fail("expanded identity has an invalid or reserved public key")
    return public.raw


def observer_authority(files):
    prefix = "observer/"
    state = files.get(prefix+"state.json")
    if prefix+"identity-state.json" in files:
        envelope = document(files[prefix+"identity-state.json"], "observer envelope")
        if envelope.get("version") != 1 or envelope.get("pending_identity") is not None:
            fail("observer: resolve pending/unsupported identity envelope before migration")
        if envelope.get("document") != "state.json":
            fail("observer: envelope document is not state.json")
        material = raw_bytes(envelope.get("identity"), 64, "observer active identity")
        state = None if envelope.get("state") is None else json.dumps(envelope["state"]).encode()
    elif prefix+"identity.expanded" in files:
        material = files[prefix+"identity.expanded"]
        if len(material) != 64:
            fail("observer: active expanded identity must contain 64 bytes")
    else:
        material = files.get(prefix+"identity.seed", b"")
        if len(material) != 32:
            fail("observer: no valid active identity envelope, expanded key or seed")
    return material, identity_public(material), state


def authority(files, role):
    if role == "observer":
        return observer_authority(files)
    prefix = role+"/"
    seed = files.get(prefix+"identity.seed")
    if seed is None or len(seed) != 32:
        fail(f"{role}: a matching 32-byte identity.seed is required; expanded-only identity migration is not yet available")
    expanded = bytearray(hashlib.sha512(seed).digest())
    expanded[0] &= 248
    expanded[31] = (expanded[31]&63)|64
    state = files.get(prefix+"state.json")
    if prefix+"identity-state.json" in files:
        envelope = document(files[prefix+"identity-state.json"],role+"/identity-state.json")
        known(envelope, {"version","identity","document","state","pending_identity"}, role+" envelope")
        if envelope.get("version") != 1 or envelope.get("pending_identity"):
            fail(f"{role}: resolve pending/unsupported identity envelope before migration")
        if raw_bytes(envelope.get("identity"),64,role+" identity") != expanded:
            fail(f"{role}: active envelope identity differs from identity.seed; do not fall back to the retained seed")
        if role != "bot" and envelope.get("document") != "state.json":
            fail(f"{role}: envelope document is not state.json")
        state = None if envelope.get("state") is None else json.dumps(envelope["state"]).encode()
    elif prefix+"identity.expanded" in files and files[prefix+"identity.expanded"] != expanded:
        fail(f"{role}: active expanded identity differs from identity.seed")
    public = Ed25519PrivateKey.from_private_bytes(seed).public_key().public_bytes(Encoding.Raw,PublicFormat.Raw)
    return seed,public,state


def region_key(region):
    known(region, {"id","parent","flags","name","keys"}, "region")
    name = region["name"]
    if name.startswith("$"):
        keys = region.get("keys") or []
        if len(keys) > 4: fail("private region: at most four keys are supported")
        return raw_bytes(keys[0],16,"private region") if keys else b""
    return hashlib.sha256((name if name.startswith("#") else "#"+name).encode()).digest()[:16]


def preserve_setting(config, key, value):
    value = str(value)
    if key in config and config[key] != value:
        fail(f"{key}: requested value differs from saved Birch behavior; migrate unchanged before reconfiguration")
    config[key] = value


def role_state(files, source_role, target_role, kind, public, config):
    _,_,raw = authority(files, source_role)
    if raw is None: fail(f"{source_role}: no active state document; supply a completed stopped-role snapshot")
    state = document(raw,source_role+"/state.json")
    known(state,ROLE_FIELDS,source_role)
    if state.get("Version") not in (1,2) or state.get("Identity") != public.hex() or state.get("Room") != (kind==3):
        fail(f"{source_role}: state identity/version/role mismatch")
    history=state.get("History") or []
    if len(history)>32: fail(f"{source_role}: more than 32 retained posts")
    if state.get("Retention") not in (None,"","native","durable-replay") or state.get("PreferenceProfile") not in (None,"","native-preferences","durable-host-preferences"):
        fail(f"{source_role}: unsupported retention/preference profile")
    latitude, longitude = state.get("Latitude", 0), state.get("Longitude", 0)
    location, rtc_offset = state.get("AdvertLocation", 0), state.get("RTCOffset", 0)
    if any(type(value) not in (int, float) or not math.isfinite(value) or abs(value) > limit
           for value, limit in ((latitude, 90), (longitude, 180))):
        fail(f"{source_role}: invalid saved coordinate")
    if type(location) is not int or location not in (0, 2):
        fail(f"{source_role}: invalid saved advert location mode")
    if type(rtc_offset) is not int or not -(1 << 63) <= rtc_offset < (1 << 63):
        fail(f"{source_role}: invalid signed clock offset")
    prefs = state.get("Preferences") or {}
    known(prefs,PREFERENCE_FIELDS,source_role+" preferences")
    if prefs.get("version") != 1 or type(prefs.get("path_hash_mode")) is not int or not 0 <= prefs["path_hash_mode"] <= 2:
        fail(f"{source_role}: require saved policy version 1 and path_hash_mode=0..2")
    rxdelay = prefs.get("rxdelay", 0)
    if type(rxdelay) not in (int, float) or not math.isfinite(rxdelay) or not 0 <= rxdelay <= 20:
        fail(f"{source_role}: invalid receive delay")
    factor = prefs.get("airtime_factor", 1)
    if type(factor) not in (int, float) or not math.isfinite(factor) or factor < 0 or factor > 3.4028234663852886e38:
        fail(f"{source_role}: invalid source airtime factor")
    preference_profile = state.get("PreferenceProfile") or "durable-host-preferences"
    preserve_setting(config, target_role+".preference_profile", preference_profile)
    if type(prefs.get("loop",0)) is not int or not 0<=prefs.get("loop",0)<=3:
        fail(f"{source_role}: invalid saved loop policy")
    owner_info = prefs.get("owner_info", "")
    if not isinstance(owner_info, str) or len(owner_info.encode()) > 119 or "\0" in owner_info:
        fail(f"{source_role}: invalid owner information")
    if type(state.get("AllowReadOnly", False)) is not bool or type(state.get("MultiACKs", 0)) is not int or not 0 <= state.get("MultiACKs", 0) <= 1:
        fail(f"{source_role}: unsupported read-only or multi-ACK setting")
    prefix=target_role+"."
    retention=state.get("Retention") or "native"
    if history and retention!="durable-replay":
        fail(f"{source_role}: retained history requires durable-replay; never discard it during migration")
    preserve_setting(config,prefix+"retention",retention)
    preserve_setting(config,prefix+"loop",prefs.get("loop",0))
    config.setdefault(prefix+"name",{"relay":"Willow-Relay","room":"Willow-Room"}[target_role])
    for target,override,raw_key,fallback in (
            ("password","GuestPasswordOverride","GuestPasswordBytes","password"),
            ("admin","AdminPasswordOverride","AdminPasswordBytes","admin")):
        if state.get(override) is not None and state.get(raw_key) is not None:
            fail(f"{source_role}: ambiguous saved {target}")
        value = state.get(override)
        if state.get(raw_key) is not None:
            value = raw_bytes(state[raw_key],None,raw_key).decode("utf-8")
        if value is not None: preserve_setting(config,prefix+target,value)
        elif prefix+target not in config:
            if fallback not in config: fail(f"{prefix+target}: supply original runtime credential through the private configuration file")
            config[prefix+target] = config[fallback]
    if kind==3:
        public_mode=config.get("room.public",config.get("room_public","0"))
        if public_mode not in ("0","1") or (config["room.password"]=="")!=(public_mode=="1"):
            fail("room.public=1 is required only for a deliberately empty room password")
        preserve_setting(config,"room.public",public_mode)
    for key,target in (("repeat","repeat"),("flood_max_hops","flood_max"),
                       ("unscoped_max_hops","unscoped_max"),("advert_max_hops","advert_max"),
                       ("local_advert_seconds","local_advert_seconds"),("flood_advert_seconds","flood_advert_seconds")):
        value=prefs.get(key,0)
        preserve_setting(config,prefix+target,int(value))
    for key in ("txdelay","direct_txdelay"):
        value=prefs.get(key,0)
        if type(value) not in (int, float) or not math.isfinite(value) or not 0 <= value <= 2:
            fail(f"{source_role}.{key}: invalid transmit delay")
        milli=round(value*1000)
        preserve_setting(config,prefix+key+"_milli",milli)
    table = region_state.from_go(state)
    regions = table["entries"]
    keys = [key.hex() for entry in regions if not entry.get("flags", 0)&1 for key in region_state.entry_keys(entry)]
    by_id = {entry["id"]: (region_state.entry_keys(entry) or [b""])[0] for entry in regions}
    home=state.get("HomeRegion",0)
    default=state.get("DefaultRegion",0)
    if state.get("ManagedDefaultRegion"):
        default_key=by_id.get(default,b"")
        entry = next((entry for entry in regions if entry["id"] == default), None)
        if entry and entry["name"].startswith("$") and (not default_key or not any(default_key)):
            fail(f"{source_role}: private default region has no usable transport key")
    else:
        default_scope=prefs.get("default_scope") or {}
        known(default_scope,{"name","key"},source_role+" default scope")
        default_key=raw_bytes(default_scope.get("key",[0]*16),16,"default scope")
        if not any(default_key): default_key=b""
    preserve_setting(config,prefix+"regions",",".join(keys))
    preserve_setting(config,prefix+"home",by_id.get(home,b"").hex())
    preserve_setting(config,prefix+"default_scope",default_key.hex())
    preserve_setting(config,prefix+"wildcard",int(not prefs.get("wildcard_flags",0)&1))
    preserve_setting(config,prefix+"region_state",region_state.encode(table).hex())
    members=state.get("Members") or {}
    if len(members)>20: fail(f"{source_role}: more than 20 members")
    order=state.get("MemberOrder") or []
    if len(order)!=len(set(order)) or any(key not in members for key in order): fail(f"{source_role}: invalid member order")
    order=order+sorted(set(members)-set(order))
    out=bytearray(b"HEW6"+public+struct.pack("<III",*(u32(state.get(k,0),source_role+"."+k) for k in ("Clock","Posted","Pushed")))+bytes([len(order),kind]))
    attempts=bytearray()
    for key in order:
        member=members[key]
        known(member,MEMBER_FIELDS,source_role+" member")
        peer=raw_bytes(member["Key"],32,"member key")
        if peer.hex()!=key: fail(f"{source_role}: member key mismatch")
        permission=member["Permissions"]
        attempt=member.get("Attempt",0)
        if type(permission) is not int or not 0<=permission<=255 or type(attempt) is not int or not 0<=attempt<=255:
            fail(f"{source_role}: invalid member permission/attempt byte")
        attempts.append(attempt)
        path=raw_bytes(member.get("Path") or [],None,"member path")
        encoded=member.get("PathLength",0)
        if not 0<=encoded<=191 or len(path)!=((encoded>>6)+1)*(encoded&63) or len(path)>64:
            fail(f"{source_role}: invalid learned path")
        out+=peer+bytes([permission])+struct.pack("<II",u32(member.get("LastTimestamp",0),"member stamp"),
                                                   u32(member.get("SyncSince",0),"member cursor"))
        out+=bytes([bool(member.get("KnownPath")),encoded,len(path)])+path
    out+=b"\0" # No all-Hew bot notes in relay/room snapshots.
    out+=bytes([retention=="durable-replay"])+attempts
    if retention=="durable-replay":
        out+=bytes([len(history)])
        previous=0
        for post in history:
            known(post,{"Author","Timestamp","Text","RawText"},source_role+" post")
            author=raw_bytes(post["Author"],32,"post author")
            stamp=u32(post["Timestamp"],"post timestamp")
            if post.get("RawText") is not None:
                if post.get("Text"): fail(f"{source_role}: ambiguous text/raw post")
                text=raw_bytes(post["RawText"],None,"raw post")
            else: text=post.get("Text","").encode("utf-8")
            if not previous<stamp<=state["Clock"] or not 1<=len(text)<=151 or b"\0" in text:
                fail(f"{source_role}: invalid retained post order, timestamp or text")
            out+=author+struct.pack("<IB",stamp,len(text))+text
            previous=stamp
    settings = bytearray(b"\2")
    for value, maximum in ((config[prefix+"name"], 31), (config[prefix+"password"], 63),
                           (config[prefix+"admin"], 63), (owner_info, 119)):
        raw = value.encode()
        if len(raw) > maximum or "\0" in value:
            fail(f"{source_role}: invalid persisted preference text")
        settings += struct.pack("<H", len(raw))+raw
    flags = int(prefs.get("repeat", False)) | (2 if state.get("AllowReadOnly", False) else 0)
    settings += struct.pack("<9I", prefs["path_hash_mode"]+1, flags, state.get("MultiACKs", 0),
        prefs.get("loop", 0), prefs.get("flood_max_hops", 0), prefs.get("unscoped_max_hops", 0),
        prefs.get("advert_max_hops", 0), prefs.get("local_advert_seconds", 0), prefs.get("flood_advert_seconds", 0))
    settings += struct.pack("<ff", prefs.get("txdelay", 0), prefs.get("direct_txdelay", 0))
    settings += struct.pack("<ddqB", latitude, longitude, rtc_offset, location)
    settings[0] = 3
    settings += struct.pack("<ffB", rxdelay, factor, int(preference_profile != "native-preferences"))
    metadata = region_state.encode(table)
    out += struct.pack("<H", len(settings))+settings+struct.pack("<H", len(metadata))+metadata
    return bytes(out), {"members":len(members),"regions":len(regions),"history":len(history),"retention":retention,
                       "preserved_attempts":sum(value!=0 for value in attempts)}


def native_records(raw):
    if len(raw)<28 or raw[:8]!=b"MCNVS\r\n\1": fail("native NVS: invalid header")
    version,names,count,length=struct.unpack_from("<IIII",raw,8)
    if version!=1 or names>64 or count>1024 or length!=len(raw)-28 or zlib.crc32(raw[:-4])!=int.from_bytes(raw[-4:],"little"):
        fail("native NVS: invalid bounds/checksum")
    at=24
    def name():
        nonlocal at
        size=raw[at]; at+=1
        value=raw[at:at+size].decode("ascii"); at+=size
        if not 1<=size<=15 or not all(c.isalnum() or c in "_-" for c in value): fail("native NVS: invalid name")
        return value
    spaces=[name() for _ in range(names)]
    if spaces!=sorted(set(spaces)): fail("native NVS: invalid namespace order")
    records={}
    for _ in range(count):
        key=(name(),name())
        size=struct.unpack_from("<I",raw,at)[0]; at+=4
        if key[0] not in spaces or key in records or size>32768 or at+size>len(raw)-4:
            fail("native NVS: invalid record")
        records[key]=raw[at:at+size]; at+=size
    if at!=len(raw)-4 or list(records)!=sorted(records): fail("native NVS: trailing/unordered records")
    return records


def native_state(files,config):
    copied={name[len("bot/native/"):]:raw for name,raw in files.items() if name.startswith("bot/native/")}
    if "nvs/nvs.snapshot" not in copied: fail("native bot: missing committed NVS snapshot")
    records=native_records(copied["nvs/nvs.snapshot"])
    radio=records.get(("mc-onchip","bot-radio"))
    if radio is None or (len(radio),radio[:4]) not in ((40,b"BRP\1"),(57,b"BRP\2")):
        fail("native bot: explicit saved bot-radio policy is required")
    if b"\0" not in radio[4:37] or (len(radio)==57 and radio[40] not in (0,1)):
        fail("native bot: invalid saved channel name or explicit-key flag")
    channel=radio[4:37].split(b"\0",1)[0].decode("ascii")
    if radio[37]!=3: fail("native bot: saved path width must be 3 before migration")
    airtime=int.from_bytes(radio[38:40],"little")
    if not 360<=airtime<=3600: fail("native bot: saved airtime budget must be 360..3600 ms/minute")
    key=radio[41:57] if len(radio)==57 and radio[40] else (
        hashlib.sha256(channel.encode()).digest()[:16] if channel else b"")
    preserve_setting(config,"channel",key.hex())
    if channel: preserve_setting(config,"channel_name",channel)
    preserve_setting(config,"native_airtime",airtime)
    if "scopes" in copied:
        scopes=copied["scopes"]
        if len(scopes)!=36 or scopes[:4]!=b"SCP1": fail("native bot: invalid scopes file")
        for label,raw in (("bot_home",scopes[4:20]),("bot_default",scopes[20:36])):
            preserve_setting(config,label,raw.hex() if any(raw) else "")
    for name,raw in copied.items():
        if name in ("scopes","nvs/nvs.snapshot"): continue
        if name=="host-name":
            if not raw.startswith(b"HNM1") or not 5<=len(raw)<=35: fail("native bot: invalid host-name file")
            continue
        if name.startswith("spiffs/spiffs."):
            if len(raw)<16 or raw[:8]!=b"MCFS\r\n\0\1" or int.from_bytes(raw[8:12],"little")!=len(raw)-16 or zlib.crc32(raw[16:])!=int.from_bytes(raw[12:16],"little"):
                fail(f"native bot {name}: invalid committed file checksum")
        else: fail(f"native bot {name}: unsupported or unfinished native file; complete recovery in Birch before freezing")
    return copied,{"nvs_records":len(records),"spiffs_files":sum(n.startswith("spiffs/") for n in copied)}


def prepare(files,config,retained=()):
    retained=retained_directories(retained)
    output={}
    report={"roles":{}}
    for name,raw in files.items():
        top,_,leaf=name.partition("/")
        if top in ("repeater","room","bot"):
            if top=="bot" and leaf.startswith("native/"): continue
            if leaf not in ("identity.seed","identity.expanded","identity-state.json","state.json","state.v1.json",".lock","packet.log"):
                fail(f"{name}: unsupported role data; no application files are silently discarded")
        elif top not in COMPATIBILITY_ROLES|retained|{"willow-reconciliation-history"} and name not in (".lock", "willow-reconciliation.json", "willow-owner.state", "willow-admin.clock"):
            fail(f"{top}: unsupported snapshot directory; use --retain-directory only for an identified inactive archive")
    if "willow-owner.state" in files:
        output["owner.state"] = owner_ledger(files["willow-owner.state"])
    if "willow-admin.clock" in files:
        output["admin.clock"] = admin_clock(files["willow-admin.clock"])
    publics=[]
    for source,target,kind in (("repeater","relay",2),("room","room",3),("bot","bot",1)):
        seed,public,state=authority(files,source)
        if public in publics: fail("role identity overlap in source snapshot")
        publics.append(public)
        output[target+".seed"]=seed
        info={"public_key":public.hex()}
        if kind!=1:
            output[target+".state"],details=role_state(files,source,target,kind,public,config)
            if source+"/packet.log" in files:
                log = files[source+"/packet.log"]
                if len(log) > 4194304: fail(f"{source}: packet log exceeds 4 MiB; archive it before migration")
                output[target+".state.packet.log"] = log
            info.update(details)
        elif state is not None and document(state,"bot/state.json"):
            fail("bot: nonempty Go bot state cannot be silently discarded; native NVS/SPIFFS must be its sole application store")
        report["roles"][target]=info
    observer_present = any(name.startswith("observer/") for name in files)
    if observer_present or config.get("observer.enabled") == "1":
        seed, public, state = authority(files, "observer")
        if public in publics: fail("observer: identity overlaps another role")
        if state is not None and document(state, "observer/state.json"):
            fail("observer: nonempty application state is unsupported; retained source is unchanged")
        identity_format = "expanded" if len(seed) == 64 else "seed"
        output["observer."+identity_format] = seed
        report["roles"]["observer"] = {"public_key": public.hex(), "identity_format": identity_format,
                                      "enabled": config.get("observer.enabled") == "1"}
    native,details=native_state(files,config)
    output.update({"native/"+name:raw for name,raw in native.items()})
    report["roles"]["bot"].update(details)
    config.setdefault("bot_name","Willow-Bot")
    if "host-name" in native:
        # Naming is the only deliberate migration override; the original stays
        # in rollback-go and the input fingerprint manifest.
        output["native/host-name"]=b"HNM1"+config["bot_name"].encode()
    for key,value in config.items():
        if "\n" in value or "\r" in value or "\0" in value: fail(f"{key}: control bytes cannot be represented in Willow configuration")
        suffix=key.split(".")[-1]
        if suffix == "preference_profile" and value not in ("native-preferences", "durable-host-preferences"):
            fail(f"{key}: invalid preference profile")
        if suffix in ("name","bot_name","channel_name"):
            maximum=32 if suffix=="channel_name" else 31
            if not 1<=len(value.encode())<=maximum or any(ord(c)<32 or ord(c)>126 for c in value) or (suffix=="bot_name" and ":" in value):
                fail(f"{key}: invalid name")
        if not key.startswith("observer.") and suffix in ("password","admin") and len(value.encode())>63: fail(f"{key}: credential exceeds 63 bytes")
        limits={"loop":3,"flood_max":64,"unscoped_max":64,"advert_max":64,"txdelay_milli":2000,"direct_txdelay_milli":2000,
                "local_advert_seconds":86400,"flood_advert_seconds":604800}
        if suffix in limits and not 0<=int(value)<=limits[suffix]: fail(f"{key}: unsupported range")
        if key in ("bot_home","bot_default") and len(bytes.fromhex(value)) not in (0,16): fail(f"{key}: expected empty or 16-byte key")
    output["config"]="".join(key+"="+value+"\n" for key,value in sorted(config.items())).encode()
    report["retained_compatibility_roles"]=sorted({n.split("/")[0] for n in files if "/" in n}&COMPATIBILITY_ROLES)
    if config.get("observer.enabled") == "1":
        report["retained_compatibility_roles"]=[role for role in report["retained_compatibility_roles"] if role != "observer"]
    report["retained_snapshot_directories"]=sorted(retained)
    report["native_setup"]="preserve: single worker, name-only pre-start override; channel/path/airtime/grants/source/data retained"
    return output,report


def write_private(path,data):
    missing=[]
    parent=path.parent
    while not parent.exists():
        missing.append(parent)
        parent=parent.parent
    private(parent,True)
    for directory in reversed(missing): directory.mkdir(mode=0o700)
    fd=os.open(path,os.O_WRONLY|os.O_CREAT|os.O_EXCL|os.O_NOFOLLOW,0o600)
    with os.fdopen(fd,"wb") as out:
        out.write(data); out.flush(); os.fsync(out.fileno())


def migrate(source,configuration=None,destination=None,retained=(),schema_only=False):
    if schema_only and (configuration is not None or destination is not None):
        fail("schema-only inspection cannot use runtime configuration or create a destination")
    if not schema_only and configuration is None:
        fail("supply --config for runtime inspection or staging; use --schema-only without credentials")
    source=Path(source).absolute()
    private(source,True)
    source=source.resolve(strict=True)
    lock=None
    lock_path=source/".lock"
    if lock_path.exists():
        private(lock_path)
        lock=lock_path.open("rb")
        try: fcntl.flock(lock,fcntl.LOCK_EX|fcntl.LOCK_NB)
        except BlockingIOError:
            lock.close()
            fail("Birch snapshot is locked by a running host; stop/freeze it before migration")
    try:
        worker=verify()
        files=inventory(source,retained)
        if schema_only:
            # These in-memory sentinels can never reach a staged service.
            config={"password":"SCHEMA-ONLY-NOT-A-RUNTIME-CREDENTIAL",
                    "admin":"SCHEMA-ONLY-NOT-A-RUNTIME-CREDENTIAL","bot_home":"","bot_default":""}
            scopes=files.get("bot/native/scopes",b"")
            if len(scopes)==36 and scopes[:4]==b"SCP1":
                for name,key in (("bot_home",scopes[4:20]),("bot_default",scopes[20:36])):
                    config[name]=key.hex() if any(key) else ""
            _,_,room_raw=authority(files,"room")
            room=document(room_raw,"room/state.json") if room_raw is not None else {}
            if room.get("GuestPasswordOverride") == "" or room.get("GuestPasswordBytes") in ("",[]):
                config["room.public"]="1"
        else:
            config=config_file(configuration)
        output,report=prepare(files,config,retained)
        report.update(version=1,source=str(source),source_files=hashes(files),worker_sha256=worker["worker_sha256"],
                      output_files=hashes(output),mode="schema-inspection" if schema_only else "inspection" if destination is None else "staged")
        if schema_only:
            report["runtime_configuration"]="not checked; private --config required before staging"
        if hashes(inventory(source,retained))!=report["source_files"]: fail("source changed during inspection; take a frozen snapshot")
        if destination is None: return report
        destination=Path(destination).absolute()
        destination=destination.parent.resolve(strict=True)/destination.name
        if os.path.lexists(destination) or destination.is_relative_to(source) or source.is_relative_to(destination):
            fail("destination must be NEW and disjoint from the source snapshot")
        encoded=json.dumps(report,indent=2,sort_keys=True).encode()+b"\n"
        if len(encoded)>65536: fail("migration record exceeds the 64 KiB service-read limit")
        destination.mkdir(mode=0o700)
        try:
            for name,raw in output.items(): write_private(destination/name,raw)
            for name,raw in files.items(): write_private(destination/"rollback-go"/name,raw)
            (destination/"native/nvs").mkdir(mode=0o700,exist_ok=True)
            (destination/"native/spiffs").mkdir(mode=0o700,exist_ok=True)
            if hashes(inventory(source,retained))!=report["source_files"]: fail("source changed while staging; destination is incomplete and must not be run")
            for directory in sorted([p for p in destination.rglob("*") if p.is_dir()],reverse=True)+[destination]:
                fd=os.open(directory,os.O_RDONLY|os.O_DIRECTORY)
                try: os.fsync(fd)
                finally: os.close(fd)
            write_private(destination/"migration.json",encoded)
            for directory in (destination,destination.parent):
                fd=os.open(directory,os.O_RDONLY|os.O_DIRECTORY)
                try: os.fsync(fd)
                finally: os.close(fd)
        except Exception:
            # A partially written NEW destination is left private for inspection,
            # and is never silently reused or promoted.
            raise
        return report
    finally:
        if lock: lock.close()


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--source",required=True,type=Path,help="private stopped/frozen Birch state copy")
    parser.add_argument("--config",type=Path,help="private Willow runtime configuration; credentials stay in this file")
    parser.add_argument("--schema-only",action="store_true",help="validate saved state without runtime credentials; cannot stage or start")
    parser.add_argument("--retain-directory",action="append",default=[],help="identified inactive snapshot directory to preserve only in rollback-go; repeatable")
    parser.add_argument("--destination",type=Path,help="NEW private destination; omit for inspection/dry-run")
    args=parser.parse_args()
    try:
        report=migrate(args.source,args.config,args.destination,args.retain_directory,args.schema_only)
        safe={key:report[key] for key in ("mode","roles","retained_compatibility_roles","retained_snapshot_directories","native_setup","worker_sha256")}
        if args.schema_only:
            safe["roles"]={name:{key:value for key,value in info.items() if key!="public_key"}|{"identity_valid":True}
                           for name,info in report["roles"].items()}
            safe["runtime_configuration"]=report["runtime_configuration"]
        safe["source_fingerprint"]=hashlib.sha256(json.dumps(report["source_files"],sort_keys=True).encode()).hexdigest()
        print(json.dumps(safe,indent=2,sort_keys=True))
    except (ValueError,OSError,KeyError,TypeError,IndexError,struct.error) as error:
        raise SystemExit(f"Willow migration refused: {error}")


if __name__=="__main__": main()
