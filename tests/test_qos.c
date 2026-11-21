// tests for the qos scheduler, uses fake times so its exact
#define _POSIX_C_SOURCE 200809L

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "../src/qos.h"

static int fails = 0;

#define CHECK(x) do { \
    if (!(x)) { printf("FAIL line %d: %s\n", __LINE__, #x); fails++; } \
} while (0)

// items are small numbers cast to pointers, 100+ high, 200+ normal, 300+ low
#define ITEM(n) ((void *)(uintptr_t)(n))
#define NUM(p) ((int)(uintptr_t)(p))

static void test_wrr_order(void)
{
    struct qos q;
    qos_init(&q);
    // 60 each so no queue runs dry during the test
    for (int i = 0; i < 60; i++) {
        CHECK(qos_push(&q, PRIO_HIGH, ITEM(100 + i), 0) == 0);
        CHECK(qos_push(&q, PRIO_NORMAL, ITEM(200 + i), 0) == 0);
        CHECK(qos_push(&q, PRIO_LOW, ITEM(300 + i), 0) == 0);
    }

    // first round should be 5 high, 3 normal, 1 low
    int want[] = {100, 101, 102, 103, 104, 200, 201, 202, 300, 105, 106};
    for (int i = 0; i < 11; i++) {
        void *got = qos_pop(&q, 0);
        CHECK(NUM(got) == want[i]);
    }

    // over the next 9 rounds the ratio holds
    int seen[3] = {0, 0, 0};
    for (int i = 0; i < 27; i++)
        seen[NUM(qos_pop(&q, 0)) / 100 - 1]++;
    printf("27 pops: high %d normal %d low %d\n", seen[0], seen[1], seen[2]);
    CHECK(seen[0] == 15 && seen[1] == 9 && seen[2] == 3);
}

static void test_skips_empty(void)
{
    struct qos q;
    qos_init(&q);
    CHECK(qos_pop(&q, 0) == NULL);

    // only low has frames, it should get all of them
    for (int i = 0; i < 4; i++)
        qos_push(&q, PRIO_LOW, ITEM(300 + i), 0);
    for (int i = 0; i < 4; i++)
        CHECK(NUM(qos_pop(&q, 0)) == 300 + i);
    CHECK(qos_pop(&q, 0) == NULL);
    CHECK(qos_count(&q) == 0);
}

static void test_aging(void)
{
    struct qos q;
    qos_init(&q);

    // low frame queued at t=0, then lots of high at t=150
    qos_push(&q, PRIO_LOW, ITEM(300), 0);
    for (int i = 0; i < 30; i++)
        qos_push(&q, PRIO_HIGH, ITEM(100 + i), 150);

    // at t=160 nothing is too old, high goes first
    CHECK(NUM(qos_pop(&q, 160)) == 100);
    // at t=201 the low frame waited over 200ms so it jumps ahead
    CHECK(NUM(qos_pop(&q, 201)) == 300);
    CHECK(NUM(qos_pop(&q, 202)) == 101);
}

// everything has been waiting a long time, aging must not turn into fifo
static void test_old_backlog(void)
{
    struct qos q;
    qos_init(&q);
    for (int i = 0; i < 40; i++) {
        qos_push(&q, PRIO_LOW, ITEM(300 + i), 0);
        qos_push(&q, PRIO_NORMAL, ITEM(200 + i), 0);
        qos_push(&q, PRIO_HIGH, ITEM(100 + i), 0);
    }
    // at t=1000 all three are starved, each gets one aging turn
    for (int i = 0; i < 3; i++)
        qos_pop(&q, 1000);

    // after that its back to 5:3:1
    int seen[3] = {0, 0, 0};
    for (int i = 0; i < 27; i++)
        seen[NUM(qos_pop(&q, 1010)) / 100 - 1]++;
    printf("old backlog, 27 pops: high %d normal %d low %d\n", seen[0], seen[1], seen[2]);
    CHECK(seen[0] == 15 && seen[1] == 9 && seen[2] == 3);

    // 200ms later low is starved again and jumps ahead once
    qos_pop(&q, 1300);  // could be any starved queue
    int lows = 0;
    for (int i = 0; i < 3; i++)
        lows += NUM(qos_pop(&q, 1300)) >= 300;
    CHECK(lows == 1);  // the three starved queues each got one turn
}

static void test_full(void)
{
    struct qos q;
    qos_init(&q);
    for (int i = 0; i < QOS_QUEUE_LEN; i++)
        CHECK(qos_push(&q, PRIO_HIGH, ITEM(1), 0) == 0);
    CHECK(qos_push(&q, PRIO_HIGH, ITEM(1), 0) == -1);   // high full
    CHECK(qos_push(&q, PRIO_LOW, ITEM(1), 0) == 0);     // low still has room
    CHECK(qos_push(&q, 7, ITEM(2), 0) == 0);            // bad prio goes to normal
    CHECK(q.ring[PRIO_NORMAL].count == 1);
}

int main(void)
{
    test_wrr_order();
    test_skips_empty();
    test_aging();
    test_old_backlog();
    test_full();
    if (fails == 0)
        printf("qos: all tests passed\n");
    else
        printf("qos: %d tests failed\n", fails);
    return fails == 0 ? 0 : 1;
}
