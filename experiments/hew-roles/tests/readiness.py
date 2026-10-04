"""Isolated RF-readiness regressions; never opens a radio."""
import os
import signal
import struct
import time
import unittest

from parity import Host, public, login as room_login, text as room_text
from service_demo import RunningService, advert, text, body, login, packet, parse


class Readiness(unittest.TestCase):
    def test_workerless_airtime_delay_and_trace(self):
        with Host(role=2,region="11"*16) as host:
            raw=packet(5,b"unscoped-with-region",route=1,width=3)
            self.assertEqual([bytes.fromhex(value) for value in host.packet(raw)],
                             [packet(5,b"unscoped-with-region",route=1,width=3,path=public(1)[:3])])
        with RunningService(native=False) as service:
            modem=service.emulator
            self.assertEqual(sum(op==15 for _,_,op,_ in modem.controls),255)
            delays=[]
            for n in range(12):
                payload=b"jitter"+bytes([n])
                modem.send(0,0,packet(5,payload,route=1,width=3))
                job=modem.receive(lambda j:j["port"]==0 and parse(j["raw"])[4]==payload)
                bound=5*int((10+len(job["raw"]))*0.5)
                self.assertLessEqual(job["delay"],bound)
                delays.append(job["delay"])
            self.assertTrue(any(delays))
            for flags in range(4):
                width=1<<flags
                payload=struct.pack("<IIB",900+flags,123,flags)+public(1)[:width]*2+public(2)[:width]
                raw=b"\x26\0"+payload
                modem.send(0,0,raw,metadata=b"\xf9\x12\xb7")
                first=modem.receive(lambda j:j["port"]==0 and j["raw"][0]==0x26)
                self.assertEqual(first["raw"],b"\x26\1\x12"+payload)
                self.assertEqual(first["priority"],5)
                modem.send(0,0,b"\x26\1\x08"+payload,metadata=b"\xf9\xfc\xb7")
                second=modem.receive(lambda j:j["port"]==0 and j["raw"][0]==0x26)
                self.assertEqual(second["raw"],b"\x26\2\x08\xfc"+payload)
                self.assertLessEqual(second["delay"],5*int((10+len(second["raw"]))*0.2))
            service.stop_process()
            modem.bad_airtime=True
            marker=len(service.logs)
            service.start_process(False)
            self.assertTrue(any("negotiation/profile rejected" in line for line in service.logs[marker:]))
            self.assertTrue(any("stage=GET_AIRTIME" in line and "size=64" in line
                                and "actual=00000000" in line for line in service.logs[marker:]))
            self.assertFalse(modem.bad_airtime)

    def test_reconnect_and_restart_do_not_reflood_roles(self):
        with RunningService(native=False) as service:
            modem=service.emulator
            for port in (0,1):
                first=modem.receive(lambda j:j["port"]==port and parse(j["raw"])[0]==4)
                self.assertEqual(parse(first["raw"])[1],1)
            initial=(service.root/"advert-times").read_bytes()
            modem.disconnect()
            service.wait("ONLINE epoch=2",timeout=15)
            for port in (0,1):
                again=modem.receive(lambda j:j["epoch"]==2 and j["port"]==port and parse(j["raw"])[0]==4)
                self.assertEqual(parse(again["raw"])[1],2)
                self.assertEqual(parse(again["raw"])[3],b"")
            self.assertEqual((service.root/"advert-times").read_bytes(),initial)
            service.stop_process(); service.start_process(False)
            for port in (0,1):
                again=modem.receive(lambda j:j["epoch"]==3 and j["port"]==port and parse(j["raw"])[0]==4)
                self.assertEqual(parse(again["raw"])[1],2)
            self.assertEqual((service.root/"advert-times").read_bytes(),initial)

    def test_native_worker_fault_isolated_and_old_jobs_fenced(self):
        with RunningService() as service:
            modem=service.emulator
            modem.send(2,0,advert(2)); time.sleep(.15)
            modem.hold=True
            modem.send(2,0,text(2,5,1700000910,"!ping"))
            held=modem.receive(lambda j:j["port"]==2 and parse(j["raw"])[0]==2)
            pid=int(service.wait("BOT_PROCESS pid=").split("pid=")[1])
            after=len(service.logs)
            os.kill(pid,signal.SIGKILL)
            service.wait("BOT_OFFLINE",after=after)
            modem.send(0,0,packet(5,b"worker-down",route=1,width=3))
            modem.send(1,0,login(stamp=200))
            modem.receive(lambda j:j["port"]==0 and parse(j["raw"])[4]==b"worker-down")
            modem.receive(lambda j:j["port"]==1 and parse(j["raw"])[0]==1)
            service.wait("BOT_READY",after=after)
            self.assertEqual(modem.epoch,1)
            self.assertTrue(any(f"job={held['job']} state=4" in line for line in service.logs))
            modem.event(2,held["generation"],held["job"],2,modem.connection)
            modem.send(2,0,advert(2)); time.sleep(.15)
            modem.send(2,0,text(2,5,1700000911,"!ping"))
            reply=modem.receive(lambda j:j["port"]==2 and parse(j["raw"])[0]==2)
            self.assertIn(b"Pong",body(reply["raw"],2,5))
            self.assertEqual(modem.epoch,1)

    def test_room_public_opt_in_and_admin_safe_eviction(self):
        with Host() as host:
            self.assertEqual(host.command("configure "+b"password=\n".hex()),["false"])
            self.assertEqual(host.command("configure "+b"password=\npublic=1\n".hex()),["true"])
            self.assertEqual(len(host.packet(room_login(2,10,""))),1)
        with Host() as host:
            self.assertEqual(host.command("configure "+b"evict=1\n".hex()),["true"])
            for seed in range(2,22):
                self.assertEqual(host.command(f"acl {public(seed).hex()} {2 if seed<4 else 3}"),["true"])
            host.packet(room_login(2,10))
            host.packet(room_login(3,20))
            host.packet(room_text(2,11,b"recent post"))
            self.assertEqual(len(host.packet(room_login(22,30))),1)
            self.assertEqual(host.command("cursor "+public(3).hex()),["absent"])
            self.assertNotEqual(host.command("cursor "+public(2).hex()),["absent"])
            for seed in range(4,22):
                self.assertIn("permission=3",host.command("cursor "+public(seed).hex())[0])
        with Host() as host:
            self.assertEqual(host.command("configure "+b"evict=1\n".hex()),["true"])
            for seed in range(2,22): host.command(f"acl {public(seed).hex()} 3")
            self.assertEqual(host.packet(room_login(22,30,"admin")),[])
            self.assertEqual(len(host.packet(room_login(2,0,""))),1)

    def test_modem_reconnect_retains_contacts_and_cancels_collectors(self):
        with RunningService() as service:
            modem=service.emulator
            modem.send(2,0,advert(2)); time.sleep(.15)
            pid=service.wait("BOT_PROCESS pid=")
            modem.send(2,0,text(2,5,1700000920,"!mt 2"))
            time.sleep(.1)
            marker=len(service.logs)
            modem.disconnect()
            service.wait("ONLINE epoch=2",timeout=20,after=marker)
            service.wait("BOT_READY native-extension retained",timeout=10,after=marker)
            self.assertEqual([line for line in service.logs if line.startswith("BOT_PROCESS")],[pid])
            # No new contact advert: the same native worker still knows this sender.
            modem.send(2,0,text(2,5,1700000921,"!ping"))
            reply=modem.receive(lambda j:j["epoch"]==2 and j["port"]==2 and parse(j["raw"])[0]==2)
            self.assertIn(b"Pong",body(reply["raw"],2,5))
            time.sleep(2.1)
            replies=[body(j["raw"],2,5) for j in modem.all_submissions
                     if j["epoch"]==2 and j["port"]==2 and parse(j["raw"])[0]==2]
            self.assertEqual(len(replies),1)

    def test_trace_counter_and_malformed_path_bounds(self):
        with Host(role=2) as host:
            payload=struct.pack("<IIB",101,123,0)+public(2)[:1]*63+public(1)[:1]
            raw=b"\x26\x3f"+b"\x04"*63+payload
            frames=[bytes.fromhex(value) for value in host.packet(raw)]
            self.assertEqual(frames,[b"\x26\x40"+b"\x04"*63+b"\0"+payload])
            self.assertEqual(host.packet(frames[0]),[])
            self.assertEqual(host.packet(b"\x26\x41"+bytes(65)+payload),[])
            bad=b"\x26\0"+struct.pack("<IIB",102,123,3)+public(1)[:7]
            self.assertEqual(host.packet(bad),[])
            self.assertEqual(host.packet(b"\x25\0"+payload),[])


if __name__=="__main__": unittest.main(verbosity=2)
