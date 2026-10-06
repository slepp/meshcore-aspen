#!/usr/bin/env python3
"""Actual TCP/KISS end-to-end service, with a private loopback modem emulator."""
import argparse
import hashlib
import hmac
from itertools import count
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
import unittest

from parity import ROOT, public, secret, seal, unseal, packet, parse, kiss
from cryptography.hazmat.primitives.asymmetric import ed25519
from cryptography.hazmat.primitives import serialization
from native_fixture import stage_wasm
sys.path.insert(0,str(ROOT))
from build_worker import verify as verify_worker

PROFILE = struct.pack("<IIBBBfBh", 912525000, 250000, 7, 5, 2, 1.0, 0, 0)
CHANNEL = bytes(range(16))
WORKER = Path(os.environ.get("BOT_NATIVE_WORKER", ROOT/"build/native-worker"))
STATE_SEQUENCE = count()
WORKER_SPEC = verify_worker(WORKER)
def scope_code(key,raw):
    kind,_,_,_,payload=parse(raw)
    value=int.from_bytes(hmac.digest(key,bytes([kind])+payload,"sha256")[:2],"little")
    return max(1,min(65534,value))
def scoped(raw,key):
    kind,_,encoded,path,payload=parse(raw)
    codes=struct.pack("<HH",scope_code(key,raw),scope_code(key,raw))
    return packet(kind,payload,route=0,width=(encoded>>6)+1,path=path,codes=codes)
def addressed(seed, recipient, kind, plain, route=2, width=1, path=b""):
    return packet(kind, public(recipient)[:1] + public(seed)[:1] + seal(secret(seed, public(recipient)), plain),
                  route=route, width=width, path=path)
def login(seed=2, recipient=4, stamp=10):
    return packet(7, public(recipient)[:1] + public(seed) + seal(secret(seed,public(recipient)),
                  struct.pack("<II",stamp,0) + b"room\0"))
def text(seed, recipient, stamp, text):
    return addressed(seed,recipient,2,struct.pack("<IB",stamp,0)+text.encode(),
                     route=1 if recipient==5 else 2,width=3 if recipient==5 else 1,
                     path=b"\x12\x34\x56" if recipient==5 else b"")
def body(raw, recipient, sender):
    return unseal(secret(recipient,public(sender)),parse(raw)[4][2:])
def advert(seed):
    prefix=public(seed)+struct.pack("<I",int(time.time()))
    app=b"\x81Client"
    signature=ed25519.Ed25519PrivateKey.from_private_bytes(bytes([seed])*32).sign(prefix+app)
    return packet(4,prefix+signature+app,route=1,width=3,path=b"\x12\x34\x56")

