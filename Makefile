CC = cc
CFLAGS = -std=c11 -Wall -Wextra -Wpedantic -Werror -O2 -pthread -g
LDLIBS = -lcrypto
PYTHON = python3

COMMON_OBJS = net_io.o config.o thread_server.o
NODE_OBJS = peer_node.o peer_server.o node_download.o $(COMMON_OBJS)

.PHONY: all test clean
all: peer1/peer peer2/peer peer3/peer peer1/node peer2/node peer3/node tracker peerServer download

net_io.o: net_io.c net_io.h
	$(CC) $(CFLAGS) -c $< -o $@
config.o: config.c config.h
	$(CC) $(CFLAGS) -c $< -o $@
thread_server.o: thread_server.c thread_server.h net_io.h
	$(CC) $(CFLAGS) -c $< -o $@
peer_server.o: peerServer.c peer_server.h thread_server.h config.h net_io.h
	$(CC) $(CFLAGS) -c $< -o $@
peer_node.o: peer_node.c node_download.h peer_server.h thread_server.h config.h
	$(CC) $(CFLAGS) -c $< -o $@
node_download.o: node_download.c node_download.h net_io.h
	$(CC) $(CFLAGS) -c $< -o $@

tracker: tracker.c net_io.o
	$(CC) $(CFLAGS) -o $@ tracker.c net_io.o $(LDLIBS)
peerServer: peerServer.c peer_server.h $(COMMON_OBJS)
	$(CC) $(CFLAGS) -DPEER_SERVER_STANDALONE -o $@ peerServer.c $(COMMON_OBJS)
download: download.c
	$(CC) $(CFLAGS) -o $@ download.c

peer1/peer: peer.c
	$(CC) $(CFLAGS) -o $@ $< $(LDLIBS)
peer2/peer peer3/peer: peer1/peer
	cp $< $@
peer1/node: $(NODE_OBJS)
	$(CC) $(CFLAGS) -o $@ $(NODE_OBJS) $(LDLIBS)
peer2/node peer3/node: peer1/node
	cp $< $@

test: all
	$(PYTHON) tests/test_peer.py
	@if test -f tests/test_integration.py; then $(PYTHON) tests/test_integration.py; fi

clean:
	rm -f tracker peerServer download peer1/peer peer2/peer peer3/peer peer1/node peer2/node peer3/node
	rm -f $(COMMON_OBJS) peer_server.o peer_node.o node_download.o
