#define _POSIX_C_SOURCE 200809L

#include "mac_table.h"
#include "log.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

uint64_t now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

// simple hash, just mix the mac bytes together
static int hash_mac(const uint8_t mac[MAC_LEN])
{
    unsigned int h = 0;
    for (int i = 0; i < MAC_LEN; i++)
        h = h * 31 + mac[i];
    return h % MAC_TABLE_BUCKETS;
}

// bucket lock must be held
static struct mac_entry *find_in_bucket(struct mac_bucket *b, const uint8_t mac[MAC_LEN])
{
    for (struct mac_entry *e = b->head; e != NULL; e = e->next) {
        if (mac_equal(e->mac, mac))
            return e;
    }
    return NULL;
}

// counter helpers, all of them only take count_lock

// a new mac on port, returns -1 if the port is full,
// 1 if the table is now over its limit, 0 otherwise
static int count_new(struct mac_table *t, int port)
{
    int rc;
    pthread_mutex_lock(&t->count_lock);
    if (t->per_port[port] >= t->max_per_port) {
        rc = -1;
    } else {
        t->per_port[port]++;
        t->count++;
        rc = t->count > t->max_entries;
    }
    pthread_mutex_unlock(&t->count_lock);
    return rc;
}

// a mac moving ports, returns -1 if the new port is full
static int count_move(struct mac_table *t, int from, int to)
{
    int rc = 0;
    pthread_mutex_lock(&t->count_lock);
    if (t->per_port[to] >= t->max_per_port) {
        rc = -1;
    } else {
        t->per_port[from]--;
        t->per_port[to]++;
    }
    pthread_mutex_unlock(&t->count_lock);
    return rc;
}

static void count_removed(struct mac_table *t, int port)
{
    pthread_mutex_lock(&t->count_lock);
    t->per_port[port]--;
    t->count--;
    pthread_mutex_unlock(&t->count_lock);
}

static int count_over(struct mac_table *t)
{
    pthread_mutex_lock(&t->count_lock);
    int over = t->count > t->max_entries;
    pthread_mutex_unlock(&t->count_lock);
    return over;
}

int mac_table_count(struct mac_table *t)
{
    pthread_mutex_lock(&t->count_lock);
    int n = t->count;
    pthread_mutex_unlock(&t->count_lock);
    return n;
}

int mac_table_port_count(struct mac_table *t, int port)
{
    if (port < 0 || port >= MAX_PORTS)
        return 0;
    pthread_mutex_lock(&t->count_lock);
    int n = t->per_port[port];
    pthread_mutex_unlock(&t->count_lock);
    return n;
}

void mac_table_set_limits(struct mac_table *t, int max_entries, int max_per_port)
{
    pthread_mutex_lock(&t->count_lock);
    t->max_entries = max_entries;
    t->max_per_port = max_per_port;
    pthread_mutex_unlock(&t->count_lock);
}

int mac_table_init(struct mac_table *t)
{
    memset(t->per_port, 0, sizeof(t->per_port));
    t->count = 0;
    t->max_entries = MAC_TABLE_DEFAULT_MAX;
    t->max_per_port = MAC_TABLE_DEFAULT_MAX;
    if (pthread_mutex_init(&t->count_lock, NULL) != 0)
        return -1;

    for (int i = 0; i < MAC_TABLE_BUCKETS; i++) {
        t->buckets[i].head = NULL;
        if (pthread_mutex_init(&t->buckets[i].lock, NULL) != 0) {
            // undo the ones we already made
            for (int j = 0; j < i; j++)
                pthread_mutex_destroy(&t->buckets[j].lock);
            pthread_mutex_destroy(&t->count_lock);
            return -1;
        }
    }
    return 0;
}

// only call this once no other thread is using the table
void mac_table_destroy(struct mac_table *t)
{
    for (int i = 0; i < MAC_TABLE_BUCKETS; i++) {
        struct mac_entry *e = t->buckets[i].head;
        while (e != NULL) {
            struct mac_entry *next = e->next;
            free(e);
            e = next;
        }
        t->buckets[i].head = NULL;
        pthread_mutex_destroy(&t->buckets[i].lock);
    }
    pthread_mutex_destroy(&t->count_lock);
}

// finds the entry seen longest ago and removes it, returns 1 if it did
static int evict_oldest(struct mac_table *t)
{
    uint8_t old_mac[MAC_LEN];
    uint64_t old_time = UINT64_MAX;
    int found = 0;

    // first pass finds it, one bucket lock at a time
    for (int i = 0; i < MAC_TABLE_BUCKETS; i++) {
        pthread_mutex_lock(&t->buckets[i].lock);
        for (struct mac_entry *e = t->buckets[i].head; e != NULL; e = e->next) {
            if (e->last_seen < old_time) {
                old_time = e->last_seen;
                memcpy(old_mac, e->mac, MAC_LEN);
                found = 1;
            }
        }
        pthread_mutex_unlock(&t->buckets[i].lock);
    }
    if (!found)
        return 0;

    // then remove it, but only if nobody refreshed it in between
    struct mac_bucket *b = &t->buckets[hash_mac(old_mac)];
    int removed = 0;
    pthread_mutex_lock(&b->lock);
    for (struct mac_entry **link = &b->head; *link != NULL; link = &(*link)->next) {
        struct mac_entry *e = *link;
        if (mac_equal(e->mac, old_mac) && e->last_seen == old_time) {
            *link = e->next;
            count_removed(t, e->port);
            free(e);
            removed = 1;
            break;
        }
    }
    pthread_mutex_unlock(&b->lock);

    if (removed) {
        char s[MAC_STR_LEN];
        mac_to_string(old_mac, s);
        log_msg("MAC table full, evicted %s", s);
    }
    return removed;
}

