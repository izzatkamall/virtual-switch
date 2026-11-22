# stress test for live reconfig, run: ./switch PORT 8 tests/stress.conf
# traffic + hosts coming and going + random swctl commands all at once,
# then checks the switch still counts its free ports right
import random, socket, struct, subprocess, sys, threading, time

PORT = int(sys.argv[1]) if len(sys.argv) > 1 else 9880
SECS = int(sys.argv[2]) if len(sys.argv) > 2 else 15
NPORTS = 8
BCAST = b"\xff" * 6
stop = time.time() + SECS
counts = {"frames": 0, "cmds": 0, "churn": 0}
lock = threading.Lock()

def frame(src, data):
    return struct.pack("!H6s6sHB", len(data), bytes([2, 0, 0, 0, 1, src]), BCAST, 1,
                       random.randint(0, 2)) + data

def swctl(*args):
    return subprocess.run(["./swctl"] + list(args), capture_output=True, text=True, timeout=10).stdout

def traffic(n):
    while time.time() < stop:
        try:
            s = socket.create_connection(("127.0.0.1", PORT))
            s.settimeout(0.5)
            for _ in range(200):
                s.sendall(frame(n, b"y" * random.randint(1, 1400)))
                try:
                    s.recv(65536)
                except socket.timeout:
                    pass
                with lock:
                    counts["frames"] += 1
                if time.time() > stop:
                    break
            s.close()
        except OSError:
            time.sleep(0.05)  # kicked off by a shutdown, try again

def churn():
    while time.time() < stop:
        try:
            s = socket.create_connection(("127.0.0.1", PORT))
            time.sleep(random.random() * 0.2)
            s.close()
            with lock:
                counts["churn"] += 1
        except OSError:
            pass

def admin():
    while time.time() < stop:
        p = str(random.randrange(NPORTS))
        cmd = random.choice([["shutdown", p], ["up", p], ["up", p], ["vlan", p, str(random.choice([1, 1, 2]))],
                             ["clear", "mac"], ["show", "mac"], ["show", "ports"]])
        swctl(*cmd)
        with lock:
            counts["cmds"] += 1

threads = [threading.Thread(target=traffic, args=(i,)) for i in range(6)]
threads += [threading.Thread(target=churn), threading.Thread(target=admin), threading.Thread(target=admin)]
for t in threads:
    t.start()
for t in threads:
    t.join()
print("frames %(frames)d, swctl commands %(cmds)d, short lived hosts %(churn)d" % counts)

# put everything back: all ports up, vlan 1, then wait for hosts to leave
for p in range(NPORTS):
    swctl("up", str(p))
    swctl("vlan", str(p), "1")
time.sleep(1.5)
states = [l.split()[1] for l in swctl("show ports").splitlines()[1:]]
print("ports after the storm:", states)
ok_free = all(s == "free" for s in states)

# the semaphore must still say exactly NPORTS free: 8 hosts get in, a 9th does not
hosts = [socket.create_connection(("127.0.0.1", PORT)) for _ in range(NPORTS)]
time.sleep(0.5)
states = [l.split()[1] for l in swctl("show ports").splitlines()[1:]]
ok_all_up = all(s == "up" for s in states)
extra = socket.create_connection(("127.0.0.1", PORT))
extra.settimeout(3)
try:
    data = extra.recv(100)
except socket.timeout:
    data = b""
ok_full = b"switch full" in data
print("all ports free after:", ok_free)
print("8 hosts all got a port:", ok_all_up)
print("9th host told switch full:", ok_full)
passed = ok_free and ok_all_up and ok_full
print("passed" if passed else "FAILED")
sys.exit(0 if passed else 1)
