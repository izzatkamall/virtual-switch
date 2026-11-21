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

    pthread_mutex_t count_lock;  // protects everything below
    int count;
    int max_entries;
    int max_per_port;
    int per_port[MAX_PORTS];  // how many macs each port has
};

// what mac_table_insert did
enum {
    MAC_ERROR = -1,
    MAC_NEW = 0,
    MAC_SAME = 1,
    MAC_MOVED = 2,
    MAC_PORT_FULL = 3  // port already has max_per_port macs, not learned
};

int mac_table_init(struct mac_table *t);
void mac_table_destroy(struct mac_table *t);

// max_entries for the whole table, max_per_port for port security
void mac_table_set_limits(struct mac_table *t, int max_entries, int max_per_port);

// evicts oldest entries until the table fits max_entries again
void mac_table_trim(struct mac_table *t);

int mac_table_count(struct mac_table *t);
int mac_table_port_count(struct mac_table *t, int port);

int mac_table_insert(struct mac_table *t, const uint8_t mac[MAC_LEN], int port);

// returns the port or -1 if we dont know the mac
int mac_table_lookup(struct mac_table *t, const uint8_t mac[MAC_LEN]);

// removes entries not seen for more than max_age_ms, returns how many
int mac_table_age(struct mac_table *t, uint64_t max_age_ms);

// removes every mac learned on a port, used when its host leaves
int mac_table_remove_port(struct mac_table *t, int port);

// returns the table as text (malloced, caller frees) or NULL on error
char *mac_table_to_string(struct mac_table *t);

// ms since some fixed point, only good for measuring gaps
uint64_t now_ms(void);

#endif
