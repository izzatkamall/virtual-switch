#define _POSIX_C_SOURCE 200809L

#include "shm_stats.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

struct shm_stats *shm_stats_create(int num_ports)
{
    if (num_ports < 1 || num_ports > MAX_PORTS)
        return NULL;

    // get rid of an old one left behind by a crash, ENOENT means there wasnt one
    if (shm_unlink(SHM_NAME) < 0 && errno != ENOENT)
        perror("shm_unlink old");

    int fd = shm_open(SHM_NAME, O_CREAT | O_EXCL | O_RDWR, 0666);
    if (fd < 0) {
        perror("shm_open");
        return NULL;
    }
    if (ftruncate(fd, sizeof(struct shm_stats)) < 0) {
        perror("ftruncate");
        close(fd);
        shm_unlink(SHM_NAME);
        return NULL;
    }

    struct shm_stats *s = mmap(NULL, sizeof(*s), PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (close(fd) < 0)  // the mapping stays valid after close
        perror("close");
    if (s == MAP_FAILED) {
        perror("mmap");
        shm_unlink(SHM_NAME);
        return NULL;
    }

    memset(s, 0, sizeof(*s));
    s->num_ports = num_ports;

    // mutex has to be process shared or swctl cant use it
    pthread_mutexattr_t attr;
    int ok = pthread_mutexattr_init(&attr) == 0;
    if (ok)
        ok = pthread_mutexattr_setpshared(&attr, PTHREAD_PROCESS_SHARED) == 0;
    if (ok)
        ok = pthread_mutex_init(&s->lock, &attr) == 0;
    pthread_mutexattr_destroy(&attr);

    if (!ok) {
        fprintf(stderr, "could not set up shared mutex\n");
        munmap(s, sizeof(*s));
        shm_unlink(SHM_NAME);
        return NULL;
    }
    return s;
}

void shm_stats_destroy(struct shm_stats *s)
{
    if (s == NULL)
        return;
    pthread_mutex_destroy(&s->lock);
    if (munmap(s, sizeof(*s)) < 0)
        perror("munmap");
    if (shm_unlink(SHM_NAME) < 0)
        perror("shm_unlink");
}

struct shm_stats *shm_stats_attach(void)
{
    // needs write access too because locking the mutex writes to it
    int fd = shm_open(SHM_NAME, O_RDWR, 0);
    if (fd < 0)
        return NULL;

    struct shm_stats *s = mmap(NULL, sizeof(*s), PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (close(fd) < 0)
        perror("close");
    if (s == MAP_FAILED)
        return NULL;
    return s;
}

void shm_stats_detach(struct shm_stats *s)
{
    if (s != NULL && munmap(s, sizeof(*s)) < 0)
        perror("munmap");
}

void shm_stats_add(struct shm_stats *s, int port, int which, unsigned long n)
{
    if (port < 0 || port >= s->num_ports || which < 0 || which >= STAT_COUNT)
        return;
    pthread_mutex_lock(&s->lock);
    s->ports[port].count[which] += n;
    pthread_mutex_unlock(&s->lock);
}

int shm_stats_snapshot(struct shm_stats *s, struct port_stats *out)
{
    pthread_mutex_lock(&s->lock);
    int n = s->num_ports;
    memcpy(out, s->ports, n * sizeof(struct port_stats));
    pthread_mutex_unlock(&s->lock);
    return n;
}
