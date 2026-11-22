CC     = gcc
CFLAGS = -std=c11 -Wall -Wextra -Werror -g -O0
LIBS   = -pthread -lrt

COMMON = src/frame.c src/net_io.c src/log.c
HDRS   = $(wildcard src/*.h)

PROGS = switch host swctl
TESTS = test_frame_mac test_net_io test_qos

all: $(PROGS) $(TESTS)

switch: src/switch.c src/mac_table.c src/config.c src/qos.c src/shm_stats.c $(COMMON) $(HDRS)
	$(CC) $(CFLAGS) -o $@ src/switch.c src/mac_table.c src/config.c src/qos.c src/shm_stats.c $(COMMON) $(LIBS)

swctl: src/swctl.c src/shm_stats.c src/net_io.c src/frame.c $(HDRS)
	$(CC) $(CFLAGS) -o $@ src/swctl.c src/shm_stats.c src/net_io.c src/frame.c $(LIBS)

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

# every test, unit and live, against fresh switches
check: all
	tests/run_all.sh

# every test with everything built with thread sanitizer, a race makes
# the switch exit with 66 so the test fails. rebuild normally afterwards
tsan:
	$(MAKE) clean
	$(MAKE) CFLAGS="$(CFLAGS) -fsanitize=thread" all
	TSAN_OPTIONS="exitcode=66" tests/run_all.sh
	$(MAKE) clean
	$(MAKE) all

# every live test with the switch under valgrind, any error or leak
# makes it exit with 99 so the test fails
VALGRIND = valgrind -q --leak-check=full --show-leak-kinds=all --errors-for-leak-kinds=all --error-exitcode=99
valgrind: all
	RUN="$(VALGRIND)" STOP_SECS=10 tests/run_all.sh

clean:
	rm -f $(PROGS) $(TESTS)

.PHONY: all test check tsan valgrind clean
