// net_io.h - reading and writing whole frames over a socket
//
// read() and write() can do less than we ask for, so these keep
// going until the full amount is done

#ifndef NET_IO_H
#define NET_IO_H

#include <stddef.h>
#include <sys/types.h>

#include "frame.h"

// returns len on success, 0 if the other side closed before any byte,
// -1 on error or if it closed half way through
ssize_t read_full(int fd, void *buf, size_t len);

// returns 0 on success, -1 on error (EPIPE if the other side is gone)
int write_full(int fd, const void *buf, size_t len);

// reads one frame, payload must hold FRAME_MAX_PAYLOAD bytes
// returns 1 for a frame, 0 for a clean close, -1 for an error or bad frame
int frame_read(int fd, struct frame_header *h, uint8_t *payload);

// writes header and payload in one go
int frame_write(int fd, const struct frame_header *h, const uint8_t *payload);

#endif
