"""Local byte-stream, process/resource ownership and actor fault contracts."""
import os
from pathlib import Path
import resource
import signal
import socket
import subprocess
import threading
import time
import unittest

from service_demo import ROOT, RunningService, advert, text, body, login, packet, parse

resource.setrlimit(resource.RLIMIT_CORE,(0,0))
LIBRARY=Path(os.environ.get("MESHCORE_HEW_LIBRARY",ROOT/"build/library-checks"))
RUNTIME=Path(os.environ.get("MESHCORE_HEW_RUNTIME",ROOT/"build/runtime-contract"))
STD_TCP=Path(os.environ.get("MESHCORE_HEW_STD_TCP",ROOT/"build/std-tcp-write-probe"))
STD_SELECT=Path(os.environ.get("MESHCORE_HEW_STD_SELECT",ROOT/"build/std-net-select-probe"))


class Libraries(unittest.TestCase):
    def run_check(self,*args,code=0,marker,timeout=8):
        result=subprocess.run([str(LIBRARY),*map(str,args)],capture_output=True,text=True,timeout=timeout)
        self.assertEqual(result.returncode,code,result.stderr+result.stdout)
        self.assertIn(marker,result.stdout)
        return result

    def test_checked_bytes_and_fragmented_framing(self):
        self.run_check(marker="LIBRARY_CHECKS_OK")

    def test_modem_actor_deadline_and_exactly_once_close(self):
        with socket.socket() as listener:
            listener.bind(("127.0.0.1",0)); listener.listen()
            errors=[]
            def peer():
                try:
                    with listener.accept()[0] as connection:
                        connection.settimeout(4)
                        self.assertEqual(connection.recv(4),b"echo")
                        connection.sendall(b"echo")
                        self.assertEqual(connection.recv(4),b"bulk")
                        connection.sendall(b"x"*12000)
                        self.assertEqual(connection.recv(1),b"")
                except BaseException as error: errors.append(error)
            thread=threading.Thread(target=peer); thread.start()
            try:
                self.run_check("network",listener.getsockname()[1],marker="NETWORK_OWNER_OK")
            finally: thread.join(timeout=5)
            self.assertFalse(thread.is_alive())
            if errors: raise errors[0]

    def test_tcp_write_deadline_and_reported_prefix(self):
        with socket.socket() as listener:
            listener.setsockopt(socket.SOL_SOCKET,socket.SO_RCVBUF,4096)
            listener.bind(("127.0.0.1",0)); listener.listen()
            done=threading.Event()
            def peer():
                with listener.accept()[0]:
                    done.wait(6)
            thread=threading.Thread(target=peer); thread.start()
            try:
                self.run_check("tcp-backpressure",listener.getsockname()[1],
                               marker="TCP_WRITE_DEADLINE_OK",timeout=5)
            finally:
                done.set(); thread.join(timeout=6)
            self.assertFalse(thread.is_alive())

    def test_std_tcp_timeout_is_caught_actor_failure_not_result(self):
        with socket.socket() as listener:
            listener.setsockopt(socket.SOL_SOCKET,socket.SO_RCVBUF,4096)
            listener.bind(("127.0.0.1",0)); listener.listen()
            done=threading.Event()
            def peer():
                with listener.accept()[0]: done.wait(6)
            thread=threading.Thread(target=peer); thread.start()
            try:
                result=subprocess.run([str(STD_TCP),f"127.0.0.1:{listener.getsockname()[1]}"],
                                      capture_output=True,text=True,timeout=5)
                self.assertEqual(result.returncode,1,result.stdout+result.stderr)
                self.assertIn("STD_TCP_TIMEOUT_IS_ACTOR_FAILURE",result.stdout)
                self.assertIn("TCP I/O timeout",result.stderr)
                self.assertNotIn("STD_TCP_RETURNED_ERROR",result.stdout)
            finally:
                done.set(); thread.join(timeout=6)
            self.assertFalse(thread.is_alive())

    def test_partial_write_reports_prefix_and_deadline(self):
        self.run_check("partial",marker="PARTIAL_WRITE_DEADLINE_OK")

    def test_std_stream_receive_select_remains_usable(self):
        with socket.socket() as listener:
            listener.bind(("127.0.0.1",0)); listener.listen()
            errors=[]
            def peer():
                try:
                    with listener.accept()[0] as connection:
                        connection.settimeout(4)
                        self.assertEqual(connection.recv(4),b"echo")
                        connection.sendall(b"echo")
                        self.assertEqual(connection.recv(1),b"")
                except BaseException as error: errors.append(error)
            thread=threading.Thread(target=peer); thread.start()
            try:
                result=subprocess.run([str(STD_SELECT),str(listener.getsockname()[1])],
                                      capture_output=True,text=True,timeout=5)
                self.assertEqual(result.returncode,0,result.stdout+result.stderr)
                self.assertIn("STD_STREAM_SELECT_REUSABLE",result.stdout)
            finally: thread.join(timeout=5)
            self.assertFalse(thread.is_alive())
            if errors: raise errors[0]

    def test_resource_cleanup_after_caught_actor_panic(self):
        self.run_check("resource-panic",code=1,marker="RESOURCE_CRASH_CLOSED_ONCE")

    def test_child_echo_exit_and_forced_retirement(self):
        for mode in ("echo","exit7","ignore-term"):
            with self.subTest(mode=mode):
                self.run_check("child",ROOT/"build/child-fixture",ROOT/"build"/mode,
                               marker="OWNED_CHILD_REAPED")

    def test_child_argument_fidelity_and_bounds(self):
        self.run_check("argv",ROOT/"build/child-fixture",marker="ARGV_FIDELITY_BOUNDS_OK")

    def test_runtime_panic_supervision_and_native_abort(self):
        for mode,code,marker in (("unsupervised",1,"UNSUPERVISED_ISOLATED"),
                                 ("supervised",0,"ONE_FOR_ONE_RESTARTED"),
                                 ("budgets",1,"INDEPENDENT_RESTART_BUDGETS"),
                                 ("abort",-signal.SIGABRT,None)):
            with self.subTest(mode=mode):
                result=subprocess.run([str(RUNTIME),mode],capture_output=True,text=True,timeout=8)
                self.assertEqual(result.returncode,code,result.stderr+result.stdout)
                if marker: self.assertIn(marker,result.stdout)
                else: self.assertNotIn("NATIVE_ABORT_RETURNED",result.stdout)

    def test_storage_fault_stops_only_affected_role(self):
        with RunningService(native=False) as service:
            modem=service.emulator
            modem.send(1,0,login(stamp=300))
            modem.receive(lambda job:job["port"]==1 and parse(job["raw"])[0]==1)
            committed=(service.root/"room.state").read_bytes()
            pending=service.root/"room.state.pending"
            pending.mkdir(mode=0o700)
            marker=len(service.logs)
            baseline=len(modem.all_submissions)
            modem.send(1,0,login(stamp=301))
            service.wait("ROLE_OFFLINE port=1",after=marker)
            modem.send(0,0,packet(5,b"room-storage-failed",route=1,width=3))
            modem.receive(lambda job:job["port"]==0 and parse(job["raw"])[4]==b"room-storage-failed")
            modem.send(2,0,advert(2)); time.sleep(.1)
            modem.send(2,0,text(2,5,1700000930,"!ping"))
            reply=modem.receive(lambda job:job["port"]==2 and parse(job["raw"])[0]==2)
            self.assertIn(b"pong",body(reply["raw"],2,5).lower())
            self.assertEqual((service.root/"room.state").read_bytes(),committed)
            self.assertFalse(any(job["port"]==1 for job in modem.all_submissions[baseline:]))
            self.assertEqual(modem.epoch,1)
            modem.disconnect()
            service.wait("ONLINE epoch=2",timeout=15)
            self.assertFalse(any(job["port"]==1 and job["epoch"]==2 for job in modem.all_submissions))
            service.stop_process()
            pending.rmdir()
            service.start_process(False)
            modem.send(1,0,login(stamp=301))
            modem.receive(lambda job:job["port"]==1 and job["epoch"]==3 and parse(job["raw"])[0]==1)

    def test_native_process_death_does_not_close_healthy_modem_roles(self):
        for death in (signal.SIGABRT,signal.SIGSEGV):
            with self.subTest(signal=death), RunningService() as service:
                modem=service.emulator
                pid=int(service.wait("BOT_PROCESS pid=").split("pid=")[1])
                marker=len(service.logs)
                os.kill(pid,death)
                service.wait("BOT_OFFLINE",after=marker)
                modem.send(0,0,packet(5,b"bot-process-died",route=1,width=3))
                modem.send(1,0,login(stamp=400))
                modem.receive(lambda job:job["port"]==0 and parse(job["raw"])[4]==b"bot-process-died")
                modem.receive(lambda job:job["port"]==1 and parse(job["raw"])[0]==1)
                service.wait("BOT_READY",after=marker)
                replacement=int(service.wait("BOT_PROCESS pid=",after=marker).split("pid=")[1])
                self.assertNotEqual(pid,replacement)
                modem.send(2,0,advert(2)); time.sleep(.1)
                modem.send(2,0,text(2,5,1700000970,"!ping"))
                reply=modem.receive(lambda job:job["port"]==2 and parse(job["raw"])[0]==2)
                self.assertIn(b"pong",body(reply["raw"],2,5).lower())
                self.assertEqual(modem.epoch,1)


if __name__=="__main__": unittest.main(verbosity=2)
