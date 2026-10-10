"""A TURN fixture with expiring REST credentials and a thirty-second allocation."""
import base64
import hashlib
import hmac
import ipaddress
import json
import secrets
import selectors
import socket
import struct
import threading
import time

COOKIE = 0x2112A442


def attribute(kind, value):
    return struct.pack('!HH', kind, len(value)) + value + bytes((-len(value)) % 4)


def attributes(packet):
    end = min(len(packet), 20 + struct.unpack_from('!H', packet, 2)[0])
    at = 20
    result = []
    while at + 4 <= end:
        kind, length = struct.unpack_from('!HH', packet, at)
        result.append((kind, packet[at + 4:at + 4 + length], at))
        at += 4 + length + (-length) % 4
    return result


def xor_address(address, transaction):
    ip, port = address
    raw = ipaddress.ip_address(ip).packed
    mask = struct.pack('!I', COOKIE) + transaction
    return struct.pack('!BBH', 0, 1 if len(raw) == 4 else 2, port ^ (COOKIE >> 16)) + bytes(a ^ b for a, b in zip(raw, mask))


def read_address(value, transaction):
    family, port = struct.unpack_from('!BH', value, 1)
    length = 4 if family == 1 else 16
    mask = struct.pack('!I', COOKIE) + transaction
    return str(ipaddress.ip_address(bytes(a ^ b for a, b in zip(value[4:4 + length], mask)))), port ^ (COOKIE >> 16)


def packet(kind, transaction, values, key=None):
    body = b''.join(attribute(*item) for item in values)
    header = struct.pack('!HHI12s', kind, len(body) + (24 if key else 0), COOKIE, transaction)
    if key:
        body += attribute(8, hmac.new(key, header + body, hashlib.sha1).digest())
    return header + body


