# qos test, run the switch first: ./switch 9600 4
# b stops reading, a sends low first then normal then high,
# high was sent last, so with plain fifo nothing would come after the
# last high frame. with qos high jumps ahead of what is still queued
import socket, struct, sys, time

PORT = int(sys.argv[1]) if len(sys.argv) > 1 else 9600

def mac(n):
    return bytes([2, 0, 0, 0, 0, n])

def frame(src, dst, prio, data):
    return struct.pack("!H6s6sHB", len(data), src, dst, 1, prio) + data

def read_frame(s):
    hdr = b""
    while len(hdr) < 17:
        hdr += s.recv(17 - len(hdr))
    ln, _, _, _, prio = struct.unpack("!H6s6sHB", hdr)
    data = b""
    while len(data) < ln:
        data += s.recv(ln - len(data))
    return prio, data

a = socket.create_connection(("127.0.0.1", PORT))
b = socket.socket()
b.setsockopt(socket.SOL_SOCKET, socket.SO_RCVBUF, 4096)  # tiny so the switch has to queue
b.connect(("127.0.0.1", PORT))
time.sleep(0.2)
b.sendall(frame(mac(2), mac(9), 1, b"x"))  # so the switch learns b
a.settimeout(0.3)
try:
    while a.recv(4096):
        pass
except socket.timeout:
    pass

N = 40  # per priority, under the qos queue size so nothing drops
big = b"q" * 1000
for prio in (2, 1, 0):  # low first
    for i in range(N):
        a.sendall(frame(mac(1), mac(2), prio, big))
time.sleep(0.3)  # let the switch queue it all

b.settimeout(2)
order = [read_frame(b)[0] for _ in range(3 * N)]
print("order:", "".join("HNL"[p] for p in order))

last_high = max(i for i, p in enumerate(order) if p == 0)
after = len(order) - 1 - last_high
print("frames that high overtook: %d (fifo would be 0)" % after)
passed = after >= 10
print("passed" if passed else "FAILED")
sys.exit(0 if passed else 1)
