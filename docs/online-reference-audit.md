# Online reference audit

Which online play code Turtles in Time Re-Shelled takes from Dragon's Crown Pro PC (Dragon) and Super Street Fighter
II Turbo HD Remix Recompiled (SSF2T), from which version, and what is new for this game. Audit date 2026-10-31.

## How the audit was done

- Both reference repositories were read locally: working tree, uncommitted changes, stashes, every branch and
  `git log --all`. GitHub was not used; every online commit of SSF2T and the two newest of Dragon exist only
  locally.
- Every online commit was followed to its final form. Where a later commit or the working tree changed a file, the
  later version was taken.
- No reference repository was modified. Nothing was built, launched or deployed.

## Dragon's Crown Pro PC

State inspected: branch `fix/issue-32-16x10-water-rendering`, HEAD `13ca700`. Uncommitted changes are water rendering
and save data only; none of the six stashes touches online code.

| Commit | Title | Published | Reused here |
|---|---|---|---|
| b25b8b6 | feat: add online co-op support | origin/main | design ancestor of SSF2T's service (identity, friends, lobby link, STUN, UPnP) |
| 706202d | feat: polish online launcher experience | origin/main | through SSF2T's Online page |
| d321d83 | feat: enable production online co-op | origin/main | production service address pattern |
| 5b5410e | feat: polish online play and disable cheats | origin/main | policy only (below) |
| 6241f88 | feat: add online latency and ping display | origin/main | through SSF2T's Ping display |
| bc186bd | feat: add online platform and version compatibility | origin/main | through SSF2T (platform, version, build) |
| cd1d4e2 | feat: enable online play across supported builds | origin/main | not applicable (one image here) |
| 5d4ccc0 | feat: add private online telemetry dashboard | origin/main | **adapted** (Telemetry, below) |
| f83a55f | feat: bring Linux to online feature parity | origin/main | through SSF2T |
| d3ea4c6 | fix: finalize v0.9.0 release readiness | origin/main | **`ssl_mutex_` fix ported** (SSF2T lacked it) |
| 0b45583 | Add in-game options and online menus | local only | not ported: TMNT has no in-game options menu |
| 3d8efd8 | feat: highlight required online connection test | local only | same gate as SSF2T's, taken from SSF2T |
| a204bde | feat: enable global feature parity across supported builds | local only | build parity test only |

Dragon is a PS4 port: its game layer (Matching2, RUDP, NpToolkit2) has no counterpart here. It has no relay; two
symmetric NATs cannot play.

## SSF2T HD Remix

State inspected: branch `feature/pt-br-localization`, HEAD `fa78641`, with uncommitted pt-BR work. Every online commit
is local only (origin/main is v0.2.2).

| Commit | Title | Reused here |
|---|---|---|
| 4df062d | feat: add rollback netplay foundation | not applicable: rollback is SSF2T's gameplay sync; this game replicates state itself |
| 9f20b39 | feat: complete online rollback netplay with direct UDP and relay fallback | the whole online client, Worker, relay, Connection test, launcher page and the XSession/XNet/VDP bridge pattern |
| 44ad861 | fix: prevent launcher idle hang on Windows | launcher only, already compared in the cross-project audit |
| fa78641 | feat: complete Windows Linux and Title Update feature parity | WebSocket framing for Linux, wire vectors, TLS tests |
| working tree | pt-BR localization | **not ported**: online behaviour is unchanged; it only wraps text in `Tr()`, which this launcher does not have |

## Local-only changes found

- Dragon: 0b45583, 3d8efd8, a204bde (above); the `ssl_mutex_` fix is published (d3ea4c6) but missing from SSF2T.
- SSF2T: all online work (4df062d, 9f20b39, fa78641) and the uncommitted pt-BR text wrapping.
- SSF2T test bug found while porting: `tests/online_connection_tests.cpp` expected `Limited` with STUN off against a
  relay-capable service; the mature verdict is `Ready` (`connection_test.cpp`, relay stands in). Fixed here.

