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

The listener must be loopback (`--bind 127.0.0.1`, the default). `--caller-mode` is required: `direct` uses the socket address for local clients; `tunnel` requires the forwarding edge's address header. Both modes refuse a non-loopback listener. `--port` defaults to `0`, an ephemeral port printed at start; deployments pass `--port 8443`. TLS requires both `--cert` and `--key`. Plain HTTP requires `--insecure-http`. `--expiry-s` defaults to 15 and `--heartbeat-s` to 5. `--log-file` rotates at 5 × 5 MB.

## What the install key is

`X-Install-Key` is a rate-limit identity, not a secret. Anyone can mint a 16–32 character value (`A-Za-z0-9-_`). It is required on every `/v1/` request. Missing or malformed key: `400 {"error":"invalid_install_key"}`. The header is not a capability: it does not authorize listing, joining, or signaling.

Two limiters apply on every `/v1/` request; the stricter wins (`429 {"error":"rate_limited","retry_after_s":...}`):

- Per install key: 10 registers/min, 120 requests/min.
- Per source IP: 30 registers/min, 300 requests/min.

Per session signal caps: at most 16 destination queues and at most 1 MiB of undrained decoded payload. A destination queue that is not drained for 120 s is dropped. The host queue is never dropped while the session lives. Over those caps: `400 {"error":"queue_full"}`. Per-queue cap remains 256 entries; per-signal decoded payload remains 64 KiB.

Idle rate-limit buckets (install key or address with no refill for 10 minutes) are dropped on every 256th check or when a limiter map exceeds 10 000 entries. A pruned key starts with a fresh budget.

## Listing sessions

`GET /v1/sessions` returns live rows ordered by `(created_at, session_id)` ascending.

- `limit` — page size, 1..200, default 100.
- `cursor` — opaque token from a previous `next_cursor` (base64 of `created_at:session_id`). A malformed cursor is `400 {"error":"invalid_field","field":"cursor"}`.
- The reply always includes `total` (live rows matching the filters). When more rows remain it also includes `next_cursor`.

Optional filters `mode`, `activity`, and `state` still apply before the page is cut.

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
openssl req -x509 -newkey rsa:2048 -keyout key.pem -out cert.pem -days 365 -nodes -subj "/CN=cortex-directory" -addext "subjectAltName=IP:<lan-ip>"
```

Use `DNS:<name>` instead of `IP:<lan-ip>` when the directory is reached by name.

Pin the leaf (64 lowercase hex):

```bash
openssl x509 -in cert.pem -outform DER | openssl dgst -sha256
```

Every client sets `SessionDirectoryCertSha256 = <hex>` in Settings.ini for a self-signed directory. An unpinned client needs a certificate the system store trusts (Let's Encrypt with a public DNS name). Pinned mode does not consult the chain, the name or the dates.

4. Follow the key-creation steps below, configure the forwarding tunnel to the loopback listener, then load the daemon (starts at boot, survives logout, KeepAlive):

```bash
sudo launchctl bootstrap system /Library/LaunchDaemons/com.cortex.session-directory.plist
```

To unload later: `sudo launchctl bootout system/com.cortex.session-directory`.

5. The forwarding tunnel reaches TCP 8443 on loopback. This process accepts no public listener and binds no UDP port.

Logs: rotating file `/Users/erol/cortex-directory/logs/session-directory.log`, plus launchd stdout/stderr in the same folder.

## Windows alternative

Same `session_directory.py`, with a forwarding tunnel to `127.0.0.1:8443`. Create the permanent owner key once as described below, then create a Task Scheduler task at logon (hidden `pythonw` is fine):

```text
Program: pythonw.exe
Arguments: D:\path\to\session_directory.py --bind 127.0.0.1 --caller-mode tunnel --port 8443 --cert D:\path\to\cert.pem --key D:\path\to\key.pem --log-file D:\path\to\logs\session-directory.log --owner-state D:\path\to\world-owners.json
Start in: D:\path\to
```

The tunnel must overwrite `CF-Connecting-IP`. Clients use its public URL. The Python listener stays on loopback.

Self-signed certificate (same `openssl` command as above, including the SAN) unless a public DNS name exists for Let's Encrypt. Clients pin that certificate as in step 3.
# World ownership across restarts

Keep `/Users/erol/cortex-directory/world-owners.json` and
`/Users/erol/cortex-directory/world-owners.key` across deployments. The supplied
service arguments select the owner file; the key defaults beside it with `.key`
suffix. The key contains a private signing secret and a service-start era. Never
log or replace it. Owner records contain hashes and generations, never tokens.
Malformed existing state prevents startup.

The public service has no legacy worlds to convert. Upgrade the service **before
distributing a new game build**: a positive-generation world resume against the
old service receives 409 and reads as superseded.

1. Stop the old service. Recreate the project's disposable test worlds.
2. Start the new service on loopback behind the existing tunnel with `--insecure-http`, `--caller-mode tunnel`,
   `--owner-state /Users/erol/cortex-directory/world-owners.json`, and
   `--create-owner-key` once. The Cloudflare edge must overwrite
   `CF-Connecting-IP`; missing or invalid addresses receive 400. Direct deployments
   use `--caller-mode direct` and the socket address. No address grants privilege.
   Missing keys without the create flag refuse startup. Creation is atomic: an
   interrupted first start leaves a complete key or no key, so repeat this step.

   The public deployment's first-start command is:

   ```bash
   /usr/bin/python3 /Users/erol/cortex-directory/session_directory.py --bind 127.0.0.1 --port 8443 --caller-mode tunnel --insecure-http --log-file /Users/erol/cortex-directory/logs/session-directory.log --owner-state /Users/erol/cortex-directory/world-owners.json --create-owner-key
   ```
3. Remove the one-time create flag. Keep `world-owners.key` and `world-owners.json`
   permanently, back them up together, and preserve `world-owners.pending.json`
   through an in-progress restart. Verify register, heartbeat, list, signals and
   resume from old games; then distribute new builds. Pending metadata expires
   within its row's lease and contains no bearer proof.

Short HTTP requests have 64 handlers. Long polls use a separate pool of 64,
with four per caller; excess polls answer immediately with a one-second retry
hint. One caller retains at most 64 of 4,096 owners. Under pressure, worlds
listed for less than 60 seconds retire first; signed returning proofs remain
verifiable. These shares prevent one address from reserving the whole service.
Short requests have a three-second deadline from accept through body end,
including TLS and headers, a 128 KiB body cap, and four open short connections
per caller. The separate four-waiter share gives at most eight admitted
connections per caller. Excess or overdue body readers close; they cannot hold
all heartbeat handlers for a lease. The owner share also applies to a /24 for
IPv4 and a /48 for IPv6. Owners listed for at least 60 seconds are never retired
for capacity: a table of established owners refuses a new world with 503/full.
Retirement of a shorter listing and replacement of a returning lease wait for
their storage acknowledgement before discarding the previous owner or signals.

Signed opaque proofs stay within the existing token size. A returning holder can
prove its original ownership after restart; an older game's normal requests keep
working. Only heartbeat acknowledges a registration and creates its durable
owner. Owners expire after 30 days without one, with bounded, batched writes on a
separate worker. Pending registrations have global/source/byte caps and live no
longer than their own lease. Previous proofs recover an unacknowledged replacement
across host boot changes; a successful heartbeat retires them.
