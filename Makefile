CC = cc
CFLAGS = -std=c11 -Wall -Wextra -Wpedantic -Werror -O2 -pthread -g
LDLIBS = -lcrypto
PYTHON = python3

.PHONY: all test clean
all: peer1/peer peer2/peer peer3/peer tracker

tracker: tracker.c
	$(CC) $(CFLAGS) -o tracker tracker.c

peer1/peer: peer.c
	$(CC) $(CPPFLAGS) $(CFLAGS) $< -o $@ $(LDFLAGS) $(LDLIBS)

peer2/peer peer3/peer: peer1/peer
	cp $< $@

test: all
	$(PYTHON) tests/test_peer.py

clean:
	rm -f peer1/peer peer2/peer peer3/peer
	rm -f tracker
	rm -rf torrents
