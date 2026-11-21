// tests for net_io.c using a socketpair
#define _POSIX_C_SOURCE 200809L

#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#include "../src/net_io.h"

static int fails = 0;

#define CHECK(x) do { \
    if (!(x)) { printf("FAIL line %d: %s\n", __LINE__, #x); fails++; } \
} while (0)

static int fds[2];

// sends a frame one byte at a time to force partial reads
static void *slow_writer(void *arg)
{
    (void)arg;
    struct frame_header h = {.length = 5, .vlan = 1, .priority = PRIO_NORMAL,
                             .src = {2, 0, 0, 0, 0, 1}, .dst = {2, 0, 0, 0, 0, 2}};
    uint8_t buf[FRAME_HEADER_LEN + 5];
    frame_header_serialize(&h, buf);
    memcpy(buf + FRAME_HEADER_LEN, "hello", 5);

    for (size_t i = 0; i < sizeof(buf); i++) {
        if (write(fds[1], buf + i, 1) != 1)
            return (void *)1;
        struct timespec ts = {0, 1000000};  // 1ms between bytes
        nanosleep(&ts, NULL);
    }
    return NULL;
}

static void test_partial_reads(void)
{
    CHECK(socketpair(AF_UNIX, SOCK_STREAM, 0, fds) == 0);

    pthread_t t;
    CHECK(pthread_create(&t, NULL, slow_writer, NULL) == 0);

    struct frame_header h;
    uint8_t payload[FRAME_MAX_PAYLOAD];
    CHECK(frame_read(fds[0], &h, payload) == 1);
    CHECK(h.length == 5);
    CHECK(memcmp(payload, "hello", 5) == 0);
    CHECK(h.src[5] == 1 && h.dst[5] == 2);

    pthread_join(t, NULL);
    close(fds[1]);
    CHECK(frame_read(fds[0], &h, payload) == 0);  // clean close
    close(fds[0]);
}

static void test_cut_mid_frame(void)
{
    CHECK(socketpair(AF_UNIX, SOCK_STREAM, 0, fds) == 0);
    uint8_t half[10] = {0};
    CHECK(write(fds[1], half, sizeof(half)) == sizeof(half));
    close(fds[1]);

    struct frame_header h;
    uint8_t payload[FRAME_MAX_PAYLOAD];
    CHECK(frame_read(fds[0], &h, payload) == -1);
    close(fds[0]);
}

static void test_bad_header(void)
{
    CHECK(socketpair(AF_UNIX, SOCK_STREAM, 0, fds) == 0);
    struct frame_header h = {.length = 10, .vlan = 0, .priority = 0};  // vlan 0 is bad
    uint8_t buf[FRAME_HEADER_LEN];
    frame_header_serialize(&h, buf);
    CHECK(write(fds[1], buf, sizeof(buf)) == sizeof(buf));

    uint8_t payload[FRAME_MAX_PAYLOAD];
    CHECK(frame_read(fds[0], &h, payload) == -1);
    close(fds[0]);
    close(fds[1]);
}

static void test_write_to_closed(void)
{
    CHECK(socketpair(AF_UNIX, SOCK_STREAM, 0, fds) == 0);
    close(fds[0]);

    struct frame_header h = {.length = 3, .vlan = 1};
    CHECK(frame_write(fds[1], &h, (const uint8_t *)"abc") == -1);  // EPIPE, no crash
    close(fds[1]);
}

static void test_big_frame(void)
{
    // max size frame through a pair, read back whole
    CHECK(socketpair(AF_UNIX, SOCK_STREAM, 0, fds) == 0);
    uint8_t out[FRAME_MAX_PAYLOAD], in[FRAME_MAX_PAYLOAD];
    for (int i = 0; i < FRAME_MAX_PAYLOAD; i++)
        out[i] = (uint8_t)i;

    struct frame_header h = {.length = FRAME_MAX_PAYLOAD, .vlan = 4094, .priority = PRIO_LOW};
    CHECK(frame_write(fds[1], &h, out) == 0);
    struct frame_header got;
    CHECK(frame_read(fds[0], &got, in) == 1);
    CHECK(got.length == FRAME_MAX_PAYLOAD && got.vlan == 4094);
    CHECK(memcmp(in, out, FRAME_MAX_PAYLOAD) == 0);
    close(fds[0]);
    close(fds[1]);
}

int main(void)
{
    signal(SIGPIPE, SIG_IGN);

    test_partial_reads();
    test_cut_mid_frame();
    test_bad_header();
    test_write_to_closed();
    test_big_frame();

    if (fails == 0)
        printf("net_io: all tests passed\n");
    else
        printf("net_io: %d tests failed\n", fails);
    return fails == 0 ? 0 : 1;
}