## Component map

| Component | Source (reference) | TMNT destination | Status |
|---|---|---|---|
| Online service client: sign-in, friends, presence, match advertise/search/join, reconnect, seat resume | SSF2T `src/online/online_service.*` (9f20b39) | `src/online/online_service.*` | reused |
| Protocol (JSON messages, errors, compatibility) | SSF2T `src/online/protocol.*` | `src/online/protocol.*` | adapted: game id `tmnt-reshelled-pc`, one image `base`, `kOnlineBuild` |
| Identity, instance lock, friend view | SSF2T `src/online/identity.*`, `instance.*`, `friend_view.*` | same | reused (TMNT salts) |
| WebSocket client (WinHTTP; sockets + run-time libssl on Linux), framing | SSF2T `src/online/websocket_*` (fa78641) | same | reused + Dragon `ssl_mutex_` fix |
| Lobby link (reader thread, keepalive 20 s, silence 45 s, relay send queue) | SSF2T `src/online/lobby_link.*` | same | reused |
| Rendezvous (punch), STUN, UPnP, net addresses, LAN candidates | SSF2T `src/online/{rendezvous,stun,upnp,net_addr,local_addresses}.*` | same | reused |
| Establishment budget (15 s, handshake 10 s) | SSF2T `src/online/establishment.h` | same | reused |
| Connection test | SSF2T `src/online/connection_test.*` | same | reused |
| Mandatory test gate, attention pulse, Play detector | SSF2T `src/connection_test_gate.h` (same semantics as Dragon 3d8efd8) | same | adapted: strict (Overall usable, configuration fingerprint) |
| Gameplay socket, relay frames, lobby mux, hold | SSF2T `src/netplay/{online_game_socket,relay_frame,lobby_mux,udp_transport}.*` | same | adapted: multi-peer relay, relay frame v2, per-slot mux |
| Online page, runtime, Ping display, corner overlay, friend code field, clipboard | SSF2T `src/online_page.*`, `online_runtime.*`, `ping_display.*`, `corner_overlay.h`, `friend_code_field.*`, `clipboard*` | same | adapted: no Infinite Time, Ping on F10, `StopOnline()` |
| XSession/XNet/VDP bridge | SSF2T `src/player_match*.{h,cpp}`, `player_match_net_hook.cpp` | `src/player_match*.{h,cpp}`, `src/player_match_hook.cpp`, `src/player_match_net_hook.cpp`, `src/online_bridge.*` | adapted to TMNT's wrappers; **new**: overlapped `WSASendTo`/`WSARecvFrom`/`WSAGetOverlappedResult` |
| Worker (Durable Object hub, relay, limits) | SSF2T `server/ssf2t-lobby` | `server/tmnt-lobby` | generalized to a directory plus one match room per party (protocol 4) |
| Telemetry and private dashboard | Dragon `server/lobby/src/{metrics,admin,admin_page}.js` | `src/online/telemetry.*`, `server/tmnt-lobby` | adapted, local only |
| Per-peer ping | - | `src/online/peer_ping.*` | new |
| Party, invites, match rooms, four-peer mesh, per-pair transport | - | `src/online/{online_service,match_session}.*`, `server/tmnt-lobby/src/{hub,room}.js` | new (generalizes SSF2T's two-player pairing) |
| Public Lobby identity: `Player-XXXX` alias and random action handle | Dragon `server/lobby/src/lobby.js` (`displayName`, `shownAs`, `showInLobby`) | `server/tmnt-lobby/src/hub.js`, `src/online/{protocol,online_service,friend_view}.*` | copied |
| Four local test instances | SSF2T `tools/online-instance.ps1` | `tools/online-instance.ps1` | generalized to a-d |

## Worker

`server/tmnt-lobby` started as SSF2T's service with its own Worker name, game id and image list and was then
generalized from two players to a party of four (protocol 4). Neither reference Worker serves several games: SSF2T
refuses any other `game` (`bad_game`), and Dragon's game key partitions rooms but its accounts and metrics are its own,
so each game has its own service. Two Durable Object classes: `TMNTLobby`, the single directory (players, friends,
presence, parties, invites, the advertisement of a party's session) and `TMNTMatch`, one per match (member sockets,
endpoint exchange, per-pair transport, relay routing, seat grace, expiry). A fifth member is refused. Endpoints:
`GET /` and `/health` (`service`, `protocol` 4, `worker_build`, `relay_supported`, `relay_max_payload`), a WebSocket to
the directory and a WebSocket per match. Limits, seat grace (30 s), validation and error codes follow SSF2T and are
extended for parties; the room validates sender, destination, generation, token, size and rate on every relay frame. It
is not deployed; `kDefaultServiceUrl` names the address it would have.

## Relay and fallback relay

There is one relay: the online service itself, over the same WebSocket. Neither reference has several relay servers,
relay selection, regional choice or a secondary relay; "fallback" in both means direct UDP first, the service relay
second. SSF2T's rules are kept and generalized per pair: the lower slot of each pair chooses direct as soon as a punch
is answered and the relay after 2.5 s without one (or at once when the service forwards no public endpoints); the other
end follows `match.transport`; the choice is made before the game's traffic and never switched mid-match; a relayed pair
ends when the service connection is lost; relay frames (version 2: source slot, destination slot, token, sequence) are
routed only within their match. Pairs are independent, so one match can mix Direct and Relay pairs. SSF2T's relay is
version 1 and has two endpoints; this one is not wire compatible with it by design.

## Connection test and Play gate

SSF2T's (and Dragon's) gate waited for any finished test. Here it is strict: with Online play on, PLAY is allowed only
while the stored result is **Overall usable** for the current configuration (UDP ports, service address, protocol,
build, local network fingerprint); never tested, running, failed, and Limited-but-not-usable all block it. The stored
result is cleared when a test ends unusable, when Online play is turned off, and whenever the configuration changes.
The report distinguishes Worker, STUN, Direct, Relay and Overall usable. Online name, Ping, display and graphics
settings do not reset it. Online play off never needs it.

