# CS 4390 — Member 4 implementation (independent infrastructure)

**Status:** Implemented and tested independently on Linux, before Members 1 and 3 deliver their server modules. This is **not** a complete P2P file-sharing application. In particular, the included C test server is *not* a tracker server or a file server. The original `peer.c` is unchanged.

## 1. Setup (Ubuntu / Windows WSL2 Ubuntu)

```sh
sudo apt update
sudo apt install build-essential python3 libssl-dev
make clean
make all
make test
```

`make all` creates the existing `peer1/peer`, `peer2/peer`, `peer3/peer`, and `libmember4.a`. If a teammate later adds `tracker.c` in the project root, `make all` also attempts to compile a real `tracker` executable. This conditional target is **not** an existing tracker implementation.

`make test-member4` runs just the independent infrastructure tests. `make test` also runs Member 2's existing peer-client tests against a mock tracker. `make clean` removes compiled artifacts without deleting source files or configuration.

Run all the build and tests from a clean checkout with `bash scripts/verify_member4.sh`.

## 2. Ownership and supplied files

| Files | Responsibility | State |
|---|---|---|
| `net_io.c/.h` | `net_connect_tcp`, timeouts, partial-send/receive-safe TCP helpers, line framing | Done (isolated tests) |
| `thread_server.c/.h` | Reusable thread-per-accepted-TCP-connection server, concurrency limit, orderly shutdown | Done (isolated tests) |
| `download_pool.c/.h` | Up to four parallel nonoverlapping chunk tasks, retries and per-task completion record | Done (isolated tests) |
| `config.c/.h` | Strict `sconfig`, `serverThreadConfig.cfg` and `clientThreadConfig.cfg` parsers | Done (isolated tests) |
| `sconfig` | Initial tracker server config: port 3490, folder `torrents` | Provided, tracker not implemented |
| `peer1/peer2/peer3/shared/.gitkeep` | Reserved file-sharing folders, no sample shared content | Provided |
| `Makefile` | Builds existing peers and Member 4 library, runs tests; conditional tracker target | Done |
| `tests/member4_*`, `tests/test_member4.py` | Real socket and concurrency regression tests using a test-only server | Done |
| `scripts/verify_member4.sh` | Reproducible clean-build + all-tests preflight | Done |
| `peer.c`, `mock_tracker.py`, `tests/test_peer.py` | Existing member work | **Not edited** |

## 3. Agreed conventions

- C on Ubuntu; TCP `SOCK_STREAM`; textual commands `<COMMAND fields>\n`.
- The tracker protocol formats from the professor's PDF take precedence over any examples here.
- File chunk convention agreed by team: `<GET filename offset length>\n`, success `<DATA length>\n` + exactly `length` binary bytes. One chunk request per TCP connection initially. Byte range `[offset, offset+length)`, max chunk length **1024**. Oversized requests **must** return `<GET invalid>\n` from the actual peer server (Member 3).
- The generic networking library intentionally does not parse commands or enforce file permissions/MD5. That work belongs to the owning server/client callback.
- Numeric ports and other binary integers placed on the network must be in network byte order (`htons`/`htonl`, etc.); the TCP connect/listener helpers use `getaddrinfo` which supplies the correct socket address representation.

### TCP I/O interface (`net_io.h`)

```c
int net_connect_tcp(const char *numeric_host, uint16_t port, int timeout_ms);
int net_set_socket_timeouts(int fd, int timeout_ms);
int net_send_all(int fd, const void *data, size_t len);
int net_recv_exact(int fd, void *buffer, size_t len);
ssize_t net_recv_line(int fd, char *buffer, size_t cap);
```

`net_recv_line` returns a line **including** the final `\n`, terminated by `\0`. It deliberately reads one byte at a time so that an immediately following binary payload is left untouched. `net_recv_exact` never silently accepts EOF before the requested length. When a helper returns `-1`, check `errno` and close the connection. Socket operations use finite configurable timeouts.

### Threaded TCP server integration (`thread_server.h`)

```c
#include "thread_server.h"
#include "net_io.h"

static volatile sig_atomic_t stopping = 0;
/* Install a SIGINT/SIGTERM handler that sets stopping = 1. */

static void handle_tracker_connection(int fd, void *context) {
    /* Member 1 supplies implementation; this is intentionally a stub.
       Read one framed command, process it, send protocol reply.
       Never call close(fd); the framework owns/ closes it. */
    (void)fd;
    (void)context;
}

/* Once a real handler has been provided, start with this configuration:
thread_server_options_t opts = {
    .bind_ip = "0.0.0.0", .port = 3490, .max_clients = 32,
    .io_timeout_ms = 5000, .backlog = 64
};
thread_server_run(&opts, handle_tracker_connection, tracker_state, &stopping);
*/
```

**Connection ownership:** server creates/listens/accepts, sets socket I/O timeouts, starts one **detached pthread per connection**, then closes the socket after the handler returns. It waits for active handlers at shutdown before destroying its synchronization structures. A callback must *not* close the client FD and must not wait indefinitely. It should process one request, send its response, and return. On overload, connections beyond `max_clients` are closed; no application-level response is fabricated.

**Shared mutable state:** the connection server does not guard user-owned structures. Member 1 must mutex-protect tracker updates and read/write of `.track` records when concurrent. Member 3 must protect any shared file metadata, writes and liveness state it owns. `pthread_mutex_t` is appropriate.

