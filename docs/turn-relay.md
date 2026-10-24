# Direct connections and TURN relays

Public STUN is enabled by default. Host Options > Network > Connection controls what the match offers. Settings > Network > Connection controls this player's route and optional private relay. The two preferences are independent.

## Cloudflare Realtime TURN

Create a TURN key in the Cloudflare dashboard, then put its key ID and credential-generation API token in a JSON file readable only by the session directory's account. Pass `--turn-config /absolute/path/turn-config.json` to the session directory (`tools/session_directory/session_directory.py`), keeping its TLS certificate, listener and other arguments, and restart it. [Cloudflare credential setup](https://developers.cloudflare.com/realtime/turn/generate-credentials/).

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

A listing client can `GET` the same path with `X-Install-Key` to receive its own login for its first dial. The directory mints it per request from its own backend, lasting ten minutes, under the same four-per-minute limit per install identity (a 429 names `"budget": "relay"`). It never returns the host's offer, a seat's login or a Fixed account: the install key is a rate-limit identity, not a secret, and anyone who reads the list knows the session ID. It lets a Relay only peer gather candidates before receiving the agreed lobby config; once seated, the player's check-ins renew its seat login. An unlisted game requires its known session ID. A match without a relay returns 404, and a directory without a relay backend returns 503 `relay_not_configured`. The host publishes its offer in the lobby config, and joiners adopt that config after admission. Directory listing rows themselves carry no login.

Fixed host credentials use the same POST with an additional `iceServers` member. That publishes the supplied private relay login instead of invoking a backend. The directory's expiry limits this publication; it cannot revoke a permanent account on someone else's TURN server. A host choosing Fixed deliberately shares that account with match participants. A player's own private relay is local and is never published to the lobby.

The engine requests the directory's longest offer (86400 s), requests another once half its lifetime has passed, and requests fresh credentials for rematch and repair. Failed requests retry no faster than every 15 seconds. Old offers remain usable only until their own expiry. Tokens and credentials are omitted from directory request logs and diagnostics; password text boxes and menu dumps are masked.

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

