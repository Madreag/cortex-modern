# Direct connections and TURN relays

Public STUN is enabled by default. Host Options > Network > Connection controls what the match offers. Settings > Network > Connection controls this player's route and optional private relay. The two preferences are independent.

## Cloudflare Realtime TURN

Create a TURN key in the Cloudflare dashboard, then put its key ID and credential-generation API token in a JSON file readable only by the session directory's account. Add `--turn-config /absolute/path/turn-config.json` to the existing directory launchd job's `ProgramArguments`, keeping its current TLS certificate, listener and other arguments. Restart that service during the deployment window. No deployment is performed by this change. [Cloudflare credential setup](https://developers.cloudflare.com/realtime/turn/generate-credentials/).

```json
{
  "backend": "cloudflare",
  "turn_key_id": "REPLACE_WITH_TURN_KEY_ID",
  "api_token": "REPLACE_WITH_CREDENTIAL_GENERATION_TOKEN"
}
```

The directory uses this server-side request. The engine receives only the resulting temporary login, never this authorization token.

```http
POST https://rtc.live.cloudflare.com/v1/turn/keys/<id>/credentials/generate-ice-servers
Authorization: Bearer <directory-only API token>
Content-Type: application/json

{"ttl":3600}
```

Cloudflare charges $0.05/GB after the first 1,000 GB/month free across Realtime SFU and TURN; STUN is free. Anycast routes clients to the nearest Cloudflare location, so there is no region selector. Its network exceeds the 310-city baseline. [Pricing and free STUN](https://developers.cloudflare.com/realtime/turn/faq/), [TURN anycast and ports](https://developers.cloudflare.com/realtime/turn/), [network footprint](https://www.cloudflare.com/network/).

The directory defaults to the `cloudflare` backend. Without a configured key it returns `relay_not_configured`; Automatic still tries direct STUN, and the host's hint reports the missing relay. Selecting Directory does not invent a Cloudflare account or credentials.

## Directory contract

The host first registers through the existing listing API. Its relay request uses both the same `X-Install-Key` identity and the session token returned by registration. All production requests use HTTPS and the existing certificate validation/pin mechanism.

```http
POST /v1/sessions/<directory-session-id>/ice-servers
X-Install-Key: <same identity used to register>
Content-Type: application/json

{"token":"<host session token>","match_id":"<match-id>:<round-id>","ttl":3600}
```

`match_id` is 1-128 ASCII letters, digits or `_.:-`; the engine uses the unique directory session ID followed by the round ID. `ttl` is an integer from 300 through 86400 seconds. A valid host identity can make four mint requests per minute. A different install identity or incorrect session token receives 403; malformed input receives 400; rate limiting receives 429 with `retry_after_s`. Missing sessions return 404. An unavailable provider returns a sanitized 502/503 error without credentials, the token or the provider's response text. The existing token-authenticated listing heartbeat transfers the install identity when an authorized successor takes over hosting.

Success is HTTP 200, `Cache-Control: no-store`. The `iceServers` member has Cloudflare's ICE-server-list shape; the directory adds the match binding and an absolute Unix-seconds expiry:

```json
{
  "match_id": "12345:1",
  "expires_at": 1789866000,
  "iceServers": [
    {"urls": ["stun:stun.cloudflare.com:3478"]},
    {
      "urls": [
        "turn:turn.cloudflare.com:3478?transport=udp",
        "turn:turn.cloudflare.com:3478?transport=tcp",
        "turn:turn.cloudflare.com:80?transport=tcp",
        "turns:turn.cloudflare.com:5349?transport=tcp",
        "turns:turn.cloudflare.com:443?transport=tcp"
      ],
      "username": "<temporary username>",
      "credential": "<temporary password>"
    }
  ]
}
```

The example expiry is illustrative. The directory computes the actual expiry conservatively from request time plus TTL. It checks expiry again before publishing. The list is bounded to eight entries and eight URLs per entry; unknown fields are refused. Neither the config struct nor its encoder has a signing-secret or API-token field.

An authenticated listing client can `GET` the same path to retrieve the current, unexpired offer. This bootstrap is necessary before a peer can establish ICE and receive the agreed lobby config. An unlisted game requires its known session ID. Expired or absent offers return 404. The host publishes the same offer in the lobby config, and joiners adopt that config after connecting. Directory listing rows themselves carry no login.

Fixed host credentials use the same POST with an additional `iceServers` member. That publishes the supplied private relay login instead of invoking a backend. The directory's expiry limits this publication; it cannot revoke a permanent account on someone else's TURN server. A host choosing Fixed deliberately shares that account with match participants. A player's own private relay is local and is never published to the lobby.

The engine requests a one-hour offer, requests another five minutes before expiry, and requests fresh credentials for rematch and repair. Failed requests retry no faster than every 15 seconds. Old offers remain usable only until their own expiry. Tokens and credentials are omitted from directory request logs and diagnostics; password text boxes and menu dumps are masked.

## Self-hosted coturn

For coturn, use this directory configuration instead. The same secret belongs in coturn's private config and the directory's private config. The host enters the directory address in Settings > Network > Internet and selects Directory in Host Options. The host never enters the signing secret in the game.

```json
{
  "backend": "coturn",
  "static_auth_secret": "REPLACE_WITH_A_RANDOM_SHARED_SECRET",
  "relay_urls": [
    "turn:relay.example.net:3478?transport=udp",
    "turn:relay.example.net:3478?transport=tcp",
    "turns:relay.example.net:5349?transport=tcp"
  ]
}
```

The directory computes `username = <expiry>:<random-match-tag>` and `credential = base64(HMAC-SHA1(secret, username))`. Minimal `turnserver.conf`:

```ini
listening-port=3478
tls-listening-port=5349
min-port=49160
max-port=49200
realm=relay.example.net
use-auth-secret
static-auth-secret=REPLACE_WITH_A_RANDOM_SHARED_SECRET
fingerprint
no-cli
no-multicast-peers
cert=/absolute/path/fullchain.pem
pkey=/absolute/path/privkey.pem
```

Use a public DNS name and a matching TLS certificate. If coturn is behind NAT, also set its advertised `external-ip=PUBLIC_IP/PRIVATE_IP` and forward its ports. Permit 3478 UDP/TCP, 5349 TCP, and 49160-49200 UDP through the server, cloud and router firewalls. The chosen small allocation range is for this measurement setup; size it for production capacity. [coturn configuration and authentication](https://github.com/coturn/coturn/blob/master/README.turnserver).

On a Debian/Ubuntu Linux VPS, install the distribution's coturn package, place the config at `/etc/turnserver.conf`, then enable/start the service:

```sh
sudo apt install coturn
sudo systemctl enable --now coturn
```

The VPS must have usable public connectivity and permit the relay allocation range. [coturn installation](https://github.com/coturn/coturn#installation).

For this PC, start with Ubuntu under WSL and the same package/config. WSL's default NAT adds another boundary; use mirrored networking where supported and explicit Windows/Hyper-V firewall rules for the ports above. The Mac must be able to reach the listener and allocated UDP ports. A TCP-only port proxy cannot prove UDP relay connectivity. [WSL networking](https://learn.microsoft.com/en-us/windows/wsl/networking).

Alternatively, use the Linux Docker engine with a read-only config mount:

```sh
docker run --name match-turn --network host \
  --mount type=bind,src=/absolute/path/turnserver.conf,dst=/etc/coturn/turnserver.conf,readonly \
  --mount type=bind,src=/absolute/path/certificates,dst=/certificates,readonly \
  coturn/coturn:4.18.0-r0
```

Adjust certificate paths in the mounted config. Host networking here means the Linux host; Docker Desktop/WSL networking and Windows firewall still need their own configuration. [Official coturn image](https://github.com/coturn/coturn/blob/master/docker/coturn/README.md).

On the Mac, `brew install coturn`, place the same config at `$(brew --prefix)/etc/turnserver.conf`, and use `brew services start coturn`. Its inbound ports and advertised public address follow the same rules. [Homebrew coturn](https://formulae.brew.sh/formula/coturn).

A private relay using permanent accounts instead of shared-secret authentication is the Fixed alternative. Enter its `host:port` or TURN URL, username and password in Host Options > Network > Connection > Relay (TURN) > Fixed. A player may instead enter their own account in Settings > Network > Connection; a nonempty address overrides the host offer. Do not paste `static-auth-secret`, a TURN key or an API token into either login form.

## Player policy and current transport limits

| Connection | Candidate policy | Consequence |
| --- | --- | --- |
| Automatic | Host/server-reflexive candidates and available relay | Direct is preferred; relay adds the relay's round trip. |
| Direct only | No local relay credentials/candidates; a reported relay route is refused | Lowest latency when reachable; failure when direct routes are blocked. |
| Relay only | Relay candidates only; no IP listener/fallback | Every accepted route must be relayed; unavailable credentials or blocked UDP fail explicitly. |

Native ICE prioritizes host candidates at 126, server-reflexive at 100 and relayed at 0 in `CSteamNetworkingICESession::CalcCandidatePriority`, before shifting the type preference by 24 bits. This is source evidence, not a WAN measurement of this binary. [GNS native ICE source](https://github.com/ValveSoftware/GameNetworkingSockets/blob/master/src/steamnetworkingsockets/clientlib/steamnetworkingsockets_ice_client.cpp).

STUN settings contain comma-separated `host:port` values: `stun.l.google.com:19302,stun.cloudflare.com:3478,stun.nextcloud.com:443`. An empty list deliberately removes server-reflexive gathering. NAT Off uses LAN or a forwarded host UDP port. Relay Off only removes the host's offer; player policy is a separate choice.

The linked native GNS ICE implementation consumes UDP TURN endpoints. The directory and lobby preserve TCP/TLS URLs, but this engine currently selects only UDP entries and passes GNS parallel comma-separated address, username and password lists. TCP/TLS-only access is not implemented by this change.

Credential refresh currently updates the offer and future listener connections. Native GNS copies credentials into each live ICE session and exposes no supported hot-renewal entry point in the installed SDK. Existing relay allocations can therefore expire even after a fresh offer was published. The menu states this limitation and the active login's expiry; reconnecting obtains the fresh offer. Uninterrupted long-round renewal needs the matching GNS source/build and an allocation restart or renewal seam. Cloudflare explicitly disconnects expired allocations. [Cloudflare allocation expiry](https://developers.cloudflare.com/realtime/turn/faq/).

These two transport limits remain unfinished. The menu must not imply that selecting Relay only solves a network that blocks UDP, or that publishing a new credential renews a running allocation. IP retry remains the last resort after ICE for Automatic and Direct only; Relay only refuses that downgrade.

Ordinary match config is version 6; persistent-world config is version 7. Versions 2-5 remain readable as recordings. The relay JSON suffix is appended after migration data; `NetLobbyProtocol` owns that encoder/decoder. Relay metadata is outside the deterministic simulation hash so rotating a login cannot alter simulation identity. Transport authorization comes from the host connection. Lua names and behavior are unchanged.

## Phase 3b WAN measurement plan

Do not execute this plan until the completion pass is authorized. First close the TCP/TLS and live-renewal gaps above, then compile the same revisions on both peers.

1. Use this PC as host and the Mac as the second peer. Begin with coturn in WSL and its directory HMAC backend. Record firewall rules, public/advertised addresses and allocation ports. A Mac on the same home LAN is a LAN baseline; use a genuinely separate WAN connection for the WAN rows.
2. Capture the host Network routing page and player Connection page at 640x360, 960x540 and 1280x720. Exercise every state, custom STUN list, empty list, Fixed credentials, masking, save/reopen, and personal-relay precedence.
3. Measure Automatic and Direct only with a direct route, then block the host's direct UDP path while leaving the relay reachable. Automatic must connect by relay; Direct only must refuse; Relay only must report a relay route and never use IP. Record GNS route flags, RTT distribution, loss, throughput, connection time and gameplay stall data beside video.
4. Repeat with the Cloudflare backend. Do not select a city; record the measured path and RTT. Exercise provider refusal and expiry, both peers' adopted offer, rematch, repair, late join and at least two credential rotations in one round. No signing secret or token may occur in captures, logs, config wire or diagnostics.
5. Once the SDK supports TCP/TLS, block relay UDP while preserving TCP 3478 and TLS 5349/443. Prove each fallback and uninterrupted renewal separately. An updated JSON offer alone is insufficient evidence.

All detecting rows are written; they have not been run.
