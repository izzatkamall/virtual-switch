# live reconfig test, run: ./switch PORT 3 tests/admin.conf   (rate_limit 10)
import socket, struct, subprocess, sys, time

PORT = int(sys.argv[1]) if len(sys.argv) > 1 else 9870
BCAST = b"\xff" * 6
fails = 0

def check(name, ok):
    global fails
    print(("ok   " if ok else "FAIL ") + name)
    fails += not ok

def swctl(*args):
    return subprocess.run(["./swctl"] + list(args), capture_output=True, text=True).stdout

def mac(n):
    return bytes([2, 0, 0, 0, 0, n])

def frame(src, dst, data):
    return struct.pack("!H6s6sHB", len(data), src, dst, 1, 1) + data

def connect():
    s = socket.create_connection(("127.0.0.1", PORT))
    time.sleep(0.15)
    return s

def read_all(s, timeout):
    # everything until close or timeout, returns (bytes, closed)
    s.settimeout(timeout)
    data = b""
    try:
        while True:
            x = s.recv(65536)
            if not x:
                return data, True
            data += x
    except socket.timeout:
        return data, False
    except ConnectionResetError:
        return data, True

def port_state(p):
    for line in swctl("show ports").splitlines()[1:]:
        f = line.split()
        if f and f[0] == str(p):
            return f[1]

# 1. shutdown drains the queue first. rate limit 10/s so frames pile up
a, b = connect(), connect()
b.sendall(frame(mac(2), mac(9), b"x"))
time.sleep(0.2)
read_all(a, 0.2)
time.sleep(1.1)  # b's bucket is full again
for i in range(18):
    a.sendall(frame(mac(1), mac(2), b"drain %02d" % i))
time.sleep(0.05)
start = time.time()
reply = swctl("shutdown", "1")
took = time.time() - start
data, closed = read_all(b, 3)
got = sum(1 for i in range(18) if b"drain %02d" % i in data)
check("shutdown waited for the queue (%.1fs)" % took, 0.5 < took < 2.0)
check("all 18 queued frames were sent before closing (%d)" % got, got == 18)
check("host got disconnected", closed)
check("show ports says down", port_state(1) == "down")
b.close()

# 2. down port takes no host, with port 0 used and 1 down a new host gets port 2
c = connect()
check("new host skips the down port", port_state(2) == "up" and port_state(1) == "down")
# and the next one has nowhere to go
d = socket.create_connection(("127.0.0.1", PORT))
data, closed = read_all(d, 7)
check("switch full while a port is down", b"switch full" in data and closed)
d.close()

# 3. up lets a waiting host in
e = socket.create_connection(("127.0.0.1", PORT))
time.sleep(0.5)
check("up reply", "is up" in swctl("up", "1"))
time.sleep(0.3)
check("waiting host got the port back", port_state(1) == "up")
data, closed = read_all(e, 0.3)
check("and it is still connected", not closed)

# 4. frozen host does not hang shutdown
f = socket.socket()
f.setsockopt(socket.SOL_SOCKET, socket.SO_RCVBUF, 4096)
for s in (a, c, e):
    s.close()
time.sleep(0.4)
f.connect(("127.0.0.1", PORT))
g = connect()
time.sleep(0.2)
f.sendall(frame(mac(6), mac(9), b"x"))
time.sleep(0.1)
for i in range(300):
    g.sendall(frame(mac(7), mac(6), b"z" * 1000))
start = time.time()
reply = swctl("shutdown", "0")
took = time.time() - start
check("frozen host: shutdown gave up after about 2s (%.1fs)" % took, 1.8 < took < 3.5)
check("and said so", "did not drain" in reply)
check("up again", "is up" in swctl("up", "0"))
f.close()
g.close()
time.sleep(0.3)

# 5. vlan command
h1, h2 = connect(), connect()
check("vlan reply", "vlan 30" in swctl("vlan", "1", "30"))
h1.sendall(frame(mac(11), BCAST, b"vlan one"))
data, _ = read_all(h2, 0.4)
check("port in vlan 30 doesnt get vlan 1 bcast", b"vlan one" not in data)
swctl("vlan", "1", "1")
h1.sendall(frame(mac(11), BCAST, b"back again"))
data, _ = read_all(h2, 0.4)
check("back in vlan 1 it does", b"back again" in data)

# 6. bad input
check("bad port", "bad port" in swctl("shutdown", "99"))
check("already up", "already up" in swctl("up", "2"))
check("bad vlan", "usage" in swctl("vlan", "1", "5000"))
h1.close()
h2.close()

print("all passed" if fails == 0 else "%d failed" % fails)
sys.exit(1 if fails else 0)
