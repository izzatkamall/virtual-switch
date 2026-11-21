// config.h - settings read from the config file
//
// the file is lines of "key value", # starts a comment
//
//   mac_age 30        seconds before an unused mac is forgotten
//   mac_max 1024      most entries in the mac table
//   mac_per_port 16   port security limit
//   port_wait 5       seconds a new host waits for a free port

#ifndef CONFIG_H
#define CONFIG_H

struct config {
    int mac_age;
    int mac_max;
    int mac_per_port;
    int port_wait;
};

void config_defaults(struct config *c);

// reads the file on top of whatever is in c already
// if anything is wrong c is left alone and -1 is returned
int config_load(const char *path, struct config *c);

#endif
