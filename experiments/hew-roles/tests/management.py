"""Isolated authenticated management, bounded replies and durable settings."""
import os
from pathlib import Path
import shutil
import struct
import time
import unittest
from unittest.mock import patch

from parity import BUILD, Host, body, dm, login, packet, parse, public, secret, seal, text, tx
from service_demo import RunningService, Emulator, addressed, body as service_body, advert as service_advert
from release_identity import VERSION


def cli(host, stamp, command):
    frames=host.packet(text(2,stamp,command.encode(),4))
    assert len(frames)==1,frames
    return body(frames[0],2)[5:].rstrip(b"\0").decode()


class Management(unittest.TestCase):
    def test_read_only_login_and_multi_ack_retries(self):
        with Host() as host:
            host.packet(login(2,10,"admin"))
            self.assertEqual(cli(host,11,"set allow.read.only on"),"OK")
            self.assertEqual(cli(host,12,"set multi.acks 1"),"OK")
            readonly=host.packet(login(3,20,"incorrect"))
            self.assertEqual(body(readonly[0],3)[6:8],b"\2\0")
            self.assertEqual(host.packet(text(3,21,b"cannot post")),[])
            self.assertEqual(host.packet(text(3,22,b"get name",4)),[])
            host.packet(login(4,30,"room"))
            host.packet(dm(4,8,b"\x80\x0f"))
            for flag in (0,1):
                raw=text(4,31,b"one durable post",flag)
                lines=host.command(f"packet 1000000 0 {raw.hex()}")
                frames=tx(lines)
                self.assertEqual([int(line.split()[1]) for line in lines],[200,500])
                self.assertEqual([parse(frame)[0] for frame in frames],[10,3])
                self.assertEqual(parse(frames[0])[4],b"\x13"+parse(frames[1])[4])
            self.assertIn("posted=1"," ".join(host.command("status")))

    def test_owner_path_capacity_and_failed_acl_do_not_change_member(self):
        with Host(role=2) as host:
            self.assertEqual(host.command("acl "+public(2).hex()+" 3"),["true"])
            self.assertEqual(cli(host,10,"set owner.info "+"é"*59),"OK")
            for width in (1,2,3):
                path=b"\xaa"*((63//width)*width)
                raw=dm(2,0,struct.pack("<IB",20+width,7),route=1,width=width,path=path)
                frames=host.packet(raw)
                self.assertEqual(len(frames),1)
                parsed=parse(frames[0])
                self.assertEqual(parsed[0],8)
                self.assertLessEqual(len(parsed[4]),184)
                value=body(frames[0],2)[2+len(path)+4:].rstrip(b"\0").decode()
                self.assertTrue(value.startswith(f"willow-{VERSION}\nHew Room\n"),value)
            for seed in range(3,22):
                self.assertEqual(host.command(f"acl {public(seed).hex()} 2"),["true"])
            before=host.command("cursor "+public(2).hex())
            raw=dm(2,0,struct.pack("<IB",100,5)+b"\0\0",route=1,width=3,path=b"\xaa"*63)
            self.assertEqual(host.packet(raw),[])
            self.assertEqual(host.command("cursor "+public(2).hex()),before)

    def test_preferences_restart_and_corruption_are_transactional(self):
        root=BUILD/f"management-state-{os.getpid()}-{time.monotonic_ns()}"
        root.mkdir(mode=0o700)
        try:
            path=root/"role.state"
            with Host() as host:
                host.packet(login(2,10,"admin"))
                self.assertEqual(cli(host,11,"set name Durable Room"),"OK")
                self.assertEqual(cli(host,12,"set owner.info operator|station"),"OK")
                self.assertEqual(host.command(f"save {path}"),["true"])
            committed=path.read_bytes()
            self.assertEqual(committed[:4],b"HEW5")
            with Host() as restored:
                self.assertEqual(restored.command(f"load {path}"),["true"])
                self.assertEqual(cli(restored,13,"get name"),"> Durable Room")
                self.assertEqual(cli(restored,14,"get owner.info"),"> operator|station")
                for broken in (committed[:-1],committed+b"\0",committed[:-4]+b"\0\0\x80\x7f"):
                    path.write_bytes(broken)
                    self.assertEqual(restored.command(f"load {path}"),["false"])
                self.assertEqual(cli(restored,15,"get name"),"> Durable Room")
        finally:
            shutil.rmtree(root)

    def test_bad_auth_and_unavailable_telemetry_have_no_success_reply(self):
        with Host() as host:
            host.packet(login(2,10,"admin"))
            raw=bytearray(text(2,11,b"set name Rejected",4));raw[-1]^=1
            self.assertEqual(host.packet(bytes(raw)),[])
            self.assertEqual(cli(host,12,"get name"),"> Hew Room")
            rejected=host.packet(text(2,13,b"set name \xff",4))
            self.assertEqual(body(rejected[0],2)[5:].rstrip(b"\0"),b"Error, command is not valid UTF-8")
            self.assertEqual(cli(host,14,"get name"),"> Hew Room")
            self.assertEqual(host.packet(dm(2,0,struct.pack("<IB",15,3))),[])
            reply=host.packet(dm(2,0,struct.pack("<IB",16,1)))[0]
            status=body(reply,2)[4:56]
            self.assertEqual(status[0:2],b"\0\0")
            self.assertEqual(status[4:8],b"\0\x80\0\x80")
            self.assertEqual(status[42:44],b"\0\x80")

    def test_service_samples_modem_without_stalling_other_roles(self):
        with RunningService() as service:
            modem=service.emulator
            deadline=time.monotonic()+5
            while time.monotonic()<deadline and not any(c[2]==0x12 for c in modem.controls):
                time.sleep(.02)
            self.assertTrue(any(c[2]==0x12 for c in modem.controls))
            login_body=struct.pack("<II",100,0)+b"admin\0"
            raw=packet(7,public(4)[:1]+public(2)+seal(secret(2,public(4)),login_body))
            modem.send(1,0,raw)
            modem.receive(lambda item:item["port"]==1 and parse(item["raw"])[0]==1)
            modem.send(1,0,addressed(2,4,0,struct.pack("<IB",101,3)))
            response=modem.receive(lambda item:item["port"]==1 and parse(item["raw"])[0]==1)
            self.assertEqual(service_body(response["raw"],2,4)[4:12],bytes.fromhex("01740186016700fb"))
            modem.send(1,0,addressed(2,4,2,struct.pack("<IB",102,4)+b"stats airtime"))
            response=modem.receive(lambda item:item["port"]==1 and parse(item["raw"])[0]==2)
            airtime=service_body(response["raw"],2,4)[5:].rstrip(b"\0")
            self.assertIn(b"tx_rf_ms=",airtime)
            self.assertNotIn(b"tx_rf_ms=unavailable",airtime)
            self.assertIn(b"rx_rf_ms=unavailable",airtime)
            self.assertFalse(any("TELEMETRY " in line for line in service.logs))
            self.assertEqual(modem.epoch,1)

    def test_optional_sensor_failure_preserves_all_roles_and_modem(self):
        for mode in ("drop","malformed"):
            with self.subTest(mode=mode), patch.object(Emulator,"telemetry_mode",mode), RunningService() as service:
                modem=service.emulator
                service.wait("TELEMETRY ",8)
                payload=b"sensor-failure-"+mode.encode()
                modem.send(0,0,packet(5,payload,route=1,width=3))
                modem.receive(lambda item:item["port"]==0 and parse(item["raw"])[4]==payload)
                login_body=struct.pack("<II",200,0)+b"admin\0"
                modem.send(1,0,packet(7,public(4)[:1]+public(2)+seal(secret(2,public(4)),login_body)))
                modem.receive(lambda item:item["port"]==1 and parse(item["raw"])[0]==1)
                modem.send(1,0,addressed(2,4,0,struct.pack("<IB",201,1)))
                response=modem.receive(lambda item:item["port"]==1 and parse(item["raw"])[0]==1)
                self.assertEqual(service_body(response["raw"],2,4)[4:6],b"\0\0")
                modem.send(2,0,packet(4,parse(service_advert(2))[4],route=2,width=3))
                modem.send(2,0,addressed(2,5,2,struct.pack("<IB",1700002200,0)+b"!ping"))
                response=modem.receive(lambda item:item["port"]==2 and parse(item["raw"])[0]==2)
                self.assertIn(b"pong",service_body(response["raw"],2,5).lower())
                self.assertEqual(modem.epoch,1)
                self.assertEqual(sum("BOT_PROCESS pid=" in line for line in service.logs),1)


if __name__=="__main__":
    unittest.main(verbosity=2)
