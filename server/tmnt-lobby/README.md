# tmnt-lobby

Online service for Teenage Mutant Ninja Turtles: Turtles in Time Re-Shelled (PC port): player identity, friends,
presence, parties of up to four players, invites and the advertisement of a party's game session. The game's traffic
runs directly between the PCs over UDP whenever it can; a pair of players who cannot reach each other is relayed
through this service, between the two sockets of that pair only.

This service is separate from every other game's. It has its own Worker name, its own Durable Object classes and its
own protocol (`game: "tmnt-reshelled-pc"`). It is derived from the Super Street Fighter II Turbo HD Remix PC service and
generalized from two players to a party of four (protocol 4, relay frame version 2).

**Status: written and tested locally only. Nothing of this service is deployed** (see "Deploy (manual)").

## Design

- `src/worker.js`: the Worker and two SQLite-backed Durable Object classes with WebSocket Hibernation.
  `GET /` and `GET /health` return `{"service":"tmnt-lobby","protocol":4,"status":"ok",...}`. `GET /telemetry/test` is the
  Connection Test beacon (see "Telemetry").
  - `TMNTLobby` (binding `LOBBY`, one object named `global`): the **directory**. Every WebSocket to `/` goes here.
  - `TMNTMatch` (binding `MATCH`, one object per match, `idFromName(<match id>)`): the **match room**. A WebSocket upgrade
    to `/match/<16 hex>` goes to the room of that match.
