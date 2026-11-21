# forwarding test, run the switch first: ./switch 9200 4
import socket, struct, sys, time

PORT = int(sys.argv[1]) if len(sys.argv) > 1 else 9200
fails = 0

def mac(n):
    return bytes([2, 0, 0, 0, 0, n])

BCAST = b"\xff" * 6

def frame(src, dst, text, vlan=1, prio=1):
    data = text.encode()
    return struct.pack("!H6s6sHB", len(data), src, dst, vlan, prio) + data

def connect():
    s = socket.create_connection(("127.0.0.1", PORT))
    s.settimeout(0.5)
    return s

def recv_frame(s):
    # returns (src, dst, text) or None if nothing came
    try:
        hdr = b""
        while len(hdr) < 17:
            x = s.recv(17 - len(hdr))
            if not x:
                return None
            hdr += x
        ln, src, dst, vlan, prio = struct.unpack("!H6s6sHB", hdr)
        data = b""
        while len(data) < ln:
            data += s.recv(ln - len(data))
        return (src, dst, data.decode())
    except socket.timeout:
        return None

def check(name, ok):
    global fails
    print(("ok   " if ok else "FAIL ") + name)
    if not ok:
        fails += 1

a, b, c = connect(), connect(), connect()
time.sleep(0.2)

# a to b, b is unknown so it floods to b and c
a.sendall(frame(mac(1), mac(2), "hello b"))
check("unknown dest reaches b", recv_frame(b) == (mac(1), mac(2), "hello b"))
check("unknown dest flooded to c", recv_frame(c) == (mac(1), mac(2), "hello b"))
check("sender does not get its own frame", recv_frame(a) is None)

# b to a, a was learned so only a gets it
b.sendall(frame(mac(2), mac(1), "hi a"))
check("learned dest reaches a", recv_frame(a) == (mac(2), mac(1), "hi a"))
check("learned dest not flooded to c", recv_frame(c) is None)

# now a to b again, b is learned too
a.sendall(frame(mac(1), mac(2), "again"))
check("a to b direct", recv_frame(b) == (mac(1), mac(2), "again"))
check("c gets nothing", recv_frame(c) is None)

# broadcast from c
c.sendall(frame(mac(3), BCAST, "to all"))
check("broadcast reaches a", recv_frame(a) == (mac(3), BCAST, "to all"))
check("broadcast reaches b", recv_frame(b) == (mac(3), BCAST, "to all"))
check("broadcast not back to c", recv_frame(c) is None)

# frame sent in tiny pieces
f = frame(mac(1), mac(2), "split up frame")
for i in range(len(f)):
    a.send(f[i:i + 1])
    time.sleep(0.002)
check("frame sent byte by byte", recv_frame(b) == (mac(1), mac(2), "split up frame"))

# host dies half way through a frame, switch must keep going
d = connect()
d.sendall(frame(mac(4), mac(1), "cut off")[:10])
d.close()
time.sleep(0.2)
b.sendall(frame(mac(2), mac(1), "still alive"))
check("switch survives cut frame", recv_frame(a) == (mac(2), mac(1), "still alive"))

# host moves: mac 3 shows up on a new connection
c.close()
e = connect()
time.sleep(0.2)
e.sendall(frame(mac(3), mac(1), "moved"))
check("moved host reaches a", recv_frame(a) == (mac(3), mac(1), "moved"))
a.sendall(frame(mac(1), mac(3), "find you"))
check("frame follows moved host", recv_frame(e) == (mac(1), mac(3), "find you"))

for s in (a, b, e):
    s.close()
print("all passed" if fails == 0 else "%d failed" % fails)
sys.exit(1 if fails else 0)
