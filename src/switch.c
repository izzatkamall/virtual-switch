// switch.c - the virtual switch
//
//   ./switch <tcp_port> <num_ports> [config_file]
//
// every host that connects gets a port with two threads
//   rx  reads frames, learns the source mac, puts the frame on the out queues
//   tx  takes frames off its own queue and writes them to the host
// a slow host only fills up its own queue, frames get dropped when its full

#define _POSIX_C_SOURCE 200809L

#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <pthread.h>
#include <semaphore.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#include "config.h"
#include "frame.h"
#include "log.h"
#include "mac_table.h"
#include "net_io.h"
#include "qos.h"
#include "shm_stats.h"

#define DUMP_FILE "mac_dump.txt"  // SIGUSR1 writes the mac table here
#define LOG_FILE "switch.log"
#define RATE_TICK_MS 100  // how often the rate thread refills tokens
#define SEND_BUF_SIZE 8192  // kernel send buffer per host, linux doubles it

// one received frame, shared by every queue it goes into
// refs counts how many queues still hold it, the last one frees it
struct pkt {
    atomic_int refs;
    struct frame_header h;
    uint8_t payload[FRAME_MAX_PAYLOAD];
};

static struct pkt *pkt_new(const struct frame_header *h, const uint8_t *payload)
{
    struct pkt *k = malloc(sizeof(*k));
    if (k == NULL)
        return NULL;
    atomic_init(&k->refs, 1);
    k->h = *h;
    memcpy(k->payload, payload, h->length);
    return k;
}

static void pkt_get(struct pkt *k)
{
    atomic_fetch_add(&k->refs, 1);
}

// drops one reference, frees it if that was the last one
static void pkt_put(struct pkt *k)
{
    if (atomic_fetch_sub(&k->refs, 1) == 1)
        free(k);
}

// FREE: nobody here, UP: host connected, CLOSING: host left and
// threads are stopping, DONE: threads stopped, rx needs a join
enum { PORT_FREE, PORT_UP, PORT_CLOSING, PORT_DONE };

struct port {
    int id;
    pthread_mutex_t lock;  // protects everything below
    pthread_cond_t not_empty;
    int fd;
    int state;
    pthread_t rx_tid;
    pthread_t tx_tid;

    // frames waiting to go out, one queue per priority
    struct qos out;

    int vlan;   // vlan of an access port
    int trunk;  // 1 if this port carries every vlan

    // token bucket, in tenths of a frame so slow rates still work
    // with 100ms ticks. rate 0 means no limit
    int rate;
    int tokens;
};

static struct port ports[MAX_PORTS];
static int num_ports;
static struct mac_table table;
static struct shm_stats *stats;  // counters swctl can read
static sem_t free_ports;  // counts ports nobody is using
static int listen_fd = -1;

// settings from the config file, SIGHUP can change them while we run
static pthread_mutex_t cfg_lock = PTHREAD_MUTEX_INITIALIZER;
static struct config cfg;
static const char *cfg_path = NULL;

static struct config get_config(void)
{
    pthread_mutex_lock(&cfg_lock);
    struct config c = cfg;
    pthread_mutex_unlock(&cfg_lock);
    return c;
}

// changes a port's vlan, its macs are forgotten since they were learned
// in the old vlan
static void set_port_vlan(int p, int vlan, int trunk)
{
    struct port *pt = &ports[p];
    pthread_mutex_lock(&pt->lock);
    int changed = pt->vlan != vlan || pt->trunk != trunk;
    pt->vlan = vlan;
    pt->trunk = trunk;
    pthread_mutex_unlock(&pt->lock);

    if (changed) {
        mac_table_remove_port(&table, p);
        if (trunk)
            log_msg("port %d is the trunk", p);
        else
            log_msg("port %d is in vlan %d", p, vlan);
    }
}

// loads the config file and puts the new values into use
static int apply_config(void)
{
    struct config c = get_config();
    if (cfg_path != NULL && config_load(cfg_path, &c) != 0)
        return -1;

    pthread_mutex_lock(&cfg_lock);
    cfg = c;
    pthread_mutex_unlock(&cfg_lock);

    mac_table_set_limits(&table, c.mac_max, c.mac_per_port);
    mac_table_trim(&table);  // in case mac_max got smaller

    for (int p = 0; p < num_ports; p++)
        set_port_vlan(p, c.port_vlan[p], c.trunk_port == p);
    return 0;
}

// set when the switch is shutting down, the cond wakes sleeping threads
static pthread_mutex_t stop_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t stop_cond = PTHREAD_COND_INITIALIZER;
static int stopping = 0;

