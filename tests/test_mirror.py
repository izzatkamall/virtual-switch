# mirroring test, run: ./switch PORT 4
import os, socket, struct, subprocess, sys, time

PORT = int(sys.argv[1]) if len(sys.argv) > 1 else 9910
fails = 0

def check(name, ok):
    global fails
    print(("ok   " if ok else "FAIL ") + name)
    fails += not ok

def swctl(*args):
    return subprocess.run(["./swctl"] + list(args), capture_output=True, text=True).stdout

def mac(n):
    return bytes([2, 0, 0, 0, 0, n])

def frame(src, dst, data, vlan=1):
    return struct.pack("!H6s6sHB", len(data), src, dst, vlan, 1) + data

def read_all(s, timeout=0.4):
    s.settimeout(timeout)
    data = b""
    try:
        while True:
            x = s.recv(65536)
            if not x:
                break
            data += x
    except socket.timeout:
        pass
    return data

def connect(small=False):
    s = socket.socket()
    if small:
        s.setsockopt(socket.SOL_SOCKET, socket.SO_RCVBUF, 4096)
    s.connect(("127.0.0.1", PORT))
    time.sleep(0.15)
    return s

a, b, sn = connect(), connect(), connect()  # ports 0, 1, 2
a.sendall(frame(mac(1), mac(9), b"x"))
b.sendall(frame(mac(2), mac(9), b"x"))
time.sleep(0.2)
for s in (a, b, sn):
    read_all(s, 0.2)

check("mirror reply", "mirrored to port 2" in swctl("mirror", "0", "2"))

a.sendall(frame(mac(1), mac(2), b"a to b"))
check("b gets it", b"a to b" in read_all(b))
check("sniffer sees frames coming in on port 0", b"a to b" in read_all(sn))

b.sendall(frame(mac(2), mac(1), b"b to a"))
check("a gets it", b"b to a" in read_all(a))
check("sniffer sees frames going out of port 0", b"b to a" in read_all(sn))

# the sniffer sees port 0 even across vlans
swctl("vlan", "2", "50")
a.sendall(frame(mac(1), mac(2), b"other vlan"))
read_all(b)
check("sniffer in another vlan still gets the copy", b"other vlan" in read_all(sn))
swctl("vlan", "2", "1")

line = [l for l in swctl("show ports").splitlines() if l.startswith("0 ")][0]
check("mirrored counter goes up (%s)" % line.split()[-1], int(line.split()[-1]) >= 3)

# frozen sniffer doesnt slow port 0 down
sn.close()
time.sleep(0.2)
slow = connect(small=True)  # lands on port 2 again
swctl("mirror", "0", "2")

def blast():
    # a sends 2000 frames to b, b is read by a child process so python's
    # gil cant starve the reader and make b's own queue overflow
    r, w = os.pipe()
    pid = os.fork()
    if pid == 0:
        os.close(r)
        b.settimeout(1)
        total = 0
        while total < 2000 * 1017:
            try:
                x = b.recv(1 << 20)
            except socket.timeout:
                break
            total += len(x)
        os.write(w, str(total // 1017).encode())  # every frame here is 1017 bytes
        os._exit(0)
    os.close(w)
    start = time.time()
    for i in range(2000):
        a.sendall(frame(mac(1), mac(2), b"Z" * 1000))
        if i % 20 == 19:
            time.sleep(0.002)
    took = time.time() - start  # how long the switch took to take it all from a
    got = int(os.read(r, 32) or b"0")
    os.waitpid(pid, 0)
    os.close(r)
    return got, took

def port_count(p, col):
    line = [l for l in swctl("show ports").splitlines() if l.startswith("%d " % p)][0]
    return int(line.split()[col])

# at full speed b can lose a few frames to its own bounded queue whenever
# the reader gets descheduled, thats counted as dropped on port 1 and has
# nothing to do with mirroring. so check the switch kept reading port 0 at
# full speed and b got about the same as without a sniffer
swctl("mirror", "0", "off")
in0 = port_count(0, 4)
base, base_t = blast()
base_in = port_count(0, 4) - in0
swctl("mirror", "0", "2")
in0 = port_count(0, 4)
got, took = blast()
got_in = port_count(0, 4) - in0
print("     a sent 2000 in %.2fs without mirror, %.2fs with a frozen sniffer" % (base_t, took))
check("switch read all 2000 from port 0 both times", base_in == 2000 and got_in == 2000)
# under valgrind the switch is many times slower so speed means nothing
if os.environ.get("RUN"):
    print("     speed checks skipped, switch runs under %s" % os.environ["RUN"].split()[0])
else:
    check("not slower with a frozen sniffer", took < base_t * 1.5 + 0.3)
    check("b got most of them both times (%d and %d)" % (base, got), got >= 1800 and base >= 1600)
check("frozen sniffer dropped its copies instead (%d)" % port_count(2, 6), port_count(2, 6) > 0)
dropped1 = port_count(1, 6)
out1 = port_count(1, 5)
check("nothing lost without being counted on port 1", out1 + dropped1 >= base + got)

check("off reply", "not mirrored" in swctl("mirror", "0", "off"))
read_all(slow, 0.3)
a.sendall(frame(mac(1), mac(2), b"after off"))
read_all(b)
time.sleep(0.2)
check("sniffer gets nothing after off", b"after off" not in read_all(slow, 1))

check("cant mirror a port to itself", "usage" in swctl("mirror", "1", "1"))
check("bad port", "usage" in swctl("mirror", "0", "9"))

for s in (a, b, slow):
    s.close()
print("all passed" if fails == 0 else "%d failed" % fails)
sys.exit(1 if fails else 0)