class Emulator:
    telemetry_mode="ok"
    def __init__(self):
        self.listener=socket.socket()
        self.listener.bind(("127.0.0.1",0)); self.listener.listen(4)
        self.listener.settimeout(.1)
        self.port=self.listener.getsockname()[1]
        self.connection=None; self.epoch=0; self.stop=False
        self.write_lock=threading.Lock()
        self.submissions=queue.Queue(); self.all_submissions=[]; self.controls=[]
        self.hold=False; self.hold_all=False; self.drift=False; self.malformed_hello=False
        self.error=None
        self.identity_root=None
        self.policy_mode="ok"; self.policy_before_apply=None
        self.policy_requests=queue.Queue(); self.policy_counts={}
        self.thread=threading.Thread(target=self.run,daemon=True); self.thread.start()
    def send(self, port, command, body, connection=None, metadata=b"\xf9\x12\xb7"):
        with self.write_lock:
            target=connection or self.connection
            if target:
                try:
                    data=kiss(body,port,command)
                    if command==0 and metadata is not None: data+=kiss(metadata,port,6)
                    target.sendall(data)
                except OSError: pass
    def event(self,port,generation,job,state,connection):
        self.send(port,6,b"\xfa\1"+struct.pack("<II",generation,job)+bytes([state,0])+
                  struct.pack("<III",1,10 if state==2 else 0,10),connection)
    def handle(self,frame,connection,epoch):
        port,command=frame[0]>>4,frame[0]&15
        assert command==6 and len(frame)>=2, frame.hex()
        op=frame[1]; data=frame[2:]
        generation=100*epoch+port+1
        self.controls.append((epoch,port,op,data.hex()))
        if op==32:
            assert data==b"\1\0", "service claimed configuration owner"
            reply=b"\1\0"+struct.pack("<II",generation,1)+b"\0\0"
            if self.malformed_hello:
                self.malformed_hello=False; reply=reply[:-1]
        elif op==37:
            assert port==0 and data==b"\1\1"
            reply=b"\1\4\5\4\1"
        elif op==34:
            assert data==b"\1\0", "service attempted PHY mutation"
            profile=PROFILE
            if self.drift:
                profile=bytes([PROFILE[0]^1])+PROFILE[1:]
                self.drift=False
            reply=b"\1\0"+struct.pack("<I",1)+profile
        elif op==35:
            assert len(data)==9 and data[:5]==b"\1"+struct.pack("<I",generation)
            factor,=struct.unpack("<f",data[5:])
            assert math.isfinite(factor) and factor>=0
            key=(epoch,port)
            count=self.policy_counts.get(key,0)+1
            self.policy_counts[key]=count
            if count>1 or factor!=1:
                if self.policy_before_apply:self.policy_before_apply(port,factor)
                self.policy_requests.put({"epoch":epoch,"port":port,"factor":factor,"bits":data[5:],
                                          "generation":generation})
                if self.policy_mode=="hold":return
            reply=b"\1\0"+data[5:]
            if (count>1 or factor!=1) and self.policy_mode=="malformed":reply=b"\1\0"+struct.pack("<f",factor+1)
            if (count>1 or factor!=1) and self.policy_mode=="reject":reply=b"\1\3"+data[5:]
        elif op==15:
            assert len(data)==1
            reply=struct.pack("<I",10+data[0])
            if data[0]==64 and getattr(self,"bad_airtime",False):
                self.bad_airtime=False
                reply=bytes(4)
        elif op==25:
            assert data==b"\1"
            reply=b"\1"
        elif op in (0x13,0x10,0x0d,0x14,0x12):
            assert port==0 and data==b"", "telemetry must be a read-only port-zero query"
            if self.telemetry_mode=="drop" and op==0x13:return
            reply={
                0x13:struct.pack("<H",3900),
                0x10:struct.pack("<h",-117),
                0x0d:struct.pack("<b",-73),
                0x14:struct.pack("<h",251),
                0x12:struct.pack("<III",120,len(self.all_submissions),0),
            }[op]
            if self.telemetry_mode=="malformed" and op==0x13:reply=b"\1"
        elif op==38:
            expected=public([1,4,5,3][port])
            if self.identity_root is not None:
                role=("relay","room","bot","observer")[port]
                expanded=self.identity_root/"observer.expanded"
                if port==3 and expanded.exists():
                    from migrate_go import identity_public
                    expected=identity_public(expanded.read_bytes())
                else:
                    seed=(self.identity_root/(role+".seed")).read_bytes()
                    expected=ed25519.Ed25519PrivateKey.from_private_bytes(seed).public_key().public_bytes(
                        serialization.Encoding.Raw,serialization.PublicFormat.Raw)
            assert data==bytes([1,port])+expected
            reply=b"\1\0"+struct.pack("<I",generation)+b"\0\0"
        elif op==36:
            assert data==b"\1" and port==2
            aggregate=len(self.all_submissions); source=sum(j["port"]==2 for j in self.all_submissions)
            reply=b"\1"+struct.pack("<9I",1,120,aggregate*10,aggregate,0,30,source*10,source,0)+b"\0\0"
        elif op==33:
            assert len(data)>=19 and data[0]==1
            gen,job=struct.unpack_from("<II",data,1)
            assert gen==generation
            priority=data[9]; delay,expiry=struct.unpack_from("<II",data,10)
            raw=data[18:]
            item={"epoch":epoch,"port":port,"generation":gen,"job":job,"raw":raw,
                  "priority":priority,"delay":delay,"expiry":expiry}
            self.all_submissions.append(item); self.submissions.put(item)
            self.event(port,gen,job,1,connection)
            if self.hold or self.hold_all:
                self.hold=False
            else:
                timer=threading.Timer(delay/1000,self.event,args=(port,gen,job,2,connection))
                timer.daemon=True; timer.start()
            return
        else:
            raise AssertionError(f"unexpected modem command {op:x}")
        response=0x9a if op==0x19 else op|128
        encoded=kiss(bytes([response])+reply,port,6)
        # Split framing deliberately across writes, including framing boundaries.
        with self.write_lock:
            connection.sendall(encoded[:2]); connection.sendall(encoded[2:])
    def run(self):
        try:
            while not self.stop:
                try: connection,_=self.listener.accept()
                except socket.timeout: continue
                except OSError: return
                self.connection=connection; self.epoch+=1; epoch=self.epoch
                connection.settimeout(.1); buffer=bytearray(); escaped=False
                while not self.stop:
                    try: chunk=connection.recv(4096)
                    except socket.timeout: continue
                    except OSError: break
                    if not chunk: break
                    for byte in chunk:
                        if byte==192:
                            if buffer: self.handle(bytes(buffer),connection,epoch)
                            buffer.clear(); escaped=False
                        elif escaped:
                            assert byte in (220,221)
                            buffer.append(192 if byte==220 else 219); escaped=False
                        elif byte==219: escaped=True
                        else: buffer.append(byte)
                connection.close()
                self.connection=None
        except Exception as exc: self.error=exc
    def receive(self,predicate,timeout=12):
        end=time.monotonic()+timeout
        while time.monotonic()<end:
            if self.error: raise self.error
            try: item=self.submissions.get(timeout=.1)
            except queue.Empty: continue
            if predicate(item): return item
        raise AssertionError("expected modem submission did not arrive")
    def disconnect(self):
        if self.connection:
            try: self.connection.shutdown(socket.SHUT_RDWR)
            except OSError: pass
    def close(self):
        self.stop=True; self.disconnect(); self.listener.close(); self.thread.join(3)
        if self.error: raise self.error

