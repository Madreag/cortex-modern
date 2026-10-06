"""A local TLS session directory retained beside a video capture."""

from contextlib import contextmanager
from datetime import datetime, timedelta, timezone
import hashlib
import ipaddress
import json
import queue
import socket
from pathlib import Path
import ssl
import threading
import time
import urllib.request
from urllib.parse import urlsplit, parse_qs

from cryptography import x509
from cryptography.hazmat.primitives import hashes, serialization
from cryptography.hazmat.primitives.asymmetric import rsa
from cryptography.x509.oid import NameOID

from session_directory.session_directory import LOGGER, spawn_server


class RelayUnavailable(RuntimeError):
    pass


def require_coturn(config, book=None):
    """Refuse a dead relay before a scene spends its start budget waiting for inputs."""
    if not config or config.get('backend') != 'coturn':
        return
    from relay_cloudflare_match import coturn_rest_login, turn_allocate
    deadline = time.monotonic() + 8
    if not isinstance(config.get('static_auth_secret'), str) or not config['static_auth_secret']:
        raise RelayUnavailable('Self-hosted relay has no authentication key. Configure it before running this scene.')
    for url in config.get('relay_urls') or []:
        try:
            parsed = urlsplit(url.replace('turn:', 'turn://', 1).replace('turns:', 'turns://', 1))
            mode = 'tls' if parsed.scheme == 'turns' else parse_qs(parsed.query).get('transport', ['udp'])[0]
            if parsed.scheme not in ('turn', 'turns') or mode not in ('udp', 'tcp', 'tls') or not parsed.hostname:
                continue
            host, port = parsed.hostname, parsed.port or (5349 if mode == 'tls' else 3478)
            found = queue.Queue()
            def resolve(host=host, port=port, found=found, mode=mode):
                try:
                    found.put(socket.getaddrinfo(host, port, 0, socket.SOCK_DGRAM if mode == 'udp' else socket.SOCK_STREAM))
                except OSError:
                    found.put([])
            threading.Thread(target=resolve, daemon=True).start()
            remaining = deadline - time.monotonic()
            if remaining <= 0:
                break
            addresses = found.get(timeout=min(2, remaining))
            user, password = coturn_rest_login(config['static_auth_secret'], 60)
            if book is not None:
                book.add('relay-preflight-username', user)
                book.add('relay-preflight-credential', password)
            for address in addresses:
                remaining = deadline - time.monotonic()
                if remaining <= 0:
                    break
                try:
                    receipt = turn_allocate((address[4][0], port), user, password, timeout=min(2, remaining / 2),
                                            transport=mode, tls_hostname=host)
                except (OSError, ValueError):
                    continue
                if receipt.get('result') == 'allocated':
                    return
        except (OSError, ValueError, KeyError, queue.Empty):
            continue
    raise RelayUnavailable('Self-hosted relay is unavailable or refuses a connection. Start it before running this scene.')


@contextmanager
def serve(root, port, block=(49400, 49479), turn_config=None, secret_book=None):
    """The directory on the given port, which must lie in the video driver's block (its default or a lane's own)."""
    if not block[0] <= port <= block[1]:
        raise ValueError(f"the directory must stay in the video driver's port block {block[0]}-{block[1]}")
    require_coturn(turn_config, secret_book)
    root = Path(root)
    root.mkdir()
    key = rsa.generate_private_key(public_exponent=65537, key_size=2048)
    name = x509.Name([x509.NameAttribute(NameOID.COMMON_NAME, "localhost")])
    now = datetime.now(timezone.utc)
    certificate = (x509.CertificateBuilder().subject_name(name).issuer_name(name).public_key(key.public_key())
                   .serial_number(x509.random_serial_number()).not_valid_before(now - timedelta(minutes=1))
                   .not_valid_after(now + timedelta(days=1)).add_extension(x509.SubjectAlternativeName([
                       x509.DNSName("localhost"), x509.IPAddress(ipaddress.ip_address("127.0.0.1"))]), critical=False)
                   .sign(key, hashes.SHA256()))
    cert, key_path = root / "cert.pem", root / "key.pem"
    cert.write_bytes(certificate.public_bytes(serialization.Encoding.PEM))
    key_path.write_bytes(key.private_bytes(serialization.Encoding.PEM, serialization.PrivateFormat.PKCS8,
                                          serialization.NoEncryption()))
    pin = hashlib.sha256(certificate.public_bytes(serialization.Encoding.DER)).hexdigest()
    previous_handlers = set(LOGGER.handlers)
    server = spawn_server(port=port, cert=cert, key=key_path, insecure_http=False, expiry_s=30,
                          heartbeat_s=2, log_file=root / "service.log", turn_config=turn_config, create_owner_key=True, caller_mode="direct")
    if secret_book is not None:
        provider = server.store.turn_provider
        mint = provider.mint
        def observed_mint(*args, **kwargs):
            offer = mint(*args, **kwargs)
            secret_book.add_offer(offer)
            return offer
        provider.mint = observed_mint
    stopped = threading.Event()
    context = ssl.create_default_context(cafile=str(cert))
    rows_path = root / "listings.jsonl"

    def observe():
        with rows_path.open("w", encoding="utf-8") as output:
            while not stopped.is_set():
                try:
                    request = urllib.request.Request(f"https://127.0.0.1:{port}/v1/sessions",
                                                     headers={"X-Install-Key": "e2evideo-local-review"})
                    with urllib.request.urlopen(request, context=context, timeout=2) as reply:
                        rows = json.load(reply)
                    output.write(json.dumps({"wall_ms": time.monotonic_ns() // 1_000_000, "listing": rows}) + "\n")
                    output.flush()
                    if rows.get("sessions"):
                        (root / "listed.json").write_text(json.dumps(rows, indent=2) + "\n", encoding="utf-8")
                except Exception as error:
                    output.write(json.dumps({"error": str(error)}) + "\n")
                    output.flush()
                stopped.wait(.5)

    observer = threading.Thread(target=observe, daemon=True)
    observer.start()
    try:
        yield {"DIRECTORY_URL": f"127.0.0.1:{port}", "DIRECTORY_PIN": pin, "DIRECTORY_ROOT": root}
    finally:
        stopped.set()
        observer.join(timeout=3)
        server.stop()
        for handler in set(LOGGER.handlers) - previous_handlers:
            LOGGER.removeHandler(handler)
            handler.close()
