"""wifi on a host: python3 test_wifi.py /path/to/circuitpython

wifi.radio is the computer's network: connect() succeeds at once, the addresses are the
computer's, SocketPool(wifi.radio) opens the computer's sockets. ap_info and a scan give the
network the computer is on when it is on Wi-Fi, else one named as the program asked for.
"""
import os, socket, subprocess, threading
from harness import check, run


def host_ssid():
    """The Wi-Fi network the computer is on, or None (Ethernet, no tool to ask)."""
    try:
        if os.name == "nt":
            out = subprocess.run(["netsh", "wlan", "show", "interfaces"], capture_output=True, text=True, timeout=10).stdout
            for line in out.splitlines():
                key, _, value = line.partition(":")
                if key.strip() == "SSID" and value.strip():
                    return value.strip()
        else:
            out = subprocess.run(["nmcli", "-t", "-f", "active,ssid", "dev", "wifi"], capture_output=True, text=True, timeout=10).stdout
            for line in out.splitlines():
                if line.startswith("yes:") and line[4:]:
                    return line[4:]
    except (OSError, subprocess.SubprocessError):
        pass
    return None


def host_ip():
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    try:
        s.connect(("192.0.2.1", 9))         # no packet goes out; the route picks the address
        return s.getsockname()[0]
    finally:
        s.close()


# An echo server for the program's socket.
server = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
server.bind(("127.0.0.1", 0))
server.listen(1)
PORT = server.getsockname()[1]


def echo():
    conn, _ = server.accept()
    conn.sendall(conn.recv(64).upper())
    conn.close()


threading.Thread(target=echo, daemon=True).start()

out, trace = run('''import wifi, socketpool, ipaddress
r = wifi.radio
print("enabled", r.enabled, "connected", r.connected, "ap", r.ap_info)
r.connect("Lab", "secret12")
print("connected", r.connected)
print("address", r.ipv4_address, "gateway", r.ipv4_gateway is not None, "subnet", r.ipv4_subnet is not None)
print("mac", len(r.mac_address), "hostname", r.hostname)
ap = r.ap_info
print("ssid", ap.ssid, "rssi", ap.rssi < 0, "channel", ap.channel > 0)
scan = list(r.start_scanning_networks())
r.stop_scanning_networks()
print("scan", len(scan), scan[0].ssid == ap.ssid)
pool = socketpool.SocketPool(r)
s = pool.socket(pool.AF_INET, pool.SOCK_STREAM)
s.connect(("127.0.0.1", %d))
s.send(b"hello")
buf = bytearray(16)
n = s.recv_into(buf)
s.close()
print("echo", bytes(buf[:n]))
print("ping", r.ping(ipaddress.ip_address("127.0.0.1")) is not None)
try:
    r.start_ap("x", "password")
except NotImplementedError:
    print("no access point")
r.stop_station()
print("stopped", r.connected)
print("wifi done")
''' % PORT, expect="wifi done")
ssid = host_ssid()
check("the radio is on and not connected until connect()", "enabled True connected False ap None" in out, out)
check("connect() succeeds", "connected True" in out, out)
check("the computer's address", "address %s gateway True subnet True" % host_ip() in out, out)
check("a MAC address and the computer's host name", "mac 6 hostname %s" % socket.gethostname() in out, out)
check("ap_info: " + ("the computer's Wi-Fi " + ssid if ssid else "the network the program asked for"),
      "ssid %s rssi True channel True" % (ssid or "Lab") in out, out)
check("a scan finds that network", "scan 1 True" in out, out)
check("SocketPool(wifi.radio) opens the computer's sockets", "echo b'HELLO'" in out, out)
check("ping answers", "ping True" in out, out)
check("no access point mode", "no access point" in out, out)
check("stop_station() disconnects", "stopped False" in out, out)
check("trace: the connect", "WIFI connect Lab" in trace, trace)

print("DONE")
