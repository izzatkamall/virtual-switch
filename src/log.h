// log.h - logging through a queue and one logger thread
//
// log_msg only copies the text into a queue, it never touches a file,
// so the forwarding threads dont wait on disk
// before log_start and after log_stop it just prints to stderr

#ifndef LOG_H
#define LOG_H

int log_start(const char *path);

void log_msg(const char *fmt, ...) __attribute__((format(printf, 1, 2)));

// writes whatever is still queued, then stops the logger thread
void log_stop(void);

#endif