// absolute time ms from now, for the timedwait calls
static struct timespec deadline_in(int ms)
{
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    ts.tv_sec += ms / 1000;
    ts.tv_nsec += (long)(ms % 1000) * 1000000;
    if (ts.tv_nsec >= 1000000000) {
        ts.tv_sec++;
        ts.tv_nsec -= 1000000000;
    }
    return ts;
}

// puts a frame on a port queue, never blocks
// returns 1 if queued, 0 if the queue was full (dropped and counted),
// -1 if the port is down or not in the frame's vlan
static int port_enqueue(int p, struct pkt *k)
{
    struct port *pt = &ports[p];
    int rc;

    pthread_mutex_lock(&pt->lock);
    if (pt->state != PORT_UP || (!pt->trunk && pt->vlan != k->h.vlan)) {
        rc = -1;
    } else if (qos_push(&pt->out, k->h.priority, k, now_ms()) < 0) {
        shm_stats_add(stats, p, STAT_DROPPED, 1);  // that priority queue is full
        rc = 0;
    } else {
        pkt_get(k);  // the queue holds a reference now
        pthread_cond_signal(&pt->not_empty);
        rc = 1;
    }
    pthread_mutex_unlock(&pt->lock);
    return rc;
}

// returns -1 if the port broke port security and has to be shut
static int forward(int in_port, struct frame_header *h, const uint8_t *payload)
{
    // access ports put the frame in their own vlan, the trunk keeps
    // whatever vlan the frame already says
    struct port *in = &ports[in_port];
    pthread_mutex_lock(&in->lock);
    if (!in->trunk)
        h->vlan = (uint16_t)in->vlan;
    pthread_mutex_unlock(&in->lock);

    if (mac_table_insert(&table, h->src, in_port) == MAC_PORT_FULL) {
        log_msg("port %d has more than %d macs, shutting it down", in_port, get_config().mac_per_port);
        return -1;
    }

    int out = -1;
    if (!mac_is_broadcast(h->dst))
        out = mac_table_lookup(&table, h->dst);

    if (out == in_port)
        return 0;  // dest is on the same port, nothing to do

    // one copy no matter how many ports it goes to
    struct pkt *k = pkt_new(h, payload);
    if (k == NULL)
        return 0;

    // known dest, unless its in another vlan, then treat it as unknown
    if (out < 0 || port_enqueue(out, k) < 0) {
        // broadcast or unknown dest, send to everyone else in the vlan
        // port_enqueue skips ports in other vlans
        for (int p = 0; p < num_ports; p++) {
            if (p != in_port)
                port_enqueue(p, k);
        }
    }

    // let go of our own reference, frees it if no queue took it
    pkt_put(k);
    return 0;
}

// takes frames off the queue and writes them to the host
// if this host is slow only this thread waits, nobody else
static void *tx_thread(void *arg)
{
    struct port *pt = arg;
    int fd = pt->fd;  // set before the thread started, never changes while we run

    while (1) {
        pthread_mutex_lock(&pt->lock);
        while (pt->state == PORT_UP) {
            if (qos_count(&pt->out) == 0) {
                pthread_cond_wait(&pt->not_empty, &pt->lock);
            } else if (pt->rate > 0 && pt->tokens < 10) {
                // out of tokens, the rate thread wakes us when it refills
                struct timespec ts = deadline_in(RATE_TICK_MS);
                pthread_cond_timedwait(&pt->not_empty, &pt->lock, &ts);
            } else {
                break;
            }
        }
        if (pt->state != PORT_UP) {
            pthread_mutex_unlock(&pt->lock);
            break;
        }
        if (pt->rate > 0)
            pt->tokens -= 10;  // one frame costs ten tenths
        // weighted round robin picks which priority goes next
        struct pkt *k = qos_pop(&pt->out, now_ms());
        pthread_mutex_unlock(&pt->lock);

        int rc = frame_write(fd, &k->h, k->payload);
        pkt_put(k);
        if (rc == 0)
            shm_stats_add(stats, pt->id, STAT_OUT, 1);
        if (rc < 0) {
            // host is gone, wake up the rx thread so it cleans up
            shutdown(fd, SHUT_RDWR);
            break;
        }
    }
    return NULL;
}

