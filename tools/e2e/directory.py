"""A local TLS session directory retained beside a video capture."""

from contextlib import contextmanager
from datetime import datetime, timedelta, timezone
import hashlib
import ipaddress
import json
from pathlib import Path
import ssl
import threading
import time
import urllib.request

from cryptography import x509
from cryptography.hazmat.primitives import hashes, serialization
from cryptography.hazmat.primitives.asymmetric import rsa
from cryptography.x509.oid import NameOID

from session_directory.session_directory import LOGGER, spawn_server


@contextmanager
def serve(root, port):
    if not 49400 <= port <= 49479:
        raise ValueError("the directory must stay in the video driver's port block")
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
                          heartbeat_s=2, log_file=root / "service.log")
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
