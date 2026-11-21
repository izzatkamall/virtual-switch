CC     = gcc
CFLAGS = -std=c11 -Wall -Wextra -Werror -g -O0
LIBS   = -pthread -lrt

COMMON = src/frame.c src/net_io.c src/log.c
HDRS   = $(wildcard src/*.h)

PROGS = switch host
TESTS = test_frame_mac test_net_io test_qos

all: $(PROGS) $(TESTS)

switch: src/switch.c src/mac_table.c src/config.c src/qos.c $(COMMON) $(HDRS)
	$(CC) $(CFLAGS) -o $@ src/switch.c src/mac_table.c src/config.c src/qos.c $(COMMON) $(LIBS)

host: src/host.c $(COMMON) $(HDRS)
	$(CC) $(CFLAGS) -o $@ src/host.c $(COMMON) $(LIBS)

test_frame_mac: tests/test_frame_mac.c src/mac_table.c $(COMMON) $(HDRS)
	$(CC) $(CFLAGS) -o $@ tests/test_frame_mac.c src/mac_table.c $(COMMON) $(LIBS)

test_net_io: tests/test_net_io.c $(COMMON) $(HDRS)
	$(CC) $(CFLAGS) -o $@ tests/test_net_io.c $(COMMON) $(LIBS)

test_qos: tests/test_qos.c src/qos.c $(HDRS)
	$(CC) $(CFLAGS) -o $@ tests/test_qos.c src/qos.c $(LIBS)

test: $(TESTS)
	./test_frame_mac
	./test_net_io
	./test_qos

# needs libtsan installed
tsan:
	$(MAKE) clean
	$(MAKE) CFLAGS="$(CFLAGS) -fsanitize=thread" test

clean:
	rm -f $(PROGS) $(TESTS)

.PHONY: all test tsan clean