## Party and invites

The game's own lobbies run over Xbox LIVE sessions and Net-Z; the service supplies what Xbox LIVE did: friends,
presence, invites, the party (one hidden match of up to four) and the advertisement of the party's session, which only
its members can search and join. Apart from a player's own public Lobby listing, nothing is listed publicly. Invites
carry the full lifecycle (received, accepted, declined, expired, cancelled, already joined, already invited, party full,
host left, version or protocol mismatch). Neither reference has a party model; SSF2T's two-player Player Match was
replaced.

## Public Lobby identity

Copied from Dragon's `showInLobby` and `displayName`. The configured Online name stays `Player`; the Worker alone
computes the presentation alias `Player-XXXX` for that exact name, using four uppercase hex characters of a salted
hash of the account under the directory's private salt. The salt is never returned. A custom name is unchanged, and the
real name stays in the account row for room and friend matching. Friend rows and requests carry the alias as the
optional `display_name`; the launcher prefers it and falls back to `name` for an older Worker.

Each public Lobby listing carries a separate random 32 lowercase hex action handle. The handle is stable while the
listing stays visible, rotates after Off -> On, and is used by Lobby friend actions instead of the visible name or
friend code. It is never displayed. The row carries only the handle, presentation name, version and relation
(`none`, `pending`, `friend`, `incoming`); incompatible builds are filtered before the row is built.

## Compatibility

Fields checked by the service: lobby protocol (3), game id, game image (`base`), online build (`kOnlineBuild`, raised
when a change breaks play with the previous build). Version and platform are shown to friends but do not decide.
Friends on another build see "Game versions do not match"; their matches are never listed and a join is refused with
`version_mismatch`.

## Ping and latency

The Connection test shows the service round trip of its sign-in. In game, the Ping display shows the worst measured
round trip to the other participants: small `TPNG` probes on the gameplay socket per peer, Direct or through the relay,
never the service's HTTP latency. SSF2T measured the round trip from its rollback acknowledgements, which this game
does not have, so the probe is new here. Without a connected peer it reads `Ping: --`.

