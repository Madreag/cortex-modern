# Session directory

This service lists multiplayer sessions, carries ICE signalling, and signs Internet seat leases. Gameplay and the seat roster remain on the peers. With `--owner-state`, sessions, signalling queues, host generations, removals and retained seats survive a restart in an encrypted SQLite file beside the owner state. Keep that file and the matching service signing key together when backing up or moving the service.

For a local test, run:

```sh
python -m pip install -r tools/session_directory/requirements.txt
python tools/session_directory/session_directory.py --caller-mode direct --insecure-http --bind 127.0.0.1 --port 8080
```

Use `--cert` and `--key` for HTTPS. Select the caller mode for the deployment; `--help` describes direct and tunnel operation. Keep the owner signing key when restarting a persistent directory. Relay setup is described in [the relay guide](../../docs/turn-relay.md).

## Connection authority

Registration and connection requests carry `connection_protocol: 1`. A mismatch refuses with both versions and an update instruction. `GET /v1/sessions/{id}/connections`, with `X-Connection-Protocol: 1`, resolves the current host even while its public listing is hidden or temporarily expired. Its reply contains the public issuer key and current host generation, without seat or relay credentials.

`POST` on the same endpoint accepts three operations. `issue` and `remove` require the current host token and generation. The host selects the seat through its existing admission transaction; the directory signs that match, seat, holder generation and participant public key. `check-in` carries the seat token and a request signed by that participant's private key. The signed request includes a nonce, time, process instance and current route. A copied token cannot prove the participant key or displace its active instance.

Seat signatures expire after five minutes and renew at half their lifetime. Peers check in every five seconds. A changed route requests a fresh relay login under the same seat; an expired seat can renew after an outage only while the retained binding still belongs to the same participant. Active check-ins retain the match for another day. Removal and host-end records remain authoritative across restarts. An unreachable directory does not stop an established peer connection.

State mutations use full SQLite transactions and AES-GCM with a key derived separately from the service's Ed25519 signing key. The database is private to the service account. An unreadable record stops startup with a recovery instruction rather than silently discarding ownership. Never log request bodies, seat tokens, relay logins or the service key.

## Host change

`POST /v1/sessions/{id}/connections` also accepts `operation: "host-change"`.
It uses the same signed seat proof as a check-in; its signed `host_change` report
names the previous generation, round, configuration hash and that seat's applied
and prepared frame. Only fifteen continuous seconds without the authenticated
host permit replacement. Each live survivor reports its retained prefix. The
directory chooses the lowest live stable seat and one boundary covering those
prefixes, then persists the decision, generation and route atomically. Repeated
requests receive the same decision; only the chosen seat receives the new host
capability. The old capability cannot renew or replace that generation.

This endpoint changes only host authority. Gameplay phases, holds, returns and
match start/end remain host decisions. Direct games and a directory outage use
the engine's unanimous agreement among all surviving owners, with no solo or
majority promotion. An outage never ends an established match.

When the directory returns after a completed fallback, every surviving owner
independently signs the same successor, generation, membership and boundary in
its next report. The directory requires all remaining owners' matching reports
before recording that authority and issuing its host capability. This can
reconcile an earlier reservation whose answer reached no participant; a single
report cannot overwrite it, and an owner cannot attest two choices for one
generation. The new host continues the established match while reconciliation
and publication retry.

## What the install key is

The `X-Install-Key` header is a rate-limit identity, not a secret. Each install keeps its own 16–32 character key. Registration ownership and reconnect credentials are separate from this header.

Each install has a budget of 10 registers/min and 120 requests/min. Each source address has a budget of 30 registers/min and 480 requests/min, so four installs behind one router can each use their request budget.

A session holds at most 16 destination queues and 1 MiB of signalling payload. An unused destination queue expires after 120 s; the host queue is never dropped by destination queue eviction. Message, source and service-wide limits also bound signalling storage.

Listing requests accept a `limit` and an opaque `cursor`. The default limit is 100 and the maximum is 200. A response includes `total` and, when another page exists, `next_cursor`; pass that cursor unchanged to read the next page.
