# Session directory

In-memory JSON session list and signaling relay. One Python 3 process, standard library only. A restart empties the list.

Do not run these install steps from a worker lane. They are for the Mac (preferred) or this PC after the lead copies the files.

## Location

This service lives in `tools/session_directory/` of the repository. It is standard library only; the tests were run with Python 3.14.2.

Run the tests from the repository root:

```text
python tools/session_directory/test_session_directory.py -v
```

`com.cortex.session-directory.plist` is a macOS LaunchDaemon that refers to the install paths it names — working directory and files under `/Users/erol/cortex-directory` (`session_directory.py`, `cert.pem`, `key.pem`, `logs/session-directory.log`, `logs/stdout.log`, `logs/stderr.log`) and interpreter `/usr/bin/python3`. The copy in the repository is a template; those paths apply on the install host, not in the worktree.

## Files

- `session_directory.py` — service
- `test_session_directory.py` — loopback tests (`python test_session_directory.py -v`)
- `com.cortex.session-directory.plist` — macOS LaunchDaemon

Listen address and port: `--bind` and `--port` (default `8443`). TLS when both `--cert` and `--key` are set. Plain HTTP only with `--insecure-http` (tests and LAN trials). `--expiry-s` default 15. `--heartbeat-s` default 5 (also returned on register). `--log-file` is a rotating log (5 × 5 MB).

## What the install key is

`X-Install-Key` is a rate-limit identity, not a secret. Anyone can mint a 16–32 character value (`A-Za-z0-9-_`). It is required on every `/v1/` request. Missing or malformed key: `400 {"error":"invalid_install_key"}`. The header is not a capability: it does not authorize listing, joining, or signaling.

Two limiters apply on every `/v1/` request; the stricter wins (`429 {"error":"rate_limited","retry_after_s":...}`):

- Per install key: 10 registers/min, 120 requests/min.
- Per source IP: 30 registers/min, 300 requests/min.

Per session signal caps: at most 16 destination queues and at most 1 MiB of undrained decoded payload. A destination queue that is not drained for 120 s is dropped. The host queue is never dropped while the session lives. Over those caps: `400 {"error":"queue_full"}`. Per-queue cap remains 256 entries; per-signal decoded payload remains 64 KiB.

## Mac install

Working directory is `/Users/erol/cortex-directory`.

1. Create the directory and log folder:

```bash
mkdir -p /Users/erol/cortex-directory/logs
```

2. Copy `session_directory.py` into `/Users/erol/cortex-directory/` and copy `com.cortex.session-directory.plist` to `/Library/LaunchDaemons/com.cortex.session-directory.plist` (root-owned, mode `0644`).

3. Certificate. If the Mini has a public DNS name, use Let's Encrypt for that name and point `--cert` / `--key` at those files. If it is LAN-only or IP-only, self-signed is enough:

```bash
cd /Users/erol/cortex-directory
openssl req -x509 -newkey rsa:2048 -keyout key.pem -out cert.pem -days 365 -nodes -subj "/CN=cortex-directory"
```

Clients trust the system store or a user-supplied CA file. Do not pin a certificate hash in the engine.

4. Load the daemon (starts at boot, survives logout, KeepAlive):

```bash
sudo launchctl bootstrap system /Library/LaunchDaemons/com.cortex.session-directory.plist
```

To unload later: `sudo launchctl bootout system/com.cortex.session-directory`.

5. Firewall: allow inbound TCP 8443 (HTTPS and signaling). UDP 3478 is only needed if a separate STUN/TURN daemon is added later; this process does not bind it.

Logs: rotating file `/Users/erol/cortex-directory/logs/session-directory.log`, plus launchd stdout/stderr in the same folder.

## Windows alternative

Same `session_directory.py`. Bind `0.0.0.0:8443` with `--cert` and `--key`. Create a Task Scheduler task that runs at logon (hidden `pythonw` is fine):

```text
Program: pythonw.exe
Arguments: D:\path\to\session_directory.py --bind 0.0.0.0 --port 8443 --cert D:\path\to\cert.pem --key D:\path\to\key.pem --log-file D:\path\to\logs\session-directory.log
Start in: D:\path\to
```

Allow inbound TCP 8443 for that Python executable (elevated firewall rule). Clients set the directory URL to this PC. There is no launchd job and no `gui/501` requirement.

Self-signed certificate (same `openssl` command as above) unless a public DNS name exists for Let's Encrypt.
