#!/usr/bin/env python3
"""Host-only differential tests. Keys are deterministic test identities."""
import hashlib
import hmac
import json
import os
import shutil
from pathlib import Path
import struct
import subprocess
import unittest

from cryptography.hazmat.primitives import serialization
from cryptography.hazmat.primitives.asymmetric import ed25519, x25519
from cryptography.hazmat.primitives.ciphers import Cipher, algorithms, modes
import ctypes

ROOT = Path(__file__).resolve().parents[1]
BUILD = ROOT / "build"
BIN = Path(os.environ.get("MESHCORE_HEW_BIN", BUILD / "roles"))
DATA = json.loads(subprocess.check_output([str(BUILD / "oracle")], text=True))
NATIVE = [json.loads(line) for line in (ROOT.parents[1] / "testdata/parity/native-events.jsonl").read_text().splitlines()]
SODIUM = ctypes.CDLL("libsodium.so.23")
def public(n):
    return ed25519.Ed25519PrivateKey.from_private_bytes(bytes([n])*32).public_key().public_bytes(
        serialization.Encoding.Raw, serialization.PublicFormat.Raw)
def secret(n, peer):
    point = ctypes.create_string_buffer(32)
    assert SODIUM.crypto_sign_ed25519_pk_to_curve25519(point, peer) == 0
    scalar = bytearray(hashlib.sha512(bytes([n])*32).digest()[:32])
    scalar[0] &= 248
    scalar[31] = (scalar[31] & 127) | 64
    return x25519.X25519PrivateKey.from_private_bytes(bytes(scalar)).exchange(
        x25519.X25519PublicKey.from_public_bytes(point.raw))
def seal(key, plain):
    cipher = Cipher(algorithms.AES(key[:16]), modes.ECB()).encryptor()
    encrypted = cipher.update(plain + bytes((-len(plain)) % 16)) + cipher.finalize()
    return hmac.digest(key, encrypted, "sha256")[:2] + encrypted
def unseal(key, data):
    assert hmac.compare_digest(data[:2], hmac.digest(key, data[2:], "sha256")[:2])
    cipher = Cipher(algorithms.AES(key[:16]), modes.ECB()).decryptor()
    return cipher.update(data[2:]) + cipher.finalize()
