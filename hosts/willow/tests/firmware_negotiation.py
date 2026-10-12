"""Willow negotiates with the actual shared-modem C++ firmware, on loopback."""
import argparse
import json
import os
from pathlib import Path
import select
import shutil
import socket
import struct
import subprocess
import threading
import time
import unittest
from unittest.mock import patch

from service_demo import ROOT, RunningService, STATE_SEQUENCE, kiss, packet, public, secret, seal, addressed, advert, parse, body
from build_worker import verify

HARNESS=ROOT/"build/mkiss-native/mkiss-session"
PROFILE=struct.pack("<IIBBBfBh",910525000,62500,7,5,20,1.0,1,0)


def build():
    spec=verify()
    native=ROOT/"build/native"
    upstream=native/("worker-"+spec["source_fingerprint"][:20])/"mesh"
    if not (upstream/"examples/kiss_modem/KissModem.cpp").is_file():
        raise RuntimeError("bound MeshCore source unavailable; run make native-worker")
    HARNESS.parent.mkdir(exist_ok=True)
    subprocess.run(["make","-f",str(ROOT/"../../test_support/phy_parity/Makefile"),
                    "BUILD="+str(HARNESS.parent),"UPSTREAM="+str(upstream),
                    "CRYPTO="+str(native/"deps/Crypto"),
                    "LPP="+str(native/"deps/CayenneLPP/src"),str(HARNESS)],check=True)


class Decoder:
    def __init__(self):
        self.data=bytearray(); self.escaped=False
    def feed(self,chunk):
        frames=[]
        for byte in chunk:
            if byte==192:
                if self.data: frames.append(bytes(self.data))
                self.data.clear(); self.escaped=False
            elif self.escaped:
                if byte not in (220,221): raise ValueError("invalid firmware KISS escape")
                self.data.append(192 if byte==220 else 219); self.escaped=False
            elif byte==219: self.escaped=True
            else: self.data.append(byte)
        return frames


class Peer:
    def __init__(self,port):
        self.socket=socket.create_connection(("127.0.0.1",port),timeout=3)
        self.decoder=Decoder()
    def request(self,command,data):
        self.socket.sendall(kiss(bytes([command])+data,0,6))
        expected=0x9a if command==0x19 else command|128
        deadline=time.monotonic()+3
        while time.monotonic()<deadline:
            for frame in self.decoder.feed(self.socket.recv(4096)):
                if frame[:2]==bytes([6,expected]): return frame[2:]
        raise AssertionError("firmware control reply unavailable")
    def close(self): self.socket.close()


class Firmware:
    def __init__(self):
        self.process=subprocess.Popen([str(HARNESS),"--serve"],
                                      stdout=subprocess.PIPE,stderr=subprocess.PIPE)
        if not select.select([self.process.stdout],[],[],5)[0]:
            self.close(); raise AssertionError("firmware fixture did not announce its port")
        line=self.process.stdout.readline().decode().strip()
        if not line.startswith("MKISS_PORT="):
            self.close(); raise AssertionError("firmware fixture startup failed: "+line)
        self.port=int(line.split("=")[1])
    def close(self):
        self.process.terminate()
        try: self.process.communicate(timeout=5)
        except subprocess.TimeoutExpired:
            self.process.kill(); self.process.communicate()
            raise AssertionError("firmware fixture did not stop")


