#define _POSIX_C_SOURCE 200809L

#include "mac_table.h"

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

// changes the entry count by d, returns 1 if we are now over the limit
static int count_add(struct mac_table *t, int d)
{
    pthread_mutex_lock(&t->count_lock);
    t->count += d;
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

void mac_table_set_max(struct mac_table *t, int max_entries)
{
    pthread_mutex_lock(&t->count_lock);
    t->max_entries = max_entries;
    pthread_mutex_unlock(&t->count_lock);
}

int mac_table_init(struct mac_table *t)
{
    t->count = 0;
    t->max_entries = MAC_TABLE_DEFAULT_MAX;
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
    t->count = 0;
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
            free(e);
            removed = 1;
            break;
        }
    }
    pthread_mutex_unlock(&b->lock);

    if (removed) {
        count_add(t, -1);
        char s[MAC_STR_LEN];
        mac_to_string(old_mac, s);
        fprintf(stderr, "MAC table full, evicted %s\n", s);
    }
    return removed;
}

int mac_table_insert(struct mac_table *t, const uint8_t mac[MAC_LEN], int port)
{
    // never learn the broadcast address
    if (mac_is_broadcast(mac) || port < 0)
        return MAC_ERROR;

    struct mac_bucket *b = &t->buckets[hash_mac(mac)];
    int result;
    int old_port = -1;

    pthread_mutex_lock(&b->lock);

    struct mac_entry *e = find_in_bucket(b, mac);
    if (e != NULL) {
        if (e->port != port) {
            old_port = e->port;
            e->port = port;
            result = MAC_MOVED;
        } else {
            result = MAC_SAME;
        }
        e->last_seen = now_ms();
    } else {
        e = malloc(sizeof(*e));
        if (e == NULL) {
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

    int over = 0;
    if (result == MAC_NEW)
        over = count_add(t, 1);

    pthread_mutex_unlock(&b->lock);

    // too many entries, throw out the least recently seen ones
    // done after unlocking because evict needs other bucket locks
    while (over) {
        evict_oldest(t);
        over = count_add(t, 0);
    }

    // print after unlocking so we dont hold the lock while doing io
    if (result == MAC_MOVED) {
        char s[MAC_STR_LEN];
        mac_to_string(mac, s);
        fprintf(stderr, "MAC moved: port %d -> port %d (%s)\n", old_port, port, s);
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

int mac_table_age(struct mac_table *t, uint64_t max_age_ms)
{
    int removed = 0;
    uint64_t now = now_ms();

    // one bucket at a time so forwarding keeps going in the others
    for (int i = 0; i < MAC_TABLE_BUCKETS; i++) {
        struct mac_bucket *b = &t->buckets[i];
        pthread_mutex_lock(&b->lock);

        // pointer to the link so we can unlink without a prev pointer
        struct mac_entry **link = &b->head;
        while (*link != NULL) {
            struct mac_entry *e = *link;
            if (now - e->last_seen > max_age_ms) {
                *link = e->next;
                free(e);
                removed++;
                count_add(t, -1);
            } else {
                link = &e->next;
            }
        }
        pthread_mutex_unlock(&b->lock);
    }
    return removed;
}
