#define _POSIX_C_SOURCE 200809L

#include "net_io.h"

#include <errno.h>
#include <string.h>
#include <unistd.h>

ssize_t read_full(int fd, void *buf, size_t len)
{
    size_t got = 0;
    while (got < len) {
        ssize_t n = read(fd, (char *)buf + got, len - got);
        if (n < 0) {
            if (errno == EINTR)
                continue;
            return -1;
        }
        if (n == 0) {
            // closed, fine if we had nothing yet, broken if mid frame
            return got == 0 ? 0 : -1;
        }
        got += n;
    }
    return (ssize_t)got;
}

int write_full(int fd, const void *buf, size_t len)
{
    size_t sent = 0;
    while (sent < len) {
        ssize_t n = write(fd, (const char *)buf + sent, len - sent);
        if (n < 0) {
            if (errno == EINTR)
                continue;
            return -1;
        }
        sent += n;
    }
    return 0;
}

int frame_read(int fd, struct frame_header *h, uint8_t *payload)
{
    uint8_t buf[FRAME_HEADER_LEN];

    ssize_t n = read_full(fd, buf, FRAME_HEADER_LEN);
    if (n <= 0)
        return (int)n;

    frame_header_deserialize(buf, h);
    if (frame_header_validate(h) != 0)
        return -1;

    if (h->length > 0 && read_full(fd, payload, h->length) <= 0)
        return -1;
    return 1;
}

int frame_write(int fd, const struct frame_header *h, const uint8_t *payload)
{
    uint8_t buf[FRAME_HEADER_LEN + FRAME_MAX_PAYLOAD];

    if (h->length > FRAME_MAX_PAYLOAD)
        return -1;

    // one buffer so its a single write in the normal case
    frame_header_serialize(h, buf);
    memcpy(buf + FRAME_HEADER_LEN, payload, h->length);
    return write_full(fd, buf, FRAME_HEADER_LEN + h->length);
}
