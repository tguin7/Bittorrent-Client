# CS4390 Member 2 â€” peer client and tracker communication

This component handles communication between a peer and the tracker server.
It connects over TCP, implements LIST and GET, parses replies and tracker records,
verifies the tracker payload MD5, and atomically saves valid metadata in a local
cache. Manual `createtracker` and `updatetracker` requests are also supported.

Shared-file transfer, automatic download initiation, multithreading, periodic
updates, and resume are not implemented in this component. A
successful GET downloads `.track` metadata only. The Python mock is a test double,
not a submitted production tracker. It is deliberately sequential.

## Build and run (Ubuntu/Linux or Ubuntu in WSL)

Prerequisites: a C11 compiler, Make, OpenSSL development headers/library, Python 3.
On Ubuntu, if needed: `sudo apt-get install build-essential libssl-dev python3`.

From this folder:

```sh
make
make test
python3 mock_tracker.py --host 127.0.0.1 --port 3490
```

Leave the mock running. In another terminal:

```sh
cd peer1
./peer
```

The Makefile compiles `peer.c` and places executables in exactly `peer1`, `peer2`,
and `peer3`. Run each from its own folder. Each has its own configuration and
local `cache/` directory, created on first successful GET. The archive contains
source only: no executables or object files. `make clean` removes built executables
and deliberately preserves caches. Native Windows compilation is not supported.

At the `peer>` prompt, type the following, without angle brackets:

```text
createtracker hello.txt 3 Demo 900150983cd24fb0d6963f7d28e17f72 127.0.0.1 2097
REQ LIST
GET hello.txt.track
updatetracker hello.txt 0 3 127.0.0.1 2097
quit
```

The fixture describes the three bytes `abc`; the mock does **not** serve those
bytes. The example creates a tracker record, not an actual seed server.
`LIST` is a convenience alias for `REQ LIST`. Keywords are case-insensitive.
Descriptions must be one token; use underscores for spaces. `quit` exits the
manual prompt. A command failure leaves the prompt usable but makes the final
process exit status nonzero.

For scripts, a command can be supplied directly:

```sh
./peer --config clientThreadConfig.cfg --cache cache GET hello.txt.track
```

Options precede the command. Exit status is 0 for success, 1 for configuration,
network, server, validation, or storage failure; invalid options/oversize CLI
arguments return 2. Config and cache paths are relative to the working directory.

## Configuration

`clientThreadConfig.cfg` follows the PDF's exact three-line layout:

```text
3490
127.0.0.1
900
```

These are tracker port, numeric IP address, and update interval in seconds.
IPv4 and IPv6 tracker addresses are accepted; DNS hostnames are intentionally
not accepted. The interval is validated but **no periodic worker
exists yet**. Default 900 seconds equals the required 15 minutes.

The supplied `serverThreadConfig.cfg` files reserve ports 2097, 2098, and 2099
and folder `shared` for integration. This client does not read these files or
start a peer server. Set the tracker IP separately on each machine before use.

## Wire protocol and explicit assumptions

Requests contain ASCII text, a literal LF byte, and no terminating NUL:

| Manual command | Bytes sent (the notation `\n` means one LF byte) |
|---|---|
| LIST / REQ LIST | `<REQ LIST>\n` |
| GET hello.txt.track | `<GET hello.txt.track >\n` |
| createtracker ... | `<createtracker filename filesize description md5 ip-address port-number>\n` |
| updatetracker ... | `<updatetracker filename start_byte end_byte ip-address port-number>\n` |

GET preserves the PDF's space before `>`. LIST requires `<REP LIST X>`, exactly
X consecutively numbered rows with filename/size/32-hex MD5, and
`<REP LIST END>`, each followed by LF. GET requires `<REP GET BEGIN>\n` and a final
`<REP GET END checksum>\n`. Extra bytes, NUL bytes, missing terminators, and
premature close are rejected. Control-message CRLF is not silently normalized.

The PDF's `<tracker_file_content >` notation is ambiguous. The default mock
sends raw tracker file bytes between BEGIN and END. The client also accepts a
single literal wrapper `<` + original bytes + ` >\n` when the MD5 matches the
unwrapped bytes. No whitespace or line ending inside the payload is changed
before hashing or caching. Use this interpretation when connecting the tracker server. No checksum match means no cache write.

