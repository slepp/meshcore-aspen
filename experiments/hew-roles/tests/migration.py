"""Actual Go state + native Lua/Wasm records -> isolated Willow KISS service."""
import copy
import base64
import fcntl
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess
import struct
import sys
import time
import unittest

sys.path.insert(0,str(Path(__file__).resolve().parents[1]))
from migrate_go import migrate, inventory, hashes, native_records
from willow import check
from parity import ROOT, Host, public, packet, tx
from service_demo import RunningService, Emulator, PROFILE, advert, text, body, parse, login, scoped, scope_code


def snapshot_tail(raw):
    if raw[:4] not in (b"HEW2",b"HEW3",b"HEW4",b"HEW5",b"HEW6"): raise AssertionError("snapshot version missing")
    at=50
    keys=[]
    for _ in range(raw[48]):
        keys.append(raw[at:at+32])
        at+=44+raw[at+43]
    notes=raw[at]; at+=1
    for _ in range(notes):
        at+=32
        size=raw[at]; at+=1+size
        size=raw[at]; at+=1+size
    with_history=raw[:4]==b"HEW3"
    attempts=dict.fromkeys(keys,0)
    if raw[:4] in (b"HEW4",b"HEW5",b"HEW6"):
        with_history=raw[at]==1
        at+=1
        attempts=dict(zip(keys,raw[at:at+len(keys)]))
        at+=len(keys)
    return at,with_history,attempts


def snapshot_history(raw):
    at,with_history,_=snapshot_tail(raw)
    if not with_history: raise AssertionError("durable history snapshot flag missing")
    count=raw[at]; at+=1
    posts=[]
    for _ in range(count):
        author=raw[at:at+32]; stamp,size=struct.unpack_from("<IB",raw,at+32); at+=37
        posts.append((author,stamp,raw[at:at+size])); at+=size
    if at!=len(raw):
        from reconcile_go import snapshot
        snapshot(raw,raw[4:36],raw[49],"migration history fixture")
    return posts