static void *rx_thread(void *arg)
{
    struct port *pt = arg;
    struct frame_header h;
    uint8_t payload[FRAME_MAX_PAYLOAD];
    int fd = pt->fd;

    while (frame_read(fd, &h, payload) == 1) {
        shm_stats_add(stats, pt->id, STAT_IN, 1);
        if (forward(pt->id, &h, payload) < 0)
            break;
    }

    // forget the macs on this port so a new host starts clean
    mac_table_remove_port(&table, pt->id);

    // stop the sender, shutdown wakes it if its stuck in write()
    pthread_mutex_lock(&pt->lock);
    pt->state = PORT_CLOSING;
    pthread_cond_broadcast(&pt->not_empty);
    pthread_mutex_unlock(&pt->lock);
    shutdown(fd, SHUT_RDWR);
    pthread_join(pt->tx_tid, NULL);

    // both threads are done with the fd now
    pthread_mutex_lock(&pt->lock);
    struct pkt *left;
    while ((left = qos_pop(&pt->out, now_ms())) != NULL)
        pkt_put(left);
    close(pt->fd);
    pt->fd = -1;
    pt->state = PORT_DONE;
    pthread_mutex_unlock(&pt->lock);

    // one more port free, wakes up a host waiting in main
    sem_post(&free_ports);

    log_msg("port %d disconnected", pt->id);
    return NULL;
}

// finds a free port and starts its threads, returns the port or -1
static int attach_host(int fd)
{
    for (int p = 0; p < num_ports; p++) {
        struct port *pt = &ports[p];

        pthread_mutex_lock(&pt->lock);
        int state = pt->state;
        pthread_mutex_unlock(&pt->lock);

        if (state == PORT_UP || state == PORT_CLOSING)
            continue;

        // old receiver already finished, join it before reusing the port
        if (state == PORT_DONE)
            pthread_join(pt->rx_tid, NULL);

        pthread_mutex_lock(&pt->lock);
        pt->fd = fd;
        pt->state = PORT_UP;
        qos_init(&pt->out);
        pt->rate = get_config().rate_limit;
        pt->tokens = pt->rate * 10;  // start with a full bucket
        pthread_mutex_unlock(&pt->lock);

        // sender first because rx joins it when the host leaves
        if (pthread_create(&pt->tx_tid, NULL, tx_thread, pt) != 0) {
            pthread_mutex_lock(&pt->lock);
            pt->fd = -1;
            pt->state = PORT_FREE;
            pthread_mutex_unlock(&pt->lock);
            return -1;
        }
        if (pthread_create(&pt->rx_tid, NULL, rx_thread, pt) != 0) {
            // stop the sender we just made
            pthread_mutex_lock(&pt->lock);
            pt->state = PORT_FREE;
            pt->fd = -1;
            pthread_cond_broadcast(&pt->not_empty);
            pthread_mutex_unlock(&pt->lock);
            pthread_join(pt->tx_tid, NULL);
            return -1;
        }
        return p;
    }
    return -1;
}

static int is_stopping(void)
{
    pthread_mutex_lock(&stop_lock);
    int s = stopping;
    pthread_mutex_unlock(&stop_lock);
    return s;
}

static void dump_mac_table(void)
{
    char *text = mac_table_to_string(&table);
    if (text == NULL) {
        log_msg("could not build mac table dump");
        return;
    }
    FILE *f = fopen(DUMP_FILE, "w");
    if (f == NULL) {
        log_msg("fopen %s: %s", DUMP_FILE, strerror(errno));
        free(text);
        return;
    }
    fputs(text, f);
    if (fclose(f) != 0)
        log_msg("fclose %s: %s", DUMP_FILE, strerror(errno));
    free(text);
    log_msg("mac table dumped to %s", DUMP_FILE);
}

// the only thread that gets SIGINT, SIGHUP and SIGUSR1
// every other thread has them blocked
static void *signal_thread(void *arg)
{
    sigset_t *set = arg;

    while (1) {
        int sig;
        if (sigwait(set, &sig) != 0) {
            log_msg("sigwait failed");
            continue;
        }

        if (sig == SIGUSR1) {
            dump_mac_table();
        } else if (sig == SIGHUP) {
            if (cfg_path == NULL) {
                log_msg("got SIGHUP but there is no config file");
            } else if (apply_config() == 0) {
                struct config c = get_config();
                log_msg("reloaded %s: mac_age %d mac_max %d mac_per_port %d port_wait %d rate_limit %d trunk_port %d",
                       cfg_path, c.mac_age, c.mac_max, c.mac_per_port, c.port_wait, c.rate_limit, c.trunk_port);
            } else {
                log_msg("config has errors, keeping the old settings");
            }
        } else if (sig == SIGINT || sig == SIGTERM) {
            log_msg("shutting down");

            pthread_mutex_lock(&stop_lock);
            stopping = 1;
            pthread_cond_broadcast(&stop_cond);  // wakes the aging thread
            pthread_mutex_unlock(&stop_lock);

            // wake main if its stuck in accept or waiting for a port
            shutdown(listen_fd, SHUT_RDWR);
            sem_post(&free_ports);
            break;
        }
    }
    return NULL;
}