class Tap:
    """Record firmware controls, optionally interleaving synthetic RX metadata."""
    def __init__(self,target):
        self.listener=socket.socket()
        self.listener.bind(("127.0.0.1",0)); self.listener.listen(2)
        self.listener.settimeout(.1)
        self.port=self.listener.getsockname()[1]
        self.target=target; self.error=None; self.stop=False
        self.requests=[]; self.replies=[]
        self.interleave_signal_metadata=False; self.metadata_insertions=0
        self.interleave_capacity_metadata=False; self.drop_capacity_reply=False
        self.interleave_rf=False; self.epoch=0; self.disconnect_requested=False
        self.bot_probe=False
        self.injected=[]; self.client=None; self.write_lock=threading.Lock()
        self.thread=threading.Thread(target=self.run,daemon=True); self.thread.start()
    def received(self,port,index):
        raw=packet(7,bytes([self.epoch,port,index])*20,route=2)
        signal=bytes([0xf9,12+port+index,(-80-port-index)&255])
        self.injected.append((self.epoch,port,raw,signal))
        return kiss(raw,port,0)+kiss(signal,port,6)
    def disconnect(self):
        self.disconnect_requested=True
    def send(self,port,raw):
        with self.write_lock:
            self.client.sendall(kiss(raw,port,0)+kiss(b"\xf9\x12\xb7",port,6))
    def run(self):
        try:
            while not self.stop:
                try: client,_=self.listener.accept()
                except socket.timeout: continue
                except OSError: return
                self.epoch+=1
                with client, socket.create_connection(("127.0.0.1",self.target),timeout=3) as server:
                    self.client=client
                    decoders={client:Decoder(),server:Decoder()}
                    while not self.stop:
                        if self.disconnect_requested:
                            self.disconnect_requested=False
                            break
                        ready,_,_=select.select([client,server],[],[],.1)
                        ended=False
                        for source in ready:
                            data=source.recv(4096)
                            if not data: ended=True; break
                            records=self.requests if source is client else self.replies
                            frames=decoders[source].feed(data)
                            records.extend(frames)
                            tail=b""
                            if source is server and (self.interleave_signal_metadata or self.interleave_capacity_metadata or self.drop_capacity_reply or self.interleave_rf or self.bot_probe):
                                data=b""
                                for frame in frames:
                                    if self.bot_probe and frame[:2]==b"\x26\x9a":
                                        for raw in (advert(2),addressed(2,5,2,struct.pack("<IB",int(time.time()),0)+b"!ping",width=3)):
                                            data+=kiss(raw,2,0)+kiss(b"\xf9\x12\xb7",2,6)
                                    if self.interleave_rf and len(frame)>=2 and frame[1]==0x9a:
                                        for index in range(9 if frame[0]>>4==0 else 1):
                                            data+=self.received(frame[0]>>4,index)
                                    if len(frame)>=2 and frame[0]&15==6 and frame[1]==0x9a:
                                        if self.interleave_signal_metadata:
                                            data+=kiss(b"\xf9\x12\xb7",frame[0]>>4,6)
                                            self.metadata_insertions+=1
                                    if len(frame)>=2 and frame[0]&15==6 and frame[1]==0xa5:
                                        if self.interleave_capacity_metadata:
                                            data+=kiss(b"\xf9\x33\xbd",frame[0]>>4,6)
                                            self.metadata_insertions+=1
                                        if self.drop_capacity_reply:continue
                                    data+=kiss(frame[1:],frame[0]>>4,frame[0]&15)
                                    if self.interleave_rf and frame[:2]==b"\x26\xa6":
                                        # Frames after the final presence reply, ending inside RX_META.
                                        for port in range(3): data+=self.received(port,10)
                                        data,tail=data[:-3],data[-3:]
                            with self.write_lock:
                                (server if source is client else client).sendall(data)
                                if tail:
                                    time.sleep(.01)
                                    client.sendall(tail)
                        if ended: break
        except Exception as error:
            if not self.stop: self.error=error
    def close(self):
        self.stop=True; self.listener.close(); self.thread.join(5)
        if self.thread.is_alive(): raise AssertionError("firmware wire tap did not stop")