Use a public DNS name and a matching TLS certificate. If coturn is behind NAT, also set its advertised `external-ip=PUBLIC_IP/PRIVATE_IP` and forward its ports. Permit 3478 UDP/TCP, 5349 TCP, and 49160-49200 UDP through the server, cloud and router firewalls. The small allocation range above suits a test relay; size it for the number of players it serves. [coturn configuration and authentication](https://github.com/coturn/coturn/blob/master/README.turnserver).

On a Debian/Ubuntu Linux VPS, install the distribution's coturn package, place the config at `/etc/turnserver.conf`, then enable/start the service:

```sh
sudo apt install coturn
sudo systemctl enable --now coturn
```

The VPS must have usable public connectivity and permit the relay allocation range. [coturn installation](https://github.com/coturn/coturn#installation).

On Windows, Ubuntu under WSL with the same package and config works. WSL's default NAT adds another boundary; use mirrored networking where supported and open the ports above in the Windows and Hyper-V firewalls. Every peer must be able to reach the listener and the allocated UDP ports. A TCP-only port proxy cannot prove UDP relay connectivity. [WSL networking](https://learn.microsoft.com/en-us/windows/wsl/networking).

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

Existing nonempty TURN settings without the new mode setting load as Fixed. Existing parallel comma-separated username/password lists retain their server mapping; one login can also apply to every listed URL. The `-net-turn` override applies for the current run and does not overwrite the menu's saved relay mode.

## Player policy and current transport limits

| Connection | Candidate policy | Consequence |
| --- | --- | --- |
| Automatic | Host/server-reflexive candidates and available relay | Direct is preferred; relay adds the relay's round trip. |
| Direct only | No local relay credentials/candidates; a reported relay route is refused | Lowest latency when reachable; failure when direct routes are blocked. |
| Relay only | Relay candidates only; no IP listener/fallback | Every accepted route must be relayed; unavailable credentials or blocked TURN endpoints fail explicitly. |

Native ICE prioritizes host candidates at 126, server-reflexive at 100 and relayed at 0 in `ICESessionInterface::NotifyLocalCandidateDiscovered` (lines 2664-2670 of the inspected source), before shifting the type preference by 24 bits. This is source evidence, not a WAN measurement of this binary. [GNS native ICE source](https://github.com/ValveSoftware/GameNetworkingSockets/blob/master/src/steamnetworkingsockets/clientlib/steamnetworkingsockets_ice_client.cpp#L2664).

The ICE checklist groups pairs by both local and remote foundations, so an unreachable local adapter cannot hold up checks from a usable adapter or relay allocation. Its check pacing and connection deadline remain unchanged.

The local candidate policy also applies when native ICE creates or accepts candidate pairs and receives peer data. A Relay only host's base socket carries TURN control traffic, but cannot become a direct peer route. Automatic joiners can therefore use that host's relay without adopting the host's player setting. When signaling identifies an already selected peer-reflexive candidate as a relay, its route type and pair priority are updated.

STUN settings contain comma-separated `host:port` values: `stun.l.google.com:19302,stun.cloudflare.com:3478,stun.nextcloud.com:443`. An empty list deliberately removes server-reflexive gathering. NAT Off uses LAN or a forwarded host UDP port. Relay Off only removes the host's offer; player policy is a separate choice.

The native GNS ICE implementation consumes UDP, TCP and TLS TURN endpoints. The engine preserves each URI's transport and its matching temporary login. UDP discovery and TCP/TLS connections run in parallel, so blocked UDP does not hold up a stream relay. TLS verifies the server name and certificate using the system trust roots. Client-to-relay TCP/TLS still uses a UDP allocation between the relay and peer, as specified by [RFC 8656](https://www.rfc-editor.org/rfc/rfc8656.html).

A relayed connection lives as long as the match because the engine links GameNetworkingSockets built with `external/patches/gns-turn-lifetime.patch` (below). Stock GNS 1.6.0 sends CreatePermission once, so the relay drops the peer when that permission's 300 s run out, and it counts an error answer to Refresh as success. The patched library requests and renews each peer permission independently every 240 s. A forbidden or unreachable address cannot block other candidates. It resends a Refresh, CreatePermission or Allocate refused for a stale nonce with the new one, re-allocates when the server holds no allocation (437) or the allocation lapsed, and reports and retries any other refusal before the allocation lapses. A renewed offer reaches the live connections: `UpdateListenerIceServers` sets the new TURN lists on each live P2P connection (the log shows `[net-relay] relay login renewed on N live connection(s)`), every later allocation uses the new login, a relay that failed is tried again with it, and a live allocation tries it on a Refresh at once. A server that holds an allocation to the username that created it (RFC 5766 section 4; coturn answers 441) keeps the allocation on its first login, which coturn goes on accepting; Cloudflare takes a renewed credential on the allocation, which it requires past a credential's 48-hour maximum. [Cloudflare allocation expiry](https://developers.cloudflare.com/realtime/turn/faq/).

If an allocation disappears, ICE removes the pairs that used it and gathers a replacement. Authenticated connectivity checks also verify the selected path every five seconds. A failed check releases that selection and rechecks available alternatives, including a replacement with equal priority. The original connection deadline and the match's rejoin path still apply if no usable route returns.

TLS endpoints on port 443 provide a relay option on networks that block UDP. The network must still permit the directory's HTTPS connection and at least one configured TURN endpoint. IP retry remains the last resort after ICE for Automatic and Direct only; Relay only refuses that downgrade.

## Seat identity and route recovery

Network protocol 6 and admission protocol 2 carry a short-lived signed seat token. It binds the match, stable seat, holder generation and participant key, with no address. Internet admission asks the directory to sign the host's reserved seat. Typed-address and LAN admission use a separate match signing key on the host; that key travels only inside the existing sealed migration state. A player's private identity key stays on that device.

`NetConnectionAuthority` owns the current route, check-in and credential renewal for admission and recovery. Every Internet return first proves the saved seat to the directory and obtains the current host and fresh relay credentials. The same reconnect schedule waits through directory outages until Cancel, a host-ended match or a terminal admission refusal. It does not reuse cached Internet endpoint lists. The existing roster, AI hold, election timing and private catch-up still decide when a returning player resumes.

Interface changes, wake gaps and native STUN mapping changes advance the route generation. Native ICE re-enumerates interfaces every two seconds and republishes a changed mapping. The authority sends that route at its next check-in and applies renewed TURN credentials to live ICE connections. If the transport cannot recover, the same signed seat drives a new ICE connection. Route changes never alter the simulation configuration or election budgets.

Local listeners start before Internet publication. Typed-address and LAN joins and returns require no directory. A running direct host continues its LAN beacon with a match identity and host generation; a returning seat can discover its changed address, then authenticate it through admission. Beacons supply route hints and never grant a seat.

## GameNetworkingSockets build

Lifetime version 4 preserves an existing STUN request when a triggered connectivity check is queued twice, removes every duplicate queue entry when deleting a candidate pair, and cancels all pending peer requests before deleting the session interfaces. Each request is a scheduled thinker, so replacing its pointer could leave it running after its interface was freed. The `test_p2p --ice-request-lifetime-selftest` regression closes 256 sessions with duplicate checks pending and requires the original transaction to remain unchanged. Run it with an ASan library build to verify teardown.

Lifetime version 5 retires an old allocation only once the peer has stopped sending through it. Each end selects its own route, and the controlled end may decline a nomination onto an allocation it is retiring, so the peer can still send to an allocation this end's route has left. A renewed login therefore keeps every old allocation until it has been retiring for 15 s and the peer has sent nothing through it for 15 s, three of the peer's 5 s checks of its selected route. The end that is retiring an allocation offers its candidate again at the lowest priority. On its next check pass the controlling end nominates the highest checked route that now outranks it. The `test_p2p --turn-rotation-selftest` regression keeps an old allocation while the host still sends to it, retires it 15 s after the host's last packet, and checks the lowest-priority offer and the controlling end's nomination.

Lifetime version 6 paces relay attempts the server never answered. A TCP or TLS stream that is refused or closed, or an Allocate whose send fails at once (an interface with no route to the relay), no longer moves to the next server inside the same pass: each interface waits its own backoff, 250 ms doubling to 5 s and reset by a successful allocation, and the discovery tick then tries the next server of that transport. A network that refuses UDP to the relay leaves only those streams, and walking every refused server of every interface under the library's lock in one pass held that lock for tens to hundreds of milliseconds, stalling every call the game makes into the library. A new login does not cut that wait short; the interface tries the new servers when it ends. Only a relay that carried the peer's traffic fails over at once, at most once a second per interface. The log names the interface: `ICE: TURN TCP stream closed on <address> for an IPv4 relay; next relay candidate in <ms> ms`.

The engine builds against GameNetworkingSockets v1.6.0 (upstream commit `2cb93a06350bb065db53abdb0d87cf297e0bfd34`) with `external/patches/gns-turn-lifetime.patch` and then `external/patches/gns-transport-diagnostics.patch` applied. The latter defines `STEAMNETWORKINGSOCKETS_TRANSPORT_DIAGNOSTICS`, also required by the engine, and exposes actual end-to-end packet send and receive timestamps, including keepalives. The patch defines `STEAMNETWORKINGSOCKETS_TURN_LIFETIME`, `STEAMNETWORKINGSOCKETS_ICE_CANDIDATE_POLICY`, `STEAMNETWORKINGSOCKETS_TURN_STREAMS` and `STEAMNETWORKINGSOCKETS_NETWORK_RECOVERY` in `steamnetworkingtypes.h`; `GnsTransport.cpp` requires all four, with lifetime at version 6 and candidate policy and streams at version 2, so a library built with an older patch must be rebuilt. IPv4 and IPv6 relay allocations are gathered independently over each allowed transport; an IPv6 allocation explicitly requests its address family, and an unavailable family does not replace the other allocation. This also permits peers using different local address families to meet on one relay family. [TURN address-family requests](https://www.rfc-editor.org/rfc/rfc8656.html#section-18.6). Build with OpenSSL, including its SSL library; macOS also links the Security and CoreFoundation frameworks for system trust roots. The patched library installs beside the stock prefix, as `<stock prefix>-turnfix`: `RTEA.vcxproj` and `meson.build` use that sibling whenever it exists, so `GNS_ROOT` / `-Dgns_root` keep naming the stock prefix and the dependency prefix does not change:

`external/patches/build_gns_turn.py` applies both patches in that order and records their SHA-256 hashes, the library hash, lifetime version 6, and transport diagnostics version 1 in `gns-build.json`. The native library and its headers must come from that same build.

- `GNS_ROOT` names the stock install prefix; the build uses `<GNS_ROOT>-turnfix` beside it
- `GNS_DEP_ROOT` names the vcpkg dependency prefix (the Windows builds use protobuf 6.33.4, abseil 20260107.1, OpenSSL 3.6.2)

Pointing `GNS_ROOT` at the `-turnfix` prefix directly works as well. Rebuild on Windows (VS 2026 CMake; the dependency prefix is only read):

```sh
git clone https://github.com/ValveSoftware/GameNetworkingSockets.git gns-src
git -C gns-src checkout v1.6.0
git -C gns-src apply <repo>/external/patches/gns-turn-lifetime.patch
git -C gns-src apply <repo>/external/patches/gns-transport-diagnostics.patch
cmake -S gns-src -B gns-build-turnfix -G "Visual Studio 18 2026" -A x64 \
  -DCMAKE_BUILD_TYPE=Release -DBUILD_SHARED_LIB=OFF -DBUILD_STATIC_LIB=ON -DBUILD_TESTS=OFF -DBUILD_EXAMPLES=OFF \
  -DBUILD_TOOLS=OFF -DENABLE_ICE=ON -DUSE_CRYPTO=OpenSSL -DUSE_STEAMWEBRTC=OFF -DLTO=OFF \
  -DCMAKE_PREFIX_PATH=<GNS_DEP_ROOT> -DProtobuf_PROTOC_EXECUTABLE=<GNS_DEP_ROOT>/tools/protobuf/protoc.exe \
  -DOPENSSL_ROOT_DIR=<GNS_DEP_ROOT> -DCMAKE_INSTALL_PREFIX=<GNS_ROOT>-turnfix
cmake --build gns-build-turnfix --config Release
cmake --install gns-build-turnfix --config Release
```

On macOS and Linux build the same checkout and both patches with the flags the stock prefix used, install into `<stock prefix>-turnfix`, and keep passing the stock prefix as `-Dgns_root`.

A UBSan build (`-Db_sanitize=` naming `undefined`) links `<prefix>-ubsan` beside the prefix it resolved, when that exists: the same checkout and patch configured with `-DSANITIZE_UNDEFINED=ON`, which keeps RTTI and instruments GNS itself. GNS release builds use `-fno-rtti`, so against them UBSan's vptr check reports every call the engine makes into `ISteamNetworkingSockets` and `ISteamNetworkingUtils`, and meson warns. On the Mac, for the libc++ prefix (`D` the dependency root):

```sh
cmake -S gns-src -B build-gns-ubsan -G Ninja -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_C_COMPILER=/usr/bin/clang -DCMAKE_CXX_COMPILER=/usr/bin/clang++ -DCMAKE_CXX_FLAGS=-stdlib=libc++ \
  -DCMAKE_EXE_LINKER_FLAGS=-stdlib=libc++ -DCMAKE_SHARED_LINKER_FLAGS=-stdlib=libc++ \
  -DBUILD_STATIC_LIB=ON -DBUILD_SHARED_LIB=OFF -DBUILD_EXAMPLES=OFF -DBUILD_TESTS=OFF -DBUILD_TOOLS=OFF \
  -DENABLE_ICE=ON -DUSE_STEAMWEBRTC=OFF -DUSE_CRYPTO=OpenSSL -DOPENSSL_ROOT_DIR=/opt/homebrew/opt/openssl@3 \
  -DProtobuf_DIR=$D/protobuf-libcxx/lib/cmake/protobuf -DProtobuf_PROTOC_EXECUTABLE=$D/protobuf-libcxx/bin/protoc \
  -DSANITIZE_UNDEFINED=ON -DCMAKE_INSTALL_PREFIX=$D/gns-libcxx-turnfix-ubsan
ninja -C build-gns-ubsan install
```

`tools/turn_relay_checks.py hold|renew --turn <host:port> --out <dir>` runs the two relay checks (`-net-p2p-selftest relay-hold <seconds> <server>` and `relay-renew <server>`) through the runner against a real TURN server, with the login from `CC_TEST_TURN_USER` / `CC_TEST_TURN_PASS` or a coturn `user=` line; `--coturn-log <ssh host>:<log>` adds the server's own log lines for the run.

`-net-p2p-selftest relay-automatic <seconds> <server>` exercises the same exchange with an Automatic joiner and a Relay only host. It requires a measured relay route at both ends and checks that messages and the close cross it. This single-process regression does not replace joins between machines and platforms.

An ordinary match config is version 8 and a persistent world's is version 9; a live session refuses an older config, and versions back to 2 still decode for recordings. The relay JSON suffix is appended after migration data; `NetLobbyProtocol` owns that encoder/decoder. Relay metadata is outside the deterministic simulation hash so rotating a login cannot alter simulation identity. Transport authorization comes from the host connection. Lua names and behavior are unchanged.

Transport health records (`[net-transport-health]`) sample each live peer every five seconds and record its packet times, application message handoff and receive times, send result, reliable queue, and route receipt. Every close records the reason, GNS end reason and detailed status before releasing the handle. `gns_clock_ms` and the packet timestamps share the GNS local clock; packet ages are local durations. Always-on TURN diagnostics record Allocate, Refresh and CreatePermission request times and their success, error code or timeout. Authentication fields remain scrubbed. `-net-p2p-selftest` checks actual packet timestamps after exchanging payloads and checks that an invalid handle returns none.
