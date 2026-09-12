"""W97 lane loopback fakes: a NAT-PMP/PCP responder and a fake UPnP IGD.

Two listeners, all on 127.0.0.1 and nowhere else:
  * UDP --natpmp-port (lane default 47601) answers NAT-PMP external-address and UDP map
    requests plus PCP MAP requests, and deletes on a zero-lifetime request.
  * HTTP --igd-port (lane default 8467) serves a one-device description document and the
    WANIPConnection:1 control endpoint (AddPortMapping / GetExternalIPAddress /
    DeletePortMapping).

--natpmp-mode / --pcp-mode pick how each protocol answers a map request:
  ok      grant the mapping (the fake external endpoint is --external-ip : the requested port)
  refuse  answer with result code 2 so the engine falls through to the next method
  silent  drop the datagram so the engine times out

Every received packet/request is logged to stdout and --log-file so the lane report can
quote the wire verbatim; on exit a summary lists every mapping added and removed.
"""
from __future__ import annotations

import argparse
import socket
import struct
import sys
import threading
import time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path

SERVICE_TYPE = "urn:schemas-upnp-org:service:WANIPConnection:1"
DEVICE_TYPE = "urn:schemas-upnp-org:device:InternetGatewayDevice:1"

_LOG_LOCK = threading.Lock()
_LOG_FILE = None
_MAPPINGS: dict[int, int] = {}  # internal port -> external port
_EVENTS: list[str] = []


def log(line: str) -> None:
    stamp = f"{time.time():.3f}"
    with _LOG_LOCK:
        out = f"[fake-port-map {stamp}] {line}"
        print(out, flush=True)
        _EVENTS.append(out)
        if _LOG_FILE is not None:
            _LOG_FILE.write(out + "\n")
            _LOG_FILE.flush()


def be16(data: bytes, at: int) -> int:
    return struct.unpack_from(">H", data, at)[0]


def be32(data: bytes, at: int) -> int:
    return struct.unpack_from(">I", data, at)[0]


def natpmp_reply(opcode: int, result: int, epoch: int, internal: int, external: int, lifetime: int) -> bytes:
    return struct.pack(">BBHIHHI", 0, 0x80 | opcode, result, epoch, internal, external, lifetime)


def pcp_reply(nonce: bytes, result: int, internal: int, external: int, external_ip: str, lifetime: int) -> bytes:
    out = bytearray()
    out += struct.pack(">BBBB", 2, 0x81, 0, result)
    out += struct.pack(">I", lifetime)
    out += struct.pack(">I", int(time.time()))  # epoch
    out += b"\x00" * 12
    out += nonce
    out += b"\x11"  # UDP
    out += b"\x00" * 3
    out += struct.pack(">H", internal)
    out += struct.pack(">H", external)
    out += b"\x00" * 10 + b"\xff\xff" + socket.inet_aton(external_ip)
    return bytes(out)


