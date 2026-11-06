# CS 4390 (Section 501) — Midterm Integration Handoff

## What is integrated

This working copy combines the repository's actual `tracker.c`, `peer.c`,
`peerServer.c`, and `download.c` with Member 4's previously supplied
`net_io`, `config`, `thread_server`, and `download_pool` libraries.

The existing **`peer.c` has not been edited** (SHA-256:
`826e259ee2f978b62fb0488e0288586fe96fa5497b40d67ec66dd0288f589a64`).

### Architecture

- `tracker` runs Member 1's tracker server. Integration changes repair complete-line
  receiving, reliable response sending, GET metadata checksum computation, and
  update semantics (replace the peer's record instead of appending duplicates).
- `peerN/peer` is **unchanged Member 2's tracker CLI**, with its original strict
  LIST, GET, createtracker, updatetracker parsing and cache verification.
- `peerServer.c` contains Member 3's file serving logic, adjusted to be a callback
  for Member 4's `thread_server_run`. Each connected client gets one POSIX thread.
  A separate standalone `peerServer` program remains available.
- `download.c` builds Member 3's **original standalone `download`** executable,
  and its legacy protocol continues to work.
- `peerN/node` is a new Member 4 *integration entry point* that starts the peer
  server in the background, invokes the unchanged `peerN/peer` to send tracker
  commands, and automatically calls `node_download.c` after successful tracker
  GET metadata verification.
- `node_download.c` uses four concurrent worker threads from Member 4's
  `download_pool` to send GET chunk requests, `pwrite` mutually exclusive chunks,
  verify the complete file's MD5, and send an `updatetracker` command when complete.
  The metadata cache entry is removed after successful checksum verification.
- `peer1`, `peer2`, and `peer3` each have **independent** configuration, shared
  storage, and cache folders. By default, the tracker listens at port 3490 and
  peers at 2097, 2098, 2099.

## Requirements

Ubuntu (including WSL2) with a working TCP stack:

```bash
sudo apt update
sudo apt install build-essential libssl-dev python3
make clean && make all
make test
```

`make test` runs Member 4's C + Python tests, Member 2's 13 original tracker-client
unit tests, and integration tests involving real local TCP sockets, all three
configured peer folders, a real tracker, the old downloader, and binary files.
It does **not** prove two-machine connectivity. Run the physical two-machine demo.

## Quick local demo (three terminals)

These commands assume you are in the repository root and that the configured
ports are free. Terminal 1:

```bash
./tracker
```

Terminal 2 (the seeding peer, leave interactive `node` running):

```bash
cd peer1
printf 'hello from peer one\n' > shared/example.txt
./node
node> SEED example.txt Example_Data
node> LIST
```

`SEED filename description` computes the actual file size and MD5 and registers
using the IP associated with the connection to the configured tracker. The
manually required `createtracker` syntax still works: enter
`createtracker filename filesize description md5 ip-address port-number`
instead of `SEED` for the graded manual command demonstration.

Terminal 3 (downloading peer):

```bash
cd peer2
./node
node> LIST
node> GET example.txt.track
node> quit
cmp shared/example.txt ../peer1/shared/example.txt
md5sum shared/example.txt ../peer1/shared/example.txt
```

`GET example.txt.track` invokes the original tracker client, verifies and saves
metadata, automatically downloads the real file over TCP from a peer, checks
its final MD5, removes the tracker cache entry, and updates the tracker record.
`RETRY example.txt.track` can retry an incomplete download **if a verified
metadata file remains in `cache/`**; best-effort completed chunk state is saved
alongside `shared/example.txt.part` as `.part.state`. Failure or power loss may
require retrying from scratch if the integrity check fails.

For all required manual tracker operations, use either the interactive `node`
or the original per-peer `./peer` CLI:

```text
LIST
GET example.txt.track
createtracker example.txt 20 Example_Data <md5-of-file> 127.0.0.1 2097
updatetracker example.txt 0 20 127.0.0.1 2097
```

Replace the example byte count and hash with real values. Trackers use exclusive
end offsets, so a complete file of N bytes is advertised as 0:N.

## Two-machine setup for Friday's recording

1. Put the tracker on machine A (or a third machine) and run `./tracker` at the
   repository root (with `sconfig` set to a reachable TCP port).
2. On the seeding machine, configure `peer1/clientThreadConfig.cfg` with tracker
   port, **machine A's reachable IPv4 address**, and update interval in seconds.
   Configure `peer1/serverThreadConfig.cfg` to use its own listening port and
   `shared` directory. Start `peer1/node` interactively and `SEED` a file.
3. On the downloading machine, configure `peer2/clientThreadConfig.cfg` with the
   same tracker IP and port, and configure its own `peer2/serverThreadConfig.cfg`.
   Start `peer2/node`, run `LIST` and `GET file.track`.
4. The tracker must accept TCP connections (default 3490), and the seeding peer
   must accept inbound TCP connections on the port it advertised (default 2097).
   Allow these connections through Windows/Ubuntu firewalls. WSL2 commonly
   requires additional inbound forwarding or mirrored-network configuration;
   validate with `nc -vz <host> <port>` before starting the recorded demo.
5. Compare actual source/destination files using `cmp` and `md5sum` on the machines,
   capture the command/reply and TCP/thread log screenshots, and record the demo.

**Do not use `127.0.0.1` for tracker or peer addresses across two machines.**
The integrated seeding command automatically detects the local IPv4 address
used to connect to the tracker. If this is not externally reachable because of
NAT/WSL2, adjust networking or manually provide the reachable IP via the
`createtracker` command.

## Peer transfer compatibility

Professor requires strict tracker commands. The professor specifies a maximum
chunk length of 1024 bytes but does not fix the peer-to-peer transfer wire
framing. Both team variants are available, with one request per connection:

| Request | Success response | Failure |
|---|---|---|
| `GET file offset length\n` (Member 3 legacy) | exactly `length` raw bytes | `<GET invalid>\n` / `<GET missing>\n` |
| `<GET file offset length>\n` (team agreed framed protocol) | `<DATA length>\n` followed by exactly `length` bytes | same errors |

`node` uses the framed variant. Member 3's unmodified `download` uses legacy.
Both enforce a 1024-byte limit and reject invalid offsets/path traversal.

## Important limitations and unfinished final-demo features

- **Two physical machines are not available inside this coding environment.**
  All automated integration tests use loopback TCP. Your team must verify the
  actual two-machine scenario, firewall, addresses, and network routes.
- The new `node` orchestrator **forks/execs the existing peer CLI** for tracker
  communication, rather than rewriting Member 2's `peer.c`. The `node` itself
  serves files with actual POSIX threads, and uses four concurrent downloading
  threads. This preserves the agreed file ownership while giving one user-facing
  CLI process.
- The 900-second refresh interval is read from config, but periodic refresh and
  resuming incomplete downloads after process restart with fresh tracker data
  need further final-project hardening. The chunk-state persistence is currently
  best-effort, not a transactional resume system.
- Source peer selection uses the newest timestamp among peers covering a chunk;
  retries use alternates. Final-project performance tuning and dead-peer
  garbage collection remain to be implemented.
- End-to-end encryption is not provided (not required by the assignment).
- Avoid placing spaces in shared filenames; names are restricted to letters,
  digits, underscore, hyphen, and period and are verified by the legacy client.
- The tracker GET reply contains the MD5 of the *tracker file's exact contents*;
  the `.track` `MD5:` line contains the shared *data file's* MD5. These are
  intentionally different values and must not be confused.

## Source ownership and change report

- **Unmodified**: `peer.c`, `download.c`, `mock_tracker.py`, original
  `tests/test_peer.py`, and existing per-peer configuration files.
- **Updated for compatibility**: `tracker.c` (Member 1's original tracker),
  `peerServer.c` (Member 3's original serving logic), `sconfig`, `Makefile`.
- **New Member 4 integration**: `peer_node.c`, `node_download.c/.h`,
  `peer_server.h`, `tests/test_integration.py` and this README.
- **Retained Member 4 infrastructure**: `config.c/.h`, `net_io.c/.h`,
  `thread_server.c/.h`, `download_pool.c/.h` and Member 4's tests.

Sources/libraries: POSIX TCP socket and pthread APIs (Linux man pages), OpenSSL
`libcrypto` EVP MD5 API (https://docs.openssl.org/), and the course's Beej socket
programming guide. Do not submit binary objects; `make clean` removes them.
