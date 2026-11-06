#!/usr/bin/env python3
"""Black-box tests of the INDEPENDENT Member 4 C infrastructure.

The C test server is a fixture, not a tracker or a production peer server.
"""
from pathlib import Path
import socket
import subprocess
import tempfile
import threading
import time
import unittest

ROOT = Path(__file__).resolve().parents[1]
SERVER = ROOT / 'tests/member4_harness'
PROBE = ROOT / 'tests/member4_config_probe'


def free_port():
    with socket.socket() as s:
        s.bind(('127.0.0.1', 0))
        return s.getsockname()[1]


def read_line(sock):
    data = bytearray()
    while not data.endswith(b'\n'):
        part = sock.recv(1)
        if not part:
            raise EOFError(f'closed after {data!r}')
        data.extend(part)
        if len(data) >= 4096:
            raise AssertionError('line too long')
    return bytes(data)


def read_exact(sock, size):
    result = bytearray()
    while len(result) < size:
        part = sock.recv(size-len(result))
        if not part:
            raise EOFError('truncated binary data')
        result.extend(part)
    return bytes(result)


class Member4Tests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.path = Path(self.temp.name)
        self.process = None
        self.port = None

    def probe(self, kind, data):
        config = self.path / 'test.cfg'
        config.write_bytes(data)
        return subprocess.run([str(PROBE), kind, str(config)], capture_output=True,
                              text=True, timeout=2)

    def start_server(self, max_clients=8):
        self.port = free_port()
        config = self.path / 'serverThreadConfig.cfg'
        config.write_text(f'{self.port}\nshared\n')
        self.process = subprocess.Popen([str(SERVER), str(config), str(max_clients)],
                                        stdout=subprocess.DEVNULL,
                                        stderr=subprocess.DEVNULL)
        self.addCleanup(self.stop_server)
        deadline = time.monotonic() + 4
        while time.monotonic() < deadline:
            if self.process.poll() is not None:
                self.fail(f'test server exited early ({self.process.returncode})')
            try:
                with socket.create_connection(('127.0.0.1', self.port), timeout=.2) as ready_socket:
                    ready_socket.settimeout(.5)
                    ready_socket.sendall(b'<PING ready>\n')
                    self.assertEqual(read_line(ready_socket), b'<PONG ready>\n')
                    self.assertEqual(ready_socket.recv(1), b'')
                time.sleep(.02)  # allow detached worker to release its slot
                return
            except OSError:
                time.sleep(.015)
        self.fail('test server did not start listening')

    def stop_server(self):
        if self.process is None:
            return
        if self.process.poll() is None:
            self.process.terminate()
            try:
                self.process.wait(timeout=4)
            except subprocess.TimeoutExpired:
                self.process.kill()
                self.process.wait(timeout=2)
                self.fail('test server did not shut down promptly')
        self.process = None

    def connect(self):
        sock = socket.create_connection(('127.0.0.1', self.port), timeout=2)
        sock.settimeout(4)
        return sock

    def test_client_configs(self):
        for data in [b'3490\n127.0.0.1\n900\n', b'3490\n::1\n900\n']:
            with self.subTest(data=data):
                p = self.probe('client', data)
                self.assertEqual(p.returncode, 0, p.stderr)
                self.assertIn('3490 ', p.stdout)
                self.assertIn(' 900', p.stdout)

    def test_invalid_client_configs(self):
        cases = [b'', b'3490\n', b'0\n127.0.0.1\n900\n',
                 b'65536\n127.0.0.1\n900\n', b'3490\nlocalhost\n900\n',
                 b'3490\n127.0.0.1\n0\n', b'3490\n127.0.0.1\n-1\n',
                 b'3490\n127.0.0.1\n900\nextra\n',
                 b'3490\n127.0.0.1\n900\x00garbage\n',
                 b'3490\n127.0.0.1\n' + b'x'*10000 + b'\n']
        for data in cases:
            with self.subTest(data=data[:60]):
                self.assertNotEqual(self.probe('client', data).returncode, 0)

    def test_server_and_tracker_configs(self):
        for name in ('server', 'tracker'):
            with self.subTest(name=name):
                p = self.probe(name, b'2097\nshared\n')
                self.assertEqual(p.returncode, 0, p.stderr)
                self.assertEqual(p.stdout, '2097 shared\n')

    def test_invalid_server_configs(self):
        cases = [b'', b'2097\n', b'2097\n\n', b'0\nshared\n',
                 b'65536\nshared\n', b'2097\nshared\nextra\n',
                 b'2097\n shared\n', b'2097\nshared\x00bad\n', b'x'*5000 + b'\nshared\n']
        for data in cases:
            with self.subTest(data=data[:60]):
                self.assertNotEqual(self.probe('server', data).returncode, 0)

    def test_all_real_repository_configs(self):
        for p in ('peer1', 'peer2', 'peer3'):
            with self.subTest(peer=p):
                self.assertEqual(subprocess.run([str(PROBE), 'server',
                                                 str(ROOT/p/'serverThreadConfig.cfg')],
                                                capture_output=True).returncode, 0)
                self.assertEqual(subprocess.run([str(PROBE), 'client',
                                                 str(ROOT/p/'clientThreadConfig.cfg')],
                                                capture_output=True).returncode, 0)
        self.assertEqual(subprocess.run([str(PROBE), 'tracker', str(ROOT/'sconfig')],
                                        capture_output=True).returncode, 0)

    def test_ping_and_invalid_request(self):
        self.start_server()
        with self.connect() as s:
            s.sendall(b'<PING alpha>\n')
            self.assertEqual(read_line(s), b'<PONG alpha>\n')
            self.assertEqual(s.recv(1), b'')
        with self.connect() as s:
            s.sendall(b'<UNKNOWN>\n')
            self.assertEqual(read_line(s), b'<ERROR invalid>\n')

    def test_fragmented_request_and_binary_round_trip(self):
        self.start_server()
        data = bytes(range(256))*4  # full 1024 bytes, including NULs
        with self.connect() as s:
            for part in (b'<E', b'CHO', b' 1024>\n', data[:3], data[3:500], data[500:]):
                s.sendall(part)
                time.sleep(.001)
            self.assertEqual(read_line(s), b'<DATA 1024>\n')
            self.assertEqual(read_exact(s, len(data)), data)
            self.assertEqual(s.recv(1), b'')

    def test_binary_zero_length_and_maximum(self):
        self.start_server()
        with self.connect() as s:
            s.sendall(b'<ECHO 0>\n')
            self.assertEqual(read_line(s), b'<DATA 0>\n')
        with self.connect() as s:
            s.sendall(b'<ECHO 1025>\n')
            self.assertEqual(read_line(s), b'<ERROR too-large>\n')

    def test_concurrent_barrier_four_workers(self):
        self.start_server(max_clients=4)
        results, errors = [], []
        def work():
            try:
                with self.connect() as s:
                    s.sendall(b'<BARRIER>\n')
                    results.append(read_line(s))
            except Exception as exc:
                errors.append(exc)
        start = time.monotonic()
        workers = [threading.Thread(target=work) for _ in range(4)]
        for t in workers: t.start()
        for t in workers: t.join(timeout=5)
        self.assertFalse(any(t.is_alive() for t in workers))
        self.assertEqual(errors, [])
        self.assertEqual(results, [b'<BARRIER ok>\n']*4)
        self.assertLess(time.monotonic()-start, 3.0)

    def test_worker_limit_closes_excess_connections(self):
        self.start_server(max_clients=2)
        with self.connect() as s1, self.connect() as s2:
            for s in (s1, s2):
                s.sendall(b'<HANG>\n')
                self.assertEqual(read_line(s), b'<WAITING>\n')
            with self.connect() as extra:
                extra.settimeout(1)
                extra.sendall(b'<PING overlimit>\n')
                try:
                    reply = extra.recv(100)
                except ConnectionResetError:
                    reply = b''
                self.assertEqual(reply, b'')
            # no server crash after rejecting excess connections
            self.assertIsNone(self.process.poll())

    def test_timeout_and_graceful_shutdown(self):
        self.start_server()
        with self.connect() as s:
            s.sendall(b'<HANG>\n')
            self.assertEqual(read_line(s), b'<WAITING>\n')
            self.assertEqual(read_line(s), b'<TIMEDOUT>\n')
            self.assertEqual(s.recv(1), b'')
        self.stop_server()

    def test_malformed_or_truncated_requests_do_not_crash(self):
        self.start_server()
        with self.connect() as s:
            s.sendall(b'<PING incomplete')
        with self.connect() as s:
            s.sendall(b'x' * 300 + b'\n')
        with self.connect() as s:
            s.sendall(b'<PING a>\x00\n')
        with self.connect() as s:
            s.sendall(b'<PING live>\n')
            self.assertEqual(read_line(s), b'<PONG live>\n')

if __name__ == '__main__':
    unittest.main(verbosity=2)