class RunningService:
    def __init__(self,native=True,malformed=False,wasm=False,bot_home=b"",bot_default=b"",extra_config="",root=None):
        # Keep absolute owner/admin UDS fixture paths short in isolated worktrees.
        self.root=root or ROOT.parents[1]/f".s-{os.getpid()}-{next(STATE_SEQUENCE)}"
        self.root.mkdir(mode=0o700)
        if wasm:
            try:
                stage_wasm(WORKER,self.root/"native",PROFILE,(ROOT/"build/arithmetic.wasm").read_bytes())
            except BaseException:
                shutil.rmtree(self.root)
                raise
        self.emulator=Emulator(); self.emulator.malformed_hello=malformed
        self.emulator.identity_root=self.root
        for name,n in (("relay",1),("room",4),("bot",5)):
            p=self.root/f"{name}.seed"; p.write_bytes(bytes([n])*32); p.chmod(0o600)
        config=self.root/"config"
        config.write_text(f"address=127.0.0.1\nport={self.emulator.port}\nprofile={PROFILE.hex()}\n"
                          f"worker={WORKER if native else '-'}\npassword=room\nadmin=admin\nwidth=3\n"
                          f"native_airtime=3600\nchannel={CHANNEL.hex()}\n")
        with config.open("a") as out:
            out.write(f"bot_home={bot_home.hex()}\nbot_default={bot_default.hex()}\n")
            out.write(extra_config)
        config.chmod(0o600)
        self.logs=[]; self.errors=[]
        try:
            self.start_process(native)
        except BaseException:
            self.close()
            raise
    def start_process(self,native,runner=False):
        after=len(self.logs)
        command=([sys.executable,"-B",str(ROOT/"willow.py"),"run","--state",str(self.root)] if runner else
                 [os.environ.get("MESHCORE_HEW_HOST",str(ROOT/"build"/"hew-host")),str(self.root)])
        if runner and Path(os.environ.get("MESHCORE_HEW_HOST","")).name=="hew-host-release":
            command.append("--release")
        self.proc=subprocess.Popen(command,
            stdout=subprocess.PIPE,stderr=subprocess.PIPE,text=True)
        self.reader=threading.Thread(target=self.capture,args=(self.proc.stdout,self.logs),daemon=True)
        self.error_reader=threading.Thread(target=self.capture,args=(self.proc.stderr,self.errors),daemon=True)
        self.reader.start(); self.error_reader.start()
        self.wait("ONLINE",30,after=after)
        if native:self.wait("BOT_READY",30,after=after)
    @staticmethod
    def capture(stream,lines):
        for line in stream: lines.append(line.strip())
    def wait(self,prefix,timeout=10,after=0):
        deadline=time.monotonic()+timeout
        while time.monotonic()<deadline:
            for line in self.logs[after:]:
                if prefix in line:return line
            if self.proc.poll() is not None:
                raise AssertionError(f"service exited {self.proc.returncode}: {self.logs[-10:]} {self.errors[-10:]}")
            if self.emulator.error: raise self.emulator.error
            time.sleep(.02)
        raise AssertionError(f"no {prefix}: {self.logs[-10:]} {self.errors[-10:]}")
    def stop_process(self):
        self.proc.terminate()
        try:self.proc.wait(timeout=8)
        except subprocess.TimeoutExpired:
            self.proc.kill(); self.proc.wait()
            raise AssertionError("service did not shut down its transports/worker")
        self.reader.join(2); self.error_reader.join(2)
        self.proc.stdout.close(); self.proc.stderr.close()
        assert self.proc.returncode==getattr(self,"expected_exit",0),(self.proc.returncode,self.errors[-10:])
    def close(self):
        try:
            try:self.stop_process()
            finally:self.emulator.close()
        finally:shutil.rmtree(self.root)
    def __enter__(self):return self
    def __exit__(self,*args):self.close()