- `src/hub.js`: the directory, independent of Cloudflare (`local/server.js` runs the same code on Node). Tables:
  `players`, `friends`, `friend_requests`, `registrations`, `parties`, `party_members`, `party_invites`,
  `player_matches` (the advertisement of a party's game session).
- `src/room.js`: the match room, independent of Cloudflare too. Tables: `room_meta`, `members`, `slot_generations`,
  `pairs`, `links`. Sockets, endpoints, per-pair transport, relay routing, budgets, seat grace and expiry live here.
- `src/protocol.js`: versions, limits, validation, relay frame codec, friend code derivation.
- `src/telemetry.js`: validation and storage of client telemetry and of the Worker's own party events, aggregates and
  percentiles (see "Telemetry"). `src/reasons.js`: the shared reason-code list. `src/beacon.js`: the Connection Test
  beacon route and its limits. `src/admin.js` + `src/admin_page.js`: the private `/admin` routes and dashboard.
- `local/server.js` + `local/rooms.js`: the development server (one in-process room per match, commands queued like
  two objects would see them).

A **party** is the user-facing name; the technical object is the **match** (16 lower-case hex). Slots are stable 1 to 4:
the host is slot 1, a joiner gets the lowest free slot from 2 at accept time and keeps it until it leaves. `diag` (12 hex)
is random per party, sent to every member and logged by the Worker and the clients; it is derived from nothing personal.

A player is identified by a private token that never leaves their PC. The public id (`XXXX-XXXX`) is derived from it and
is also the friend code. The directory stores only a hash of the token and refuses a known id with another token. A
member's `join_token` (32 hex) is shown only to its owner; the directory stores its sha256 and hands only the hash to
the room.

Directory and room talk through one-way commands, never waiting for an answer: the directory sends the room the full
roster (`apply`, newest revision wins) or `end`; the room reports `roomMember`, `roomLeft`, `roomEnded` (each with a
sequence number, so an out-of-order report is ignored). Durable Object RPC in production, direct calls on a queue in the
tests and the dev server.

### Flow

1. `hello` signs the player in on the directory (presence, platform, version, guest image).
2. Friends: `friends.add` with a friend code, the other player `friends.accept`s.
3. `party.create` makes a party of one (or `pm.host` does, implicitly). `invite.send` invites an online friend with the
   same game image and online build; all invites of one host carry the same match. `invite.accept` joins the party:
   the answer is `{match, diag, slot, join_token, generation, party}`. A fifth member is refused (`party_full`).
4. Every member opens `wss://<host>/match/<match>` and sends `match.hello` with its slot and `join_token`. The room
   answers with its `relay_token` and the member list. Each member publishes its gameplay endpoint with `match.endpoint`;
   the room tells every other participant (`match.peer`) and tells a newcomer about each existing endpoint.
5. For each pair of participants the **lower slot** chooses the transport once (`match.transport`, `direct` or `relay`);
   the pair and the host are told. Pairs are independent: some can be direct and others relayed in one room.
6. Relayed datagrams are binary frames on the room socket (below). `match.link` reports link states for party cards.
7. `match.leave` frees the seat at once; the host leaving ends the party for everyone. A dropped room socket keeps its
   seat for 30 s; the same slot and token resume it with a higher socket generation.
8. `pm.host` advertises the party's game session. Party members other than the host see it with `pm.search` and join with
   `pm.join`; a player in no party sees the live, compatible, non-full sessions of online friends and `pm.join` seats
   them in the host's party at the next free slot (non-friends never see or join it).

### Protocol 4

One JSON object per text frame; the literal `ping` is answered `pong`. Requests are `{"op":..., "seq":n, ...}`, answers
`{"re":n,"code":"ok"|<error>,...}`, events `{"ev":...}`. Binary frames are relay frames (room sockets only).

Directory ops (`wss://<host>/`):

| op | fields | answer |
|---|---|---|
| `hello` | `proto` (4), `game`, `guest` (`base`), `build`, `player`, `token`, `name`, `version`, `platform`, `presence`, `relay` | `player`, `name`, `friend_code`, `capabilities`; a party member then also gets `party.update` |
| `ping` | | `time` |
| `friends.list` / `friends.requests` | | `friends[]` / `incoming[]`, `outgoing[]` |
| `friends.add` | `code` | `player`, `name`, `state` (`pending`/`friends`) |
| `friends.accept` / `.decline` / `.remove` | `player` | `friend` / - / - |
| `party.create` | | `match`, `diag`, `slot` 1, `join_token`, `generation` 0, `party`; `busy` when already in a party |
| `party.leave` | | -; the host leaving ends the party (`host_left`) |
| `party.kick` | `player` | -; host only |
| `invite.send` | `player` | `invite`; errors `no_party`, `not_host`, `not_friends`, `offline`, `busy`, `already_invited`, `version_mismatch`, `party_full`, `limit` |
| `invite.accept` | `invite` | `match`, `diag`, `slot`, `join_token`, `generation`, `party`; errors `not_found`, `expired`, `host_left`, `party_full`, `version_mismatch`, `busy` |
| `invite.decline` | `invite` | - |
| `pm.host` | `slots` (2 to 4), `rules` (up to 8 small integers), `qos` | `session`; creates the party when there is none (the answer then also carries its credentials); `not_host` for a guest |
| `pm.cancel` | | - |
| `pm.search` | | `sessions[]`: the own party's advertisement to its non-host members; online friends' open sessions to a player in no party |
| `pm.join` | `session` | `session`, `match`, `slot`, `generation` (a player in no party also gets `join_token` and the party snapshot) |
| `telemetry.batch` | `diag`, `ctx`, optional `session`, `peers[]`, `events[]`, `dropped` (see "Telemetry") | `accepted`, `ignored`, `duplicates`; errors `bad_request`, `too_large`, `limit`, `internal`; never affects a party |

Directory events: `friend.request`, `friend.added`, `friend.removed`, `friend.status`, `friend.offline`,
`invite` (`invite`, `from {player, name}`, `expires_ms`), `invite.ended` (`invite`, `reason`: `declined`, `expired`,
`cancelled`, `host_left`, `party_full`, `accepted_elsewhere`, `accepted`), `party.update` (`party`), `party.ended`
(`match`, `reason`: `host_left`, `kicked`, `expired`, `closed`).

The party snapshot is `{match, diag, host, generation, session, members[{slot, player, name, platform, version, state}],
invited[{player, name}]}`; `generation` is a revision counter, `state` is `joined` (accepted, no room socket yet),
`connecting`, `connected` or `disconnected` (seat in grace), derived from room reports. A friend row is
`{player, name, online, state, platform, version, guest, build, compatible}`; `state` is `offline`, `online`, `party`,
`connecting` or `in_game`. `capabilities` is `{protocol_version, worker_build, public_endpoints, relay_supported,
relay_max_payload, relay_frame_version, max_party}`.

Room ops (`wss://<host>/match/<match>`; the first frame must be `match.hello` within 10 s, else close `4003`):

| op | fields | answer |
|---|---|---|
| `match.hello` | `proto`, `match`, `slot`, `token` (the `join_token`), `relay`, `generation_seen` | `generation`, `relay_token`, `members[]`, `capabilities`; errors `proto`, `not_found`, `unauthorized`, `party_full`; the previous socket of the slot is closed `4001` |
| `match.endpoint` | `port`, `lan[]`, optional `public_ip` + `public_port` (both or neither) | -; once per socket generation (`already_set`) |
| `match.transport` | `peer`, `transport` (`direct`/`relay`) | -; only the lower slot of the pair (`not_owner`), immutable (`already_set`), `relay_unavailable` |
| `match.link` | `peer`, `state` (`connecting`/`connected`/`lost`) | - |
| `match.leave` | | - |
| `ping` | | `time` |

Room events: `match.members` (`members[{slot, state, name, links[{peer, transport, state}]}]`, `generation`),
`match.peer` (`slot`, `name`, `ip` as the room observed it, `port`, `lan[]`, optional `public_ip`/`public_port`,
`same_network`, `service_forwards_public`), `match.transport` (`a`, `b`, `transport`, `by`; to both ends and to the host),
`match.peer_lost`, `match.peer_back`, `match.left` (`slot`, `reason`: `left`, `disconnect`, `kicked`, `expired`),
`match.ended` (`reason`: `host_left`, `expired`, `closed`), `relay.closed` (`peer`, `reason`: `peer_disconnected`,
`rate_limit`, `peer_backpressure`, `peer_unavailable`, `unavailable`).

Errors: `bad_request`, `too_large`, `no_session`, `proto`, `bad_game`, `bad_guest`, `unauthorized`, `not_found`,
`not_friends`, `no_party`, `not_host`, `not_owner`, `already_set`, `offline`, `busy`, `already_invited`,
`version_mismatch`, `party_full`, `expired`, `host_left`, `stale`, `limit`, `relay_unavailable`, `internal`.

Relay frames (binary, big-endian): `"SR"`, version `2`, type `1` (data), `src` (u8 slot), `dst` (u8 slot), token (u32),
sequence (u32), payload length (u16): a 16-byte header, then 1 to 1472 bytes. The room forwards a frame unchanged to the
current socket of `dst` only when it parses, `src` is the sender socket's own slot, the sender socket is the current one
of that slot, `dst` is another connected participant, the token is the room's `relay_token`, the pair has not chosen
`direct`, and the budgets allow it. Anything else is dropped and counted by reason (`bad_magic`, `bad_version`,
`bad_type`, `truncated`, `oversized`, `bad_length`, `src_mismatch`, `dst_invalid`, `dst_unavailable`, `bad_token`,
`pair_direct`, `rate_limit`, `stale`, plus `not_joined` and `relay_closed`); the first three of each reason per room are
logged, never the payload, and frames are never stored. A sender whose frame cannot be routed hears
`relay.closed {peer, reason: "unavailable"}` once per destination.

Rules: a party is joined only by an online friend of its host with the same game image (`base`) and online build, who is
not in a party; invites never reserve a slot and last 2 minutes. A peer's address is always the address of its own
connection; only the port comes from the client, and LAN candidates reach only a peer behind the same address. A party
of the host alone expires after 30 minutes without activity. A room ends when its host's seat expires, when nobody has
been connected for 60 s, or after 4 hours. A socket silent for 45 s (no message, frame or runtime-answered `ping`) is
closed. A member with no directory socket for 30 s and no held room seat is removed (the host takes the party with it).

Limits: 4 KB frames (a `telemetry.batch` may be up to 8320 bytes), 16-character names, 60 messages burst then 20/s per socket (`friends.add`, `party.create`,
`party.kick`, `invite.send`, `invite.accept`, `pm.host`, `pm.join`: 10 burst then 1/s; room `match.endpoint`,
`match.transport`, `match.link`: 40 burst then 5/s), 16 directory sockets per address, 10 room sockets, 20 new players per
day per address, 200 friends, 50 pending friend requests each way, 8 pending invites per party. Relay: 400 frames/s
(burst 800) and 128 KB/s (burst 256 KB) per room socket, 800 frames/s (burst 1600) and 256 KB/s (burst 512 KB) per match;
256 rate-limit drops on a pair end that pair's relay (`relay.closed`), as do an unwritable or backed-up peer socket
(256 KB buffered). A pair's relay comes back when either end reconnects. Rooms are separate Durable Objects, so relayed
traffic of one match counts against that match's object only.

## Telemetry

**What is collected.** The game sends `telemetry.batch` on its directory socket (signed-in sockets only, about every 15 s
while a party exists and once more when it ends). A batch carries the context of one player's view of one party and what
happened in it:

- `diag`: the party's random 12 hex id (the same value the directory gave every member of the party; in a lobby row, the game process's own random id).
- `ctx`: `game`, `proto`, `version` (port version), `platform`, `os`, `arch`, `fp` (build fingerprint prefix), `slot` (1 to
  4 in a party, 0 for a lobby row; required), `host`, `party` (size, 1 to 4; 0 with slot 0).
