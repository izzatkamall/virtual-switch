#!/bin/bash
# runs every test, each against a fresh switch with the right settings
# usage: tests/run_all.sh        (from the project folder, after make)
cd "$(dirname "$0")/.."
PORT=9500
# RUN puts a tool in front of the switch, eg RUN="valgrind -q" tests/run_all.sh
RUN=${RUN:-}
OUT=/tmp/vswitch_switch.out

# waits until the switch says its listening (slow under valgrind)
wait_ready() {
    for i in $(seq 300); do
        grep -q "switch listening" $OUT 2>/dev/null && return 0
        sleep 0.1
    done
    return 1
}
fails=0

pass() { echo "ok   $1"; }
fail() { echo "FAIL $1"; fails=$((fails + 1)); }

# runs a c test program
unit() {
    if ./$1 > /dev/null 2>&1; then pass "$1"; else fail "$1"; fi
}

# $1 test file, $2 num ports, $3 config file (optional), $4 extra args
live() {
    rm -f $OUT
    $RUN ./switch $PORT $2 $3 > $OUT 2>&1 &
    local sw=$!
    wait_ready
    if timeout 120 python3 -I tests/$1 $PORT $4 > /tmp/vswitch_test.out 2>&1; then
        pass "$1"
    else
        fail "$1"
        sed 's/^/     /' /tmp/vswitch_test.out | grep -iE "fail|error" | head -5
    fi
    kill -INT $sw 2> /dev/null
    wait $sw
    [ $? -eq 0 ] || fail "$1: switch did not exit cleanly"
    PORT=$((PORT + 1))
}

unit test_frame_mac
unit test_net_io
unit test_qos

live test_forward.py 4
live test_slow_host.py 4
live test_broadcast.py 8
live test_mac_flood.py 4
live test_port_limit.py 2
live test_priority.py 4
live test_rate.py 4 tests/rate.conf 20
live test_vlan.py 4 tests/vlan.conf
live test_admin.py 3 tests/admin.conf
live test_mirror.py 4
live test_stress.py 8 tests/stress.conf 10

if tests/test_shutdown.sh $PORT > /dev/null 2>&1; then pass "test_shutdown.sh"; else fail "test_shutdown.sh"; fi

rm -f /tmp/vswitch_test.out mac_dump.txt
echo
[ $fails -eq 0 ] && echo "everything passed" || echo "$fails failed"
exit $fails
