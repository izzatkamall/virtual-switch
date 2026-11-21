#define _POSIX_C_SOURCE 200809L

#include "frame.h"

#include <arpa/inet.h>
#include <ctype.h>
#include <stdio.h>
#include <string.h>

// byte offsets of each field inside the 17 byte header
#define OFF_LENGTH   0
#define OFF_SRC      2
#define OFF_DST      8
#define OFF_VLAN     14
#define OFF_PRIORITY 16

const uint8_t MAC_BROADCAST[MAC_LEN] = {0xff, 0xff, 0xff, 0xff, 0xff, 0xff};

void frame_header_serialize(const struct frame_header *h, uint8_t buf[FRAME_HEADER_LEN])
{
    uint16_t len = htons(h->length);
    uint16_t vlan = htons(h->vlan);

    // memcpy so we dont do unaligned writes
    memcpy(buf + OFF_LENGTH, &len, 2);
    memcpy(buf + OFF_SRC, h->src, MAC_LEN);
    memcpy(buf + OFF_DST, h->dst, MAC_LEN);
    memcpy(buf + OFF_VLAN, &vlan, 2);
    buf[OFF_PRIORITY] = h->priority;
}

void frame_header_deserialize(const uint8_t buf[FRAME_HEADER_LEN], struct frame_header *h)
{
    uint16_t len, vlan;

    memcpy(&len, buf + OFF_LENGTH, 2);
    memcpy(h->src, buf + OFF_SRC, MAC_LEN);
    memcpy(h->dst, buf + OFF_DST, MAC_LEN);
    memcpy(&vlan, buf + OFF_VLAN, 2);

    h->length = ntohs(len);
    h->vlan = ntohs(vlan);
    h->priority = buf[OFF_PRIORITY];
}

int frame_header_validate(const struct frame_header *h)
{
    if (h->length > FRAME_MAX_PAYLOAD)
        return -1;
    if (h->vlan < VLAN_MIN || h->vlan > VLAN_MAX)
        return -1;
    if (h->priority >= PRIO_COUNT)
        return -1;
    return 0;
}

bool mac_is_broadcast(const uint8_t mac[MAC_LEN])
{
    return memcmp(mac, MAC_BROADCAST, MAC_LEN) == 0;
}

bool mac_equal(const uint8_t a[MAC_LEN], const uint8_t b[MAC_LEN])
{
    return memcmp(a, b, MAC_LEN) == 0;
}

void mac_to_string(const uint8_t mac[MAC_LEN], char out[MAC_STR_LEN])
{
    snprintf(out, MAC_STR_LEN, "%02X:%02X:%02X:%02X:%02X:%02X",
             mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
}

static int hex_val(char c)
{
    if (c >= '0' && c <= '9')
        return c - '0';
    c = (char)tolower((unsigned char)c);
    if (c >= 'a' && c <= 'f')
        return c - 'a' + 10;
    return -1;
}

// parses "aa:bb:cc:dd:ee:ff", returns 0 on success
int mac_from_string(const char *s, uint8_t mac[MAC_LEN])
{
    if (s == NULL || strlen(s) != MAC_STR_LEN - 1)
        return -1;

    for (int i = 0; i < MAC_LEN; i++) {
        const char *p = s + i * 3;
        int hi = hex_val(p[0]);
        int lo = hex_val(p[1]);
        if (hi < 0 || lo < 0)
            return -1;
        if (i < MAC_LEN - 1 && p[2] != ':')
            return -1;
        mac[i] = (uint8_t)(hi * 16 + lo);
    }
    return 0;
}
