# Session directory

This service lists multiplayer sessions and carries ICE signalling. It stores live listings in memory. Persistent world ownership uses the configured owner state and signing key.

For a local test, run:

```sh
python tools/session_directory/session_directory.py --caller-mode direct --insecure-http --bind 127.0.0.1 --port 8080
```

Use `--cert` and `--key` for HTTPS. Select the caller mode for the deployment; `--help` describes direct and tunnel operation. Keep the owner signing key when restarting a persistent directory. Relay setup is described in [the relay guide](../../docs/turn-relay.md).

## What the install key is

The `X-Install-Key` header is a rate-limit identity, not a secret. Each install keeps its own 16–32 character key. Registration ownership and reconnect credentials are separate from this header.

Each install has a budget of 10 registers/min and 120 requests/min. Each source address has a budget of 30 registers/min and 480 requests/min, so four installs behind one router can each use their request budget.

A session holds at most 16 destination queues and 1 MiB of signalling payload. An unused destination queue expires after 120 s; the host queue is never dropped by destination queue eviction. Message, source and service-wide limits also bound signalling storage.

Listing requests accept a `limit` and an opaque `cursor`. The default limit is 100 and the maximum is 200. A response includes `total` and, when another page exists, `next_cursor`; pass that cursor unchanged to read the next page.
