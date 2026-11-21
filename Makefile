CC     = gcc
CFLAGS = -std=c11 -Wall -Wextra -Werror -g -O0
LIBS   = -pthread -lrt

COMMON = src/frame.c src/net_io.c
HDRS   = $(wildcard src/*.h)

TESTS = test_frame_mac test_net_io

all: $(TESTS)

test_frame_mac: tests/test_frame_mac.c src/frame.c src/mac_table.c $(HDRS)
	$(CC) $(CFLAGS) -o $@ tests/test_frame_mac.c src/frame.c src/mac_table.c $(LIBS)

test_net_io: tests/test_net_io.c $(COMMON) $(HDRS)
	$(CC) $(CFLAGS) -o $@ tests/test_net_io.c $(COMMON) $(LIBS)

test: $(TESTS)
	./test_frame_mac
	./test_net_io

# needs libtsan installed
tsan:
	$(MAKE) clean
	$(MAKE) CFLAGS="$(CFLAGS) -fsanitize=thread" test

clean:
	rm -f $(TESTS) switch host swctl

.PHONY: all test tsan clean