class UdpResponder(threading.Thread):
    def __init__(self, options: argparse.Namespace) -> None:
        super().__init__(daemon=True)
        self.options = options
        self.epoch_start = time.monotonic()

    def _epoch(self) -> int:
        return int(time.monotonic() - self.epoch_start)

    def run(self) -> None:
        sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        sock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        sock.bind((self.options.bind, self.options.natpmp_port))
        log(f"udp listening on {self.options.bind}:{self.options.natpmp_port} natpmp={self.options.natpmp_mode} pcp={self.options.pcp_mode}")
        while True:
            data, peer = sock.recvfrom(4096)
            version = data[0] if data else -1
            if version == 0:
                self._natpmp(sock, data, peer)
            elif version == 2 and len(data) >= 60 and data[1] & 0x7F == 1:
                self._pcp(sock, data, peer)
            else:
                log(f"udp {peer} {len(data)}B dropped (unrecognised)")

    def _natpmp(self, sock: socket.socket, data: bytes, peer) -> None:
        if len(data) == 2:
            reply = natpmp_reply(0, 0, self._epoch(), 0, 0, 0)[:4] + socket.inet_aton(self.options.external_ip)
            reply = struct.pack(">BBH", 0, 0x80, 0) + struct.pack(">I", self._epoch()) + socket.inet_aton(self.options.external_ip)
            sock.sendto(reply, peer)
            log(f"natpmp {peer} address request -> external {self.options.external_ip}")
            return
        if len(data) < 12 or data[1] != 2:
            log(f"natpmp {peer} {len(data)}B dropped (bad shape)")
            return
        internal, suggested, lifetime = be16(data, 4), be16(data, 6), be32(data, 8)
        if lifetime == 0:
            removed = _MAPPINGS.pop(internal, None)
            sock.sendto(natpmp_reply(2, 0, self._epoch(), internal, suggested, 0), peer)
            log(f"natpmp {peer} delete udp {internal} -> result=0 (had external {removed})")
            return
        if self.options.natpmp_mode == "silent":
            log(f"natpmp {peer} map udp {internal} lifetime {lifetime} -> silent drop")
            return
        if self.options.natpmp_mode == "refuse":
            sock.sendto(natpmp_reply(2, 2, self._epoch(), internal, 0, 0), peer)
            log(f"natpmp {peer} map udp {internal} -> result=2 refused")
            return
        external = suggested or internal
        _MAPPINGS[internal] = external
        sock.sendto(natpmp_reply(2, 0, self._epoch(), internal, external, 600), peer)
        log(f"natpmp {peer} map udp {internal} -> {self.options.external_ip}:{external} result=0 lifetime=600")

    def _pcp(self, sock: socket.socket, data: bytes, peer) -> None:
        nonce = data[24:36]
        lifetime, protocol, internal, suggested = be32(data, 4), data[36], be16(data, 40), be16(data, 42)
        if lifetime == 0:
            removed = _MAPPINGS.pop(internal, None)
            sock.sendto(pcp_reply(nonce, 0, internal, 0, "0.0.0.0", 0), peer)
            log(f"pcp {peer} delete udp {internal} -> result=0 (had external {removed})")
            return
        if self.options.pcp_mode == "silent":
            log(f"pcp {peer} map udp {internal} proto {protocol} -> silent drop")
            return
        if self.options.pcp_mode == "refuse":
            sock.sendto(pcp_reply(nonce, 2, internal, 0, "0.0.0.0", 0), peer)
            log(f"pcp {peer} map udp {internal} -> result=2 refused")
            return
        external = suggested or internal
        _MAPPINGS[internal] = external
        sock.sendto(pcp_reply(nonce, 0, internal, external, self.options.external_ip, 600), peer)
        log(f"pcp {peer} map udp {internal} -> {self.options.external_ip}:{external} result=0 lifetime=600")


def description_document(bind: str, port: int) -> str:
    return (
        '<?xml version="1.0"?>'
        '<root xmlns="urn:schemas-upnp-org:device-1-0">'
        "<specVersion><major>1</major><minor>0</minor></specVersion>"
        f"<device><deviceType>{DEVICE_TYPE}</deviceType><friendlyName>w97 fake igd</friendlyName>"
        "<deviceList><device><deviceType>urn:schemas-upnp-org:device:WANDevice:1</deviceType>"
        "<deviceList><device><deviceType>urn:schemas-upnp-org:device:WANConnectionDevice:1</deviceType>"
        "<serviceList><service>"
        f"<serviceType>{SERVICE_TYPE}</serviceType>"
        "<serviceId>urn:upnp-org:serviceId:WANIPConn1</serviceId>"
        "<controlURL>/ctl/IPConn</controlURL>"
        "<eventSubURL>/evt/IPConn</eventSubURL>"
        "<SCPDURL>/scpd/IPConn</SCPDURL>"
        "</service></serviceList></device></deviceList></device></deviceList></device></root>"
    )


def soap_envelope(element: str, inner: str = "") -> bytes:
    return (
        '<?xml version="1.0"?>'
        '<s:Envelope xmlns:s="http://schemas.xmlsoap.org/soap/envelope/" s:encodingStyle="http://schemas.xmlsoap.org/soap/encoding/">'
        f'<s:Body><u:{element} xmlns:u="{SERVICE_TYPE}">{inner}</u:{element}></s:Body></s:Envelope>'
    ).encode()