- `session` (optional): `dur_ms`, `ct_usable`, `worker`, `stun`, `direct`, `relay` (connection test facts), `compat`,
  `reason`.
- `peers[]` (at most 8, optional): per link `ls`, `rs` (slots 1 to 4), `state`, `transport` (`pending`/`direct`/`relay`/
  `failed`), `direct`, `relay`, `fallback`, `conn_ms`, `est_ms`, `ping {sent, recv, last, min, max, avg, jit, to, ooo, dup,
  loss}`, `tx`, `rx`, `reconn`, `reason`.
- `events[]` (at most 200): `{e, n, t, f}` with `e` one of the allow-listed names below, `n` the event's number (see
  "Retries are not counted twice"), `t` seconds since the client's zero, `f` at most 8 short text fields. Unknown names are ignored (counted); they
  never fail the batch.
- `dropped`: events the client had to drop.

Client event names (all others, including the names a client sent before the Worker took them over, are ignored and counted
as `ignored`, never an error): `invite_received`, `invite_failed`, `join_started`, `join_completed`, `join_failed`,
`game_start_attempt`, `game_start_success`, `game_start_failure`, `worker_reconnect`, `direct_attempt`, `direct_result`,
`relay_fallback`, `relay_result`, `transport_selected`, `peer_disconnect`, `session_end`.

