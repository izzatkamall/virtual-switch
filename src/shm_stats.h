// shm_stats.h - per port counters in posix shared memory
//
// the switch creates it, swctl attaches to it and reads it
// the mutex inside is process shared so both can use it safely

#ifndef SHM_STATS_H
#define SHM_STATS_H

#include <pthread.h>

#include "frame.h"

#define SHM_NAME "/vswitch_stats"
#define ADMIN_SOCK "/tmp/vswitch.sock"  // swctl talks to the switch here

enum {
    STAT_IN = 0,
    STAT_OUT,
    STAT_DROPPED,
    STAT_MIRRORED,
    STAT_COUNT
};

struct port_stats {
    unsigned long count[STAT_COUNT];
};

struct shm_stats {
    pthread_mutex_t lock;  // protects everything below
    int num_ports;
    struct port_stats ports[MAX_PORTS];
};

// switch side, makes a fresh segment, NULL on error
struct shm_stats *shm_stats_create(int num_ports);

// switch side, destroys the mutex, unmaps and unlinks
void shm_stats_destroy(struct shm_stats *s);

// swctl side, maps an existing segment, NULL on error
struct shm_stats *shm_stats_attach(void);
void shm_stats_detach(struct shm_stats *s);

// add n to one counter of one port
void shm_stats_add(struct shm_stats *s, int port, int which, unsigned long n);

// copy all counters out while holding the lock, returns num_ports
int shm_stats_snapshot(struct shm_stats *s, struct port_stats *out);

#endif
