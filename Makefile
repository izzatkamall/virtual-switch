CC      := gcc
CFLAGS  := -std=c11 -Wall -Wextra -Werror -g -O0
LDLIBS  := -pthread

SRC_DIR := src
TEST_DIR := tests

CORE_SRCS := $(SRC_DIR)/frame.c $(SRC_DIR)/mac_table.c
CORE_HDRS := $(SRC_DIR)/frame.h $(SRC_DIR)/mac_table.h

# Programs (switch, host, swctl) get added here as they are written.
PROGRAMS := test_frame_mac

.PHONY: all clean test tsan asan

all: $(PROGRAMS)

test_frame_mac: $(TEST_DIR)/test_frame_mac.c $(CORE_SRCS) $(CORE_HDRS)
	$(CC) $(CFLAGS) -o $@ $(TEST_DIR)/test_frame_mac.c $(CORE_SRCS) $(LDLIBS)

test: test_frame_mac
	./test_frame_mac

# ThreadSanitizer build of the tests
tsan: $(TEST_DIR)/test_frame_mac.c $(CORE_SRCS) $(CORE_HDRS)
	$(CC) $(CFLAGS) -fsanitize=thread -o test_frame_mac_tsan $(TEST_DIR)/test_frame_mac.c $(CORE_SRCS) $(LDLIBS)
	./test_frame_mac_tsan

# AddressSanitizer + leak check (stand-in when valgrind isn't installed)
asan: $(TEST_DIR)/test_frame_mac.c $(CORE_SRCS) $(CORE_HDRS)
	$(CC) $(CFLAGS) -fsanitize=address,undefined -o test_frame_mac_asan $(TEST_DIR)/test_frame_mac.c $(CORE_SRCS) $(LDLIBS)
	./test_frame_mac_asan

clean:
	rm -f $(PROGRAMS) test_frame_mac_tsan test_frame_mac_asan