## Telemetry

New for this game. The client (`src/online/telemetry.*`, `peer_ping.*`) collects a bounded, allow-listed, privacy-safe
stream and per-peer reports and sends batches to the directory; the service stores bounded aggregates and serves a
private admin summary and dashboard behind a secret bearer token (`server/tmnt-lobby/README.md`). The design follows
Dragon's anonymous aggregate dashboard (`metrics.js`, `admin.js`, `admin_page.js`, `test/metrics.test.js`), adapted to
parties, per-pair transports and reason codes. SSF2T has none. It is local only: nothing is deployed and no secret
exists. No friend codes, tokens, payloads or full IPs are collected. The port's own logs never contain the token.

## Linux

The WebSocket client on Linux uses POSIX sockets and loads the system's libssl at run time (system certificate store,
SNI, host name check, TLS 1.2 minimum). Dragon's `ssl_mutex_` fix (the reader thread and senders share one SSL
object) is applied. `tests/online_websocket_tls_tests.cpp` runs on Linux only. Everything else is shared code.

## Cheats and desync

Dragon disables every cheat online; SSF2T disables Infinite Time. This port follows Dragon: turning Online play on
switches every saved cheat Off, the Cheats page and F10 show **Cheats are disabled while Online Play is enabled**, and
turning it off restores nothing.

## Tests

| Test | From | What |
|---|---|---|
| `server/tmnt-lobby/test/*.test.js` | SSF2T, rebuilt for parties | directory, parties, invites, match rooms, relay routing and limits, real WebSockets, wire parity, telemetry |
| `tests/online_protocol_tests.cpp` | SSF2T, extended | protocol 4 messages, errors, compatibility |
| `tests/online_identity_tests.cpp` | SSF2T | identity file, instance lock |
| `tests/online_service_tests.cpp` | SSF2T, rewritten | the service state machine over a scripted channel: sign-in, retry, friends, party, invites, room, reconnect, telemetry, Connection test |
| `tests/online_rendezvous_tests.cpp` | SSF2T, extended | punching, several pairs on one socket |
| `tests/online_connection_tests.cpp` | SSF2T, extended | verdicts, ports, STUN, UPnP, relay, summary fields; real service when a lobby URL is set |
| `tests/online_integration_tests.cpp` | SSF2T, rewritten | four clients against a real local lobby: host plus three guests, mixed Direct and Relay, fifth player rejected, spoofing, reconnect |
| `tests/online_peer_ping_tests.cpp`, `tests/online_telemetry_tests.cpp` | new | per-peer RTT, jitter and loss; telemetry bounds and privacy |
| `tests/online_match_session_tests.cpp` | new | per-pair establishment: STUN and endpoint advertisement, Direct success, relay fallback and its reason codes, non-chooser following, pair independence, the fatal host-pair rule, relay closes, privacy of logs and events |
| `tests/online_setting_tests.cpp` | SSF2T, extended | settings, Online page sections, strict Play gate and its reset rules, Ping, wiring |
| `tests/online_websocket_tls_tests.cpp` + `tests/tls_front.mjs` | SSF2T | Linux TLS |
| `tests/wire_parity_tests.cpp` + `tests/wire-vectors.txt` | SSF2T | byte layouts shared with the Worker (relay frame version 2) |
| `tests/netplay_online_game_socket_tests.cpp` | SSF2T, rewritten | multi-peer gameplay socket, relay, overflow, per-peer counters |
| `tests/friend_field_tests.cpp`, `tests/clipboard_tests.cpp` | SSF2T | friend code entry and clipboard |
| `tests/player_match_net_tests.cpp` | SSF2T, extended | synthetic addresses, QoS, VDP shim, per-slot mux, hold, overlapped receives, buffer gather/scatter, search results, hook coverage |
| `tools/test-online-instance.ps1` | new | four isolated local instances (scratch folders only) |