The MD5 in the GET footer is the checksum of the **tracker file bytes**. The
`MD5:` field inside that file refers to the **shared file**; it is checked for
valid syntax but cannot be verified until the actual shared file is downloaded.
MD5 is used because the course protocol requires it; it is not authentication.

Metadata fields must appear in the assignment order: Filename, Filesize,
optional Description, MD5, then peer records. Blank lines and lines starting
with `#` are ignored for parsing, but included in the checksum. Peer records use
the PDF's IPv4 colon-separated layout. Range validation permits
`0 <= start <= end <= filesize`; the mock uses an exclusive end offset, matching
the example `0:filesize`. Use consistent chunk boundary semantics in the transfer component.
An empty peer list is accepted (discovery can succeed without a live seed).

The tracker must close its connection after each response, as the PDF requires.
The client reads until close, with one five-second deadline covering connection,
send, and receive. It bounds replies at 1 MiB and LIST counts at 10,000. File
names use letters/digits, dot, underscore, or hyphen, cannot start with a dot,
and are at most 180 bytes including `.track`. These are conservative local
limits, not additional protocol claims. The cache directory is trusted local
configuration. A temporary file is flushed, synced, and renamed only after
validation; failed retrievals preserve an existing cached file. Cache rename
is atomic, but no directory fsync/power-loss durability guarantee is made.

The PDF specifies no GET-missing error reply. The mock uses `<REP GET ferr>\n`;
the client treats any reply without valid success framing as a failure. Manual
create/update succeeds only on the exact expected `succ` response; `ferr`,
`fail`, wrong filenames, and malformed replies yield nonzero status.

## Code map

- `peer.c`: configuration, bounded nonblocking TCP exchange, request construction,
  LIST/GET parsing, OpenSSL EVP checksum, metadata validation, atomic cache, CLI.
- `mock_tracker.py`: in-memory registration/update/list/get test server; no expiry,
  file serving, persistence, or concurrent request handling.
- `tests/test_peer.py`: black-box tests against the compiled C client using real
  loopback connections, with malformed replies and deliberate fragmentation.
- `RECORDING_WALKTHROUGH.md`: manual demo commands, narration, and integration gates.
- `TEST_RESULTS.md`: results actually observed in this environment.

The supplied `peer.c` established the socket/connect/request outline but lacked
an initialized address, definitions for LIST/msg, and working parsing. This
implementation completes that outline and replaces the numeric LIST with the
PDF text protocol. `skeleton_peer.c` is actually RTF text despite its extension.
Both tracker starter variants have incomplete handlers and use `fork()` rather
than threads; they cannot establish production compatibility by themselves.
The provided originals are preserved separately under `reference/`.

## Remaining functionality

- Verify all four commands against the production tracker, including payload
  wrapping and error responses.
- After a successful GET, use the verified tracker data to select peers and
  request file chunks with a maximum size of 1,024 bytes. Merge the received
  data and verify the shared-file checksum.
- Add concurrent workers, periodic updates, retries, resume, and cache cleanup
  after a completed download.
- Validate the complete system with three peers across two machines.

The current download entry point is the successful return from `get_parse`.
Metadata is validated and saved, but is not yet exposed as a structured API.

## Sources and dependencies

- Supplied `project-CS4390-501.pdf`, especially pages 2â€“5, 7â€“9: protocol, config,
  cache, and demo requirements; supplied team plan: Member 2 scope.
- Supplied `peer.c`, `skeleton_peer.c`, `tracker.c`, `skeleton_tracker.rtf`:
  socket/connect outline and tracker command dispatch.
- Supplied Socket Programming Tutorial (Beej's Guide): sockets, partial sends,
  TCP framing. Project site: https://beej.us/guide/bgnet/
- OpenSSL `libcrypto`, EVP digest API: https://docs.openssl.org/3.0/man3/EVP_DigestInit/
- Python standard library (socket, hashlib, unittest, subprocess):
  https://docs.python.org/3/library/
- Build uses the system C library/POSIX sockets and a C11 compiler/Make.

No third-party implementation source was copied. This package was developed
with AI assistance; review the code, understand it, and follow your course's
disclosure rules when incorporating it into your team's submission.
