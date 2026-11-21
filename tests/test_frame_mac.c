// tests for frame.c and mac_table.c
#define _POSIX_C_SOURCE 200809L

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "../src/frame.h"
#include "../src/mac_table.h"

static int fails = 0;

#define CHECK(x) do { \
    if (!(x)) { printf("FAIL line %d: %s\n", __LINE__, #x); fails++; } \
} while (0)

static void test_frame(void)
{
    struct frame_header h = {
        .length = 0x1234, .vlan = 0x0abc, .priority = PRIO_LOW,
        .src = {0x00, 0x11, 0x22, 0x33, 0x44, 0x55},
        .dst = {0xff, 0xff, 0xff, 0xff, 0xff, 0xff},
    };
    uint8_t buf[FRAME_HEADER_LEN];
    frame_header_serialize(&h, buf);

    // what the bytes should look like on the wire
    uint8_t want[FRAME_HEADER_LEN] = {
        0x12, 0x34,
        0x00, 0x11, 0x22, 0x33, 0x44, 0x55,
        0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
        0x0a, 0xbc,
        0x02
    };
    CHECK(memcmp(buf, want, FRAME_HEADER_LEN) == 0);

    struct frame_header back;
    frame_header_deserialize(buf, &back);
    CHECK(back.length == 0x1234);
    CHECK(back.vlan == 0x0abc);
    CHECK(back.priority == PRIO_LOW);
    CHECK(mac_equal(back.src, h.src));
    CHECK(mac_is_broadcast(back.dst));

    CHECK(frame_header_validate(&h) == -1);  // length too big
    h.length = 1500; h.vlan = 1;
    CHECK(frame_header_validate(&h) == 0);
    h.vlan = 0;
    CHECK(frame_header_validate(&h) == -1);
    h.vlan = 4095;
    CHECK(frame_header_validate(&h) == -1);
    h.vlan = 10; h.priority = 3;
    CHECK(frame_header_validate(&h) == -1);

    uint8_t mac[MAC_LEN];
    char s[MAC_STR_LEN];
    CHECK(mac_from_string("aa:bb:cc:dd:ee:0f", mac) == 0);
    mac_to_string(mac, s);
    CHECK(strcmp(s, "AA:BB:CC:DD:EE:0F") == 0);
    CHECK(mac_from_string("aa:bb:cc", mac) == -1);
    CHECK(mac_from_string("zz:bb:cc:dd:ee:ff", mac) == -1);
}

static void test_mac_basic(void)
{
    struct mac_table t;
    CHECK(mac_table_init(&t) == 0);

    uint8_t a[MAC_LEN] = {0, 0, 0, 0, 0, 1};
    uint8_t b[MAC_LEN] = {0, 0, 0, 0, 0, 2};

    CHECK(mac_table_lookup(&t, a) == -1);
    CHECK(mac_table_insert(&t, a, 3) == MAC_NEW);
    CHECK(mac_table_insert(&t, b, 4) == MAC_NEW);
    CHECK(mac_table_lookup(&t, a) == 3);
    CHECK(mac_table_lookup(&t, b) == 4);
    CHECK(mac_table_insert(&t, a, 3) == MAC_SAME);
    CHECK(mac_table_insert(&t, a, 7) == MAC_MOVED);
    CHECK(mac_table_lookup(&t, a) == 7);
    CHECK(mac_table_insert(&t, MAC_BROADCAST, 1) == MAC_ERROR);

    char *text = mac_table_to_string(&t);
    CHECK(text != NULL);
    CHECK(text != NULL && strstr(text, "00:00:00:00:00:01  7") != NULL);
    CHECK(text != NULL && strstr(text, "2 entries") != NULL);
    free(text);

    mac_table_destroy(&t);
}

// counts entries by walking the lists, to compare with the counter
static int real_count(struct mac_table *t)
{
    int n = 0;
    for (int i = 0; i < MAC_TABLE_BUCKETS; i++)
        for (struct mac_entry *e = t->buckets[i].head; e != NULL; e = e->next)
            n++;
    return n;
}

static void test_mac_lru(void)
{
    struct mac_table t;
    CHECK(mac_table_init(&t) == 0);
    mac_table_set_limits(&t, 3, 100);
    struct timespec wait = {0, 5 * 1000000};  // 5ms so timestamps differ

    uint8_t m[5][MAC_LEN];
    for (int i = 0; i < 5; i++) {
        uint8_t x[MAC_LEN] = {2, 0, 0, 0, 1, (uint8_t)i};
        memcpy(m[i], x, MAC_LEN);
    }

    for (int i = 0; i < 3; i++) {
        mac_table_insert(&t, m[i], i);
        nanosleep(&wait, NULL);
    }
    CHECK(mac_table_count(&t) == 3);

    // touch m0 so m1 is now the oldest
    mac_table_insert(&t, m[0], 0);
    nanosleep(&wait, NULL);

    mac_table_insert(&t, m[3], 3);  // full, m1 should go
    CHECK(mac_table_count(&t) == 3);
    CHECK(mac_table_lookup(&t, m[1]) == -1);
    CHECK(mac_table_lookup(&t, m[0]) == 0);
    CHECK(mac_table_lookup(&t, m[2]) == 2);
    CHECK(mac_table_lookup(&t, m[3]) == 3);

    nanosleep(&wait, NULL);
    mac_table_insert(&t, m[4], 4);  // now m2 is oldest
    CHECK(mac_table_lookup(&t, m[2]) == -1);
    CHECK(mac_table_count(&t) == 3);

    mac_table_destroy(&t);
}

static void test_mac_port_security(void)
{
    struct mac_table t;
    CHECK(mac_table_init(&t) == 0);
    mac_table_set_limits(&t, 100, 2);  // 2 macs per port

    uint8_t a[MAC_LEN] = {2, 0, 0, 0, 2, 1};
    uint8_t b[MAC_LEN] = {2, 0, 0, 0, 2, 2};
    uint8_t c[MAC_LEN] = {2, 0, 0, 0, 2, 3};

    CHECK(mac_table_insert(&t, a, 0) == MAC_NEW);
    CHECK(mac_table_insert(&t, b, 0) == MAC_NEW);
    CHECK(mac_table_insert(&t, c, 0) == MAC_PORT_FULL);  // third one on port 0
    CHECK(mac_table_lookup(&t, c) == -1);
    CHECK(mac_table_insert(&t, a, 0) == MAC_SAME);       // old ones still fine
    CHECK(mac_table_port_count(&t, 0) == 2);

    // c on port 1 is fine, then a moving to port 1 fills it
    CHECK(mac_table_insert(&t, c, 1) == MAC_NEW);
    CHECK(mac_table_insert(&t, a, 1) == MAC_MOVED);
    CHECK(mac_table_port_count(&t, 0) == 1);
    CHECK(mac_table_port_count(&t, 1) == 2);
    CHECK(mac_table_insert(&t, b, 1) == MAC_PORT_FULL);  // cant move, port 1 full
    CHECK(mac_table_lookup(&t, b) == 0);                 // b stays where it was

    // flushing a port frees its slots
    CHECK(mac_table_remove_port(&t, 1) == 2);
    CHECK(mac_table_port_count(&t, 1) == 0);
    CHECK(mac_table_count(&t) == 1);
    CHECK(mac_table_lookup(&t, a) == -1);
    CHECK(mac_table_lookup(&t, b) == 0);

    mac_table_destroy(&t);
}

static void test_mac_aging(void)
{
    struct mac_table t;
    CHECK(mac_table_init(&t) == 0);

    uint8_t a[MAC_LEN] = {0, 0, 0, 0, 0, 1};
    uint8_t b[MAC_LEN] = {0, 0, 0, 0, 0, 2};
    struct timespec wait = {0, 50 * 1000000};  // 50ms

    mac_table_insert(&t, a, 1);
    nanosleep(&wait, NULL);
    mac_table_insert(&t, b, 2);

    CHECK(mac_table_age(&t, 1000) == 0);  // nothing is a second old
    CHECK(mac_table_age(&t, 30) == 1);    // only a is older than 30ms
    CHECK(mac_table_lookup(&t, a) == -1);
    CHECK(mac_table_lookup(&t, b) == 2);

    // seeing a mac again resets its age
    nanosleep(&wait, NULL);
    mac_table_insert(&t, b, 2);
    CHECK(mac_table_age(&t, 30) == 0);
    CHECK(mac_table_lookup(&t, b) == 2);

    mac_table_destroy(&t);
}

// lots of threads hitting the same macs, run this with tsan
#define NTHREADS 8
#define ITERS 2000

static struct mac_table shared;

static void *worker(void *arg)
{
    int port = *(int *)arg;
    for (int i = 0; i < ITERS; i++) {
        uint8_t mac[MAC_LEN] = {0x02, 0, 0, 0, 0, (uint8_t)(i % 200)};
        if (mac_table_insert(&shared, mac, port) == MAC_ERROR)
            return (void *)1;
        // could be evicted already, but if found the port must be sane
        int p = mac_table_lookup(&shared, mac);
        if (p < -1 || p >= NTHREADS)
            return (void *)1;
    }
    return NULL;
}

static void test_mac_threads(void)
{
    pthread_t th[NTHREADS];
    int ports[NTHREADS];

    CHECK(mac_table_init(&shared) == 0);
    mac_table_set_limits(&shared, 50, 1000);  // forces lots of evictions while threads run
    for (int i = 0; i < NTHREADS; i++) {
        ports[i] = i;
        CHECK(pthread_create(&th[i], NULL, worker, &ports[i]) == 0);
    }
    for (int i = 0; i < NTHREADS; i++) {
        void *ret;
        CHECK(pthread_join(th[i], &ret) == 0);
        CHECK(ret == NULL);
    }
    CHECK(mac_table_count(&shared) <= 50);
    CHECK(mac_table_count(&shared) == real_count(&shared));
    mac_table_destroy(&shared);
}

int main(void)
{
    test_frame();
    test_mac_basic();
    test_mac_aging();
    test_mac_lru();
    test_mac_port_security();

    // the thread test prints a lot of mac moved lines, hide them
    if (freopen("/dev/null", "w", stderr) == NULL)
        perror("freopen");
    test_mac_threads();

    if (fails == 0)
        printf("all tests passed\n");
    else
        printf("%d tests failed\n", fails);
    return fails == 0 ? 0 : 1;
}
