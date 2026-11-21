#!/bin/bash
# sends SIGINT to the switch in different situations and checks it
# stops by itself, quickly, with exit code 0
cd "$(dirname "$0")/.."
PORT=${1:-9900}
fails=0

# $1 name, $2 num ports, $3 python code to run against the switch (backgrounded)
try() {
    ./switch $PORT $2 > sw.log 2>&1 &
    local sw=$!
    sleep 0.3
    python3 -I -c "$3" $PORT > /dev/null 2>&1 &
    local py=$!
    sleep 1
    local start=$(date +%s.%N)
    kill -INT $sw
    # give it 3 seconds to stop
    for i in $(seq 30); do
        kill -0 $sw 2>/dev/null || break
        sleep 0.1
    done
    if kill -0 $sw 2>/dev/null; then
        echo "FAIL $1: still running after SIGINT"
        kill -9 $sw
        fails=$((fails + 1))
    else
        wait $sw
        local rc=$?
        local took=$(echo "$(date +%s.%N) - $start" | bc)
        if [ $rc -eq 0 ] && grep -q "stopped cleanly" sw.log; then
            echo "ok   $1 (stopped in ${took:0:4}s)"
        else
            echo "FAIL $1: exit code $rc"
            fails=$((fails + 1))
        fi
    fi
    kill $py 2>/dev/null
    wait $py 2>/dev/null
    PORT=$((PORT + 1))
}

HEAD='
import socket, struct, sys, time
P = int(sys.argv[1])
def f(s, d, n=10): return struct.pack("!H6s6sHB", n, bytes([2,0,0,0,0,s]), d, 1, 1) + b"z" * n
B = b"\xff" * 6
'

try "idle switch" 4 "$HEAD
time.sleep(10)"

try "hosts connected" 4 "$HEAD
s = [socket.create_connection(('127.0.0.1', P)) for _ in range(3)]
time.sleep(10)"

try "frozen host with full queue" 4 "$HEAD
a = socket.socket(); b = socket.socket()
b.setsockopt(socket.SOL_SOCKET, socket.SO_RCVBUF, 4096)
a.connect(('127.0.0.1', P)); b.connect(('127.0.0.1', P))
time.sleep(0.1)
b.sendall(f(2, bytes([2,0,0,0,0,9])))
for i in range(5000): a.sendall(f(1, bytes([2,0,0,0,0,2]), 1400))
time.sleep(10)"

try "host waiting for a port" 1 "$HEAD
a = socket.create_connection(('127.0.0.1', P))
b = socket.create_connection(('127.0.0.1', P))
time.sleep(10)"

try "during a broadcast storm" 8 "$HEAD
s = [socket.create_connection(('127.0.0.1', P)) for _ in range(6)]
while True:
    for x in s: x.sendall(f(s.index(x) + 1, B, 1000))"

rm -f sw.log
[ $fails -eq 0 ] && echo "all passed" || echo "$fails failed"
exit $fails