// kicks every host off, the rx threads then clean up like a normal disconnect
// only main calls this, after it stopped accepting, so no new port can show up
static void close_all_ports(void)
{
    for (int p = 0; p < num_ports; p++) {
        struct port *pt = &ports[p];
        pthread_mutex_lock(&pt->lock);
        if (pt->state == PORT_UP)
            shutdown(pt->fd, SHUT_RDWR);
        int started = pt->state != PORT_FREE;
        pthread_mutex_unlock(&pt->lock);

        // every port that isnt FREE has an rx thread nobody joined yet
        if (started) {
            pthread_join(pt->rx_tid, NULL);
            pt->state = PORT_FREE;
        }
    }
}

// waits ms or until we start stopping, returns 1 if we should stop
static int wait_or_stop(int ms)
{
    struct timespec ts = deadline_in(ms);

    pthread_mutex_lock(&stop_lock);
    while (!stopping) {
        if (pthread_cond_timedwait(&stop_cond, &stop_lock, &ts) == ETIMEDOUT)
            break;
    }
    int s = stopping;
    pthread_mutex_unlock(&stop_lock);
    return s;
}

// removes old mac entries once a second
// refills every port's token bucket each tick
static void *rate_thread(void *arg)
{
    (void)arg;
    while (!wait_or_stop(RATE_TICK_MS)) {
        // read the config once, not while holding a port lock
        int rate = get_config().rate_limit;

        for (int p = 0; p < num_ports; p++) {
            struct port *pt = &ports[p];
            pthread_mutex_lock(&pt->lock);
            pt->rate = rate;
            // rate frames per second is rate tenths per 100ms tick,
            // bucket holds one second worth at most
            pt->tokens += rate;
            if (pt->tokens > rate * 10)
                pt->tokens = rate * 10;
            pthread_cond_broadcast(&pt->not_empty);
            pthread_mutex_unlock(&pt->lock);
        }
    }
    return NULL;
}

static void *aging_thread(void *arg)
{
    (void)arg;
    while (!wait_or_stop(1000)) {
        int n = mac_table_age(&table, (uint64_t)get_config().mac_age * 1000);
        if (n > 0) {
            log_msg("aged out %d mac entries", n);
        }
    }
    return NULL;
}

// waits for a free port, returns 0 if we got one, -1 on timeout
static int wait_for_port(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    ts.tv_sec += get_config().port_wait;

    while (sem_timedwait(&free_ports, &ts) < 0) {
        if (errno != EINTR)
            return -1;
    }
    return 0;
}

// tells a host there is no room, as a normal frame so test hosts can read it
static void send_switch_full(int fd)
{
    struct frame_header h;
    memset(&h, 0, sizeof(h));
    memcpy(h.dst, MAC_BROADCAST, MAC_LEN);
    h.vlan = 1;
    h.priority = PRIO_HIGH;
    h.length = 11;
    frame_write(fd, &h, (const uint8_t *)"switch full");
}

static int make_listen_socket(int tcp_port)
{
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
        perror("socket");
        return -1;
    }

    int yes = 1;
    if (setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes)) < 0)
        perror("setsockopt");

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port = htons((uint16_t)tcp_port);

    if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        perror("bind");
        close(fd);
        return -1;
    }
    if (listen(fd, 16) < 0) {
        perror("listen");
        close(fd);
        return -1;
    }
    return fd;
}