class TurnFixture:
    def __init__(self, address, log):
        self.address = address
        self.log = log
        self.secret = secrets.token_bytes(32)
        self.selector = selectors.DefaultSelector()
        self.socket = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        self.socket.bind((address, 0))
        self.selector.register(self.socket, selectors.EVENT_READ, None)
        self.allocations = {}
        self.started = time.monotonic()
        self.stopped = threading.Event()
        self.thread = threading.Thread(target=self.serve, daemon=True)
        self.errors = []
        self.counts = dict(allocate=0, refresh=0, wrong_credentials=0, expired_credentials=0, stale_nonce=0, forwarded=0)

    @property
    def uri(self):
        return f'turn:{self.address}:{self.socket.getsockname()[1]}?transport=udp'

    def login(self, ttl, path):
        username = f'{int(time.time()) + ttl}:{secrets.token_hex(12)}'
        password = base64.b64encode(hmac.new(self.secret, username.encode(), hashlib.sha1).digest()).decode()
        path.write_text(username + '\n' + password + '\n', encoding='utf-8')
        path.chmod(0o600)

    def note(self, operation, **fields):
        self.log.write(json.dumps(dict(t=round(time.monotonic() - self.started, 3), operation=operation, **fields)) + '\n')
        self.log.flush()

    def key(self, username):
        password = base64.b64encode(hmac.new(self.secret, username.encode(), hashlib.sha1).digest()).decode()
        return hashlib.md5((username + ':turn-check:' + password).encode()).digest()

    def nonce(self):
        return str(int(time.monotonic() - self.started) // 10).encode()

    def error(self, kind, transaction, peer, code, key=None):
        values = [(9, struct.pack('!I', ((code // 100) << 8) | (code % 100)))]
        if code in (401, 438):
            values += [(0x14, b'turn-check'), (0x15, self.nonce())]
        self.socket.sendto(packet(kind | 0x110, transaction, values, key), peer)

    def request(self, data, peer):
        if len(data) < 20 or struct.unpack_from('!I', data, 4)[0] != COOKIE:
            return
        kind = struct.unpack_from('!H', data)[0]
        transaction = data[8:20]
        values = attributes(data)
        fields = {key: value for key, value, _ in values}
        allocation = self.allocations.get(peer)
        now = time.monotonic()
        if kind == 0x16:
            if allocation and allocation['expires'] > now and 0x12 in fields and 0x13 in fields:
                destination = read_address(fields[0x12], transaction)
                if allocation['permissions'].get(destination[0], 0) > now:
                    allocation['socket'].sendto(fields[0x13], destination)
                    self.counts['forwarded'] += 1
            return
        if kind not in (3, 4, 8):
            return
        username = fields.get(6, b'').decode('ascii', errors='ignore')
        if not username:
            self.error(kind, transaction, peer, 401)
            return
        key = self.key(username)
        if allocation and kind in (4, 8) and username != allocation['username']:
            self.counts['wrong_credentials'] += 1
            self.note('refused', code=441, allocation=allocation['id'])
            self.error(kind, transaction, peer, 441, key)
            return
        try:
            expired = int(username.split(':', 1)[0]) <= time.time()
        except ValueError:
            expired = True
        if expired:
            self.counts['expired_credentials'] += 1
            self.note('refused', code=401, allocation=allocation['id'] if allocation else 0)
            self.error(kind, transaction, peer, 401)
            return
        if fields.get(0x15) != self.nonce():
            self.counts['stale_nonce'] += 1
            self.error(kind, transaction, peer, 438, key)
            return
        integrity = next(((value, at) for code, value, at in values if code == 8), None)
        if integrity is None:
            self.error(kind, transaction, peer, 401)
            return
        digest, at = integrity
        signed = bytearray(data[:at])
        struct.pack_into('!H', signed, 2, at - 20 + 24)
        if not hmac.compare_digest(digest, hmac.new(key, signed, hashlib.sha1).digest()):
            self.error(kind, transaction, peer, 401)
            return
        if kind == 3:
            if fields.get(0x17, bytes([1]))[:1] == bytes([2]):
                self.error(kind, transaction, peer, 440, key)
                return
            if not allocation:
                relay = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
                relay.bind((self.address, 0))
                allocation = dict(socket=relay, username=username, expires=now + 30, permissions={}, id=self.counts['allocate'] + 1, peer=peer)
                self.allocations[peer] = allocation
                self.selector.register(relay, selectors.EVENT_READ, allocation)
                self.counts['allocate'] += 1
                self.note('allocate', allocation=allocation['id'], lifetime_s=30, login_sha256=hashlib.sha256(username.encode()).hexdigest())
            reply = [(0x16, xor_address(allocation['socket'].getsockname(), transaction)), (0x20, xor_address(peer, transaction)), (0xD, struct.pack('!I', 30))]
        elif not allocation:
            self.error(kind, transaction, peer, 437, key)
            return
        elif kind == 4:
            allocation['expires'] = now + 30
            self.counts['refresh'] += 1
            self.note('refresh', allocation=allocation['id'], lifetime_s=30, expires_t=round(allocation['expires'] - self.started, 3))
            reply = [(0xD, struct.pack('!I', 30))]
        else:
            for code, value, _ in values:
                if code == 0x12:
                    allocation['permissions'][read_address(value, transaction)[0]] = now + 300
            reply = []
        self.socket.sendto(packet(kind | 0x100, transaction, reply, key), peer)

    def serve(self):
        try:
            while not self.stopped.is_set():
                for event, _ in self.selector.select(.1):
                    data, peer = event.fileobj.recvfrom(65535)
                    allocation = event.data
                    if allocation is None:
                        self.request(data, peer)
                    elif allocation['expires'] > time.monotonic() and allocation['permissions'].get(peer[0], 0) > time.monotonic():
                        self.socket.sendto(packet(0x17, bytes(12), [(0x12, xor_address(peer, bytes(12))), (0x13, data)]), allocation['peer'])
                for peer, allocation in list(self.allocations.items()):
                    if allocation['expires'] <= time.monotonic():
                        self.note('allocation_expired', allocation=allocation['id'])
                        self.selector.unregister(allocation['socket'])
                        allocation['socket'].close()
                        del self.allocations[peer]
        except Exception as error:
            self.errors.append(type(error).__name__)

    def close(self):
        self.stopped.set()
        self.thread.join(2)
        self.selector.close()
        self.socket.close()
        for allocation in self.allocations.values():
            allocation['socket'].close()
