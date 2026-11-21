# broadcast storm test, run the switch first: ./switch 9400 8
# checks every host gets every broadcast with the right payload,
# and hosts leaving in the middle dont break anything
import socket, struct, sys, threading, time

PORT = int(sys.argv[1]) if len(sys.argv) > 1 else 9400
ROUNDS = int(sys.argv[2]) if len(sys.argv) > 2 else 5
N = 6           # hosts that stay
COUNT = 40      # broadcasts per round, less than the queue size
BCAST = b"\xff" * 6

def mac(n):
    return bytes([2, 0, 0, 0, 0, n])

def frame(src, dst, data):
    return struct.pack("!H6s6sHB", len(data), src, dst, 1, 1) + data

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
        x = s.recv(ln - len(data))
        if not x:
            return None
        data += x
    return hdr[2:8], data

def payload(sender, i):
    # long and easy to check, any corruption shows up
    return (b"%02d-%04d-" % (sender, i)) * 100

bad = 0
for r in range(ROUNDS):
    hosts = [socket.create_connection(("127.0.0.1", PORT)) for _ in range(N)]
    for s in hosts:
        s.settimeout(3)

    # two extra hosts that join and leave in the middle of the storm
    def churn():
        for _ in range(5):
            x = socket.create_connection(("127.0.0.1", PORT))
            time.sleep(0.01)
            x.close()
    t = threading.Thread(target=churn)
    t.start()
    time.sleep(0.1)

    # host 0 sends COUNT broadcasts, everyone else must get all of them
    for i in range(COUNT):
        hosts[0].sendall(frame(mac(1), BCAST, payload(1, i)))

    for h in hosts[1:]:
        for i in range(COUNT):
            got = read_frame(h)
            if got is None or got[1] != payload(1, i):
                bad += 1
                break
    t.join()
    for s in hosts:
        s.close()
    time.sleep(0.1)

print("rounds=%d bad=%d" % (ROUNDS, bad))
print("passed" if bad == 0 else "FAILED")
sys.exit(1 if bad else 0)
