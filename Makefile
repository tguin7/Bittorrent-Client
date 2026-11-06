# CS 4390 integrated build. Member 2's original peer.c remains unchanged.
CC ?= cc
AR ?= ar
PYTHON ?= python3
CFLAGS ?= -std=c11 -Wall -Wextra -Wpedantic -Werror -O2 -g
CPPFLAGS ?=
LDFLAGS ?=
LDLIBS ?= -lcrypto
THREAD_FLAGS = -pthread
MEMBER4_OBJECTS = net_io.o config.o thread_server.o download_pool.o
LIBRARY = libmember4.a

.PHONY: all test test-member4 test-integration clean
all: tracker peerServer download peer1/peer peer2/peer peer3/peer peer1/node peer2/node peer3/node

%.o: %.c %.h
	$(CC) $(CPPFLAGS) $(CFLAGS) $(THREAD_FLAGS) -c $< -o $@
thread_server.o: thread_server.c thread_server.h net_io.h
	$(CC) $(CPPFLAGS) $(CFLAGS) $(THREAD_FLAGS) -c $< -o $@
$(LIBRARY): $(MEMBER4_OBJECTS)
	$(AR) rcs $@ $^

tracker: tracker.c $(LIBRARY)
	$(CC) $(CPPFLAGS) $(CFLAGS) $(THREAD_FLAGS) $< -o $@ $(LIBRARY) $(LDFLAGS) $(LDLIBS)

peer1/peer: peer.c
	$(CC) $(CPPFLAGS) $(CFLAGS) $< -o $@ $(LDFLAGS) $(LDLIBS)
peer2/peer peer3/peer: peer1/peer
	cp $< $@

peer_server.o: peerServer.c peer_server.h thread_server.h net_io.h config.h
	$(CC) $(CPPFLAGS) $(CFLAGS) $(THREAD_FLAGS) -c peerServer.c -o $@
node_download.o: node_download.c node_download.h net_io.h download_pool.h
	$(CC) $(CPPFLAGS) $(CFLAGS) $(THREAD_FLAGS) -c node_download.c -o $@
peer_node.o: peer_node.c node_download.h peer_server.h config.h thread_server.h
	$(CC) $(CPPFLAGS) $(CFLAGS) $(THREAD_FLAGS) -c peer_node.c -o $@

peer1/node: peer_node.o peer_server.o node_download.o $(LIBRARY)
	$(CC) $(CPPFLAGS) $(CFLAGS) $(THREAD_FLAGS) peer_node.o peer_server.o node_download.o -o $@ $(LIBRARY) $(LDFLAGS) $(LDLIBS)
peer2/node peer3/node: peer1/node
	cp $< $@

peerServer: peerServer.c $(LIBRARY)
	$(CC) $(CPPFLAGS) $(CFLAGS) $(THREAD_FLAGS) -DPEER_SERVER_STANDALONE peerServer.c -o $@ $(LIBRARY) $(LDFLAGS)
download: download.c
	$(CC) $(CPPFLAGS) $(CFLAGS) $(THREAD_FLAGS) $< -o $@ $(LDFLAGS)

tests/member4_harness: tests/member4_harness.c $(LIBRARY)
	$(CC) $(CPPFLAGS) $(CFLAGS) $(THREAD_FLAGS) -I. $< -o $@ $(LIBRARY) $(LDFLAGS)
tests/member4_config_probe: tests/member4_config_probe.c $(LIBRARY)
	$(CC) $(CPPFLAGS) $(CFLAGS) $(THREAD_FLAGS) -I. $< -o $@ $(LIBRARY) $(LDFLAGS)
tests/member4_pool_test: tests/member4_pool_test.c $(LIBRARY)
	$(CC) $(CPPFLAGS) $(CFLAGS) $(THREAD_FLAGS) -I. $< -o $@ $(LIBRARY) $(LDFLAGS)
tests/member4_io_test: tests/member4_io_test.c $(LIBRARY)
	$(CC) $(CPPFLAGS) $(CFLAGS) $(THREAD_FLAGS) -I. $< -o $@ $(LIBRARY) $(LDFLAGS)

test-member4: tests/member4_harness tests/member4_config_probe tests/member4_pool_test tests/member4_io_test
	./tests/member4_pool_test
	./tests/member4_io_test
	$(PYTHON) tests/test_member4.py

test-integration: all
	$(PYTHON) tests/test_integration.py

test: all test-member4
	$(PYTHON) tests/test_peer.py
	$(PYTHON) tests/test_integration.py

clean:
	rm -f tracker peerServer download peer1/peer peer2/peer peer3/peer peer1/node peer2/node peer3/node
	rm -f $(MEMBER4_OBJECTS) $(LIBRARY) peer_server.o peer_node.o node_download.o
	rm -f tests/member4_harness tests/member4_config_probe tests/member4_pool_test tests/member4_io_test
