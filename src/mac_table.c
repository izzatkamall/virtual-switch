#define _POSIX_C_SOURCE 200809L

#include "mac_table.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

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

int mac_table_init(struct mac_table *t)
{
    for (int i = 0; i < MAC_TABLE_BUCKETS; i++) {
        t->buckets[i].head = NULL;
        if (pthread_mutex_init(&t->buckets[i].lock, NULL) != 0) {
            // undo the ones we already made
            for (int j = 0; j < i; j++)
                pthread_mutex_destroy(&t->buckets[j].lock);
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
        e->last_seen = time(NULL);
    } else {
        e = malloc(sizeof(*e));
        if (e == NULL) {
            result = MAC_ERROR;
        } else {
            memcpy(e->mac, mac, MAC_LEN);
            e->port = port;
            e->last_seen = time(NULL);
            e->next = b->head;
            b->head = e;
            result = MAC_NEW;
        }
    }

    pthread_mutex_unlock(&b->lock);

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
