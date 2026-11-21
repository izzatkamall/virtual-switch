#define _POSIX_C_SOURCE 200809L

#include "qos.h"

#include <string.h>

static const int weights[PRIO_COUNT] = {5, 3, 1};  // high, normal, low

void qos_init(struct qos *q)
{
    memset(q, 0, sizeof(*q));
    q->cur = PRIO_HIGH;
    q->credit = weights[PRIO_HIGH];
}

int qos_push(struct qos *q, int prio, void *item, uint64_t now)
{
    if (prio < 0 || prio >= PRIO_COUNT)
        prio = PRIO_NORMAL;

    struct qos_ring *r = &q->ring[prio];
    if (r->count == QOS_QUEUE_LEN)
        return -1;

    struct qos_slot *s = &r->slots[(r->head + r->count) % QOS_QUEUE_LEN];
    s->item = item;
    s->added = now;
    r->count++;
    return 0;
}

static void *take(struct qos *q, int p, uint64_t now)
{
    struct qos_ring *r = &q->ring[p];
    q->last_served[p] = now;
    void *item = r->slots[r->head].item;
    r->head = (r->head + 1) % QOS_QUEUE_LEN;
    r->count--;
    return item;
}

void *qos_pop(struct qos *q, uint64_t now)
{
    // aging first, a queue that has been waiting too long gets a turn
    // it waits from when it was last served or when its frame came in
    int starved = -1;
    uint64_t longest = QOS_AGE_MS;
    for (int p = 0; p < PRIO_COUNT; p++) {
        struct qos_ring *r = &q->ring[p];
        if (r->count == 0)
            continue;
        uint64_t since = r->slots[r->head].added;
        if (q->last_served[p] > since)
            since = q->last_served[p];
        if (now - since > longest) {
            longest = now - since;
            starved = p;
        }
    }
    if (starved >= 0)
        return take(q, starved, now);

    // normal weighted round robin, skip queues that are empty or out of credit
    for (int tries = 0; tries <= PRIO_COUNT; tries++) {
        if (q->ring[q->cur].count > 0 && q->credit > 0) {
            q->credit--;
            return take(q, q->cur, now);
        }
        q->cur = (q->cur + 1) % PRIO_COUNT;
        q->credit = weights[q->cur];
    }
    return NULL;
}

int qos_count(struct qos *q)
{
    return q->ring[0].count + q->ring[1].count + q->ring[2].count;
}
