# vlan test, run: ./switch 9800 4 tests/vlan.conf
# port 0 and 1 are vlan 10, port 2 is vlan 20, port 3 is the trunk
import socket, struct, sys, time

PORT = int(sys.argv[1]) if len(sys.argv) > 1 else 9800
BCAST = b"\xff" * 6
fails = 0

def mac(n):
    return bytes([2, 0, 0, 0, 0, n])

def frame(src, dst, data, vlan=1):
    return struct.pack("!H6s6sHB", len(data), src, dst, vlan, 1) + data

def recv(s):
    # (vlan, payload) or None
    try:
        hdr = b""
        while len(hdr) < 17:
            x = s.recv(17 - len(hdr))
            if not x:
                return None
            hdr += x
        ln, _, _, vlan, _ = struct.unpack("!H6s6sHB", hdr)
        data = b""
        while len(data) < ln:
            data += s.recv(ln - len(data))
        return (vlan, data)
    except socket.timeout:
        return None

def check(name, ok):
    global fails
    print(("ok   " if ok else "FAIL ") + name)
    fails += not ok

# connect one at a time so they land on ports 0, 1, 2, 3
hosts = []
for i in range(4):
    s = socket.create_connection(("127.0.0.1", PORT))
    s.settimeout(0.4)
    hosts.append(s)
    time.sleep(0.1)
a, b, c, t = hosts  # a b in vlan 10, c in vlan 20, t is the trunk

# broadcast from a stays in vlan 10, the trunk gets it tagged 10
a.sendall(frame(mac(1), BCAST, b"hi ten"))
check("vlan 10 bcast reaches b", recv(b) == (10, b"hi ten"))
check("trunk gets it with vlan 10", recv(t) == (10, b"hi ten"))
check("vlan 20 host does not", recv(c) is None)

# c broadcasts in vlan 20, only the trunk sees it
c.sendall(frame(mac(3), BCAST, b"hi twenty"))
check("trunk gets vlan 20 bcast tagged 20", recv(t) == (20, b"hi twenty"))
check("vlan 10 hosts dont", recv(a) is None and recv(b) is None)

# a host cant pick its own vlan, the access port overrides it
a.sendall(frame(mac(1), BCAST, b"sneaky", vlan=20))
check("access port forces its own vlan", recv(b) == (10, b"sneaky"))
recv(t)
check("fake vlan 20 does not reach c", recv(c) is None)

# unicast to a mac in another vlan does not cross over
a.sendall(frame(mac(1), mac(3), b"to c"))
check("unicast across vlans blocked", recv(c) is None)
recv(b)
recv(t)

# frames from the trunk keep their vlan
t.sendall(frame(mac(50), BCAST, b"from trunk 20", vlan=20))
check("trunk vlan 20 reaches c", recv(c) == (20, b"from trunk 20"))
check("trunk vlan 20 not a or b", recv(a) is None and recv(b) is None)
t.sendall(frame(mac(51), BCAST, b"from trunk 10", vlan=10))
check("trunk vlan 10 reaches a", recv(a) == (10, b"from trunk 10"))
check("trunk vlan 10 reaches b", recv(b) == (10, b"from trunk 10"))
check("trunk vlan 10 not c", recv(c) is None)

# learned unicast through the trunk, the switch learned mac 50 on the trunk
c.sendall(frame(mac(3), mac(50), b"to the trunk"))
check("unicast to a host behind the trunk", recv(t) == (20, b"to the trunk"))

for s in hosts:
    s.close()
print("all passed" if fails == 0 else "%d failed" % fails)
sys.exit(1 if fails else 0)