class FirmwareNegotiation(unittest.TestCase):
    def setUp(self):
        self.firmware=Firmware(); self.addCleanup(self.firmware.close)
        self.tap=Tap(self.firmware.port); self.addCleanup(self.tap.close)
    def peer(self,owner=False,session=False):
        peer=Peer(self.firmware.port); self.addCleanup(peer.close)
        self.assertEqual(peer.request(32,bytes([1,int(owner)]))[:2],b"\1\0")
        if session: self.assertEqual(peer.request(37,b"\1\1")[-1],1)
        return peer
    def refused(self,profile):
        root=ROOT.parents[1]/f".f-{os.getpid()}-{next(STATE_SEQUENCE)}"
        for name in ("owner.sock","admin.sock","profile.sock"):
            path=root/name
            if len(os.fsencode(path))>107:
                raise ValueError(f"firmware refusal fixture socket exceeds 107 bytes: {path}")
        root.mkdir(mode=0o700); self.addCleanup(shutil.rmtree,root)
        config=root/"config"
        config.write_text(f"address=127.0.0.1\nport={self.tap.port}\nprofile={profile.hex()}\n"
                          "worker=-\npassword=room\nadmin=admin\nrun_ms=1500\n")
        config.chmod(0o600)
        binary=os.environ.get("MESHCORE_HEW_HOST",str(ROOT/"build/hew-host"))
        result=subprocess.run([binary,str(root)],capture_output=True,text=True,timeout=15)
        self.assertEqual(result.returncode,0,result.stderr)
        self.assertNotIn("ONLINE epoch=",result.stdout)
        self.assertFalse(any(frame[1]==0x22 and frame[3]!=0 for frame in self.tap.requests))
        return result.stdout
    def test_real_firmware_handshake_with_two_per_role_sidecars(self):
        self.tap.interleave_signal_metadata=True
        self.tap.interleave_capacity_metadata=True
        self.tap.bot_probe=True
        observer=self.peer(owner=True); base=self.peer()
        self.assertEqual(observer.request(34,b"\1\0")[6:],PROFILE)
        with patch("service_demo.Emulator",lambda:self.tap), patch("service_demo.PROFILE",PROFILE):
            with RunningService() as service:
                service.wait("BOT_READY")
                deadline=time.monotonic()+5
                replies=[]
                while time.monotonic()<deadline:
                    replies=[frame[20:] for frame in self.tap.requests if frame[:2]==b"\x26\x21" and len(frame)>22 and parse(frame[20:])[0]==2]
                    if replies: break
                    time.sleep(.02)
                self.assertEqual(len(replies),1,service.logs)
                self.assertIn(b"Pong",body(replies[0],2,5))
                self.assertFalse(any("NEGOTIATION_REJECTED" in line for line in service.logs))
                self.assertFalse(any("MODEM_REQUEST_FAILED" in line for line in service.logs),service.logs)
                self.assertEqual(observer.request(34,b"\1\0")[6:],PROFILE)
                self.assertEqual(base.request(34,b"\1\0")[6:],PROFILE)
                for port in range(3):
                    self.assertIn(bytes([(port<<4)|6,0x19,1]),self.tap.requests)
                    self.assertIn(bytes([(port<<4)|6,0x9a,1]),self.tap.replies)
                self.assertEqual(self.tap.metadata_insertions,4)
                self.assertTrue(all(frame[2:]==b"\1\0" for frame in self.tap.requests if frame[1] in (0x20,0x22)))
                self.assertEqual([frame[2] for frame in self.tap.requests if frame[1]==0x0f],list(range(1,256)))
    def test_rf_survives_negotiation_presence_and_reconnect(self):
        self.tap.interleave_rf=True
        with patch("service_demo.Emulator",lambda:self.tap), patch("service_demo.PROFILE",PROFILE):
            with RunningService(native=False) as service:
                def received(epoch,after):
                    expected=[entry for entry in self.tap.injected if entry[0]==epoch]
                    deadline=time.monotonic()+5
                    while time.monotonic()<deadline:
                        lines=[line for line in service.logs[after:] if line.startswith("ROLE_RX ")]
                        if len(lines)>=len(expected): break
                        time.sleep(.02)
                    self.assertEqual(len(expected),14)
                    self.assertEqual(len(lines),len(expected),service.logs)
                    for port in range(3):
                        seen=[line for line in lines if f"port={port} " in line]
                        self.assertEqual(len(seen),sum(item[1]==port for item in expected))
                        self.assertTrue(all("origin=0 " in line for line in seen),seen)
                    self.assertFalse(any("RX_DROPPED" in line for line in service.logs),service.logs)
                    return expected
                received(1,0)
                for port,recipient in ((0,1),(1,4)):
                    login=packet(7,public(recipient)[:1]+public(2)+seal(secret(2,public(recipient)),
                        struct.pack("<I",10)+(bytes(4) if port==1 else b"")+b"admin\0"))
                    self.tap.send(port,login)
                    self.tap.send(port,addressed(2,recipient,2,struct.pack("<IB",11,4)+b"log start"))
                deadline=time.monotonic()+5
                while time.monotonic()<deadline:
                    if all((service.root/(name+".state.packet.log")).exists() for name in ("relay","room")): break
                    time.sleep(.02)
                self.assertTrue(all((service.root/(name+".state.packet.log")).exists() for name in ("relay","room")))
                after=len(service.logs)
                self.tap.disconnect()
                service.wait("ONLINE epoch=2",15,after=after)
                expected=received(2,after)
                for epoch,port,raw,signal in expected:
                    if port==2: continue
                    path=service.root/("relay.state.packet.log" if port==0 else "room.state.packet.log")
                    records=[json.loads(line) for line in path.read_bytes().splitlines()]
                    matches=[row for row in records if row["direction"]=="RX" and row["raw_packet_hex"]==raw.hex()]
                    self.assertEqual(len(matches),1)
                    self.assertEqual((matches[0]["snr"],matches[0]["rssi"]),
                                     (signal[1]/4,signal[2]-256))
    def test_real_firmware_busy_session_is_actionable_without_takeover(self):
        holder=self.peer(session=True)
        log=self.refused(PROFILE)
        self.assertIn("stage=CAPACITY",log)
        self.assertIn("sidecar-radio_session=per_role",log)
        self.assertIn("actual=0104040400",log)
        self.assertEqual(holder.request(34,b"\1\0")[6:],PROFILE)
    def test_real_firmware_profile_mismatch_reports_expected_and_actual(self):
        wrong=bytearray(PROFILE); wrong[0]^=1
        log=self.refused(wrong)
        self.assertIn("stage=CONFIG_GET",log)
        self.assertIn("profile="+wrong.hex(),log)
        self.assertIn(PROFILE.hex(),log)
    def test_capacity_timeout_is_reported_once_after_interleaved_metadata(self):
        self.tap.interleave_capacity_metadata=True
        self.tap.drop_capacity_reply=True
        log=self.refused(PROFILE)
        failures=[line for line in log.splitlines() if line.startswith("MODEM_REQUEST_FAILED")]
        self.assertEqual(len(failures),1,log)
        self.assertIn("stage=CAPACITY",failures[0])
        self.assertIn("actual=f933bd",failures[0])
        self.assertTrue(failures[0].endswith("reason=timeout"),failures[0])


if __name__=="__main__":
    parser=argparse.ArgumentParser()
    parser.add_argument("--build",action="store_true")
    args=parser.parse_args()
    if args.build: build()
    else: unittest.main(argv=[__file__],verbosity=2)
