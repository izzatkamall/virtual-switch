# port security test, run the switch first: ./switch 9700 4
# a host that sends from too many source macs gets its port shut
import socket, struct, sys, time

PORT = int(sys.argv[1]) if len(sys.argv) > 1 else 9700
LIMIT = 16  # MAC_PER_PORT in switch.c

def frame(src, dst, data):
    return struct.pack("!H6s6sHB", len(data), src, dst, 1, 1) + data

def mac(a, b):
    return bytes([2, 0, 0, 0, a, b])

good = socket.create_connection(("127.0.0.1", PORT))
bad = socket.create_connection(("127.0.0.1", PORT))
good.settimeout(1)
bad.settimeout(1)
time.sleep(0.2)

# up to the limit is fine
for i in range(LIMIT):
    bad.sendall(frame(mac(9, i), mac(0, 1), b"x"))
time.sleep(0.2)

# one more mac should get the port shut
try:
    bad.sendall(frame(mac(9, 200), mac(0, 1), b"x"))
except OSError:
    pass

shut = False
try:
    while True:
        if bad.recv(4096) == b"":
            shut = True
            break
except (socket.timeout, ConnectionResetError):
    shut = isinstance(sys.exc_info()[1], ConnectionResetError)
print("flooding port was shut:", shut)

# the other host still works and can connect a new one
good.sendall(frame(mac(0, 1), b"\xff" * 6, b"still here"))
newbie = socket.create_connection(("127.0.0.1", PORT))
newbie.settimeout(1)
time.sleep(0.2)
good.sendall(frame(mac(0, 1), b"\xff" * 6, b"hello newbie"))
data = b""
try:
    while b"hello newbie" not in data:
        x = newbie.recv(4096)
        if not x:
            break
        data += x
except socket.timeout:
    pass
ok2 = b"hello newbie" in data
print("switch still forwards after shutting a port:", ok2)

passed = shut and ok2
print("passed" if passed else "FAILED")
sys.exit(0 if passed else 1)