void mac_table_trim(struct mac_table *t)
{
    while (count_over(t) && evict_oldest(t))
        ;
}

int mac_table_insert(struct mac_table *t, const uint8_t mac[MAC_LEN], int port)
{
    // never learn the broadcast address
    if (mac_is_broadcast(mac) || port < 0 || port >= MAX_PORTS)
        return MAC_ERROR;

    struct mac_bucket *b = &t->buckets[hash_mac(mac)];
    int result;
    int old_port = -1;
    int over = 0;

    pthread_mutex_lock(&b->lock);

    struct mac_entry *e = find_in_bucket(b, mac);
    if (e != NULL && e->port == port) {
        e->last_seen = now_ms();
        result = MAC_SAME;
    } else if (e != NULL) {
        if (count_move(t, e->port, port) < 0) {
            result = MAC_PORT_FULL;
        } else {
            old_port = e->port;
            e->port = port;
            e->last_seen = now_ms();
            result = MAC_MOVED;
        }
    } else {
        over = count_new(t, port);
        if (over < 0) {
            over = 0;
            result = MAC_PORT_FULL;
        } else {
            e = malloc(sizeof(*e));
            if (e == NULL) {
                count_removed(t, port);  // undo
                over = 0;
                result = MAC_ERROR;
            } else {
                memcpy(e->mac, mac, MAC_LEN);
                e->port = port;
                e->last_seen = now_ms();
                e->next = b->head;
                b->head = e;
                result = MAC_NEW;
            }
        }
    }

    pthread_mutex_unlock(&b->lock);

    // too many entries, throw out the least recently seen ones
    // done after unlocking because evict needs other bucket locks
    while (over) {
        evict_oldest(t);
        over = count_over(t);
    }

    // print after unlocking so we dont hold the lock while doing io
    if (result == MAC_MOVED) {
        char s[MAC_STR_LEN];
        mac_to_string(mac, s);
        log_msg("MAC moved: port %d -> port %d (%s)", old_port, port, s);
    }
    return result;
}

int mac_table_lookup(struct mac_table *t, const uint8_t mac[MAC_LEN])
{
    struct mac_bucket *b = &t->buckets[hash_mac(mac)];
    int port = -1;

    pthread_mutex_lock(&b->lock);
    struct mac_entry *e = find_in_bucket(b, mac);
    if (e != NULL)
        port = e->port;  // copy it out, the entry could be freed after we unlock
    pthread_mutex_unlock(&b->lock);

    return port;
}

// removes entries where old(e) is true, used by aging and port flush
static int remove_if(struct mac_table *t, int (*old)(struct mac_entry *, uint64_t), uint64_t arg)
{
    int removed = 0;

    // one bucket at a time so forwarding keeps going in the others
    for (int i = 0; i < MAC_TABLE_BUCKETS; i++) {
        struct mac_bucket *b = &t->buckets[i];
        pthread_mutex_lock(&b->lock);

        // pointer to the link so we can unlink without a prev pointer
        struct mac_entry **link = &b->head;
        while (*link != NULL) {
            struct mac_entry *e = *link;
            if (old(e, arg)) {
                *link = e->next;
                count_removed(t, e->port);
                free(e);
                removed++;
            } else {
                link = &e->next;
            }
        }
        pthread_mutex_unlock(&b->lock);
    }
    return removed;
}

static int too_old(struct mac_entry *e, uint64_t max_age_ms)
{
    return now_ms() - e->last_seen > max_age_ms;
}

static int on_port(struct mac_entry *e, uint64_t port)
{
    return (uint64_t)e->port == port;
}

int mac_table_age(struct mac_table *t, uint64_t max_age_ms)
{
    return remove_if(t, too_old, max_age_ms);
}

int mac_table_remove_port(struct mac_table *t, int port)
{
    return remove_if(t, on_port, (uint64_t)port);
}

char *mac_table_to_string(struct mac_table *t)
{
    char *buf = NULL;
    size_t size = 0;
    int n = 0;

    // memstream is just memory so its fine to write to it while locked
    FILE *out = open_memstream(&buf, &size);
    if (out == NULL)
        return NULL;

    fprintf(out, "%-18s %-5s %s\n", "MAC", "PORT", "AGE(s)");
    uint64_t now = now_ms();

    // one bucket at a time so we never block the whole table
    for (int i = 0; i < MAC_TABLE_BUCKETS; i++) {
        pthread_mutex_lock(&t->buckets[i].lock);
        for (struct mac_entry *e = t->buckets[i].head; e != NULL; e = e->next) {
            char s[MAC_STR_LEN];
            mac_to_string(e->mac, s);
            fprintf(out, "%-18s %-5d %llu\n", s, e->port,
                    (unsigned long long)((now - e->last_seen) / 1000));
            n++;
        }
        pthread_mutex_unlock(&t->buckets[i].lock);
    }
    fprintf(out, "%d entries\n", n);

    if (fclose(out) != 0) {
        free(buf);
        return NULL;
    }
    return buf;
}
