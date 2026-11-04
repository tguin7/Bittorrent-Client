#!/usr/bin/env python3
"""Sequential in-memory test double, NOT the team's production tracker.

Exact LF request framing; no shared-file serving, expiry, or persistence.
"""
import argparse
import hashlib
import re
import socket
import time

def md5(data):
    return hashlib.md5(data).hexdigest()

class Tracker:
    def __init__(self):
        self.files = {}

    def respond(self, request):
        if request == b'<REQ LIST>\n':
            rows = [f'<REP LIST {len(self.files)}>\n']
            for i, (name, record) in enumerate(sorted(self.files.items()), 1):
                rows.append(f'<{i} {name} {record[0]} {record[2]}>\n')
            return (''.join(rows) + '<REP LIST END>\n').encode()
        match = re.fullmatch(rb'<GET ([A-Za-z0-9_.-]+)\.track >\n', request)
        if match:
            name = match[1].decode()
            if name not in self.files:
                # GET error format is unspecified by the PDF; documented fixture.
                return b'<REP GET ferr>\n'
            size, description, digest, peers = self.files[name]
            body = (f'Filename: {name}\nFilesize: {size}\n'
                    f'Description: {description}\nMD5: {digest}\n'
                    '# peer records: end byte is exclusive in this fixture\n' +
                    ''.join(f'{ip}:{port}:{start}:{end}:{stamp}\n'
                            for (ip, port), (start, end, stamp) in peers.items())).encode()
            return b'<REP GET BEGIN>\n' + body + f'<REP GET END {md5(body)}>\n'.encode()
        match = re.fullmatch(rb'<createtracker ([A-Za-z0-9_.-]+) ([0-9]+) ([^ <>\r\n]+) ([0-9a-fA-F]{32}) ([0-9.]+) ([0-9]+)>\n', request)
        if match:
            name, size, description, digest, ip, port = [s.decode() for s in match.groups()]
            if name in self.files:
                return b'<createtracker ferr>\n'
            self.files[name] = [int(size), description, digest,
                                {(ip, int(port)): (0, int(size), int(time.time()))}]
            return b'<createtracker succ>\n'
        match = re.fullmatch(rb'<updatetracker ([A-Za-z0-9_.-]+) ([0-9]+) ([0-9]+) ([0-9.]+) ([0-9]+)>\n', request)
        if match:
            name, start, end, ip, port = [s.decode() for s in match.groups()]
            if name not in self.files:
                return f'<updatetracker {name} ferr>\n'.encode()
            if int(start) > int(end) or int(end) > self.files[name][0]:
                return f'<updatetracker {name} fail>\n'.encode()
            self.files[name][3][(ip, int(port))] = (int(start), int(end), int(time.time()))
            return f'<updatetracker {name} succ>\n'.encode()
        return b'<ERROR invalid>\n'

def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--host', default='127.0.0.1')
    parser.add_argument('--port', type=int, default=3490)
    parser.add_argument('--fragment', type=int, default=0, help='reply bytes per send; 1 exercises stream handling')
    parser.add_argument('--bad-md5', action='store_true')
    args = parser.parse_args()
    tracker = Tracker()
    with socket.socket() as server:
        server.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        server.bind((args.host, args.port)); server.listen(8)
        print(f'MOCK tracker listening at {args.host}:{args.port}; Ctrl+C to stop', flush=True)
        while True:
            conn, _ = server.accept()
            with conn:
                conn.settimeout(5)
                try:
                    request = b''
                    while not request.endswith(b'\n') and len(request) < 2048:
                        chunk = conn.recv(2048 - len(request))
                        if not chunk:
                            break
                        request += chunk
                    print(f'Received {request!r}', flush=True)
                    response = tracker.respond(request)
                    if args.bad_md5 and response.startswith(b'<REP GET BEGIN>'):
                        response = response[:-34] + b'0' * 32 + b'>\n'
                    step = args.fragment or len(response)
                    for offset in range(0, len(response), step):
                        conn.sendall(response[offset:offset+step])
                except (OSError, ValueError) as exc:
                    print(f'Mock connection error: {exc}', flush=True)

if __name__ == '__main__':
    try:
        main()
    except KeyboardInterrupt:
        print('\nMock stopped.')
