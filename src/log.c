#define _POSIX_C_SOURCE 200809L

#include "log.h"

#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#define LOG_QUEUE_LEN 1024
#define LOG_LINE_LEN 256

struct log_line {
    struct timespec when;  // time it was logged, not when it got written
    char text[LOG_LINE_LEN];
};

static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;  // protects all of this
static pthread_cond_t not_empty = PTHREAD_COND_INITIALIZER;
static struct log_line queue[LOG_QUEUE_LEN];
static int head = 0;
static int count = 0;
static int running = 0;
static unsigned long dropped = 0;

static FILE *log_file = NULL;
static pthread_t logger_tid;

static void write_line(const struct log_line *l)
{
    struct tm tm;
    char stamp[32];
    localtime_r(&l->when.tv_sec, &tm);
    strftime(stamp, sizeof(stamp), "%Y-%m-%d %H:%M:%S", &tm);

    fprintf(log_file, "%s.%03ld %s\n", stamp, l->when.tv_nsec / 1000000, l->text);
    printf("%s\n", l->text);
}

static void *logger_thread(void *arg)
{
    (void)arg;
    struct log_line l;

    pthread_mutex_lock(&lock);
    while (1) {
        while (count == 0 && running)
            pthread_cond_wait(&not_empty, &lock);
        if (count == 0 && !running)
            break;  // stopped and nothing left to write

        l = queue[head];
        head = (head + 1) % LOG_QUEUE_LEN;
        count--;
        unsigned long lost = dropped;
        dropped = 0;
        int more = count;  // read it now, we let go of the lock next

        // do the slow file write without holding the lock
        pthread_mutex_unlock(&lock);
        if (lost > 0)
            fprintf(log_file, "(log queue was full, %lu messages lost)\n", lost);
        write_line(&l);
        if (more == 0) {
            fflush(log_file);
            fflush(stdout);
        }
        pthread_mutex_lock(&lock);
    }
    pthread_mutex_unlock(&lock);

    fflush(log_file);
    fflush(stdout);
    return NULL;
}

int log_start(const char *path)
{
    log_file = fopen(path, "a");
    if (log_file == NULL) {
        perror(path);
        return -1;
    }

    pthread_mutex_lock(&lock);
    running = 1;
    pthread_mutex_unlock(&lock);

    if (pthread_create(&logger_tid, NULL, logger_thread, NULL) != 0) {
        pthread_mutex_lock(&lock);
        running = 0;
        pthread_mutex_unlock(&lock);
        fclose(log_file);
        log_file = NULL;
        return -1;
    }
    return 0;
}

void log_msg(const char *fmt, ...)
{
    struct log_line l;
    if (clock_gettime(CLOCK_REALTIME, &l.when) != 0)
        memset(&l.when, 0, sizeof(l.when));  // no time, still log it

    // format before locking so the lock is held for a tiny moment
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(l.text, sizeof(l.text), fmt, ap);
    va_end(ap);

    pthread_mutex_lock(&lock);
    if (!running) {
        pthread_mutex_unlock(&lock);
        fprintf(stderr, "%s\n", l.text);
        return;
    }
    if (count == LOG_QUEUE_LEN) {
        dropped++;  // never block the caller, just lose the line
    } else {
        queue[(head + count) % LOG_QUEUE_LEN] = l;
        count++;
        pthread_cond_signal(&not_empty);
    }
    pthread_mutex_unlock(&lock);
}

void log_stop(void)
{
    pthread_mutex_lock(&lock);
    if (!running) {
        pthread_mutex_unlock(&lock);
        return;
    }
    running = 0;
    pthread_cond_broadcast(&not_empty);
    pthread_mutex_unlock(&lock);

    int err = pthread_join(logger_tid, NULL);
    if (err != 0)
        fprintf(stderr, "pthread_join logger: %s\n", strerror(err));
    if (fclose(log_file) != 0)
        perror("fclose log");
    log_file = NULL;
}
