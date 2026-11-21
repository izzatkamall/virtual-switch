// frame.h - the frame format used on the wire
//
// every frame is a 17 byte header and then the payload
//
//   offset  size  field
//        0     2  length    payload length, 0 to 1500
//        2     6  src       source mac
//        8     6  dst       dest mac, ff:ff:ff:ff:ff:ff is broadcast
//       14     2  vlan      1 to 4094
//       16     1  priority  0 high, 1 normal, 2 low
//
// length and vlan are sent in network byte order

#ifndef FRAME_H
#define FRAME_H

#include <stdint.h>
#include <stdbool.h>

#define MAC_LEN           6
#define FRAME_HEADER_LEN  17
#define FRAME_MAX_PAYLOAD 1500

#define VLAN_MIN 1
#define VLAN_MAX 4094

#define MAX_PORTS 64  // most ports a switch can have

#define MAC_STR_LEN 18  // "AA:BB:CC:DD:EE:FF" plus the null

enum {
    PRIO_HIGH = 0,
    PRIO_NORMAL = 1,
    PRIO_LOW = 2,
    PRIO_COUNT = 3
};

// header in host byte order, this is what the switch works with
struct frame_header {
    uint16_t length;
    uint8_t src[MAC_LEN];
    uint8_t dst[MAC_LEN];
    uint16_t vlan;
    uint8_t priority;
};

extern const uint8_t MAC_BROADCAST[MAC_LEN];

void frame_header_serialize(const struct frame_header *h, uint8_t buf[FRAME_HEADER_LEN]);
void frame_header_deserialize(const uint8_t buf[FRAME_HEADER_LEN], struct frame_header *h);

// returns 0 if the header is ok, -1 if not
int frame_header_validate(const struct frame_header *h);

bool mac_is_broadcast(const uint8_t mac[MAC_LEN]);
bool mac_equal(const uint8_t a[MAC_LEN], const uint8_t b[MAC_LEN]);
void mac_to_string(const uint8_t mac[MAC_LEN], char out[MAC_STR_LEN]);
int mac_from_string(const char *s, uint8_t mac[MAC_LEN]);

#endif
