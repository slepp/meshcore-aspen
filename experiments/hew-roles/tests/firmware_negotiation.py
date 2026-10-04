"""Willow negotiates with the actual shared-modem C++ firmware, on loopback."""
import argparse
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

from service_demo import ROOT, RunningService, kiss
from build_worker import verify

HARNESS=ROOT/"build/mkiss-native/mkiss-session"
PROFILE=struct.pack("<IIBBBfBh",910525000,62500,7,5,20,1.0,0,0)


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
        self.thread=threading.Thread(target=self.run,daemon=True); self.thread.start()
    def run(self):
        try:
            while not self.stop:
                try: client,_=self.listener.accept()
                except socket.timeout: continue
                except OSError: return
                with client, socket.create_connection(("127.0.0.1",self.target),timeout=3) as server:
                    decoders={client:Decoder(),server:Decoder()}
                    while not self.stop:
                        ready,_,_=select.select([client,server],[],[],.1)
                        ended=False
                        for source in ready:
                            data=source.recv(4096)
                            if not data: ended=True; break
                            records=self.requests if source is client else self.replies
                            frames=decoders[source].feed(data)
                            records.extend(frames)
                            if source is server and (self.interleave_signal_metadata or self.interleave_capacity_metadata or self.drop_capacity_reply):
                                data=b""
                                for frame in frames:
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
                            (server if source is client else client).sendall(data)
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
        root=ROOT/"build"/f"firmware-refusal-{os.getpid()}-{time.monotonic_ns()}"
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
        observer=self.peer(owner=True); base=self.peer()
        self.assertEqual(observer.request(34,b"\1\0")[6:],PROFILE)
        with patch("service_demo.Emulator",lambda:self.tap), patch("service_demo.PROFILE",PROFILE):
            with RunningService() as service:
                service.wait("BOT_READY")
                time.sleep(.2)
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
