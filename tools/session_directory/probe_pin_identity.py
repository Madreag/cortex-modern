"""CN-mismatch pin identity: correct pin, wrong pin, unpinned."""

import argparse
import hashlib
import shutil
import subprocess
import sys
import time
from pathlib import Path

TOOLS = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(TOOLS))
from run_sim_test import make_run  # noqa: E402

SERVICE = Path(__file__).resolve().parent / "session_directory.py"


def openssl_bin():
    path = shutil.which("openssl")
    if path is None:
        raise SystemExit("openssl not on PATH")
    return path


def make_cn_mismatch_cert(cert: Path, key: Path) -> str:
    openssl = openssl_bin()
    subprocess.run(
        [
            openssl,
            "req",
            "-x509",
            "-newkey",
            "rsa:2048",
            "-keyout",
            str(key),
            "-out",
            str(cert),
            "-days",
            "1",
            "-nodes",
            "-subj",
            "/CN=cortex-directory",
        ],
        check=True,
        capture_output=True,
        text=True,
    )
    der = subprocess.check_output([openssl, "x509", "-in", str(cert), "-outform", "DER"])
    return hashlib.sha256(der).hexdigest()


def request_lines(text: str) -> list[str]:
    return [line for line in text.splitlines() if " \"" in line and "HTTP/1.1\"" in line]


def wait_listening(log: Path, deadline: float) -> None:
    while time.monotonic() < deadline:
        if log.exists() and "listening on" in log.read_text(encoding="utf-8", errors="replace"):
            return
        time.sleep(0.05)
    raise SystemExit(f"service did not listen; log={log}")


def probe(repo: Path, out: Path, url: str, pin: str, env: dict) -> tuple[int, str]:
    args = ["-net-directory-probe", url]
    if pin:
        args.append(pin)
    run = make_run(repo, args, out, 90, env=env)
    try:
        record = run.start().finish()
    finally:
        run.close()
    stdout = (out / "stdout.log").read_text(encoding="utf-8", errors="replace")
    return record["exit_code"], stdout


def flip_pin(pin: str) -> str:
    first = "0" if pin[0] != "0" else "1"
    return first + pin[1:]


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--repo", type=Path, required=True)
    parser.add_argument("--out", type=Path, required=True)
    parser.add_argument("--bind", default="127.0.0.1")
    parser.add_argument("--port", type=int, default=18447)
    parser.add_argument("--cert", type=Path)
    parser.add_argument("--key", type=Path)
    parser.add_argument("--service-log", type=Path)
    options = parser.parse_args()
    root = options.out.resolve()
    root.mkdir(parents=True, exist_ok=False)
    cert = options.cert.resolve() if options.cert else root / "cert.pem"
    key = options.key.resolve() if options.key else root / "key.pem"
    if options.cert:
        der = subprocess.check_output(
            [openssl_bin(), "x509", "-in", str(cert), "-outform", "DER"]
        )
        pin = hashlib.sha256(der).hexdigest()
    else:
        pin = make_cn_mismatch_cert(cert, key)
    (root / "cert-sha256.txt").write_text(pin + "\n", encoding="utf-8")
    service_log = (options.service_log or (root / "service.log")).resolve()
    url = f"https://{options.bind}:{options.port}"
    env = {"CCCP_HEADLESS": "1"}
    service = subprocess.Popen(
        [
            sys.executable,
            str(SERVICE),
            "--bind",
            options.bind,
            "--port",
            str(options.port),
            "--cert",
            str(cert),
            "--key",
            str(key),
            "--log-file",
            str(service_log),
        ]
    )
    try:
        wait_listening(service_log, time.monotonic() + 10)
        cases = (
            ("correct", pin, "[net-directory-probe] PASS", 0, True),
            ("wrong", flip_pin(pin), "certificate pin mismatch", 1, False),
            ("unpinned", "", "certificate verification failed", 1, False),
        )
        ok = True
        for name, case_pin, needle, expect_exit, expect_request in cases:
            before = service_log.read_text(encoding="utf-8", errors="replace")
            before_n = len(request_lines(before))
            exit_code, stdout = probe(options.repo, root / f"probe-{name}", url, case_pin, env)
            after = service_log.read_text(encoding="utf-8", errors="replace")
            new_requests = request_lines(after)[before_n:]
            hit = needle in stdout
            requests_ok = bool(new_requests) == expect_request
            passed = exit_code == expect_exit and hit and requests_ok
            ok = ok and passed
            print(f"CASE {name} pass={passed} exit={exit_code} requests={len(new_requests)}")
            for line in stdout.splitlines():
                if line.startswith("[net-directory-probe]"):
                    print(line)
            for line in new_requests:
                print(line)
        return 0 if ok else 1
    finally:
        service.terminate()
        try:
            service.wait(timeout=5)
        except subprocess.TimeoutExpired:
            service.kill()
            service.wait(timeout=5)


if __name__ == "__main__":
    raise SystemExit(main())
