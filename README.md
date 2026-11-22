# Virtual Switch

OS Fall 2026 project, Muhammad Izzat Kamal (BSAI24078)

Repo: https://github.com/izzatkamall/virtual-switch

A multithreaded layer 2 switch in C. Hosts connect over TCP, each one gets a
port, and the switch learns MAC addresses and forwards frames between them.

## Building

```
make            # builds switch, host and swctl
make clean
```

Flags are `-std=c11 -Wall -Wextra -Werror -g -O0`, needs pthreads and `-lrt`.

## Running

### switch

```
./switch <tcp_port> <num_ports> [config_file]
./switch 9000 4 my.conf
```

`num_ports` is 1 to 64. The switch logs to `switch.log` and to stdout.

Signals:

| signal | what it does |
|---|---|
| SIGINT / SIGTERM | clean shutdown: closes every port, joins every thread, removes the shared memory and socket |
| SIGHUP | reloads the config file (a bad file is ignored and the old config kept) |
| SIGUSR1 | writes the MAC table to `mac_dump.txt` |

```
kill -HUP $(pgrep -x switch)
kill -USR1 $(pgrep -x switch)
```

If every port is taken, a new host waits up to `port_wait` seconds for one
to free up. If none does it gets a normal frame with the payload
`switch full` (src 00:00:00:00:00:00, dst broadcast) and is disconnected.

### host

```
./host <switch_ip> <tcp_port> <mac> [vlan]
./host 127.0.0.1 9000 02:00:00:00:00:01
```

Then type lines on stdin:

```
02:00:00:00:00:02 hello        send to one host
bcast hello everyone           broadcast
p0 02:00:00:00:00:02 urgent    same with priority 0 (high), 1 (normal) or 2 (low)
```

Every frame received is printed with its source, vlan and priority.
Ctrl+D closes the host.

### swctl

```
./swctl show ports                     state, vlan, mac count and counters of every port
./swctl show mac                       the mac table
./swctl clear mac                      empties the mac table
./swctl shutdown <port>                drains the port (2s max), disconnects the host, keeps it down
./swctl up <port>                      lets the port be used again
./swctl vlan <port> <vlan>             moves a port into a vlan (access port)
./swctl mirror <port> <sniffer|off>    copies all traffic in and out of port to sniffer
./swctl stats                          per port in/out/dropped/mirrored counters
./swctl stats --watch                  same, redrawn every second
```

`stats` reads the shared memory `/dev/shm/vswitch_stats` directly. Every
other command is sent over the unix socket `/tmp/vswitch.sock`. Both names
are fixed so swctl needs no arguments to find the switch.

## Config file

One `key value` per line, `#` starts a comment. Unknown keys or bad values
make the whole file fail to load (nothing is half applied).

| key | default | meaning |
|---|---|---|
| `mac_age <secs>` | 30 | a MAC not seen as a source for this long is forgotten |
| `mac_max <n>` | 1024 | most entries in the MAC table, the least recently seen one is evicted |
| `mac_per_port <n>` | 16 | port security: more MACs than this on one port and the port is shut |
| `port_wait <secs>` | 5 | how long a new host waits for a free port |
| `rate_limit <n>` | 0 | most frames per second out of each port, 0 is no limit |
| `port_vlan <port> <vlan>` | 1 | puts a port in a vlan, can appear many times |
| `trunk_port <port>` | -1 | the trunk port, carries every vlan and frames keep their vlan field |

Example:

```
mac_age 30
mac_max 1024
mac_per_port 16
port_wait 5
rate_limit 0
port_vlan 0 10
port_vlan 1 10
port_vlan 2 20
trunk_port 3
```

## Frame format

17 byte header then up to 1500 bytes of payload. Length and vlan are in
network byte order.

| offset | size | field |
|---|---|---|
| 0 | 2 | payload length (0 to 1500) |
| 2 | 6 | source MAC |
| 8 | 6 | destination MAC (ff:ff:ff:ff:ff:ff is broadcast) |
| 14 | 2 | vlan (1 to 4094) |
| 16 | 1 | priority (0 high, 1 normal, 2 low) |

## Features

Tier 1 (all done)
1. wire format with htons/ntohs and partial read/write handling
2. per port rx and tx threads with their own queue, a slow host doesnt block others
3. refcounted shared frames, a broadcast is stored once
4. MAC table with 16 striped locks, aging, size limit with LRU eviction, port security
5. port limit with a semaphore and timeout

Tier 2 (all done)
6. three priority queues per port, weighted round robin 5:3:1 with aging so low never starves
7. token bucket rate limit per port
8. vlans and a trunk port
9. port mirroring
10. live reconfig from swctl (shutdown, up, vlan, clear mac)
11. shared memory counters and the swctl admin tool
12. signal handling (INT/TERM shutdown, USR1 dump, HUP reload)
13. logger thread, logging never blocks the switch

## Files

```
src/frame.c        header serialize/deserialize, mac helpers
src/net_io.c       read_full/write_full, frame_read/frame_write
src/mac_table.c    striped mac table
src/qos.c          priority queues and WRR
src/log.c          logger thread
src/config.c       config file parser
src/shm_stats.c    shared memory counters
src/switch.c       the switch
src/host.c         test host
src/swctl.c        admin tool
```

## Tests

The tests are not part of the submission, they are in the repo
(https://github.com/izzatkamall/virtual-switch) under `tests/`. From a
clone of the repo:

```
make test       # the 3 C unit tests
make check      # every test (unit + live, needs python3), about a minute
make tsan       # every test with everything built with -fsanitize=thread
make valgrind   # every live test with the switch running under valgrind
```