class ServiceTests(unittest.TestCase):
    def test_first_addressed_channel_calc_after_dm_commands(self):
        with RunningService() as service:
            modem=service.emulator
            modem.send(2,0,packet(4,parse(advert(2))[4],route=2,width=3,path=b""))
            commands=["!calc 8100000 + 57255","!ping","!help","!help calc",
                      "!about","!uptime","!path","!air","!help 2"]
            for index,command in enumerate(commands):
                plain=struct.pack("<IB",1791057257+index,0)+command.encode()
                modem.send(2,0,addressed(2,5,2,plain,route=2,width=3,path=b""))
                reply=modem.receive(lambda job:job["port"]==2 and parse(job["raw"])[0]==2)
                if index==0:
                    self.assertEqual(body(reply["raw"],2,5)[5:].rstrip(b"\0"),b"= 8157255")
            command=f"!@{public(5)[:4].hex()} calc 8100000 + 57255"
            plain=struct.pack("<IB",1791057284,0)+b"Client: "+command.encode()
            raw=packet(5,hashlib.sha256(CHANNEL).digest()[:1]+seal(CHANNEL,plain),
                       route=1,width=3,path=b"")
            modem.send(2,0,raw)
            reply=modem.receive(lambda job:job["port"]==2 and parse(job["raw"])[0]==5)
            self.assertIn(b"= 8157255",unseal(CHANNEL,parse(reply["raw"])[4][1:]))
            self.assertFalse(any("OFFLINE" in line or "MODEM_REQUEST_FAILED" in line
                                 for line in service.logs),service.logs)
    def test_hew_note_whitespace_survives_restart(self):
        with RunningService(native=False) as service:
            modem=service.emulator
            modem.send(2,0,advert(2))
            time.sleep(.1)
            stamp=1700000200
            def command(value):
                nonlocal stamp
                stamp+=1
                modem.send(2,0,text(2,5,stamp,value))
                reply=modem.receive(lambda j:j["port"]==2 and parse(j["raw"])[0]==2)
                return body(reply["raw"],2,5)[5:].rstrip(b"\0")
            self.assertIn(b"Error: !remember requires",command("!remember  value"))
            empty=command("!notes")
            self.assertEqual(empty,b"No notes")
            self.assertIn(b"created; committed",command("!remember   camp   bring  tea"))
            self.assertIn(b"Error: !remember requires",command("!remember camp    "))
            service.stop_process(); service.start_process(False)
            self.assertEqual(command("!recall   camp"),b"bring  tea")
            self.assertIn(b"Notes (1)",command("!notes"))

    def test_service_recovers_seed_and_state_staging(self):
        with RunningService(native=False) as service:
            modem=service.emulator
            modem.send(0,0,packet(5,b"staging-fixture",route=1))
            modem.receive(lambda j:j["port"]==0)
            modem.send(2,0,advert(2)); time.sleep(.1)
            modem.send(2,0,text(2,5,1700000300,"!ping"))
            modem.receive(lambda j:j["port"]==2 and parse(j["raw"])[0]==2)
            modem.send(1,0,login(stamp=101))
            modem.receive(lambda j:j["port"]==1 and parse(j["raw"])[0]==1)
            service.stop_process()
            committed={name:(service.root/(name+".seed")).read_bytes() for name in ("relay","room","bot")}
            for name in ("relay","room","bot"):
                for suffix in ("seed","state"):
                    target=service.root/(name+"."+suffix)
                    raw=target.read_bytes()
                    pending=Path(str(target)+".pending")
                    pending.write_bytes(raw[:len(raw)//2]); pending.chmod(0o600)
            service.start_process(False)
            for name in ("relay","room","bot"):
                self.assertEqual((service.root/(name+".seed")).read_bytes(),committed[name])
            self.assertFalse(list(service.root.glob("*.pending")))
            start=len(modem.all_submissions)
            modem.send(1,0,login(stamp=101)); time.sleep(.15)
            self.assertFalse(any(j["port"]==1 and parse(j["raw"])[0]==1 for j in modem.all_submissions[start:]))
            modem.send(1,0,login(stamp=102))
            modem.receive(lambda j:j["port"]==1 and parse(j["raw"])[0]==1)
            for legacy in (False,True):
                with self.subTest(legacy_without_marker=legacy):
                    service.stop_process()
                    if legacy: (service.root/"roles.initialized").unlink()
                    seed=service.root/"bot.seed"; seed.unlink()
                    pending=service.root/"bot.seed.pending"
                    pending.write_bytes(committed["bot"][:16]); pending.chmod(0o600)
                    service.expected_exit=1
                    with self.assertRaisesRegex(AssertionError,"initialized role identity missing"):
                        service.start_process(False)
                    self.assertFalse(seed.exists())
                    self.assertEqual(pending.read_bytes(),committed["bot"][:16])
                    service.stop_process()
                    seed.write_bytes(committed["bot"]); seed.chmod(0o600)
                    service.expected_exit=0
                    service.start_process(False)
                    self.assertFalse(pending.exists())
                    self.assertEqual(seed.read_bytes(),committed["bot"])

    def test_tcp_relay_room_native_lua_and_reconnect(self):
        self.assertTrue(WORKER.is_file(),"set BOT_NATIVE_WORKER to the installed production native extension")
        with RunningService(wasm=True) as service:
            modem=service.emulator
            transcript={"advert":advert(2).hex(),"state":{str(p.relative_to(service.root/"native")):p.read_bytes().hex()
                        for p in (service.root/"native").rglob("*") if p.is_file()},"commands":[]}
            original=packet(5,b"relay-frame",route=1,width=3)
            modem.send(0,0,original)
            forwarded=modem.receive(lambda j:j["port"]==0 and parse(j["raw"])[4]==b"relay-frame")
            self.assertEqual(forwarded["raw"],packet(5,b"relay-frame",route=1,width=3,path=public(1)[:3]))
            modem.send(1,0,login())
            logged=modem.receive(lambda j:j["port"]==1 and parse(j["raw"])[0]==1)
            self.assertEqual(body(logged["raw"],2,4)[4:8],b"\0\0\0\2")
            modem.send(1,0,text(2,4,11,"hello room"))
            ack=modem.receive(lambda j:j["port"]==1 and parse(j["raw"])[0]==3)
            proof=hashlib.sha256(struct.pack("<IB",11,0)+b"hello room"+public(2)).digest()[:4]
            self.assertEqual(parse(ack["raw"])[4],proof)
            modem.send(2,0,bytes.fromhex(transcript["advert"])); time.sleep(.15)
            for stamp,command,expected in [(1700000001,"!ping",b"Pong"),(1700000002,"!calc (2+3)*4",b"20"),
                                            (1700000003,"!remember camp bring tea",b"committed"),
                                            (1700000004,"!recall camp",b"bring tea")]:
                request=text(2,5,stamp,command)
                modem.send(2,0,request)
                try:
                    reply=modem.receive(lambda j:j["port"]==2 and parse(j["raw"])[0]==2)
                except AssertionError as error:
                    raise AssertionError((command,service.logs[-25:],service.errors[-25:],
                                          [(j["port"],parse(j["raw"])[0],j["raw"].hex()) for j in modem.all_submissions[-8:]])) from error
                self.assertIn(expected,body(reply["raw"],2,5)[5:].rstrip(b"\0"))
                transcript["commands"].append({"request":request.hex(),"reply":reply["raw"].hex()})
            wasm_request=text(2,5,1700000006,"!wadd 17 25")
            modem.send(2,0,wasm_request)
            reply=modem.receive(lambda j:j["port"]==2 and parse(j["raw"])[0]==2)
            self.assertEqual(body(reply["raw"],2,5)[5:].rstrip(b"\0"),b"42")
            transcript["commands"].append({"request":wasm_request.hex(),"reply":reply["raw"].hex()})
            start=len(modem.all_submissions)
            retry=addressed(2,5,2,struct.pack("<IB",1700000006,1)+b"!wadd 17 25",route=1,width=3,path=b"\xab\xcd\xef")
            modem.send(2,0,retry)
            tampered=bytearray(text(2,5,1700000007,"!ping")); tampered[-1]^=1
            modem.send(2,0,bytes(tampered)); time.sleep(.2)
            self.assertFalse(any(j["port"]==2 and parse(j["raw"])[0]==2 for j in modem.all_submissions[start:]))
            for stamp,command,expected in [(1700000020,"!ping",b"Pong"),
                    (1700000021,f"!@{public(5)[:4].hex()} remember camp leak",b"Error: !remember permission not granted: send this command by DM")]:
                plain=struct.pack("<IB",stamp,0)+b"Client: "+command.encode()
                raw=packet(5,hashlib.sha256(CHANNEL).digest()[:1]+seal(CHANNEL,plain),route=1,width=3,path=b"\x12\x34\x56")
                modem.send(2,0,raw)
                reply=modem.receive(lambda j:j["port"]==2 and parse(j["raw"])[0]==5)
                self.assertIn(expected,unseal(CHANNEL,parse(reply["raw"])[4][1:]))
            mt=text(2,5,1700000030,"!mt 1")
            modem.send(2,0,mt); time.sleep(.1)
            alternate=addressed(2,5,2,struct.pack("<IB",1700000030,0)+b"!mt 1",route=1,width=3,path=b"\xab\xcd\xef")
            modem.send(2,0,alternate)
            reply=modem.receive(lambda j:j["port"]==2 and parse(j["raw"])[0]==2)
            observed=body(reply["raw"],2,5)
            self.assertIn(b"123456",observed); self.assertIn(b"abcdef",observed)
            modem.send(2,0,text(2,5,1700000040,"!air"))
            reply=modem.receive(lambda j:j["port"]==2 and parse(j["raw"])[0]==2)
            self.assertIn(b"Air snapshot ms: credit bot=30 all=120",body(reply["raw"],2,5),service.errors[-12:])
            time.sleep(.5)
            # Lose an already accepted job. Reconnect must not replay it or call it successful.
            modem.hold=True
            lost=packet(5,b"uncertain-only-once",route=1)
            modem.send(0,0,lost)
            job=modem.receive(lambda j:parse(j["raw"])[4]==b"uncertain-only-once")
            service.wait(f"generation={job['generation']} job={job['job']} state=1")
            marker=len(service.logs); modem.disconnect()
            service.wait(f"generation={job['generation']} job={job['job']} state=4",after=marker)
            service.wait("ONLINE epoch=2",20,after=marker); service.wait("BOT_READY",30,after=marker)
            self.assertEqual(sum(parse(j["raw"])[4]==b"uncertain-only-once" for j in modem.all_submissions),1)
            # Stale terminal event from the old source epoch is ignored.
            modem.event(0,job["generation"],job["job"],2,modem.connection)
            time.sleep(.1)
            self.assertFalse(any(f"generation={job['generation']} job={job['job']} state=2" in l for l in service.logs[marker:]))
            # Notes persist; signed contacts must be rediscovered after worker restart.
            modem.send(2,0,advert(2)); time.sleep(.15)
            modem.send(2,0,text(2,5,1700000005,"!recall camp"))
            reply=modem.receive(lambda j:j["epoch"]==2 and j["port"]==2 and parse(j["raw"])[0]==2)
            self.assertIn(b"bring tea",body(reply["raw"],2,5))
            # PHY drift retires the connection; every CONFIG request remains read-only.
            marker=len(service.logs); modem.drift=True
            service.wait("OFFLINE epoch=2",8,after=marker)
            service.wait("ONLINE epoch=3",20,after=marker)
            self.assertTrue(all(data=="0100" for _,_,op,data in modem.controls if op==34))
            report={"native_worker_sha256":hashlib.sha256(WORKER.read_bytes()).hexdigest(),
                    "epochs":modem.epoch,"submissions":len(modem.all_submissions),
                    "contracts":["HELLO","CAPACITY","CONFIG readback","SOURCE_POLICY","255 AIRTIME queries",
                    "ROLE_PRESENCE","RX_META pairing/reflection","PHY_STATS native mapping",
                    "KISS fragmented TCP","relay exact frame","room login/post ACK","native Lua ping/calc/notes",
                    "Wasm arithmetic plus Lua","native authenticated group/DM permission",
                    "native canonical retry dedup","invalid DM MAC","native async multi-path collection",
                    "ACCEPTED versus SUCCEEDED","disconnect UNKNOWN","no replay","stale epoch ignored",
                    "worker restart persistence","PHY drift no retune"]}
            (ROOT/"build"/"service-results.json").write_text(json.dumps(report,indent=2)+"\n")
            (ROOT/"build"/"bot-differential.json").write_text(json.dumps(transcript,indent=2)+"\n")
    def test_malformed_negotiation_retires_epoch(self):
        with RunningService(native=False,malformed=True) as service:
            self.assertGreaterEqual(service.emulator.epoch,2)
            self.assertTrue(any("negotiation/profile rejected" in line for line in service.logs))
            self.assertTrue(any("stage=HELLO" in line and "expected=12-bytes" in line
                                and "actual=" in line for line in service.logs))
            self.assertFalse(any(j["epoch"]==1 for j in service.emulator.all_submissions))
    def test_metadata_reflection_orphans_and_timeout(self):
        with RunningService(native=False) as service:
            modem=service.emulator
            reflected=packet(5,b"local-only",route=1)
            modem.send(0,0,reflected,metadata=b"\xf9\x80\x7f")
            time.sleep(.1)
            self.assertFalse(any(parse(j["raw"])[4]==b"local-only" for j in modem.all_submissions))
            # Orphan metadata must not mark the following unmeasured packet local.
            modem.send(0,6,b"\xf9\x80\x7f")
            modem.send(0,0,packet(5,b"unmeasured",route=1),metadata=None)
            modem.receive(lambda j:parse(j["raw"])[4]==b"unmeasured")
    def test_queue_saturation_keeps_other_roles_live(self):
        with RunningService(native=False) as service:
            modem=service.emulator
            time.sleep(.15); modem.hold_all=True
            for index in range(40):
                modem.send(0,0,packet(5,b"burst"+bytes([index]),route=1))
                time.sleep(.012)
            service.wait("REJECTED port=0",8)
            self.assertEqual(sum(parse(j["raw"])[4].startswith(b"burst") for j in modem.all_submissions),32)
            modem.send(1,0,login())
            modem.receive(lambda j:j["port"]==1 and parse(j["raw"])[0]==1)
            modem.send(2,0,advert(2))
            modem.send(2,0,text(2,5,1700000100,"!ping"))
            reply=modem.receive(lambda j:j["port"]==2 and parse(j["raw"])[0]==2)
            self.assertIn(b"Pong",body(reply["raw"],2,5))
    def test_process_restart_keeps_identity_and_login_replay_stamp(self):
        with RunningService(native=False) as service:
            modem=service.emulator
            modem.send(1,0,login(stamp=100))
            modem.receive(lambda j:j["port"]==1 and parse(j["raw"])[0]==1)
            before={p.name:p.read_bytes() for p in service.root.glob("*.seed")}
            service.stop_process(); service.start_process(False)
            self.assertEqual(before,{p.name:p.read_bytes() for p in service.root.glob("*.seed")})
            start=len(modem.all_submissions)
            modem.send(1,0,login(stamp=100)); time.sleep(.15)
            self.assertFalse(any(j["port"]==1 and parse(j["raw"])[0]==1 for j in modem.all_submissions[start:]))
            modem.send(1,0,login(stamp=101))
            modem.receive(lambda j:j["epoch"]==2 and j["port"]==1 and parse(j["raw"])[0]==1)
    def test_room_automatic_delivery_ack_and_keepalive(self):
        with RunningService(native=False) as service:
            modem=service.emulator
            for seed,stamp in ((2,10),(3,20)):
                modem.send(1,0,login(seed=seed,stamp=stamp))
                modem.receive(lambda j:j["port"]==1 and parse(j["raw"])[0]==1)
            modem.send(1,0,addressed(3,4,8,b"\x81\x12\x34\x56\x0f"))
            modem.send(1,0,text(2,4,11,"automatic"))
            modem.receive(lambda j:j["port"]==1 and parse(j["raw"])[0]==3)
            delivered=modem.receive(lambda j:j["port"]==1 and parse(j["raw"])[0]==2,timeout=10)
            plain=body(delivered["raw"],3,4).rstrip(b"\0")
            self.assertEqual(plain[4]&252,8)
            self.assertEqual(plain[5:],public(2)[:4]+b"automatic")
            modem.send(1,0,packet(3,hashlib.sha256(plain+public(3)).digest()[:4]))
            keep=struct.pack("<IBI",21,2,0)
            modem.send(1,0,addressed(3,4,0,keep))
            response=modem.receive(lambda j:j["port"]==1 and parse(j["raw"])[0]==3)
            self.assertEqual(parse(response["raw"])[4],hashlib.sha256(keep+public(3)).digest()[:4]+b"\0")
    def test_burst_rx_batches_are_bounded_and_fair(self):
        with RunningService(native=False) as service:
            modem=service.emulator
            packets=[(0,packet(5,b"batch"+bytes([i]),route=1)) for i in range(80)]
            packets[4:4]=[(1,login()),(2,advert(2)),(2,text(2,5,1700000100,"!ping"))]
            burst=b"".join(kiss(raw,port,0)+kiss(b"\xf9\x12\xb7",port,6) for port,raw in packets)
            with modem.write_lock:modem.connection.sendall(burst)
            service.wait("RX_DROPPED",5)
            modem.receive(lambda j:j["port"]==1 and parse(j["raw"])[0]==1)
            modem.receive(lambda j:j["port"]==2 and parse(j["raw"])[0]==2)
    def test_native_scope_reply_and_home_default_separation(self):
        home=bytes([17])*16; fallback=bytes([34])*16
        with RunningService(bot_home=home,bot_default=fallback) as service:
            modem=service.emulator
            startup=modem.receive(lambda j:j["port"]==2 and parse(j["raw"])[0]==4)
            self.assertEqual(parse(startup["raw"])[1],0)
            self.assertEqual(startup["raw"][1:5],struct.pack("<HH",*[scope_code(fallback,startup["raw"])]*2))
            transcript={"advert":advert(2).hex(),"startup_advert":startup["raw"].hex(),
                        "state":{str(p.relative_to(service.root/"native")):p.read_bytes().hex()
                        for p in (service.root/"native").rglob("*") if p.is_file()},"commands":[]}
            modem.send(2,0,bytes.fromhex(transcript["advert"])); time.sleep(.15)
            requests=[
                (scoped(text(2,5,1700000101,"!ping"),home),home),
                (text(2,5,1700000102,"!ping"),b""),
                (scoped(text(2,5,1700000103,"!ping"),bytes([51])*16),fallback),
                (addressed(2,5,2,struct.pack("<IB",1700000104,0)+b"!ping"),fallback),
                (scoped(text(2,5,1700000105,"!ping"),fallback),fallback)]
            for raw,expected in requests:
                modem.send(2,0,raw)
                reply=modem.receive(lambda j:j["port"]==2 and parse(j["raw"])[0]==2)
                self.assertIn(b"Pong",body(reply["raw"],2,5))
                if expected:
                    self.assertEqual(parse(reply["raw"])[1],0)
                    code=scope_code(expected,reply["raw"])
                    self.assertEqual(reply["raw"][1:5],struct.pack("<HH",code,code))
                else:self.assertEqual(parse(reply["raw"])[1],1)
                transcript["commands"].append({"request":raw.hex(),"reply":reply["raw"].hex()})
            (ROOT/"build/bot-scoped-differential.json").write_text(json.dumps(transcript,indent=2)+"\n")
            # A home selection is not an implicit default TX scope.
            service.stop_process()
            config=service.root/"config"
            config.write_text(config.read_text().replace("bot_default="+fallback.hex(),"bot_default="))
            service.start_process(True)
            startup=modem.receive(lambda j:j["epoch"]==2 and j["port"]==2 and parse(j["raw"])[0]==4)
            self.assertEqual(parse(startup["raw"])[1],1)

if __name__=="__main__":
    unittest.main(verbosity=2)
