# port limit test, run the switch with 2 ports: ./switch 9800 2
import socket, struct, sys, time

PORT = int(sys.argv[1]) if len(sys.argv) > 1 else 9800
WAIT = 5  # PORT_WAIT_SECS in switch.c
fails = 0

def check(name, ok):
    global fails
    print(("ok   " if ok else "FAIL ") + name)
    fails += not ok

def read_all(s, timeout):
    s.settimeout(timeout)
    data = b""
    try:
        while True:
            x = s.recv(4096)
            if not x:
                return data, True  # closed
            data += x
    except socket.timeout:
        return data, False

a = socket.create_connection(("127.0.0.1", PORT))
b = socket.create_connection(("127.0.0.1", PORT))
time.sleep(0.2)

# third host gets nothing for a while, then switch full
start = time.time()
c = socket.create_connection(("127.0.0.1", PORT))
data, closed = read_all(c, WAIT + 3)
took = time.time() - start
check("extra host told switch full", b"switch full" in data)
check("and disconnected", closed)
check("after waiting about %ds (took %.1fs)" % (WAIT, took), WAIT - 0.5 < took < WAIT + 1.5)
hdr = struct.unpack("!H6s6sHB", data[:17]) if len(data) >= 17 else None
check("reply is a proper frame", hdr is not None and hdr[0] == 11 and data[17:28] == b"switch full")

# a host that waits gets in once someone leaves
d = socket.create_connection(("127.0.0.1", PORT))
time.sleep(1)
a.close()
time.sleep(0.3)
mac = lambda n: bytes([2, 0, 0, 0, 0, n])
d.sendall(struct.pack("!H6s6sHB", 2, mac(4), b"\xff" * 6, 1, 1) + b"hi")
data, closed = read_all(b, 1)
check("waiting host gets the freed port", b"hi" in data and not closed)

b.close()
d.close()
print("all passed" if fails == 0 else "%d failed" % fails)
sys.exit(1 if fails else 0)