### Chunk worker integration (`download_pool.h`)

```c
download_chunk_t jobs[] = {
    {.offset = 0, .length = 1024},
    {.offset = 1024, .length = 512},
};
unsigned char success[2] = {0};

/* Member 3 supplies this callback, including peer selection, real TCP
   file transfer, pwrite(), timeout, retry target selection, and file state. */
int transfer_chunk(const download_chunk_t *chunk,
                   unsigned attempt, void *context);

/* Pseudocode: call after the real callback is supplied:
int state = download_pool_run(jobs, 2, 4, 3,
                              transfer_chunk, context, success);
*/
```

`download_pool_run` verifies all requested ranges are non-overlapping and at most 1,024 bytes. Each job is owned by exactly one worker; an unsuccessful callback is retried for the same job up to `max_attempts`. Each `success[i]` is 1 only if the callback reports that chunk completed successfully. Return values: `0` all done, `1` at least one failed, `-1` setup error. **It does not write a local completion record to disk.** The caller can persist `success` in its own recovery format and omit completed jobs on subsequent runs. If the callback writes chunks, use `pwrite` at the assigned offset. The owning module must perform final MD5 validation, delete cached tracker metadata once the file is complete, and notify the tracker as required by the assignment.

The transfer algorithm and eventual download triggering from `GET filename.track` are responsibilities of Members 2/3; they cannot be functionally integrated while their modules are absent. No real end-to-end file download has been claimed or tested yet.

### Configuration layouts (`config.h`)

`clientThreadConfig.cfg`:
```
3490
127.0.0.1
900
```

`serverThreadConfig.cfg`:
```
2097
shared
```

`sconfig`:
```
3490
torrents
```

`config_read_client()`, `config_read_server()`, and `config_read_tracker()` validate the exact fields and reject extra non-whitespace data. The original peer client has its own config parser and is unaffected. The folders supplied by the zip are placeholders; relative paths are interpreted from each program's **working directory**. For peer1, run from within `peer1/` or supply explicit paths when the team's final launcher is established.

## 4. Repeatable tests

```sh
make test-member4   # Member 4 tests only
make test           # Member 4 tests plus existing Member 2 client tests
```

Infrastructure test cases verify:

- Actual concurrent TCP requests (four-worker barrier cannot complete with serial handlers).
- Maximum connection worker count, termination and I/O timeouts.
- Fragmented TCP command headers and exact 1,024-byte binary payloads with NUL bytes.
- Zero-length and oversized test payload handling in the **test fixture only**.
- Truncated, malformed, and NUL-containing text requests.
- Client/server/tracker configuration validation including existing peer1–3 files.
- Four concurrent chunk jobs, retries, failed-job status, overlapping-job rejection.
- Direct socket I/O helper behavior, TCP connect, and I/O timeout.

Manual local observation of **test-only infrastructure**:

```sh
make tests/member4_harness
./tests/member4_harness peer1/serverThreadConfig.cfg
```

In a second terminal (with the harness running):

```sh
python3 - <<'PY'
import socket
with socket.create_connection(('127.0.0.1', 2097)) as s:
    s.sendall(b'<PING example>\n')
    print(s.recv(100).decode(), end='')
PY
```

It should print `<PONG example>` and the harness logs a worker thread ID. **These messages are for testing only and are not professor-required tracker or file-transfer protocol commands.** The test harness deliberately listens only on loopback; it is not a two-machine demo.

## 5. Critical next integration tasks (depend on teammates)

1. **Member 1:** contribute a real `tracker.c`, implement all four required command handlers, read `sconfig`, and synchronize file registration/update. Wire these handlers into `thread_server_run()` and match Member 2's exact requests and expected responses. Test against the real `peer.c`, not just mocks.
2. **Member 3:** contribute a real peer server and chunk-transfer implementation, including oversize error `<GET invalid>\n`, safe paths, byte-range validation, correct binary payload, and `pwrite()`. Wire its server handler into `thread_server_run()` and its download callback into `download_pool_run()`.
3. **Member 2 + integration:** decide how to start the peer server and auto-start file downloads after a tracker GET **without changing `peer.c` unless Member 2 agrees**. Current peer executable only performs tracker communication; simply building the Member 4 library does not make it an integrated peer.
4. **Two-machine check:** compile on Ubuntu/WSL2, use actual LAN addresses in peer config, verify firewall/inbound reachability, and compare source/destination MD5 of a real downloaded file. This **cannot** be marked complete until the production server/client exist.
5. **Midterm report:** gather screenshots of the actual tracker, real transfer, worker-thread logs, completed tests, and each team member's contributions.

## 6. External components and attribution

- POSIX C / Linux sockets and threads via the system C library (`pthread`, `poll`, `socket`, `getaddrinfo`), headers, and standard documentation. Threading and I/O helper implementation in this contribution is original.
- Existing `peer.c` already links OpenSSL libcrypto for MD5. See https://docs.openssl.org/ and https://man7.org/linux/man-pages/ for API documentation.
- Python 3 standard library `unittest`, `subprocess`, and `socket` for test orchestration, https://docs.python.org/3/library/unittest.html.

The assignment recommends *Beej's Guide to Network Programming* (https://beej.us/guide/bgnet/), but no code was copied from it.