def packet(kind, payload, route=2, width=1, path=b"", codes=b""):
    return bytes([(kind << 2) | route]) + codes + bytes([((width-1)<<6) | (len(path)//width)]) + path + payload
def parse(raw):
    raw = bytes.fromhex(raw) if isinstance(raw, str) else raw
    at = 5 if raw[0] & 3 in (0,3) else 1
    plen = raw[at]; at += 1
    size = ((plen >> 6)+1)*(plen&63)
    return raw[0]>>2, raw[0]&3, plen, raw[at:at+size], raw[at+size:]
def dm(n, kind, plain, **kw):
    key = public(n)
    return packet(kind, public(1)[:1]+key[:1]+seal(secret(n,public(1)),plain), **kw)
def login(n, stamp, password="room", since=0, **kw):
    return packet(7, public(1)[:1]+public(n)+seal(secret(n,public(1)),
        struct.pack("<II",stamp,since)+password.encode()+b"\0"), **kw)
def text(n, stamp, value, flags=0):
    return dm(n,2,struct.pack("<IB",stamp,flags)+value)
def body(raw, n):
    return unseal(secret(n,public(1)),parse(raw)[4][2:])
def tx(lines):
    return [line.split()[2] for line in lines if line.startswith("TX ")]
def kiss(data, port=0, cmd=0):
    data = bytes([(port<<4)|cmd])+data
    return b"\xc0"+data.replace(b"\xdb",b"\xdb\xdd").replace(b"\xc0",b"\xdb\xdc")+b"\xc0"

class Host:
    def __init__(self, role=3, width=3, region="-", channel="-", seed=None, expected=None, binary=BIN, env=None):
        self.proc = subprocess.Popen([str(binary),"serve",str(role),seed or "01"*32,"Hew Room","room","admin",str(width),region,channel],
            stdin=subprocess.PIPE,stdout=subprocess.PIPE,stderr=subprocess.PIPE,text=True,bufsize=1,env=env)
        assert self.proc.stdout.readline().strip() == "READY "+(expected or DATA["role"])
    def command(self, command):
        self.proc.stdin.write(command+"\n"); self.proc.stdin.flush()
        lines=[]
        while True:
            line=self.proc.stdout.readline()
            if not line:
                raise AssertionError(f"native process exited ({self.proc.poll()}): {self.proc.stderr.read()}")
            if line.strip()=="END": return lines
            lines.append(line.strip())
    def packet(self, raw, now=1_000_000, local=False):
        return tx(self.command(f"packet {now} {int(local)} {raw.hex()}"))
    def close(self):
        self.proc.stdin.close()
        self.proc.wait(timeout=10)
        error=self.proc.stderr.read()
        self.proc.stdout.close(); self.proc.stderr.close()
        assert self.proc.returncode==0,error
    def __enter__(self): return self
    def __exit__(self,*_): self.close()

class Parity(unittest.TestCase):
    def test_pinned_native_corpus(self):
        with Host() as h:
            for event in NATIVE:
                if event["event"] == "path_valid":
                    self.assertEqual(h.command(f"path {event['encoded']}"),[str(event["valid"]).lower()])
                if event["event"] == "transport_code":
                    self.assertEqual(h.command(f"region {event['key']} {event['payload_type']} {event['payload']}"),[str(event["code"])])
                if event["event"] == "datagram_inputs":
                    reference=next(e for e in NATIVE if e["event"]=="encrypted_packet")
                    payload=parse(reference["bytes"])[4]
                    self.assertEqual(h.command(f"seal {event['secret']} {event['plaintext']}"),["HEX "+payload[2:].hex()])
                if event["event"] == "ack_hash":
                    plain=bytes.fromhex(event["plaintext"])
                    offset=9 if event["signed"] else 5
                    length=plain.find(b"\0",offset)
                    if length<0: length=len(plain)
                    key=event["recipient_public_key"] if event["signed"] else event["sender_public_key"]
                    self.assertEqual(h.command(f"ack {plain[:length].hex()} {key}"),["HEX "+event["ack_hash"]])
    def test_crypto_go_differential(self):
        self.assertEqual(secret(1,public(2)).hex(),DATA["secret"])
        with Host() as h:
            for case in DATA["cipher"]:
                self.assertEqual(h.command(f"seal {DATA['secret']} {case['plain']}"),["HEX "+case["sealed"]])
                got=h.command(f"open {DATA['secret']} {case['sealed']}")[0][4:]
                self.assertEqual(got,case["plain"]+"00"*((-len(bytes.fromhex(case["plain"])))%16))
            bad=bytearray.fromhex(DATA["cipher"][0]["sealed"]); bad[0]^=1
            self.assertEqual(h.command(f"open {DATA['secret']} {bad.hex()}"),["HEX"])
    def test_packets_go_differential_and_malformed(self):
        with Host() as h:
            for p in DATA["packets"]:
                self.assertEqual(h.command("wire "+p),["WIRE "+p])
            for p in ("00","12c0","1281","52","1200"+"ff"*185,"1201"):
                self.assertEqual(h.command("wire "+p),["INVALID"])
    def test_signed_advert_go_exact(self):
        with Host(width=1) as h:
            self.assertEqual(h.command("advert 1000"),["WIRE "+DATA["advert"]])
        with Host(width=3) as h:
            raw=bytes.fromhex(h.command("advert 1000")[0].split()[1])
            self.assertEqual(raw[1],128)
            self.assertEqual(raw[2:],bytes.fromhex(DATA["advert"])[2:])
    def test_relay_policy_go_differential(self):
        for scoped in (False,True):
            with Host(role=2,region=DATA["region"] if scoped else "-") as h:
                for case in DATA["forwarding"]:
                    if case["scoped"]!=scoped: continue
                    got=h.packet(bytes.fromhex(case["raw"]))
                    self.assertEqual(got,[case["want"]] if case["want"] else [],case)
                    self.assertEqual(h.packet(bytes.fromhex(case["raw"])),[])
    def test_relay_local_loop_and_direct(self):
        with Host(role=2) as h:
            p=packet(5,b"abc",route=1,width=3)
            self.assertEqual(h.packet(p,local=True),[])
            p=packet(5,b"xyz",route=1,width=3,path=public(1)[:3])
            self.assertEqual(h.packet(p),[])
            p=packet(5,b"direct",width=3,path=public(1)[:3]+b"123")
            result=h.packet(p)
            self.assertEqual(bytes.fromhex(result[0]),packet(5,b"direct",width=3,path=b"123"))
    def test_kiss_stream_escape_fragment_recovery(self):
        with Host() as h:
            frame=kiss(b"\xc0\xdb\0",port=2)
            result=[]
            for b in frame: result += h.command("kiss "+bytes([b]).hex())
            self.assertEqual(result,["FRAME 20c0db00"])
            self.assertEqual(h.command("kiss c000db00c0"),[])
            self.assertEqual(h.command("kiss "+(b"\xc0"+b"a"*515+b"\xc0").hex()),[])
            self.assertEqual(h.command("kiss "+kiss(b"ok").hex()),["FRAME 006f6b"])
    def test_room_authenticated_login_paths_post_retry(self):
        with Host() as h:
            self.assertEqual(h.packet(login(2,10,"wrong")),[])
            reply=h.packet(login(2,10,route=1,width=3,path=b"abcdef"))[0]
            plain=body(reply,2)
            self.assertEqual(parse(reply)[:3],(8,1,128))
            self.assertEqual(plain[:8],b"\x82abcdef\x01")
            self.assertEqual(plain[12:16],b"\0\0\0\2")
            self.assertEqual(h.packet(login(2,10)),[])
            self.assertEqual(len(h.packet(login(3,20))),1)
            raw=text(2,11,b"hello")
            reply=h.packet(raw)[0]
            self.assertEqual(parse(reply)[4],hashlib.sha256(struct.pack("<IB",11,0)+b"hello"+public(2)).digest()[:4])
            self.assertEqual(len(h.packet(text(2,11,b"hello",1))),1)
            self.assertEqual(h.packet(text(2,9,b"old")),[])
            self.assertIn("posts=1",h.command("status")[0])
    def test_room_sync_ack_keepalive_suppression(self):
        with Host() as h:
            h.packet(login(2,10)); h.packet(login(3,20))
            h.packet(dm(3,8,b"\x81xyz\x0f"))
            h.packet(text(2,11,b"hello"))
            pushes=[]
            for now in (1_010_000,1_010_150): pushes+=tx(h.command(f"tick {now}"))
            self.assertEqual(len(pushes),1)
            raw=pushes[0]; plain=body(raw,3)
            self.assertEqual(parse(raw)[:4],(2,2,129,b"xyz"))
            self.assertEqual(plain[4]&252,8)
            self.assertEqual(plain[5:14],public(2)[:4]+b"hello")
            h.packet(packet(3,hashlib.sha256(plain[:14]+public(1)).digest()[:4]))
            self.assertNotIn("pending=0",h.command("cursor "+public(3).hex())[0])
            h.packet(packet(3,hashlib.sha256(plain[:14]+public(3)).digest()[:4]))
            cursor=h.command("cursor "+public(3).hex())[0]
            self.assertIn("pending=0",cursor)
            self.assertIn(f"since={int.from_bytes(plain[:4],'little')}",cursor)
            keep=struct.pack("<IBI",21,2,0)
            response=h.packet(dm(3,0,keep))[0]
            self.assertEqual(parse(response)[4],hashlib.sha256(keep+public(3)).digest()[:4]+b"\0")
            self.assertEqual(tx(h.command("tick 1030000")),[])
    def test_room_permissions_and_capacity(self):
        for permission in (0,1,2,3,129,130,131):
            with Host() as h:
                h.command(f"acl {public(2).hex()} {permission}")
                self.assertEqual(len(h.packet(login(2,0,""))),1)
                self.assertEqual(len(h.packet(text(2,1,b"post"))),int(permission&3>=2))
                self.assertEqual(len(h.packet(text(2,2,b"get name",4))),int(permission&3==3))
        with Host() as h:
            for n in range(2,22): self.assertEqual(h.command(f"acl {public(n).hex()} 2"),["true"])
            self.assertEqual(h.command(f"acl {public(22).hex()} 2"),["false"])
    def test_room_three_failed_pushes(self):
        with Host() as h:
            h.packet(login(2,10)); h.packet(login(3,20)); h.packet(text(2,11,b"hello"))
            count=0
            for n in range(12):
                count+=len(tx(h.command(f"tick {1_010_000+n*13000}")))
            self.assertEqual(count,3)
            self.assertIn("failures=3",h.command("cursor "+public(3).hex())[0])
            h.packet(dm(3,0,struct.pack("<IBI",21,2,0)))
            self.assertIn("failures=0",h.command("cursor "+public(3).hex())[0])
    def test_bot_signed_contacts_dm_dedup_and_reflection(self):
        with Host(role=1) as h:
            key=public(2); app=b"\x81Alice"; signed=key+struct.pack("<I",10)+app
            sig=ed25519.Ed25519PrivateKey.from_private_bytes(b"\2"*32).sign(signed)
            h.packet(packet(4,key+struct.pack("<I",10)+sig+app,route=1))
            self.assertIn("members=1",h.command("status")[0])
            forged=bytearray(sig); forged[0]^=1
            h.packet(packet(4,public(3)+struct.pack("<I",10)+bytes(forged)+app,route=1))
            self.assertIn("members=1",h.command("status")[0])
            self.assertEqual(h.packet(text(2,10,b"!ping"),local=True),[])
            out=h.packet(text(2,11,b"!ping"))
            self.assertEqual(len(out),2)
            self.assertEqual(body(out[1],2)[5:].rstrip(b"\0"),b"Pong")
            self.assertEqual(h.packet(text(2,11,b"!ping",1)),[])
            self.assertIn("commands=1",h.command("status")[0])
            out=h.packet(text(2,12,b"!admin bot status"))
            self.assertIn(b"trusted-owner",body(out[-1],2))
            h.command(f"acl {key.hex()} 3")
            out=h.packet(text(2,13,b"!admin bot status"))
            self.assertIn(b"commands=",body(out[-1],2))
    def test_bot_bounded_request_dedup_and_expiry(self):
        with Host(role=1) as h:
            h.command(f"acl {public(2).hex()} 0")
            for stamp in range(1,65):
                self.assertEqual(len(h.packet(text(2,stamp,b"!ping"))),2)
            self.assertEqual(h.packet(text(2,65,b"!ping")),[])
            self.assertEqual(len(h.packet(text(2,65,b"!ping"),now=1_120_001)),2)
    def test_bot_group_auth_and_private_notes(self):
        channel=hashlib.sha256(b"#hew-test").digest()[:16]
        with Host(role=1,channel=channel.hex()) as h:
            for n in (2,3): h.command(f"acl {public(n).hex()} 0")
            plain=struct.pack("<IB",10,0)+b"Alice: !ping"
            p=packet(5,hashlib.sha256(channel).digest()[:1]+seal(channel,plain),route=1)
            self.assertEqual(h.packet(p,local=True),[])
            out=h.packet(p)
            self.assertEqual(len(out),1)
            self.assertEqual(unseal(channel,parse(out[0])[4][1:])[5:].rstrip(b"\0"),b"Hew Room: Pong")
            p=bytearray(p); p[-1]^=1
            self.assertEqual(h.packet(bytes(p)),[])
            h.packet(text(2,11,b"!remember camp bring tea"))
            out=h.packet(text(3,12,b"!recall camp"))
            self.assertEqual(body(out[-1],3)[5:].rstrip(b"\0"),b"No note: camp")
            out=h.packet(text(2,13,b"!recall camp"))
            self.assertEqual(body(out[-1],2)[5:].rstrip(b"\0"),b"bring tea")
    def test_bot_note_arguments_roundtrip_and_nonmutation(self):
        root=BUILD/f"note-roundtrip-{os.getpid()}"
        root.mkdir(mode=0o700)
        state=root/"bot.state"
        stamp=100
        try:
            with Host(role=1) as h:
                self.assertEqual(h.command("state "+str(state)),["LOCKED"])
                h.command(f"acl {public(2).hex()} 0")
                def command(value):
                    nonlocal stamp
                    stamp+=1
                    return body(h.packet(text(2,stamp,value.encode()))[-1],2)[5:].rstrip(b"\0")
                accepted=[
                    ("!remember  camp   bring  tea","camp","bring  tea"),
                    ("!remember "+"k"*32+" value","k"*32,"value"),
                    ("!remember max "+"v"*120,"max","v"*120)]
                for request,key,value in accepted:
                    self.assertIn(b" created; committed",command(request))
                    with Host(role=1) as restored:
                        self.assertEqual(restored.command("load "+str(state)),["true"])
                        result=restored.packet(text(2,stamp+1000,("!recall "+key).encode()))
                        self.assertEqual(body(result[-1],2)[5:].rstrip(b"\0"),value.encode())
                before=command("!notes")
                for request in ("!remember","!remember ","!remember    ","!remember  value",
                                "!remember camp ","!remember camp    ",
                                "!remember "+"k"*33+" x",
                                "!remember camp "+"v"*121):
                    with self.subTest(request=request):
                        snapshot=state.read_bytes()
                        self.assertIn(b"Error: !remember requires",command(request))
                        self.assertEqual(state.read_bytes(),snapshot)
                        self.assertEqual(command("!notes"),before)
                        with Host(role=1) as restored:
                            self.assertEqual(restored.command("load "+str(state)),["true"])
                            result=restored.packet(text(2,stamp+1000,b"!notes"))
                            self.assertEqual(body(result[-1],2)[5:].rstrip(b"\0"),before)
                self.assertEqual(command("!recall   camp"),b"bring  tea")
        finally:
            shutil.rmtree(root)

    def test_bot_deferred_observations_and_close(self):
        with Host(role=1) as h:
            h.command(f"acl {public(2).hex()} 0")
            plain=struct.pack("<IB",10,0)+b"!mt 5"
            first=dm(2,2,plain,route=1,width=3,path=b"abc")
            second=dm(2,2,plain,route=1,width=3,path=b"xyz")
            self.assertEqual(len(h.packet(first)),1)  # immediate DM ACK, not completion
            self.assertEqual(h.packet(second,now=1_000_500),[])
            self.assertEqual(tx(h.command("tick 1004999")),[])
            out=tx(h.command("tick 1005000"))
            self.assertEqual(len(out),1)
            self.assertEqual(body(out[0],2)[5:].rstrip(b"\0"),b"2 unique paths in 5000 ms; 3:616263 | 3:78797a")
            self.assertEqual(tx(h.command("tick 1006000")),[])
            h.packet(text(2,11,b"!mt"),now=1_010_000)
            h.command("close")
            self.assertEqual(tx(h.command("tick 1020000")),[])
    def test_modem_queued_bytes_and_terminal_outcomes(self):
        with Host() as h:
            raw=packet(5,b"\xc0\xdb",route=1)
            result=h.command(f"submit 1 300 2000 {raw.hex()}")[0][5:]
            expected=b"\x21\x01"+struct.pack("<II",1,1)+b"\1"+struct.pack("<II",300,2000)+raw
            self.assertEqual(bytes.fromhex(result),kiss(expected,cmd=6))
            event=lambda state,g=1: b"\1"+struct.pack("<II",g,1)+bytes([state,0])+bytes(12)
            self.assertEqual(h.command("event "+event(1,2).hex()),["ignored"])
            self.assertEqual(h.command("event "+event(1).hex()),["job=1 state=1"])
            self.assertEqual(h.command("event "+event(2).hex()),["job=1 state=2"])
            self.assertEqual(h.command("event "+event(2).hex()),["ignored"])
            h.command(f"submit 1 0 0 {raw.hex()}")
            self.assertEqual(h.command("disconnect"),["1"])
            self.assertEqual(h.command(f"submit 1 0 0 {raw.hex()}"),["KISS"])
    def test_persistence_restart_and_failure_closed(self):
        state=BUILD/f"state-{os.getpid()}"
        with Host() as h:
            h.packet(login(2,10))
            h.packet(dm(2,8,b"\x81abc\x0f"))
            self.assertEqual(h.command("save "+str(state)),["true"])
        self.assertEqual(state.stat().st_mode&0o777,0o600)
        with Host() as h:
            self.assertEqual(h.command("load "+str(state)),["true"])
            self.assertIn("active=false",h.command("cursor "+public(2).hex())[0])
            self.assertIn("known=true",h.command("cursor "+public(2).hex())[0])
            self.assertEqual(h.packet(login(2,10)),[])
            self.assertEqual(len(h.packet(login(2,0,""))),1)
            self.assertEqual(h.command("save "+str(BUILD/"missing"/"state")),["false"])
            self.assertIn("closed=true",h.command("status")[0])
            self.assertEqual(h.packet(text(2,11,b"lost")),[])
        state.unlink()
    def test_private_identity_locking_and_automatic_state(self):
        path=BUILD/f"identity-{os.getpid()}"
        state=BUILD/f"automatic-{os.getpid()}"
        generated=subprocess.check_output([str(BIN),"identity",str(path)],text=True,timeout=10).strip()
        self.assertEqual(path.stat().st_mode&0o777,0o600)
        expected=ed25519.Ed25519PrivateKey.from_private_bytes(path.read_bytes()).public_key().public_bytes(
            serialization.Encoding.Raw,serialization.PublicFormat.Raw).hex()
        self.assertEqual(generated,expected)
        with Host(seed="@"+str(path),expected=generated) as h:
            self.assertEqual(h.command("state "+str(state)),["LOCKED"])
            h.command(f"acl {public(2).hex()} 130")
            with Host(seed="@"+str(path),expected=generated) as other:
                self.assertEqual(other.command("state "+str(state)),["ERROR state lock"])
        with Host(seed="@"+str(path),expected=generated) as h:
            self.assertEqual(h.command("state "+str(state)),["LOCKED"])
            self.assertIn("permission=130",h.command("cursor "+public(2).hex())[0])
        for item in (path,state,Path(str(path)+".lock"),Path(str(state)+".lock")): item.unlink()
    def test_native_concurrency_and_lifetime(self):
        for _ in range(5):
            out=subprocess.check_output([str(BIN),"selftest"],text=True,timeout=20)
            self.assertIn("PASS",out)

if __name__=="__main__":
    unittest.main(verbosity=2)