def tag_value(body: str, tag: str) -> str:
    open_tag, close_tag = f"<{tag}>", f"</{tag}>"
    begin = body.find(open_tag)
    if begin < 0:
        return ""
    begin += len(open_tag)
    end = body.find(close_tag, begin)
    return body[begin:end] if end >= 0 else ""


class IgdHandler(BaseHTTPRequestHandler):
    options: argparse.Namespace = None  # installed on the class before serve()

    def log_message(self, fmt, *args):  # keep BaseHTTPRequestHandler quiet; we log ourselves
        pass

    def do_GET(self) -> None:
        body = description_document(self.options.bind, self.options.igd_port).encode()
        log(f"http GET {self.path} -> 200 description ({len(body)}B)")
        self.send_response(200)
        self.send_header("Content-Type", "text/xml")
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def do_POST(self) -> None:
        length = int(self.headers.get("Content-Length", "0"))
        body = self.rfile.read(length).decode("utf-8", "replace")
        action = self.headers.get("SOAPAction", "")
        log(f"http POST {self.path} SOAPAction={action} body={body[:400]}")
        if "AddPortMapping" in body:
            internal = int(tag_value(body, "NewInternalPort") or 0)
            external = int(tag_value(body, "NewExternalPort") or 0)
            client = tag_value(body, "NewInternalClient")
            _MAPPINGS[internal] = external
            reply = soap_envelope("AddPortMappingResponse")
            log(f"igd AddPortMapping udp {external} -> {client}:{internal} stored, 200")
        elif "GetExternalIPAddress" in body:
            reply = soap_envelope("GetExternalIPAddressResponse", f"<NewExternalIPAddress>{self.options.external_ip}</NewExternalIPAddress>")
            log(f"igd GetExternalIPAddress -> {self.options.external_ip}, 200")
        elif "DeletePortMapping" in body:
            external = int(tag_value(body, "NewExternalPort") or 0)
            removed = [k for k, v in _MAPPINGS.items() if v == external]
            for key in removed:
                del _MAPPINGS[key]
            reply = soap_envelope("DeletePortMappingResponse")
            log(f"igd DeletePortMapping external {external} removed {removed}, 200")
        else:
            reply = soap_envelope("Fault", "<errorCode>401</errorCode><errorDescription>Invalid Action</errorDescription>")
            log("igd unrecognised action, 500")
            self.send_response(500)
            self.send_header("Content-Type", "text/xml")
            self.send_header("Content-Length", str(len(reply)))
            self.end_headers()
            self.wfile.write(reply)
            return
        self.send_response(200)
        self.send_header("Content-Type", "text/xml")
        self.send_header("Content-Length", str(len(reply)))
        self.end_headers()
        self.wfile.write(reply)


def main() -> int:
    global _LOG_FILE
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--bind", default="127.0.0.1")
    parser.add_argument("--natpmp-port", type=int, default=47601)
    parser.add_argument("--igd-port", type=int, default=8467)
    parser.add_argument("--external-ip", default="203.0.113.7")
    parser.add_argument("--natpmp-mode", choices=["ok", "refuse", "silent"], default="ok")
    parser.add_argument("--pcp-mode", choices=["ok", "refuse", "silent"], default="ok")
    parser.add_argument("--log-file", type=Path, default=None)
    options = parser.parse_args()
    if options.log_file:
        options.log_file.parent.mkdir(parents=True, exist_ok=True)
        _LOG_FILE = options.log_file.open("w", encoding="utf-8")

    IgdHandler.options = options
    udp = UdpResponder(options)
    udp.start()
    server = ThreadingHTTPServer((options.bind, options.igd_port), IgdHandler)
    httpd = threading.Thread(target=server.serve_forever, daemon=True)
    httpd.start()
    log(f"igd http listening on {options.bind}:{options.igd_port} external_ip={options.external_ip}")

    try:
        while True:
            time.sleep(0.5)
    except KeyboardInterrupt:
        pass
    finally:
        server.shutdown()
        log(f"summary: mappings still held {_MAPPINGS}; events={len(_EVENTS)}")
        if _LOG_FILE is not None:
            _LOG_FILE.close()
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
