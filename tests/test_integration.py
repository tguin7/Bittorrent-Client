#!/usr/bin/env python3
"""End-to-end integration tests using isolated loopback processes and temp folders.
Do not confuse these local tests with the professor's required two-machine demo.
"""
import hashlib
import os
from pathlib import Path
import shutil
import socket
import subprocess
import tempfile
import time
import unittest

ROOT = Path(__file__).resolve().parents[1]


def port():
    with socket.socket() as sock:
        sock.bind(("127.0.0.1", 0))
        return sock.getsockname()[1]


def until_open(p, process=None, seconds=4):
    deadline = time.monotonic()+seconds
    while time.monotonic()<deadline:
        if process is not None and process.poll() is not None:
            raise AssertionError(f"service exited: {process.poll()}")
        try:
            with socket.create_connection(("127.0.0.1",p),timeout=.2):
                return
        except OSError:
            time.sleep(.03)
    raise AssertionError(f"port {p} not listening")


class Integration(unittest.TestCase):
    def setUp(self):
        self.tmp=tempfile.TemporaryDirectory(prefix="cs4390-e2e-")
        self.addCleanup(self.tmp.cleanup)
        self.dir=Path(self.tmp.name)
        self.tracker_port=port()
        self.peer_ports=[port() for _ in range(3)]
        (self.dir/"torrents").mkdir()
        (self.dir/"sconfig").write_text(f"{self.tracker_port}\ntorrents\n")
        self.procs=[]
        self.addCleanup(self.stop_procs)
        for idx in range(1,4):
            peer=self.dir/f"peer{idx}"
            (peer/"shared").mkdir(parents=True)
            shutil.copy2(ROOT/f"peer{idx}/peer",peer/"peer")
            shutil.copy2(ROOT/f"peer{idx}/node",peer/"node")
            (peer/"clientThreadConfig.cfg").write_text(f"{self.tracker_port}\n127.0.0.1\n900\n")
            (peer/"serverThreadConfig.cfg").write_text(f"{self.peer_ports[idx-1]}\nshared\n")
        shutil.copy2(ROOT/"tracker",self.dir/"tracker")
        self.start("tracker",self.dir)
        until_open(self.tracker_port,self.procs[-1])

    def start(self,exe,cwd):
        p=subprocess.Popen([str(cwd/exe)],cwd=cwd,stdin=subprocess.PIPE,
            stdout=subprocess.DEVNULL,stderr=subprocess.PIPE)
        self.procs.append(p)
        return p

    def stop_procs(self):
        for p in self.procs:
            if p.poll() is None:
                p.terminate()
        for p in self.procs:
            if p.poll() is None:
                try:p.wait(timeout=3)
                except subprocess.TimeoutExpired:p.kill();p.wait(timeout=3)
            if p.stderr:
                p.stderr.close()
            if p.stdin:
                p.stdin.close()

    def client(self,index,*args,expect=0):
        cwd=self.dir/f"peer{index}"
        p=subprocess.run([str(cwd/"peer"),*args],cwd=cwd,timeout=10,
            capture_output=True,text=True)
        self.assertEqual(p.returncode,expect,p.stdout+"\n"+p.stderr)
        return p

    def node(self,index,*args,expect=0):
        cwd=self.dir/f"peer{index}"
        p=subprocess.run([str(cwd/"node"),*args],cwd=cwd,timeout=30,
            capture_output=True,text=True)
        self.assertEqual(p.returncode,expect,p.stdout+"\n"+p.stderr)
        return p

    def serve(self,index):
        cwd=self.dir/f"peer{index}"
        p=self.start("node",cwd)
        until_open(self.peer_ports[index-1],p)
        return p

    def register(self,filename,data,index=1):
        cwd=self.dir/f"peer{index}"
        (cwd/"shared"/filename).write_bytes(data)
        digest=hashlib.md5(data).hexdigest()
        self.client(index,"createtracker",filename,str(len(data)),"Integration_Test",digest,
                    "127.0.0.1",str(self.peer_ports[index-1]))
        return digest

    def wire(self,request,portnum):
        with socket.create_connection(("127.0.0.1",portnum),timeout=3) as sock:
            sock.sendall(request)
            sock.shutdown(socket.SHUT_WR)
            out=bytearray()
            while True:
                block=sock.recv(4096)
                if not block:break
                out.extend(block)
            return bytes(out)

    def test_tracker_commands_and_auto_binary_download(self):
        name="movie.bin"
        data=bytes(range(256))*9+b"tail"  # > two 1024-byte chunks, contains NUL
        self.serve(1)
        md5=self.register(name,data)
        listing=self.client(2,"LIST").stdout
        self.assertIn(name,listing)
        # Verify GET metadata transfer and a sequential single-peer download.
        run=self.node(2,"GET",name+".track")
        self.assertIn("COMPLETE",run.stdout)
        received=(self.dir/"peer2/shared"/name).read_bytes()
        self.assertEqual(received,data)
        self.assertEqual(hashlib.md5(received).hexdigest(),md5)
        self.assertFalse((self.dir/"peer2/cache"/(name+".track")).exists())
        tracker_text=(self.dir/"torrents"/(name+".track")).read_text()
        self.assertIn(f"127.0.0.1:{self.peer_ports[0]}:0:{len(data)}:",tracker_text)
        # Updating peer availability is a separate manual command for midterm.
        self.assertNotIn(f"127.0.0.1:{self.peer_ports[1]}:", tracker_text)
        self.client(1,"updatetracker",name,"0",str(len(data)),"127.0.0.1",str(self.peer_ports[0]))

    def test_legacy_and_framed_protocol_and_oversize(self):
        data=b"hello world\x00"*100
        self.serve(1)
        (self.dir/"peer1/shared/hello.bin").write_bytes(data)
        self.assertEqual(self.wire(b"GET hello.bin 0 8\n",self.peer_ports[0]),data[:8])
        self.assertEqual(self.wire(b"<GET hello.bin 7 6>\n",self.peer_ports[0]),
                         b"<DATA 6>\n"+data[7:13])
        self.assertEqual(self.wire(b"<GET hello.bin 0 1025>\n",self.peer_ports[0]),b"<GET invalid>\n")
        self.assertEqual(self.wire(b"GET missing.bin 0 2\n",self.peer_ports[0]),b"<GET missing>\n")
        self.assertEqual(self.wire(b"<GET ../secret 0 2>\n",self.peer_ports[0]),b"<GET invalid>\n")

    def test_concurrent_chunk_requests(self):
        data=os.urandom(4096)
        self.serve(1)
        (self.dir/"peer1/shared/payload.bin").write_bytes(data)
        from concurrent.futures import ThreadPoolExecutor
        def req(i):
            return self.wire(f"<GET payload.bin {i*1024} 1024>\n".encode(),self.peer_ports[0])
        with ThreadPoolExecutor(max_workers=4) as pool:
            actual=list(pool.map(req,range(4)))
        for i,part in enumerate(actual):
            self.assertEqual(part,b"<DATA 1024>\n"+data[i*1024:(i+1)*1024])

    def test_manual_createtracker_and_replacing_update(self):
        self.serve(1)
        data=b"21-byte example file!"  # actual size checked via SEED
        (self.dir/"peer1/shared/example.txt").write_bytes(data)
        seeded=self.node(2,"LIST")
        self.assertIn("LIST complete: 0",seeded.stdout)
        # The interactive node accepts the professor's manual createtracker
        # command while continuing to serve incoming chunks.
        live=self.procs[-1]
        md5=hashlib.md5(data).hexdigest()
        command=f"createtracker example.txt {len(data)} Example_Data {md5} 127.0.0.1 {self.peer_ports[0]}\n"
        live.stdin.write(command.encode())
        live.stdin.flush()
        deadline=time.monotonic()+3
        track=self.dir/"torrents/example.txt.track"
        while not track.exists() and time.monotonic()<deadline:time.sleep(.03)
        self.assertTrue(track.exists())
        self.client(1,"updatetracker","example.txt","0",str(len(data)),
                    "127.0.0.1",str(self.peer_ports[0]))
        text=track.read_text()
        records=[line for line in text.splitlines() if line.startswith("127.0.0.1:")]
        self.assertEqual(len(records),1,text)
        self.node(2,"GET","example.txt.track")
        self.assertEqual((self.dir/"peer2/shared/example.txt").read_bytes(),data)

    def test_three_peers_concurrent_download_and_standalone_legacy_client(self):
        data=os.urandom(2500)
        self.serve(1)
        self.register("parallel.bin",data)
        # Two independent sequential downloaders run at the same time;
        # server-side concurrency remains available for the midterm.
        peers=[self.dir/"peer2",self.dir/"peer3"]
        runners=[subprocess.Popen([str(p/"node"),"GET","parallel.bin.track"],cwd=p,
                                  stdout=subprocess.PIPE,stderr=subprocess.PIPE) for p in peers]
        for runner in runners:
            out,err=runner.communicate(timeout=15)
            self.assertEqual(runner.returncode,0,(out+err).decode(errors="replace"))
        for p in peers:
            self.assertEqual((p/"shared/parallel.bin").read_bytes(),data)
        # Member 3's original download.c still works with the legacy raw format.
        output=self.dir/"legacy-copy.bin"
        legacy=subprocess.run([str(ROOT/"download"),"127.0.0.1",str(self.peer_ports[0]),
                               "parallel.bin",str(len(data)),str(output)],
                               capture_output=True,text=True,timeout=10)
        self.assertEqual(legacy.returncode,0,legacy.stdout+legacy.stderr)
        self.assertEqual(output.read_bytes(),data)

    def test_stale_partial_file_is_restarted(self):
        data=os.urandom(2600)
        name="restart.bin"
        self.serve(1)
        self.register(name,data)
        peer=self.dir/"peer2"
        # A stale partial file is not trusted; midterm starts at byte zero.
        (peer/"shared"/(name+".part")).write_bytes(b"broken old download")
        result=self.node(2,"GET",name+".track")
        self.assertIn("COMPLETE",result.stdout)
        self.assertEqual((peer/"shared"/name).read_bytes(),data)
        self.assertFalse((peer/"shared"/(name+".part")).exists())
        self.assertFalse((peer/"shared"/(name+".part.state")).exists())

    def test_md5_mismatch_keeps_metadata_and_removes_partial(self):
        data=os.urandom(1800)
        name="corrupt.bin"
        self.serve(1)
        self.register(name,data)
        # Corrupt the seed's file without changing the tracker metadata.
        (self.dir/"peer1/shared"/name).write_bytes(b"X"*len(data))
        result=self.node(2,"GET",name+".track",expect=1)
        self.assertIn("MD5 mismatch",result.stderr)
        peer=self.dir/"peer2"
        self.assertFalse((peer/"shared"/name).exists())
        self.assertFalse((peer/"shared"/(name+".part")).exists())
        self.assertTrue((peer/"cache"/(name+".track")).exists())

    def test_no_full_seed_is_not_downloaded(self):
        name="partial.bin"
        data=os.urandom(2000)
        self.serve(1)
        self.register(name,data)
        self.client(1,"updatetracker",name,"0","1024","127.0.0.1",
                    str(self.peer_ports[0]))
        result=self.node(2,"GET",name+".track",expect=1)
        self.assertIn("No peer has the complete file",result.stderr)
        self.assertFalse((self.dir/"peer2/shared"/name).exists())

    def test_get_missing_rejected_and_update_ferr(self):
        self.serve(1)
        failed=self.node(2,"GET","never.track",expect=1)
        self.assertIn("rejected",failed.stderr.lower())
        missing=self.client(1,"updatetracker","never.bin","0","1","127.0.0.1",
                            str(self.peer_ports[0]),expect=1)
        self.assertIn("ferr",missing.stdout)

    def test_split_tracker_message_and_md5(self):
        self.register("tiny.txt",b"sample")
        with socket.create_connection(("127.0.0.1",self.tracker_port),timeout=3) as sock:
            sock.sendall(b"<REQ ")
            time.sleep(.03)
            sock.sendall(b"LIST>\n")
            sock.shutdown(socket.SHUT_WR)
            response=b""
            while True:
                block=sock.recv(4096)
                if not block:break
                response+=block
        self.assertIn(b"<REP LIST 1>\n",response)
        p=self.client(2,"GET","tiny.txt.track")
        self.assertIn("MD5 verified",p.stdout)


if __name__=="__main__":
    unittest.main(verbosity=2)
