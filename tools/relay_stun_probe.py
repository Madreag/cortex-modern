"""One STUN Binding request per host:port argument: the mapped address and the round trip, or the failure. No login involved."""
import os
import socket
import struct
import sys
import time


def binding(host, port, timeout=3.0):
    txn = os.urandom(12)
    request = struct.pack('!HHI', 0x0001, 0, 0x2112A442) + txn
    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    sock.settimeout(timeout)
    address = '?'
    try:
        address = socket.gethostbyname(host)
        started = time.perf_counter()
        sock.sendto(request, (address, port))
        data, _ = sock.recvfrom(2048)
        rtt = (time.perf_counter() - started) * 1000
    except OSError as error:
        return f'{host}:{port} ({address}) FAIL {type(error).__name__}'
    finally:
        sock.close()
    kind, length = struct.unpack('!HH', data[:4])
    offset, mapped = 20, None
    while offset + 4 <= 20 + length:
        attr, size = struct.unpack('!HH', data[offset:offset + 4])
        value = data[offset + 4:offset + 4 + size]
        if attr == 0x0020 and size >= 8:
            xport = struct.unpack('!H', value[2:4])[0] ^ 0x2112
            xaddr = struct.unpack('!I', value[4:8])[0] ^ 0x2112A442
            mapped = f'{socket.inet_ntoa(struct.pack("!I", xaddr))}:{xport}'
        offset += 4 + size + ((4 - size % 4) % 4)
    return f'{host}:{port} ({address}) type=0x{kind:04x} mapped={mapped} rtt_ms={rtt:.1f}'


for target in sys.argv[1:]:
    host, _, port = target.rpartition(':')
    print(binding(host, int(port)), flush=True)