int main(int argc, char *argv[])
{
    if (argc != 3 && argc != 4) {
        fprintf(stderr, "usage: %s <tcp_port> <num_ports> [config_file]\n", argv[0]);
        return 1;
    }
    int tcp_port = atoi(argv[1]);
    num_ports = atoi(argv[2]);
    if (tcp_port < 1 || tcp_port > 65535) {
        fprintf(stderr, "bad tcp port\n");
        return 1;
    }
    if (num_ports < 1 || num_ports > MAX_PORTS) {
        fprintf(stderr, "num_ports must be 1 to %d\n", MAX_PORTS);
        return 1;
    }

    // block these before any thread starts so every thread inherits it,
    // then only the signal thread sees them through sigwait
    sigset_t set;
    sigemptyset(&set);
    sigaddset(&set, SIGINT);
    sigaddset(&set, SIGTERM);
    sigaddset(&set, SIGHUP);
    sigaddset(&set, SIGUSR1);
    if (pthread_sigmask(SIG_BLOCK, &set, NULL) != 0) {
        fprintf(stderr, "pthread_sigmask failed\n");
        return 1;
    }

    // logger thread, started after the mask so it doesnt get signals either
    if (log_start(LOG_FILE) != 0)
        return 1;

    // a host leaving should give us EPIPE, not kill the switch
    if (signal(SIGPIPE, SIG_IGN) == SIG_ERR) {
        perror("signal");
        return 1;
    }

    if (mac_table_init(&table) != 0) {
        fprintf(stderr, "mac_table_init failed\n");
        return 1;
    }
    for (int p = 0; p < num_ports; p++) {
        ports[p].id = p;
        ports[p].fd = -1;
        ports[p].state = PORT_FREE;
        ports[p].vlan = 1;
        ports[p].trunk = 0;
        if (pthread_mutex_init(&ports[p].lock, NULL) != 0 ||
            pthread_cond_init(&ports[p].not_empty, NULL) != 0) {
            fprintf(stderr, "port init failed\n");
            return 1;
        }
    }

    // ports have to exist first since the config sets their vlans
    config_defaults(&cfg);
    if (argc == 4)
        cfg_path = argv[3];
    if (apply_config() != 0) {
        fprintf(stderr, "could not load config %s\n", cfg_path);
        mac_table_destroy(&table);
        return 1;
    }

    stats = shm_stats_create(num_ports);
    if (stats == NULL) {
        fprintf(stderr, "could not create shared memory for stats\n");
        return 1;
    }

    if (sem_init(&free_ports, 0, (unsigned int)num_ports) != 0) {
        perror("sem_init");
        return 1;
    }

    listen_fd = make_listen_socket(tcp_port);
    if (listen_fd < 0)
        return 1;

    pthread_t sig_tid;
    if (pthread_create(&sig_tid, NULL, signal_thread, &set) != 0) {
        fprintf(stderr, "could not start signal thread\n");
        return 1;
    }

    pthread_t rate_tid;
    if (pthread_create(&rate_tid, NULL, rate_thread, NULL) != 0) {
        fprintf(stderr, "could not start rate thread\n");
        return 1;
    }

    pthread_t aging_tid;
    if (pthread_create(&aging_tid, NULL, aging_thread, NULL) != 0) {
        fprintf(stderr, "could not start aging thread\n");
        return 1;
    }

    log_msg("switch listening on port %d with %d ports", tcp_port, num_ports);

    while (!is_stopping()) {
        int fd = accept(listen_fd, NULL, NULL);
        if (fd < 0) {
            if (is_stopping())
                break;
            if (errno == EINTR)
                continue;
            log_msg("accept: %s", strerror(errno));
            break;
        }

        // all ports busy, wait a bit for one to free up
        int got_port = wait_for_port();
        if (is_stopping()) {
            close(fd);
            break;
        }
        if (got_port < 0) {
            log_msg("switch full, sending a new host away");
            send_switch_full(fd);
            close(fd);
            continue;
        }

        // small kernel buffer so frames wait in our own priority queues,
        // with the default few MB the kernel would send everything in order
        int sndbuf = SEND_BUF_SIZE;
        if (setsockopt(fd, SOL_SOCKET, SO_SNDBUF, &sndbuf, sizeof(sndbuf)) < 0)
            log_msg("setsockopt SO_SNDBUF: %s", strerror(errno));

        int p = attach_host(fd);
        if (p < 0) {
            log_msg("could not attach host");
            sem_post(&free_ports);  // give the slot back
            close(fd);
        } else {
            log_msg("host connected on port %d", p);
        }
    }

    // if accept failed for some other reason, still shut down properly
    pthread_mutex_lock(&stop_lock);
    stopping = 1;
    pthread_cond_broadcast(&stop_cond);
    pthread_mutex_unlock(&stop_lock);

    close_all_ports();
    pthread_join(aging_tid, NULL);
    pthread_join(rate_tid, NULL);

    // signal thread might still be in sigwait if we got here without SIGINT
    pthread_kill(sig_tid, SIGINT);
    pthread_join(sig_tid, NULL);

    // nothing else is running now, free everything
    close(listen_fd);
    for (int p = 0; p < num_ports; p++) {
        pthread_mutex_destroy(&ports[p].lock);
        pthread_cond_destroy(&ports[p].not_empty);
    }
    sem_destroy(&free_ports);
    shm_stats_destroy(stats);  // also removes it from /dev/shm
    mac_table_destroy(&table);
    log_msg("switch stopped cleanly");
    log_stop();
    return 0;
}
