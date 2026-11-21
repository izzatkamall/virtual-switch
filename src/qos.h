// qos.h - three priority queues with weighted round robin
//
// high, normal and low each get their own ring. the sender takes up to
// 5 high, then 3 normal, then 1 low, and around again (5:3:1)
// aging: if a queue has frames but wasnt served for QOS_AGE_MS it gets
// one turn right away, so low frames never starve even when high is busy
// (aging per frame was tried first but under a long backlog every frame
// is old and it turned into plain fifo)
//
// not thread safe by itself, the port lock protects it

#ifndef QOS_H
#define QOS_H

#include <stdint.h>

#include "frame.h"

#define QOS_QUEUE_LEN 64   // per priority
#define QOS_AGE_MS 200

struct qos_slot {
    void *item;
    uint64_t added;  // ms when it was queued, for aging
};

struct qos_ring {
    struct qos_slot slots[QOS_QUEUE_LEN];
    int head;
    int count;
};

struct qos {
    struct qos_ring ring[PRIO_COUNT];
    uint64_t last_served[PRIO_COUNT];
    int cur;     // queue we are serving right now
    int credit;  // how many more we can take from it this round
};

void qos_init(struct qos *q);

// returns -1 if that priority queue is full
int qos_push(struct qos *q, int prio, void *item, uint64_t now);

// returns the next item to send or NULL if all queues are empty
void *qos_pop(struct qos *q, uint64_t now);

int qos_count(struct qos *q);

#endif