The Worker records what it sees itself, correlated by the party's `diag` (the directory and the room report them through a
best-effort internal `record`, the room over the same one-way commands as `roomMember`): `party_created`,
`party_reached_4` (no fields; once per party, when its member count first reaches 4), `party_ended` (reason),
`member_joined`, `member_left` (reason), `invite_sent`, `invite_accepted`, `invite_declined`, `invite_expired`,
`invite_ended` (reason, for the other endings), `party_full` (a fifth member refused), `rate_limited` (counted only),
`room_joined`, `room_left` (reason), `room_peer_lost`, `room_transport`, `relay_closed` (reason), `room_ended` (reason). A
failing store never changes a request: the worst a telemetry error does is a `telemetry.batch` answered `internal`.

**One source per event, so nothing is counted twice.** The Worker is authoritative for everything it can observe: parties
(`party_created`, `party_reached_4`, `party_ended`), members (`member_joined`, `member_left`), invites (`invite_sent`,
`invite_accepted`, `invite_declined`, `invite_expired`, `invite_ended`), `party_full`, rate limits and the room and relay
events. A client is authoritative only for what happens on its own machine or between the players' machines: invites it
received or could not send (`invite_received`, `invite_failed`), the join and game start it attempted, its Worker
reconnects, the Direct and Relay attempts, the chosen transport, peer disconnects and the end of its session. The client's
allow-list therefore does not contain `invite_sent`, `invite_accepted`, `invite_declined`, `invite_expired`, `party_created`,
`party_2_of_4`, `party_3_of_4`, `party_4_of_4`, `player_joined`, `player_left`, `party_full`, `host_disconnect`,
`guest_disconnect` or `connection_test` (the last one is the beacon below). Derived, with no extra events: parties reaching 4/4
is `party_reached_4` over `party_created`; a host disconnect is a `party_ended` with reason `host_left`; a guest disconnect is a
`member_left` with reason `disconnect`, `left`, `kicked` or `expired` (the host never leaves through `member_left`).

