// mac_table.h - mac address to port table
//
// hash table with one mutex per bucket (striped locking)
// a thread only ever holds one bucket lock at a time so it cant deadlock
// count_lock is always taken last and nothing is locked while holding it

#ifndef MAC_TABLE_H
#define MAC_TABLE_H

#include <pthread.h>
#include <stddef.h>
#include <stdint.h>

#include "frame.h"

#define MAC_TABLE_BUCKETS 16
#define MAC_TABLE_DEFAULT_MAX 1024

struct mac_entry {
    uint8_t mac[MAC_LEN];
    int port;
    uint64_t last_seen;  // ms on the monotonic clock
    struct mac_entry *next;
};

struct mac_bucket {
    pthread_mutex_t lock;  // protects the list in this bucket
    struct mac_entry *head;
};

struct mac_table {
    struct mac_bucket buckets[MAC_TABLE_BUCKETS];

    pthread_mutex_t count_lock;  // protects count and max_entries
    int count;
    int max_entries;
};

// what mac_table_insert did
enum {
    MAC_ERROR = -1,
    MAC_NEW = 0,
    MAC_SAME = 1,
    MAC_MOVED = 2
};

int mac_table_init(struct mac_table *t);
void mac_table_set_max(struct mac_table *t, int max_entries);
int mac_table_count(struct mac_table *t);
void mac_table_destroy(struct mac_table *t);

int mac_table_insert(struct mac_table *t, const uint8_t mac[MAC_LEN], int port);

// returns the port or -1 if we dont know the mac
int mac_table_lookup(struct mac_table *t, const uint8_t mac[MAC_LEN]);

// removes entries not seen for more than max_age_ms, returns how many
int mac_table_age(struct mac_table *t, uint64_t max_age_ms);

// ms since some fixed point, only good for measuring gaps
uint64_t now_ms(void);

#endif