class Migration(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.root=ROOT.parents[1]/f".m-{os.getpid()}"
        cls.root.mkdir(mode=0o700)
        cls.addClassCleanup(shutil.rmtree,cls.root)
        cls.source=cls.root/"go-source"
        cls.history_oracle=cls.root/"go-history-frames.json"
        env=os.environ|{"TMPDIR":str(ROOT/"build"),"MESHCORE_WILLOW_FIXTURE":str(cls.source),
                       "MESHCORE_WILLOW_HISTORY_ORACLE":str(cls.history_oracle)}
        subprocess.run(["go","test","../../internal/roles","-run","^TestWillowMigrationFixture$","-count=1"],
                       cwd=ROOT,env=env,check=True,capture_output=True)
        with RunningService(wasm=True) as service:
            modem=service.emulator
            modem.send(2,0,advert(2)); time.sleep(.15)
            modem.send(2,0,text(2,5,1700000800,"!remember migration native data"))
            reply=modem.receive(lambda j:j["port"]==2 and parse(j["raw"])[0]==2)
            if b"committed" not in body(reply["raw"],2,5): raise AssertionError("fixture note was not committed")
            service.stop_process()
            shutil.copytree(service.root/"native",cls.source/"bot/native")
        cls.before=hashes(inventory(cls.source))
        cls.settings=cls.root/"settings"
        cls.write_config(cls.settings,1)
    @classmethod
    def write_config(cls,path,port):
        path.write_text(f"address=127.0.0.1\nport={port}\nprofile={PROFILE.hex()}\npassword=room\nadmin=admin\n"
                        "bot_home=\nbot_default=\nbot_name=Willow-Imported\n")
        path.chmod(0o600)
    def test_dry_run_and_exact_state_conversion(self):
        report=migrate(self.source,self.settings)
        self.assertEqual(report["mode"],"inspection")
        self.assertEqual(report["roles"]["room"]["members"],8)
        self.assertEqual(report["roles"]["room"]["history"],12)
        self.assertEqual(report["roles"]["relay"]["members"],4)
        self.assertEqual(report["roles"]["relay"]["regions"],2)
        self.assertEqual(hashes(inventory(self.source)),self.before)
        destination=self.root/"converted"
        migrate(self.source,self.settings,destination)
        self.assertEqual(destination.stat().st_mode&0o777,0o700)
        config=check(destination)
        self.assertEqual(config["relay.home"],hashlib.sha256(b"#fixture-a").digest()[:16].hex())
        self.assertEqual(config["relay.default_scope"],"")
        self.assertEqual(config["native_setup"],"preserve")
        self.assertEqual(config["native_airtime"],"3600")
        self.assertEqual(config["room.retention"],"durable-replay")
        self.assertEqual(config["relay.loop"],"3")
        for role,kind,seed in (("relay",2,1),("room",3,4)):
            self.assertEqual((destination/(role+".seed")).read_bytes(),bytes([seed])*32)
            with Host(role=kind,seed=(bytes([seed])*32).hex(),expected=public(seed).hex()) as host:
                self.assertEqual(host.command("load "+str(destination/(role+".state"))),["true"])
                cursor=host.command("cursor "+public(2).hex())[0]
                for expected in ("permission="+("130" if kind==3 else "3"),"stamp=101","since=99","known=true"):
                    self.assertIn(expected,cursor)
                saved=json.loads((self.source/("room" if kind==3 else "repeater")/"state.json").read_text())
                attempts={bytes(member["Key"]):member["Attempt"] for member in saved["Members"].values()}
                self.assertEqual(snapshot_tail((destination/(role+".state")).read_bytes())[2],attempts)
                roundtrip=destination/(role+"-roundtrip")
                self.assertEqual(host.command("save "+str(roundtrip)),["true"])
                self.assertEqual(snapshot_tail(roundtrip.read_bytes())[2],attempts)
                for key,member in saved["Members"].items():
                    cursor=host.command("cursor "+key)[0]
                    for field,value in (("permission",member["Permissions"]),("stamp",member["LastTimestamp"]),("since",member["SyncSince"])):
                        self.assertIn(f"{field}={value}",cursor)
        self.assertEqual(hashes(inventory(destination/"rollback-go")),self.before)
        self.assertEqual(hashes(inventory(self.source)),self.before)
        with self.assertRaisesRegex(ValueError,"NEW"):
            migrate(self.source,self.settings,destination)

    def test_packet_log_byte_preservation_append_and_erasure(self):
        from reconcile_go import reconcile
        source = self.root/"packet-log-source"
        shutil.copytree(self.source, source)
        original = b'{"direction":"RX","raw_packet_hex":"1200"}\n'
        (source/"room/packet.log").write_bytes(original)
        (source/"room/packet.log").chmod(0o600)
        before = hashes(inventory(source))
        destination = self.root/"packet-log-imported"
        migrate(source, self.settings, destination)
        log = destination/"room.state.packet.log"
        self.assertEqual(log.read_bytes(), original)
        self.assertEqual(log.stat().st_mode&0o777, 0o600)
        returned = self.root/"packet-log-returned"
        reconcile(destination, returned)
        self.assertEqual((returned/"room/packet.log").read_bytes(), original)
        log.write_bytes(original+b'{"direction":"TX","raw_packet_hex":"1201"}\n')
        updated = self.root/"packet-log-updated"
        reconcile(destination, updated)
        self.assertEqual((updated/"room/packet.log").read_bytes(), log.read_bytes())
        log.unlink()
        erased = self.root/"packet-log-erased"
        reconcile(destination, erased)
        self.assertFalse((erased/"room/packet.log").exists())
        self.assertEqual(hashes(inventory(source)), before)

    def test_observer_identity_and_runtime_config(self):
        source = self.root/"observer-source"
        shutil.copytree(self.source, source)
        (source/"observer").mkdir(mode=0o700)
        seed = source/"observer/identity.seed"
        seed.write_bytes(bytes([3])*32)
        seed.chmod(0o600)
        (source/"base").mkdir(mode=0o700)
        base = source/"base/identity.seed"
        base.write_bytes(bytes([6])*32)
        base.chmod(0o600)
        before = hashes(inventory(source))
        settings = self.root/"observer-settings"
        self.write_config(settings, 1)
        with settings.open("a") as out:
            out.write("observer.enabled=1\nobserver.url=tcp://127.0.0.1:1883\n"
                      "observer.format=capture-v1\nobserver.iata=YYC\n"
                      "observer.topic_prefix=willow-local\nobserver.packet_filter=65535\nobserver.username=fixture\n"
                      "observer.password=" + "fixture-"*10 + "\n")
        destination = self.root/"observer-converted"
        report = migrate(source, settings, destination)
        self.assertEqual(check(destination)["observer.packet_filter"], "65535")
        self.assertEqual(report["roles"]["observer"]["public_key"], public(3).hex())
        self.assertNotIn("observer", report["retained_compatibility_roles"])
        self.assertIn("base", report["retained_compatibility_roles"])
        self.assertEqual((destination/"observer.seed").read_bytes(), seed.read_bytes())
        self.assertEqual(hashes(inventory(source)), before)
        self.assertEqual(hashes(inventory(destination/"rollback-go")), before)
        (destination/"observer.seed").write_bytes(bytes([7])*32)
        with self.assertRaisesRegex(ValueError, "observer: imported identity changed"):
            check(destination)
        (source/"observer/identity.expanded").write_bytes(bytes(64))
        (source/"observer/identity.expanded").chmod(0o600)
        with self.assertRaisesRegex(ValueError, "clamped native scalar"):
            migrate(source, settings)
        expanded = bytearray(hashlib.sha512(bytes([7])*32).digest())
        expanded[0] &= 248
        expanded[31] = (expanded[31]&63)|64
        (source/"observer/identity.expanded").write_bytes(expanded)
        destination = self.root/"observer-expanded"
        report = migrate(source, settings, destination)
        self.assertEqual(report["roles"]["observer"]["public_key"], public(7).hex())
        self.assertEqual(report["roles"]["observer"]["identity_format"], "expanded")
        self.assertFalse((destination/"observer.seed").exists())
        self.assertEqual((destination/"observer.expanded").read_bytes(), expanded)
        check(destination)
        from reconcile_go import reconcile
        returned = self.root/"observer-expanded-returned"
        reconcile(destination, returned)
        self.assertEqual((returned/"observer/identity.expanded").read_bytes(), expanded)
        self.assertEqual((returned/"observer/identity.seed").read_bytes(), seed.read_bytes())

    def test_rejects_unsupported_or_mismatched_authority(self):
        for label,change in (
                ("history",lambda state:state.update(History=[{"Author":[2]*32,"Timestamp":1,"Text":"retained"}]*33)),
                ("history-order",lambda state:state["History"][1].update(Timestamp=81)),
                ("history-clock",lambda state:state["History"][-1].update(Timestamp=103)),
                ("history-nul",lambda state:state["History"][0].update(Text="bad\0post")),
                ("history-ambiguous",lambda state:state["History"][2].update(Text="ambiguous")),
                ("location",lambda state:state.update(Latitude=91.0)),
                ("unknown",lambda state:state.update(FutureNonempty={"value":1}))):
            source=self.root/("reject-"+label)
            shutil.copytree(self.source,source)
            path=source/"room/state.json"
            state=json.loads(path.read_text()); change(state)
            path.write_text(json.dumps(state))
            before=hashes(inventory(source))
            destination=self.root/("not-created-"+label)
            with self.assertRaises(ValueError): migrate(source,self.settings,destination)
            self.assertFalse(destination.exists())
            self.assertEqual(hashes(inventory(source)),before)
        source=self.root/"reject-expanded"
        shutil.copytree(self.source,source)
        key=source/"bot/identity.expanded"; key.write_bytes(bytes(64)); key.chmod(0o600)
        with self.assertRaisesRegex(ValueError,"active expanded identity differs"):
            migrate(source,self.settings)

    def test_imported_service_preserves_native_data_and_names(self):
        destination=self.root/"running"
        service=RunningService.__new__(RunningService)
        service.root=destination
        service.emulator=Emulator(); service.emulator.identity_root=destination
        service.logs=[]; service.errors=[]
        settings=self.root/"running-settings"
        self.write_config(settings,service.emulator.port)
        migrate(self.source,settings,destination)
        check(destination)
        original=inventory(destination/"native")
        original_records=native_records(original["nvs/nvs.snapshot"])
        try:
            service.start_process(True,runner=True)
            self.assertTrue((destination/"willow.started").exists())
            modem=service.emulator
            adverts={}
            while len(adverts)<3:
                packet=modem.receive(lambda j:parse(j["raw"])[0]==4)
                adverts[packet["port"]]=packet["raw"]
            for port,name in ((0,b"Willow-Relay"),(1,b"Willow-Room"),(2,b"Willow-Imported")):
                self.assertEqual(parse(adverts[port])[2],128)
                self.assertTrue(parse(adverts[port])[4].endswith(name))
                self.assertEqual(parse(adverts[port])[1],1) # home does not become default
            modem.send(2,0,advert(2)); time.sleep(.15)
            for stamp,command,expected in ((1700000810,"!recall migration",b"native data"),
                                           (1700000811,"!wadd 17 25",b"42")):
                modem.send(2,0,text(2,5,stamp,command))
                reply=modem.receive(lambda j:j["port"]==2 and parse(j["raw"])[0]==2)
                self.assertEqual(body(reply["raw"],2,5)[5:].rstrip(b"\0"),expected)
            start=len(modem.all_submissions)
            modem.send(1,0,login(stamp=101)); time.sleep(.15)
            self.assertFalse(any(j["port"]==1 and parse(j["raw"])[0]==1 for j in modem.all_submissions[start:]))
            modem.send(1,0,login(stamp=102))
            modem.receive(lambda j:j["port"]==1 and parse(j["raw"])[0]==1)
            service.stop_process()
            after=inventory(destination/"native")
            records=native_records(after["nvs/nvs.snapshot"])
            for key,value in original_records.items():
                if key==("mc-onchip","bot-mesh"):
                    self.assertEqual(records[key][:5],value[:5])
                    self.assertEqual(records[key][38:],value[38:])
                else: self.assertEqual(records[key],value,key)
            for name,value in original.items():
                if name.startswith("spiffs/"): self.assertEqual(after[name],value,name)
            self.assertEqual(hashes(inventory(self.source)),self.before)
            check(destination)
        finally: service.close()

    def test_initialized_import_quarantines_missing_room_snapshot(self):
        destination=self.root/"missing"
        service=RunningService.__new__(RunningService)
        service.root=destination
        service.emulator=Emulator(); service.emulator.identity_root=destination
        service.logs=[]; service.errors=[]
        settings=self.root/"initialized-missing-settings"
        self.write_config(settings,service.emulator.port)
        migrate(self.source,settings,destination)
        try:
            service.start_process(True,runner=True)
            service.stop_process()
            marker=(destination/"roles.initialized").read_bytes()
            self.assertEqual(marker,b"RLS1"+public(1)+public(4)+public(5))
            (destination/"room.state").unlink()
            after=len(service.logs)
            service.start_process(True,runner=True)
            service.wait("committed role snapshot unavailable or invalid",after=after)
            self.assertFalse((destination/"room.state").exists())
            self.assertEqual((destination/"roles.initialized").read_bytes(),marker)
            modem=service.emulator
            payload=b"initialized-import-relay"
            modem.send(0,0,packet(5,payload,route=1,width=3))
            modem.receive(lambda j:j["port"]==0 and parse(j["raw"])[4]==payload)
            modem.send(2,0,packet(4,parse(advert(2))[4],route=2,width=3))
            modem.send(2,0,text(2,5,1700000820,"!recall migration"))
            reply=modem.receive(lambda j:j["port"]==2 and parse(j["raw"])[0]==2)
            self.assertEqual(body(reply["raw"],2,5)[5:].rstrip(b"\0"),b"native data")
            self.assertEqual(hashes(inventory(self.source)),self.before)
        finally: service.close()

    def test_envelope_authority_and_pending_identity(self):
        source=self.root/"envelope-source"
        shutil.copytree(self.source,source)
        path=source/"room/state.json"
        active=json.loads(path.read_text())
        expanded=bytearray(hashlib.sha512(bytes([4])*32).digest())
        expanded[0]&=248; expanded[31]=(expanded[31]&63)|64
        envelope={"version":1,"identity":base64.b64encode(expanded).decode(),
                  "document":"state.json","state":active}
        target=source/"room/identity-state.json"
        target.write_text(json.dumps(envelope)); target.chmod(0o600)
        path.write_text('{"stale_legacy_data":"must not become authoritative"}')
        self.assertEqual(migrate(source,self.settings)["roles"]["room"]["members"],8)
        envelope["pending_identity"]=base64.b64encode(expanded).decode()
        target.write_text(json.dumps(envelope))
        with self.assertRaisesRegex(ValueError,"pending"): migrate(source,self.settings)
        envelope.pop("pending_identity"); envelope["state"]=None
        target.write_text(json.dumps(envelope))
        with self.assertRaisesRegex(ValueError,"no active state"): migrate(source,self.settings)

    def test_source_lock_and_disjoint_destination(self):
        source=self.root/"locked-source"
        shutil.copytree(self.source,source)
        lock=source/".lock"; lock.touch(mode=0o600)
        with lock.open("rb") as handle:
            fcntl.flock(handle,fcntl.LOCK_EX|fcntl.LOCK_NB)
            with self.assertRaisesRegex(ValueError,"locked"): migrate(source,self.settings)
        alias=self.root/"source-alias"; alias.symlink_to(source,target_is_directory=True)
        with self.assertRaisesRegex(ValueError,"disjoint"): migrate(source,self.settings,alias/"new")
        self.assertFalse((source/"new").exists())

    def test_schema_only_and_explicit_archive_retention(self):
        source=self.root/"archive-source"
        shutil.copytree(self.source,source)
        archive=source/"bot-archive"; archive.mkdir(mode=0o700)
        large=archive/"legacy.db"
        with large.open("wb") as out:
            out.truncate(33*1024*1024)
        large.chmod(0o600)
        compatibility=source/"bot_companion"; compatibility.mkdir(mode=0o700)
        seed=compatibility/"identity.seed"; seed.write_bytes(bytes([99])*32); seed.chmod(0o600)
        with self.assertRaisesRegex(ValueError,"excessive file size"):
            migrate(source,schema_only=True)
        with self.assertRaisesRegex(ValueError,"active/compatibility"):
            migrate(source,schema_only=True,retained=["room"])
        retained=["bot-archive"]
        before=hashes(inventory(source,retained))
        report=migrate(source,schema_only=True,retained=retained)
        self.assertEqual(report["mode"],"schema-inspection")
        self.assertIn("not checked",report["runtime_configuration"])
        self.assertEqual(report["retained_snapshot_directories"],retained)
        self.assertIn("bot_companion",report["retained_compatibility_roles"])
        self.assertEqual(report["source_files"],before)
        destination=self.root/"archives-staged"
        with self.assertRaisesRegex(ValueError,"cannot"):
            migrate(source,destination=destination,schema_only=True,retained=retained)
        self.assertFalse(destination.exists())
        migrate(source,self.settings,destination,retained)
        self.assertEqual((destination/"rollback-go/bot-archive/legacy.db").stat().st_size,33*1024*1024)
        self.assertEqual(hashlib.sha256((destination/"rollback-go/bot-archive/legacy.db").read_bytes()).hexdigest(),before["bot-archive/legacy.db"])
        self.assertEqual(hashes(inventory(source,retained)),before)
        check(destination)

    def test_saved_loop_modes_and_attempt_snapshot_bounds(self):
        source=self.root/"loop-source"
        shutil.copytree(self.source,source)
        path=source/"repeater/state.json"
        saved=json.loads(path.read_text())
        for mode in range(4):
            saved["Preferences"]["loop"]=mode
            path.write_text(json.dumps(saved))
            destination=self.root/("loop-"+str(mode))
            migrate(source,self.settings,destination)
            self.assertEqual(check(destination)["relay.loop"],str(mode))
        state=(self.root/"loop-0/room.state").read_bytes()
        at,_,_=snapshot_tail(state)
        flag=at-1-state[48]
        for label,bad in (("attempts",state[:at-1]),
                          ("flag",state[:flag]+b"\2"+state[flag+1:])):
            bad_path=self.root/("bad-"+label); bad_path.write_bytes(bad); bad_path.chmod(0o600)
            with Host(seed=(bytes([4])*32).hex(),expected=public(4).hex()) as host:
                self.assertEqual(host.command("load "+str(self.root/"loop-0/room.state")),["true"])
                self.assertEqual(host.command("load "+str(bad_path)),["false"])
                self.assertIn("posts=12",host.command("status")[0])
        saved["Preferences"]["loop"]=4
        path.write_text(json.dumps(saved))
        with self.assertRaisesRegex(ValueError,"loop policy"): migrate(source,self.settings)

    def test_first_start_integrity_and_identity_guard(self):
        destination=self.root/"integrity"
        migrate(self.source,self.settings,destination)
        check(destination)
        path=destination/"room.state"
        original=path.read_bytes(); path.write_bytes(original+b"\0")
        with self.assertRaisesRegex(ValueError,"changed before first startup"): check(destination)
        path.write_bytes(original)
        marker=destination/"willow.started"
        marker.write_text("started\n"); marker.chmod(0o600)
        (destination/"bot.seed").write_bytes(bytes([6])*32)
        with self.assertRaisesRegex(ValueError,"imported identity changed"): check(destination)

    def test_retained_history_delivery_ack_and_restart(self):
        destination=self.root/"history-running"
        migrate(self.source,self.settings,destination)
        source=json.loads((self.source/"room/state.json").read_text())
        expected=[(bytes(p["Author"]),p["Timestamp"],
                   base64.b64decode(p["RawText"]) if p.get("RawText") is not None else p["Text"].encode())
                  for p in source["History"]]
        oracle=[bytes.fromhex(raw) for raw in json.loads(self.history_oracle.read_text())]
        path=destination/"room.state"
        self.assertEqual(snapshot_history(path.read_bytes()),expected)
        attempts={bytes(m["Key"]):m["Attempt"] for m in source["Members"].values()}
        with Host(seed=(bytes([4])*32).hex(),expected=public(4).hex()) as host:
            self.assertEqual(host.command("state "+str(path)),["LOCKED"])
            self.assertIn("posts=12",host.command("status")[0])
            host.packet(login(stamp=103),now=110000)
            deliveries=[]
            wanted=[p for p in expected if p[0]!=public(2)]
            for step in range(200):
                now=120000+step*150
                for raw in tx(host.command("tick "+str(now))):
                    if parse(raw)[0]==4:
                        continue
                    plain=body(raw,2,4).rstrip(b"\0")
                    author,stamp,text_bytes=wanted[len(deliveries)]
                    self.assertEqual(int.from_bytes(plain[:4],"little"),stamp)
                    self.assertEqual(plain[4]&252,8)
                    self.assertEqual(plain[5:],author[:4]+text_bytes)
                    go_raw=oracle[len(deliveries)]
                    self.assertEqual(parse(raw)[:4],parse(go_raw)[:4])
                    go_plain=body(go_raw,2,4).rstrip(b"\0")
                    self.assertEqual(plain[:4]+bytes([plain[4]&252])+plain[5:],
                                     go_plain[:4]+bytes([go_plain[4]&252])+go_plain[5:])
                    proof=hashlib.sha256(plain+public(2)).digest()[:4]
                    host.packet(packet(3,proof),now=now)
                    deliveries.append(stamp)
                if len(deliveries)==len(wanted): break
            self.assertEqual(deliveries,[p[1] for p in wanted])
            self.assertIn("since=92",host.command("cursor "+public(2).hex())[0])
        self.assertEqual(snapshot_history(path.read_bytes()),expected)
        self.assertEqual(snapshot_tail(path.read_bytes())[2],attempts)
        with Host(seed=(bytes([4])*32).hex(),expected=public(4).hex()) as host:
            self.assertEqual(host.command("state "+str(path)),["LOCKED"])
            self.assertIn("posts=12",host.command("status")[0])
            self.assertIn("since=92",host.command("cursor "+public(2).hex())[0])
            self.assertEqual([raw for raw in tx(host.command("tick 200000")) if parse(raw)[0]==2],[])
        self.assertEqual(snapshot_history(path.read_bytes()),expected)
        self.assertEqual(snapshot_tail(path.read_bytes())[2],attempts)
        with Host(seed=(bytes([4])*32).hex(),expected=public(4).hex()) as host:
            self.assertEqual(host.command("state "+str(path)),["LOCKED"])
            host.packet(login(stamp=104),now=200000)
            self.assertEqual(len(host.packet(text(2,4,105,"new retained"),now=200000)),1)
        added=snapshot_history(path.read_bytes())
        self.assertEqual(snapshot_tail(path.read_bytes())[2],attempts)
        self.assertEqual(added[:-1],expected)
        self.assertEqual((added[-1][0],added[-1][2]),(public(2),b"new retained"))
        expected=added
        # A truncated durable tail must neither replace live history nor mutate its snapshot.
        bad=destination/"truncated"; bad.write_bytes(path.read_bytes()[:-1]); bad.chmod(0o600)
        with Host(seed=(bytes([4])*32).hex(),expected=public(4).hex()) as host:
            self.assertEqual(host.command("load "+str(path)),["true"])
            self.assertEqual(host.command("load "+str(bad)),["false"])
            self.assertIn("posts=13",host.command("status")[0])
        self.assertEqual(hashes(inventory(self.source)),self.before)

    def test_migrated_regional_send_receive(self):
        source=self.root/"scoped-source"
        shutil.copytree(self.source,source)
        home=hashlib.sha256(b"#fixture-a").digest()[:16]
        default=hashlib.sha256(b"#fixture-b").digest()[:16]
        for role in ("repeater","room"):
            path=source/role/"state.json"
            state=json.loads(path.read_text())
            state.update(DefaultRegion=2,ManagedDefaultRegion=True)
            path.write_text(json.dumps(state))
        (source/"bot/native/scopes").write_bytes(b"SCP1"+home+default)
        destination=self.root/"scoped"
        service=RunningService.__new__(RunningService)
        service.root=destination
        service.emulator=Emulator(); service.emulator.identity_root=destination
        service.logs=[]; service.errors=[]
        settings=self.root/"scoped-settings"
        self.write_config(settings,service.emulator.port)
        settings.write_text(settings.read_text().replace("bot_home=\n","bot_home="+home.hex()+"\n").replace(
            "bot_default=\n","bot_default="+default.hex()+"\n"))
        migrate(source,settings,destination)
        try:
            service.start_process(True,runner=True)
            modem=service.emulator
            adverts={}
            while len(adverts)<3:
                submission=modem.receive(lambda j:parse(j["raw"])[0]==4)
                adverts[submission["port"]]=submission["raw"]
            for raw in adverts.values():
                self.assertEqual(parse(raw)[1],0)
                self.assertEqual(raw[1:5],struct.pack("<HH",*[scope_code(default,raw)]*2))
            modem.send(2,0,advert(2)); time.sleep(.15)
            for stamp,incoming,outgoing in ((1700000850,home,home),(1700000851,b"",b""),
                                            (1700000852,bytes([99])*16,default)):
                raw=text(2,5,stamp,"!ping")
                modem.send(2,0,scoped(raw,incoming) if incoming else raw)
                reply=modem.receive(lambda j:j["port"]==2 and parse(j["raw"])[0]==2)["raw"]
                self.assertIn(b"Pong",body(reply,2,5))
                if outgoing:
                    self.assertEqual(reply[1:5],struct.pack("<HH",*[scope_code(outgoing,reply)]*2))
                else: self.assertEqual(parse(reply)[1],1)
            request=scoped(advert(3),home)
            modem.send(0,0,request)
            forwarded=modem.receive(lambda j:j["port"]==0 and parse(j["raw"])[0]==4)["raw"]
            self.assertEqual(forwarded[1:5],request[1:5])
            self.assertEqual(parse(forwarded)[3],parse(request)[3]+public(1)[:3])
            before=len(modem.all_submissions)
            modem.send(0,0,scoped(advert(6),bytes([99])*16)); time.sleep(.3)
            self.assertFalse(any(j["port"]==0 for j in modem.all_submissions[before:]))
        finally: service.close()


if __name__=="__main__": unittest.main(verbosity=2)
