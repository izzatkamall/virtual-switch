# head of line test, run the switch first: ./switch 9300 4
# host b never reads, a floods it, c must still get its frames fast
import socket, struct, sys, time

PORT = int(sys.argv[1]) if len(sys.argv) > 1 else 9300

def mac(n):
    return bytes([2, 0, 0, 0, 0, n])

def frame(src, dst, data, vlan=1, prio=1):
    return struct.pack("!H6s6sHB", len(data), src, dst, vlan, prio) + data

def connect(small=False):
    s = socket.socket()
    if small:
        s.setsockopt(socket.SOL_SOCKET, socket.SO_RCVBUF, 4096)  # so b fills up fast
    s.connect(("127.0.0.1", PORT))
    return s

def read_frame(s):
    hdr = b""
    while len(hdr) < 17:
        x = s.recv(17 - len(hdr))
        if not x:
            return None
        hdr += x
    ln = struct.unpack("!H", hdr[:2])[0]
    data = b""
    while len(data) < ln:
        data += s.recv(ln - len(data))
    return data

a, b, c = connect(), connect(small=True), connect()
time.sleep(0.2)

# let the switch learn b and c, then empty their sockets
b.sendall(frame(mac(2), mac(9), b"x"))
c.sendall(frame(mac(3), mac(9), b"x"))
time.sleep(0.2)
a.settimeout(0.3)
c.settimeout(0.3)
for s in (a, c):
    try:
        while s.recv(4096):
            pass
    except socket.timeout:
        pass

# 3000 big frames to b who never reads
big = b"z" * 1400
start = time.time()
for i in range(3000):
    a.sendall(frame(mac(1), mac(2), big))
print("sent 3000 frames to the frozen host in %.2fs" % (time.time() - start))

# now c should still get frames right away
c.settimeout(2)
ok = 0
start = time.time()
for i in range(100):
    a.sendall(frame(mac(1), mac(3), b"msg %d" % i))
    if read_frame(c) == b"msg %d" % i:
        ok += 1
took = time.time() - start
print("c got %d/100 frames in %.3fs" % (ok, took))

passed = ok == 100 and took < 1.0
for s in (a, b, c):
    s.close()
print("passed" if passed else "FAILED")
sys.exit(0 if passed else 1)