**Reasons.** `tests/reason-codes.txt` (repository root) is the one list of reason codes, shared with the game; the Worker's
copy is `src/reasons.js` and `test/reasons.test.js` fails unless the two are identical. Every reason-valued field is checked
against it on the way in: an event's `reason` and `fallback` field, a peer's `fallback` and `reason`, the session's `reason`,
and the `reason` of every event the Worker records. A known code is kept as it is, an empty value stays empty, anything else is
stored as `other` (never dropped, never an error) and adds to the daily counter `unknown_reason` (events: every one counted;
a session or peer report: counted with the reporter's first batch only, since those are re-sent whole). Every code the
Worker emits (protocol error codes, party, member, invite, room and relay reasons) is in the list; the tests read the sources
to keep it so. The summary's `reasons` groups all reason counters by the category of the list, so a reason can only appear in
its own group: `fallback` (category `path`), `disconnect`, `invite` (invite outcomes, including `accepted`, `declined` and
`expired` from the Worker's invite events), `service`, `connection_test`, plus `other` (a count). The sources are the clients'
event reasons and the Worker's `party_ended`, `member_left`, `invite_ended` and `relay_closed` reasons.

**Retries are not counted twice.** Every client event carries `n`, a positive integer up to 4294967295 that grows for the
life of the game process and is never reused; a retried batch (new `seq`) sends the same events with the same `n`. For each
session row (`diag`, slot, reporter) the Worker keeps `last_n`, the highest number accepted. An event with `n` at or below
`last_n`, or repeated within the batch, is a duplicate: it is not stored in the ring, not counted in any counter or aggregate,
and adds to the daily counter `duplicate_events`. Accepted events raise `last_n`. An event without a usable `n` (absent, not an
integer, 0, negative or above u32) is accepted without deduplication. A batch of duplicates only is answered `ok` all the same
and its `dropped` figure is not added again. The session report and the peer reports replace the stored ones, so they are idempotent
already. The mark is a column of the session row and goes with it: rows are kept for 7 days (newest 2000, at most 8 per `diag`),
after which a retry of an old event would count again. There is no separate table, and rows of different reporters never share a
mark. The answer of `telemetry.batch` is `{code: "ok", accepted, ignored, duplicates}`.

**Lobby rows.** A client sends `ctx.slot` 0 with `ctx.party` 0 (`host` is stored as false) and a random per-process `diag` for
what happens before a party exists (an invite received, a join started or failed, a Worker reconnect). Slot 0 is accepted only
with party 0, and slots 1 to 4 only with a party of 1 to 4; anything else is `bad_request`. A lobby row is not a party
session: it is counted as `lobby_sessions`, not in `sessions` or its breakdowns, and its session report and peers never enter
the transport mix, the percentiles or the Connection Test reports. Its events count normally.

**Connection Test beacon.** `GET /telemetry/test?...` records one outcome of the game's Connection Test before any session or
sign-in exists. It is unauthenticated and best effort: well-formed requests are answered 204 whatever the storage did, a
malformed one 400 without detail, a request over the limits 429 (nothing recorded), any method other than GET 405. Parameters
(an unknown parameter is ignored and never stored; repeating a parameter is a 400):

| Parameter | Required | Values |
| --- | --- | --- |
| `diag` | yes | 12 lower-case hex; checked, never stored |
| `ver` | yes | port version, `[0-9A-Za-z._-]`, 1 to 16 characters |
| `plat` | yes | `Windows`, `Linux`, `SteamDeck`, `Other` |
| `os`, `arch` | no | `[A-Za-z0-9._-]`, up to 24 characters; validated, not stored |
| `proto` | yes | client lobby protocol, integer 0 to 1000 |
| `sproto` | no | service protocol, integer 0 to 1000 |
| `worker` | yes | `ok`, `unreachable`, `bad_status`, `unrecognized`, `protocol_mismatch` |
| `ws` | yes | `ok`, `failed`, `refused`, `skipped` |
| `ms` | no | integer 0 to 60000 |
| `stun` | yes | `ok`, `failed`, `off` |
| `direct`, `relay`, `usable` | yes | `0` or `1` |
| `fail` | no | empty, or a code of `tests/reason-codes.txt` whose category is `connection_test` |
| `router` | no | `none`, `mapped`, `nat`, `unknown` (default) |

Limits are bounded in-memory token buckets of the Worker (per isolate): 60 burst then 5/s for the whole service, and 6 burst
then one per 5 s per address; the address is only ever used as a salted hash (the salt lives in memory) in a map of at most 512
addresses, the oldest dropped first, and an IPv6 address counts as its /64. Loopback is exempt from the per-address bucket in the
development server only. What is stored: daily counters `ct.total`, `ct.usable`, `ct.unusable`, `ct.fail.<code>`,
`ct.worker.<status>`, `ct.ws.<status>`, `ct.stun.<status>`, `ct.router.<kind>`, `ct.platform.<name>`, `ct.version.<version>`,
`ct.direct` and `ct.relay` (tests where that path was possible), plus the newest 1000 values of `ms` for 7 days at most (table
`tel_latency`, a time and a number). The summary's `connection_beacon` holds the totals, the breakdowns and the latency
percentiles; the beacon is not an event of the batch and appears in no event table.

**What is never collected.** Friend codes, tokens (identity or join), names, addresses, match ids, relay payloads or any
free text. The Connection Test beacon stores no address, query string or identifier either (see above). The server re-validates every string independently of the client: only `[A-Za-z0-9_.:-]` survives, at most 48
characters, and a value that looks like a friend code (`XXXX-XXXX`), 32 or more hex characters, an IPv4/IPv6 address, a URL
or an e-mail, contains a path separator or whitespace becomes `redacted` (counted as `redacted_values`). Keys are cleaned the
same way. Numbers are range-checked and a wrongly typed field fails the batch (`bad_request`). A session row is keyed by the
party `diag`, the reporting slot and a salted hash of the reporter that is used only to keep one player from overwriting
another's row and is never returned. The party's match id is not stored at all; `diag` is the only identifier shown.

**Limits.** 6 batches burst then one per 5 s per player, 120 burst then 10/s for the whole directory (`limit`), at most 8
reporting rows per `diag`, 200 events and 8 peers per batch (`too_large`), frames up to 8320 bytes. Rate-limit hits are
counted in memory and written in groups.

**Retention.** SQLite tables of the directory object: `tel_events` (the newest 5000 events and no more than 7 days, client
and Worker side), `tel_sessions` (one row per party, slot and reporter: context, session report, per-peer reports; the
newest 2000 and no more than 7 days) and `tel_counters` (daily counters, UTC days, at most 512 distinct keys a day, kept 35
days); `tel_sessions` also holds each row's `last_n` (added to an older table by `ALTER TABLE` when missing), and `tel_latency`
the Connection Test latencies (newest 1000, 7 days). Aggregates are read from the counters (sessions by platform/version/OS/protocol, fallback and disconnect reasons,
invite and room outcomes, rate limits, Connection Test beacons) and from the kept session rows of parties (transport mix counting
each link once, RTT/jitter/loss percentiles overall and for direct and relay links, connection test reports).

**Admin access.** Nothing is public. The routes exist only when the `ADMIN_TOKEN` secret (16 or more characters) is set;
otherwise every `/admin*` path answers 404.

- `GET /admin`: the dashboard, one self-contained page (inline style and script allowed by hash under a strict CSP, no
  external resources). It holds no data; it asks for the token (kept in memory, or this tab with "Keep for this tab") and
  calls the two routes below with it. A browser cannot send a bearer header on navigation, which is why the page itself is
  the only unauthenticated route.
- `GET /admin/telemetry/summary[?days=1..30]`: the aggregates as JSON.
- `GET /admin/telemetry/session?diag=<12 hex>`: everything kept about one party (reporters, per-peer reports, merged event
  timeline). 400 for a malformed `diag`, 404 when nothing is kept.

Both need `Authorization: Bearer <ADMIN_TOKEN>` (401 otherwise, compared in constant time).

```
npx wrangler secret put ADMIN_TOKEN       # manual; never commit a value
curl -H "Authorization: Bearer $ADMIN_TOKEN" https://<worker>/admin/telemetry/summary
```

The dashboard shows counts, the transport mix, reason tables grouped by category (Fallback / path reasons, Disconnect reasons,
Invite outcomes, Service errors, Connection Test failures), the Connection Tests block (total, usable share, failures,
Worker/WebSocket/STUN status, median latency), parties reaching 4/4 with the derived host and guest disconnects, the percentile
table, the recent sessions list and a session drill-down. Every value is written as text, never as markup. **None of this is deployed**: the Worker and its secret do
not exist yet.

## Local

No dependencies and no Cloudflare account are needed (Node 22.13 or newer, for `node:sqlite`).

```
cd server/tmnt-lobby
node --test "test/*.test.js"        # protocol, friends, parties, rooms, relay, wire parity, Worker glue, real WebSockets
node local/server.js                # ws://127.0.0.1:8791/ and ws://127.0.0.1:8791/match/<id> (--db <file> keeps players and friends)
node local/server.js --admin-token <16+ characters>   # also http://127.0.0.1:8791/admin (or TMNT_ADMIN_TOKEN in the environment)
```

The development server is for the automated tests and diagnostics only; players and the manual four-instance test use
the deployed Worker, which the game connects to with no environment variable. `TMNT_LOBBY` (or `-Lobby` of
`tools\online-instance.ps1`) is a diagnostic override and is never needed. Room sockets use `/match/<match>` on the same
host and port.

`tests/wire-vectors.txt` (repo root) holds the relay frame bytes the Worker and the game must agree on, including
`relay.v2.slot2to3`; `test/wire.test.js` checks them and the constants in `src/netplay/relay_frame.h` and
`src/online/protocol.h`.

## Deploy (manual; nothing in this repository deploys)

**This service has not been deployed.** Nothing of it exists on Cloudflare yet, so the first deploy creates the Worker
`tmnt-lobby` and both Durable Object classes at once, with the migrations of `wrangler.toml` in order:

- `v1`: `new_sqlite_classes = ["TMNTLobby"]` (the directory).
- `v2`: `new_sqlite_classes = ["TMNTMatch"]` (the match room). New classes are only ever added as a new migration; an
  applied migration is never edited.
- Bindings: `LOBBY` -> `TMNTLobby`, `MATCH` -> `TMNTMatch`. Both are needed: the directory calls the rooms through
  `MATCH`, and a room calls the directory through `LOBBY`.

```
cd server/tmnt-lobby
npx wrangler whoami                 # the account must be the one that owns the workers.dev subdomain
npx wrangler deploy --dry-run       # bundles the Worker without uploading it
npx wrangler deploy
```

Telemetry reads need the secret, set once and never committed:

```
npx wrangler secret put ADMIN_TOKEN
```

Without it the `/admin` routes stay 404. Check the Worker:

```
curl https://tmnt-lobby.saulobenigno.workers.dev/health
```

The game's built-in address is `kDefaultServiceUrl` in `src/online/online_service.h`
(`wss://tmnt-lobby.saulobenigno.workers.dev/`, the same workers.dev subdomain as the account's other Workers); once the
Worker answers there, the launcher, the Connection test and the game use it with zero environment variables. If the
Worker is unreachable the Online page says so; nothing falls back to another address. The directory keeps its data in its
object's SQLite tables and each room in its own, created on first use; an ended room leaves a small tombstone that
erases itself after an hour.

After the first deploy, the four-client check against the real Worker needs no game and no GUI: build
`tmnt_online_integration_tests` and run it with `TMNT_LOBBY_TEST_URL=wss://tmnt-lobby.saulobenigno.workers.dev/`. It signs
in four throwaway identities, makes them friends, creates one party, sends three invites, joins all of them (4/4),
checks that a fifth is refused and relay readiness, and leaves cleanly. It writes test players and a test party to the
production data, which expire on their own; it carries no gameplay.
