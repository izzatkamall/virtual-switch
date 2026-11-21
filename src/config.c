#define _POSIX_C_SOURCE 200809L

#include "config.h"
#include "log.h"

#include <stdio.h>
#include <errno.h>
#include <stdlib.h>
#include <string.h>

void config_defaults(struct config *c)
{
    c->mac_age = 30;
    c->mac_max = 1024;
    c->mac_per_port = 16;
    c->port_wait = 5;
    c->rate_limit = 0;
}

// reads a whole number, returns -1 if it isnt one or is out of range
static int parse_int(const char *s, int min, int max, int *out)
{
    char *end;
    long v = strtol(s, &end, 10);
    if (end == s || *end != '\0' || v < min || v > max)
        return -1;
    *out = (int)v;
    return 0;
}

int config_load(const char *path, struct config *c)
{
    FILE *f = fopen(path, "r");
    if (f == NULL) {
        log_msg("%s: %s", path, strerror(errno));
        return -1;
    }

    // fill a copy so a bad file doesnt leave half the settings changed
    struct config tmp = *c;
    char line[256];
    int lineno = 0;
    int bad = 0;

    while (fgets(line, sizeof(line), f) != NULL) {
        lineno++;
        line[strcspn(line, "#\r\n")] = '\0';  // drop comments and newline

        char key[64], val[64], extra[8];
        int n = sscanf(line, "%63s %63s %7s", key, val, extra);
        if (n <= 0)
            continue;  // empty line
        if (n != 2) {
            log_msg("%s:%d: expected 'key value'", path, lineno);
            bad = 1;
            continue;
        }

        int rc;
        if (strcmp(key, "mac_age") == 0)
            rc = parse_int(val, 1, 86400, &tmp.mac_age);
        else if (strcmp(key, "mac_max") == 0)
            rc = parse_int(val, 1, 1000000, &tmp.mac_max);
        else if (strcmp(key, "mac_per_port") == 0)
            rc = parse_int(val, 1, 1000000, &tmp.mac_per_port);
        else if (strcmp(key, "port_wait") == 0)
            rc = parse_int(val, 0, 3600, &tmp.port_wait);
        else if (strcmp(key, "rate_limit") == 0)
            rc = parse_int(val, 0, 100000, &tmp.rate_limit);
        else {
            log_msg("%s:%d: unknown key %s", path, lineno, key);
            bad = 1;
            continue;
        }
        if (rc < 0) {
            log_msg("%s:%d: bad value %s for %s", path, lineno, val, key);
            bad = 1;
        }
    }

    if (ferror(f)) {
        log_msg("%s: read error", path);
        bad = 1;
    }
    fclose(f);

    if (bad)
        return -1;
    *c = tmp;
    return 0;
}
