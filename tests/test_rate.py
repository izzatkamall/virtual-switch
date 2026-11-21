# rate limit test, run the switch with a config that has rate_limit 20:
#   ./switch 9700 4 rate.conf
# a sends 50 frames at once, b should get them at about 20 a second
import socket, struct, sys, time

PORT = int(sys.argv[1]) if len(sys.argv) > 1 else 9700
RATE = int(sys.argv[2]) if len(sys.argv) > 2 else 20
N = 50  # under the queue size so none get dropped

def mac(n):
    return bytes([2, 0, 0, 0, 0, n])

def frame(src, dst, data):
    return struct.pack("!H6s6sHB", len(data), src, dst, 1, 1) + data

def read_frame(s):
    hdr = b""
    while len(hdr) < 17:
        hdr += s.recv(17 - len(hdr))
    ln = struct.unpack("!H", hdr[:2])[0]
    data = b""
    while len(data) < ln:
        data += s.recv(ln - len(data))
    return data

a = socket.create_connection(("127.0.0.1", PORT))
b = socket.create_connection(("127.0.0.1", PORT))
time.sleep(0.2)
b.sendall(frame(mac(2), mac(9), b"x"))  # learn b
time.sleep(1.2)  # a's bucket refills after the flood of b's first frame

start = time.time()
for i in range(N):
    a.sendall(frame(mac(1), mac(2), b"n%d" % i))

b.settimeout(5)
times = []
for i in range(N):
    read_frame(b)
    times.append(time.time() - start)

# bucket starts full (RATE frames), the rest come at RATE per second
burst = sum(1 for t in times if t < 0.05)
total = times[-1]
expect = (N - RATE) / RATE
print("burst at the start: %d (bucket size %d)" % (burst, RATE))
print("all %d took %.2fs, expected about %.2fs" % (N, total, expect))
passed = RATE - 3 <= burst <= RATE + 3 and expect - 0.3 < total < expect + 0.4
print("passed" if passed else "FAILED")
sys.exit(0 if passed else 1)
