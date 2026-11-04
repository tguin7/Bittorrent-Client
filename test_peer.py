#!/usr/bin/env python3
"""Black-box integration tests: real C executable and loopback TCP sockets."""
import hashlib
import os
from pathlib import Path
import socket
import subprocess
import sys
import tempfile
import threading
import time
import unittest

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT))
from mock_tracker import Tracker
BIN = Path(os.environ.get('PEER_BIN', ROOT / 'peer1/peer')).resolve()
BODY = b'Filename: hello.txt\nFilesize: 3\nDescription: Demo\nMD5: 900150983cd24fb0d6963f7d28e17f72\n127.0.0.1:2097:0:3:123\n'

def get_reply(body=BODY, wrapped=False):
    data = b'<' + body + b' >\n' if wrapped else body
    return b'<REP GET BEGIN>\n' + data + b'<REP GET END ' + hashlib.md5(body).hexdigest().encode() + b'>\n'

class PeerTests(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)
        self.directory = Path(self.tmp.name)
        self.config = self.directory / 'clientThreadConfig.cfg'
        self.cache = self.directory / 'cache'

    def call(self, command, response, expected=None, fragment=0, stall=False, interactive=False):
        server = socket.socket()
        server.bind(('127.0.0.1', 0)); server.listen(1); server.settimeout(8)
        self.config.write_text(f'{server.getsockname()[1]}\n127.0.0.1\n900\n')
        requests, failures = [], []
        def serve():
            try:
                with server:
                    conn, _ = server.accept()
                    with conn:
                        conn.settimeout(8)
                        data = b''
                        while not data.endswith(b'\n'):
                            chunk = conn.recv(2048)
                            if not chunk:
                                break
                            data += chunk
                        requests.append(data)
                        payload = response(data) if callable(response) else response
                        step = fragment or max(len(payload), 1)
                        for i in range(0, len(payload), step):
                            conn.sendall(payload[i:i+step])
                            if fragment:
                                time.sleep(.0002)
                        if stall:
                            time.sleep(5.5)
            except (BrokenPipeError, ConnectionResetError):
                pass  # expected when rejecting a response over the size limit
            except Exception as exc:
                failures.append(exc)
        thread = threading.Thread(target=serve)
        thread.start()
        args = [str(BIN), '--config', str(self.config), '--cache', str(self.cache)]
        proc = subprocess.run(args + ([] if interactive else command.split()),
                              input=command+'\nquit\n' if interactive else None,
                              text=True, capture_output=True, timeout=9)
        thread.join(9)
        self.assertFalse(thread.is_alive())
        self.assertEqual(failures, [])
        if expected is not None:
            self.assertEqual(requests, [expected])
        return proc

    def test_empty_list_exact_wire(self):
        p = self.call('LIST', b'<REP LIST 0>\n<REP LIST END>\n', b'<REQ LIST>\n')
        self.assertEqual(p.returncode, 0, p.stderr)

    def test_fragmented_list(self):
        p = self.call('REQ LIST', b'<REP LIST 1>\n<1 hello.txt 3 900150983cd24fb0d6963f7d28e17f72>\n<REP LIST END>\n', b'<REQ LIST>\n', 1)
        self.assertEqual(p.returncode, 0, p.stderr)
        self.assertIn('hello.txt | 3 bytes', p.stdout)

    def test_get_exact_bytes_and_wire(self):
        p = self.call('GET hello.txt.track', get_reply(), b'<GET hello.txt.track >\n', 1)
        self.assertEqual(p.returncode, 0, p.stderr)
        self.assertEqual((self.cache/'hello.txt.track').read_bytes(), BODY)

    def test_wrapped_get(self):
        p = self.call('GET hello.txt.track', get_reply(wrapped=True))
        self.assertEqual(p.returncode, 0, p.stderr)
        self.assertEqual((self.cache/'hello.txt.track').read_bytes(), BODY)

    def test_md5_rejection_preserves_cache(self):
        self.cache.mkdir(); dest=self.cache/'hello.txt.track'; dest.write_bytes(b'previous')
        p=self.call('GET hello.txt.track',get_reply().replace(b'Demo',b'Evil'))
        self.assertNotEqual(p.returncode,0)
        self.assertIn('MD5 mismatch',p.stderr)
        self.assertEqual(dest.read_bytes(),b'previous')
        self.assertEqual(list(self.cache.iterdir()),[dest])

    def test_bad_list_replies(self):
        for response in [b'', b'<REP LIST 1>\n<REP LIST END>\n', b'<REP LIST 0>\n', b'<REP LIST 0>\n<REP LIST END>\nextra', b'<REP LIST -1>\n<REP LIST END>\n', b'<REP LIST 1>\n<2 f 3 900150983cd24fb0d6963f7d28e17f72>\n<REP LIST END>\n', b'<REP LIST 0>\x00\n<REP LIST END>\n']:
            with self.subTest(response=response):
                self.assertNotEqual(self.call('LIST',response).returncode,0)

    def test_bad_get_replies(self):
        for response in [b'<REP GET ferr>\n',get_reply()[:-2],get_reply()+b'extra',get_reply(BODY.replace(b'hello.txt',b'wrong.txt')),get_reply(BODY.replace(b':0:3:',b':4:3:')),get_reply(BODY.replace(b'Filesize: 3',b'Filesize: -1'))]:
            with self.subTest(response=response):
                self.assertNotEqual(self.call('GET hello.txt.track',response).returncode,0)
                self.assertFalse(self.cache.exists())

    def test_cache_write_failure(self):
        self.cache.write_text('not a directory')
        self.assertNotEqual(self.call('GET hello.txt.track',get_reply()).returncode,0)

    def test_registration_and_update_exact_wire(self):
        tracker=Tracker()
        cmd='createtracker hello.txt 3 Demo 900150983cd24fb0d6963f7d28e17f72 127.0.0.1 2097'
        self.assertEqual(self.call(cmd,tracker.respond,('<'+cmd+'>\n').encode()).returncode,0)
        self.assertNotEqual(self.call(cmd,tracker.respond).returncode,0)
        cmd='updatetracker hello.txt 0 3 127.0.0.1 2097'
        self.assertEqual(self.call(cmd,tracker.respond,('<'+cmd+'>\n').encode()).returncode,0)
        self.assertNotEqual(self.call(cmd.replace('hello.txt','missing.txt'),tracker.respond).returncode,0)
        self.assertEqual(self.call('GET hello.txt.track',tracker.respond).returncode,0)

    def test_interactive(self):
        self.assertEqual(self.call('LIST',b'<REP LIST 0>\n<REP LIST END>\n',interactive=True).returncode,0)

    def test_timeout(self):
        p=self.call('LIST',b'',stall=True)
        self.assertNotEqual(p.returncode,0)
        self.assertIn('timeout',p.stderr)

    def test_size_limit(self):
        self.assertNotEqual(self.call('LIST',b'x'*(1024*1024+1)).returncode,0)

    def test_local_validation_and_connection_refused(self):
        s=socket.socket(); s.bind(('127.0.0.1',0)); port=s.getsockname()[1]; s.close()
        self.config.write_text(f'{port}\n127.0.0.1\n900\n')
        for cmd in [['GET','../evil.track'],['GET','bad'],['LIST'],['createtracker','x','-1','d','bad','127.0.0.1','9']]:
            p=subprocess.run([str(BIN),'--config',str(self.config)]+cmd,capture_output=True,timeout=8)
            self.assertNotEqual(p.returncode,0)
        for config in ['0\n127.0.0.1\n900\n','3490\ninvalid\n900\n','3490\n127.0.0.1\n0\n','3490\n','3490\n127.0.0.1\n900\nextra\n']:
            self.config.write_text(config)
            p=subprocess.run([str(BIN),'--config',str(self.config),'LIST'],capture_output=True,timeout=2)
            self.assertNotEqual(p.returncode,0)

if __name__=='__main__':
    unittest.main(verbosity=2)
