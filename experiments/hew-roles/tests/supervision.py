"""Actual Willow role/modem supervisor replacement and committed-state tests."""
import os
from pathlib import Path
import signal
import shutil
import subprocess
import struct
import time
import unittest
from unittest.mock import patch

ROOT=Path(__file__).resolve().parents[1]
os.environ.setdefault("MESHCORE_HEW_HOST",str(ROOT/"build/hew-host-supervision"))
from service_demo import RunningService, advert, text, body, login, packet, parse
from service_demo import addressed, public, seal, secret
from service_demo import Emulator, PROFILE
from reconcile_go import snapshot
from willow import owner_rpc
from source_policy import RoleClient


def write_private(path,data):
    path.write_bytes(data)
    path.chmod(0o600)


class Supervision(unittest.TestCase):
    def healthy(self,service,nonce):
        modem=service.emulator
        payload=f"healthy-{nonce}".encode()
        modem.send(0,0,packet(5,payload,route=1,width=3))
        modem.receive(lambda job:job["port"]==0 and parse(job["raw"])[4]==payload)
        # A flood advert can be deferred behind the shorter DM by the native
        # dispatcher. A zero-hop direct advert establishes the contact in order.
        modem.send(2,0,packet(4,parse(advert(2))[4],route=2,width=3))
        modem.send(2,0,text(2,5,1700001500+nonce,"!ping"))
        try:
            reply=modem.receive(lambda job:job["port"]==2 and parse(job["raw"])[0]==2)
        except AssertionError as error:
            raise AssertionError((service.logs[-20:],service.errors[-20:])) from error
        self.assertIn(b"pong",body(reply["raw"],2,5).lower())

    def restored(self,service,generation,after):
        service.wait(f"room.state generation={generation}",after=after)

    def test_room_panic_reloads_committed_history_and_replay(self):
        with RunningService(extra_config="room.retention=durable-replay\n") as service:
            modem=service.emulator
            modem.send(1,0,login(stamp=600))
            modem.receive(lambda job:job["port"]==1 and parse(job["raw"])[0]==1)
            modem.send(1,0,text(2,4,601,"retained across actor replacement"))
            modem.receive(lambda job:job["port"]==1 and parse(job["raw"])[0]==3)
            state=service.root/"room.state"
            committed=state.read_bytes()
            self.assertIn(b"retained across actor replacement",committed)
            before=len(modem.all_submissions); marker=len(service.logs)
            write_private(service.root/"room.state.inject",bytes((1,1)))
            modem.send(1,0,login(stamp=602))
            self.restored(service,2,marker)
            self.assertEqual(state.read_bytes(),committed)
            self.assertFalse(any(j["port"]==1 and parse(j["raw"])[0]==1
                                 for j in modem.all_submissions[before:]))
            self.healthy(service,1)
            modem.send(1,0,login(stamp=602))
            modem.receive(lambda job:job["port"]==1 and parse(job["raw"])[0]==1)
            before=len(modem.all_submissions); marker=len(service.logs)
            write_private(service.root/"room.state.inject",bytes((2,2)))
            modem.send(1,0,login(stamp=603))
            self.restored(service,3,marker)
            after_commit=state.read_bytes()
            self.assertNotEqual(after_commit,committed)
            self.assertIn(b"retained across actor replacement",after_commit)
            modem.send(1,0,login(stamp=603)); time.sleep(.25)
            self.assertFalse(any(j["port"]==1 and parse(j["raw"])[0]==1
                                 for j in modem.all_submissions[before:]))
            modem.send(1,0,login(stamp=604))
            modem.receive(lambda job:job["port"]==1 and parse(job["raw"])[0]==1)
            self.healthy(service,2)
            self.assertEqual(modem.epoch,1)
            self.assertEqual(sum("BOT_PROCESS pid=" in line for line in service.logs),1)
            self.assertFalse(any(j["port"] in (0,1) and parse(j["raw"])[0]==4
                                 and parse(j["raw"])[1]==1
                                 for j in modem.all_submissions[before:]))

    def test_remote_preferences_survive_real_role_replacement(self):
        with RunningService() as service:
            modem=service.emulator
            plain=struct.pack("<II",650,0)+b"admin\0"
            modem.send(1,0,packet(7,public(4)[:1]+public(2)+seal(secret(2,public(4)),plain)))
            modem.receive(lambda item:item["port"]==1 and parse(item["raw"])[0]==1)
            def command(stamp,value):
                modem.send(1,0,addressed(2,4,2,struct.pack("<IB",stamp,4)+value.encode()))
                return modem.receive(lambda item:item["port"]==1 and parse(item["raw"])[0]==2)
            self.assertEqual(body(command(651,"set name Restored Room")["raw"],2,4)[5:].rstrip(b"\0"),b"OK")
            state=service.root/"room.state"
            committed=state.read_bytes()
            self.assertEqual(committed[:4],b"HEW5")
            marker=len(service.logs)
            write_private(service.root/"room.state.inject",bytes((1,1)))
            modem.send(1,0,addressed(2,4,2,struct.pack("<IB",652,4)+b"get name"))
            self.restored(service,2,marker)
            self.assertEqual(state.read_bytes(),committed)
            self.assertEqual(body(command(652,"get name")["raw"],2,4)[5:].rstrip(b"\0"),b"> Restored Room")
            self.healthy(service,3)
            self.assertEqual(modem.epoch,1)
            self.assertEqual(sum("BOT_PROCESS pid=" in line for line in service.logs),1)

    def test_missing_or_corrupt_snapshot_quarantines_only_room(self):
        for missing in (False,True):
            with self.subTest(missing=missing), RunningService(native=False) as service:
                modem=service.emulator
                modem.send(1,0,login(stamp=700))
                modem.receive(lambda job:job["port"]==1 and parse(job["raw"])[0]==1)
                state=service.root/"room.state"; committed=state.read_bytes()
                if missing: state.unlink()
                else: write_private(state,b"corrupt committed snapshot")
                write_private(service.root/"room.state.inject",bytes((1,1)))
                marker=len(service.logs)
                modem.send(1,0,login(stamp=701))
                service.wait("committed role snapshot unavailable or invalid",after=marker)
                if missing: self.assertFalse(state.exists())
                else: self.assertEqual(state.read_bytes(),b"corrupt committed snapshot")
                self.healthy(service,10)
                service.stop_process()
                (service.root/"room.state.inject").unlink()
                service.start_process(False)
                self.assertTrue(any("committed role snapshot unavailable or invalid" in line
                                    for line in service.logs[marker:]))
                if missing: self.assertFalse(state.exists())
                else: self.assertEqual(state.read_bytes(),b"corrupt committed snapshot")
                self.healthy(service,11)
                write_private(state,committed)
                before=len(modem.all_submissions)
                modem.send(1,0,login(stamp=701)); time.sleep(.2)
                self.assertFalse(any(j["port"]==1 for j in modem.all_submissions[before:]))
                service.stop_process(); service.start_process(False)
                modem.send(1,0,login(stamp=701))
                modem.receive(lambda job:job["port"]==1 and parse(job["raw"])[0]==1)

    def test_room_budget_exhaustion_does_not_exhaust_other_branches(self):
        with RunningService(native=False) as service:
            modem=service.emulator
            self.assertEqual(RoleClient(service,1).command("set af 2"),"OK")
            modem.send(1,0,login(stamp=800))
            modem.receive(lambda job:job["port"]==1 and parse(job["raw"])[0]==1)
            committed=(service.root/"room.state").read_bytes()
            for nonce in (1,2,3):
                # An exhausted native supervisor reports failure on final shutdown.
                if nonce==3: service.expected_exit=1
                marker=len(service.logs)
                write_private(service.root/"room.state.inject",bytes((1,nonce)))
                modem.send(1,0,login(stamp=800+nonce))
                if nonce<3: self.restored(service,nonce+1,marker)
                else:
                    deadline=time.monotonic()+5
                    while time.monotonic()<deadline and sum("injected production role incarnation failure" in line for line in service.errors)<3:
                        time.sleep(.02)
                    self.assertEqual(sum("injected production role incarnation failure" in line for line in service.errors),3)
            self.healthy(service,20)
            self.assertEqual(modem.epoch,1)
            self.assertEqual((service.root/"room.state").read_bytes(),committed)
            with self.assertRaisesRegex(ValueError,"outcome-unknown"):
                owner_rpc(service.root,b"\x01\x05\x03get name")
            self.healthy(service,22)
            modem.disconnect()
            service.wait("ONLINE epoch=2",timeout=15)
            self.healthy(service,21)
            self.assertFalse(any("room.state generation=4" in line for line in service.logs))
            self.assertFalse(any(job["port"]==1 and job["epoch"]==2 for job in modem.all_submissions))
            self.assertTrue(any(epoch==2 and port==1 and command==35 and bytes.fromhex(data)[5:]==struct.pack("<f",2)
                                for epoch,port,command,data in modem.controls))

    def test_modem_panic_replaces_socket_and_fences_pending_epoch(self):
        with RunningService() as service:
            modem=service.emulator
            modem.send(1,0,login(stamp=900))
            modem.receive(lambda job:job["port"]==1 and parse(job["raw"])[0]==1)
            committed=(service.root/"room.state").read_bytes()
            modem.hold=True
            modem.send(0,0,packet(5,b"old-supervised-epoch",route=1,width=3))
            job=modem.receive(lambda item:item["port"]==0 and parse(item["raw"])[4]==b"old-supervised-epoch")
            service.wait(f"generation={job['generation']} job={job['job']} state=1")
            marker=len(service.logs)
            write_private(service.root/"modem.inject",b"\1")
            service.wait(f"generation={job['generation']} job={job['job']} state=4",after=marker)
            modem.hold=False
            service.wait("ONLINE epoch=2",timeout=15,after=marker)
            modem.receive(lambda item:item["port"]==1 and item["epoch"]==2 and parse(item["raw"])[0]==4)
            before=snapshot(committed,public(4),3,"before reconnect")
            after=snapshot((service.root/"room.state").read_bytes(),public(4),3,"after reconnect")
            old_clock=before.pop("Clock"); new_clock=after.pop("Clock")
            self.assertGreater(new_clock,old_clock)
            self.assertEqual(after,before)
            adverts=[parse(item["raw"])[4] for item in modem.all_submissions
                     if item["port"]==1 and item["epoch"]==2 and parse(item["raw"])[0]==4]
            self.assertTrue(adverts)
            self.assertEqual(struct.unpack_from("<I",adverts[-1],32)[0],new_clock)
            self.healthy(service,30)
            modem.send(1,0,login(stamp=901))
            modem.receive(lambda item:item["port"]==1 and parse(item["raw"])[0]==1)
            self.assertEqual(sum("BOT_PROCESS pid=" in line for line in service.logs),1)
            self.assertEqual(sum(parse(item["raw"])[4]==b"old-supervised-epoch" for item in modem.all_submissions),1)
            self.assertEqual(sum("room.state generation=" in line for line in service.logs),1)

    def test_native_owner_panic_replaces_child_and_preserves_bot_data(self):
        with RunningService() as service:
            modem=service.emulator
            self.healthy(service,40)
            modem.send(2,0,text(2,5,1700001600,"!remember supervision preserved"))
            modem.receive(lambda item:item["port"]==2 and parse(item["raw"])[0]==2)
            pid=int(service.wait("BOT_PROCESS pid=").split("pid=")[1])
            marker=len(service.logs)
            write_private(service.root/"native-owner.inject",b"\1")
            service.wait("BOT_OFFLINE",after=marker)
            modem.send(0,0,packet(5,b"native-owner-restarting",route=1,width=3))
            modem.send(1,0,login(stamp=1100))
            modem.receive(lambda item:item["port"]==0 and parse(item["raw"])[4]==b"native-owner-restarting")
            modem.receive(lambda item:item["port"]==1 and parse(item["raw"])[0]==1)
            service.wait("BOT_READY",after=marker)
            replacement=int(service.wait("BOT_PROCESS pid=",after=marker).split("pid=")[1])
            self.assertNotEqual(pid,replacement)
            with self.assertRaises(ProcessLookupError): os.kill(pid,0)
            self.healthy(service,41)
            modem.send(2,0,text(2,5,1700001601,"!recall supervision"))
            reply=modem.receive(lambda item:item["port"]==2 and parse(item["raw"])[0]==2)
            self.assertIn(b"preserved",body(reply["raw"],2,5))
            self.assertEqual(modem.epoch,1)

    def test_initialized_state_starts_with_native_worker(self):
        service = RunningService.__new__(RunningService)
        service.root = ROOT.parents[1]/f".i-{os.getpid()}"
        service.emulator = Emulator()
        service.emulator.identity_root = service.root
        service.logs, service.errors = [], []
        try:
            completed = subprocess.run([os.environ["MESHCORE_HEW_HOST"],"init",str(service.root),"--address","127.0.0.1",
                                        "--port",str(service.emulator.port),"--profile",PROFILE.hex()],
                                       capture_output=True,text=True,timeout=10)
            self.assertEqual(completed.returncode,0,completed.stderr)
            service.start_process(True)
        finally:
            if hasattr(service,"proc"):
                service.close()
            else:
                service.emulator.close()
                shutil.rmtree(service.root,ignore_errors=True)

    def test_init_failure_after_creation_removes_partial_state(self):
        root = ROOT/"build"/f"init-cleanup-{os.getpid()}"
        root.mkdir(mode=0o700)
        try:
            (root/"state.init-fail.inject").write_bytes(b"1")
            completed = subprocess.run([os.environ["MESHCORE_HEW_HOST"],"init",str(root/"state"),"--address","127.0.0.1",
                                        "--port","5000","--profile","c806643690d003000705020000803f010000"],
                                       capture_output=True,text=True,timeout=10)
            self.assertNotEqual(completed.returncode,0)
            self.assertIn("no state was kept",completed.stdout+completed.stderr)
            self.assertEqual(sorted(path.name for path in root.iterdir()),["state.init-fail.inject"])
        finally:
            shutil.rmtree(root)

    def test_worker_verification_failure_on_relaunch_keeps_relay_and_room(self):
        with RunningService() as service:
            modem=service.emulator
            self.healthy(service,50)
            pid=int(service.wait("BOT_PROCESS pid=").split("pid=")[1])
            marker=len(service.logs)
            inject=service.root/"native-verify.inject"
            write_private(inject,b"\1")
            os.kill(pid,signal.SIGKILL)
            service.wait("BOT_OFFLINE native worker verification failed",after=marker)
            service.wait("BOT_OFFLINE native worker launch failed retry_ms=500",after=marker)
            service.wait("BOT_OFFLINE native worker launch failed retry_ms=1000",timeout=5,after=marker)
            modem.send(0,0,packet(5,b"verify-refused",route=1,width=3))
            modem.send(1,0,login(stamp=1200))
            modem.receive(lambda item:item["port"]==0 and parse(item["raw"])[4]==b"verify-refused")
            modem.receive(lambda item:item["port"]==1 and parse(item["raw"])[0]==1)
            self.assertFalse(any("BOT_PROCESS pid=" in line for line in service.logs[marker:]))
            inject.unlink()
            service.wait("BOT_READY",timeout=15,after=marker)
            self.healthy(service,51)
            self.assertEqual(modem.epoch,1)

    def test_normal_binary_cannot_arm_test_file_panics(self):
        normal=os.environ.get("MESHCORE_HEW_NORMAL_HOST",str(ROOT/"build/hew-host"))
        with patch.dict(os.environ,{"MESHCORE_HEW_HOST":normal}), RunningService(native=False) as service:
            write_private(service.root/"room.state.inject",bytes((1,1)))
            write_private(service.root/"modem.inject",b"\1")
            service.emulator.send(1,0,login(stamp=1000))
            service.emulator.receive(lambda item:item["port"]==1 and parse(item["raw"])[0]==1)
            self.assertEqual(service.emulator.epoch,1)
            self.assertFalse(any("incarnation failure" in line for line in service.errors))


if __name__=="__main__": unittest.main(verbosity=2)
