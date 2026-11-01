# Online play

Play Turtles in Time Re-Shelled with up to three friends over the internet through the game's own online menus.
Players meet through a small online service (`server/tmnt-lobby`); the game's peer traffic runs directly between the
PCs over UDP whenever it can, and through the online service's relay for any pair that cannot.

Status: **not deployed and not yet tested between real PCs.** The online service, the client, the Connection test, the relay, the party
model and the launcher page pass their headless tests here, including a four-client run on loopback against the local
service. The bridge to this game's own Xbox LIVE session and peer layers has only been checked statically. Four-player
play is claimed only after four real instances have been tested.

## Playing

1. Launcher > **Online** > **Online play** On. The rest of the page appears only then. **Online name** is what other
   players see (up to 16 letters, digits, `-` or `_`).
2. Share **Your friend code** (`XXXX-XXXX`, **Copy** puts it on the clipboard). Paste or type a friend's code under
   **Friends** and **Send request**; they **Accept** or **Decline** it on their page.
3. **Connection test** > **Run test**. PLAY stays blocked until it reports **Overall usable** (see below).
4. **Invite** each friend from **Friends**. Invited players see the invite on their own Online page and **Join** it;
   the **Party** section shows Player 1 to Player 4. The host can **Remove** a player or **Leave party**.
5. Everyone presses **Play**. In the game, with the first controller (or the keyboard), the host uses **Create Match**
   and the guests **Quick Match** or **Custom Match**, which find only the friends' party.

There are no rooms to pick: the party is the match. Player 1 is the host; a guest takes the lowest free slot when it
joins and keeps it. A fifth player is refused (`party full`). One local player per PC is online (the first controller);
the game's own Live check keeps other local players out, because only the first controller gets the online profile.

While Online play is signed in, the first controller sees a title-local Xbox LIVE profile: signed in, multiplayer
allowed, the Online name as gamer name and a stable XUID derived from the friend code (never from the private token).
With Online play Off nothing changes: no sign-in, no service, no relay, no test and no telemetry, and every local feature
(local multiplayer, achievements, leaderboards, Gamer Card, player names) behaves as before.

### The Party section

Four cards, always visible, empty slots included. Each shows the name, a **Host** tag, **Connecting** or
**Connected**, **Direct** or **Relay**, the measured **ping**, and **Invited** or **Joining** while a player has not yet
arrived. Invites end with an explained reason: declined, expired, already joined, already invited, party full, the
host left, version or protocol mismatch, or no longer valid. Nothing depends on the Xbox Guide.

### Connection test

A strict gate. Online play off never needs it. With Online play on:

| State | PLAY |
|---|---|
| never tested, running, failed, or Limited but not usable | blocked |
| **Overall usable** | allowed |

The report separates **Worker** (health check and WebSocket sign-in, round trip in ms), **STUN** (two servers from the
gameplay UDP port), **Direct** (a public endpoint or a router mapping exists), **Relay** (the service offers it) and
**Overall usable** (the Worker is reachable and at least Direct or Relay works). The result is stored with the
configuration it was run for: the UDP ports, the service address, the protocol and build, and a fingerprint of the
PC's local networks. Changing any of those, or turning Online play off, makes the test required again. Online name,
Ping and display settings do not. On Windows the test's traffic also gives Windows Defender Firewall its chance to ask
for network access (choose Allow access).

### Ping

**F10** shows or hides it in game. The value is the worst round trip to the other players in the party, measured on
the peer connections themselves with 13-byte probes (`TPNG`) on the gameplay socket, Direct or relayed. It is never the
service's HTTP latency; without a connected peer it reads `Ping: --`.

## Identity

`online_identity.ini` in the user folder holds a private 32-byte token, made on first use and never overwritten. It is
only ever sent to the online service, never shown or logged. The public friend code is derived from it. A second
instance on the same user folder signs in under a temporary identity.

## How the game goes online

The game uses Xbox LIVE sessions for matchmaking and Quazal Net-Z over VDP sockets for its peer traffic: the host owns
the world, each console owns its own turtles, and the four consoles form a full mesh (six pairs). The port keeps all of
that and answers the calls underneath it (`src/player_match_hook.cpp`, `src/player_match_net_hook.cpp`):

- `XUserGetSigninState`, `XUserCheckPrivilege`, `XUserGetSigninInfo`, `XUserGetXUID`, `XUserGetName`,
  `XNetGetTitleXnAddr`: the title-local LIVE profile of the first controller.
- `XSessionCreate` (host) advertises the party's session to its members; `XSessionSearchEx` returns it;
  `XSessionCreate` with a listed session joins it; `XSessionJoinRemote`/`XSessionLeaveRemote` add and remove members;
  `XSessionGetDetails` reports the real member list (up to four, with slot counts and state); `XSessionDelete` leaves.
- `XNetXnAddrToInAddr` maps a participant's XNADDR to a synthetic address (10.83.x.y); QoS answers with the host's
  advertised listen data.
- The game's datagram sockets become shim handles. `WSASendTo`/`sendto` to a participant's synthetic address go to that
  participant's transport; `WSARecvFrom` completes at once when a datagram is waiting, or stays pending (overlapped,
  error 997) and is completed by the network pump every presented frame, which sets its event;
  `WSAGetOverlappedResult` reports it.
- `src/online_bridge.cpp` opens each participant's transport once the service has chosen it: the online gameplay
  socket, directly to that peer, or through the relay. Datagrams the game sends before that are held (up to 32).

## Topology

One technical match per party, one Durable Object per match. The host is slot 1; guests are slots 2 to 4. Every pair of
participants has its own transport. For each pair the lower slot chooses it: **Direct** as soon as a punch is answered,
**Relay** after 2.5 s without one (or at once when the service forwards no public endpoints), and the other end follows
before the game's traffic starts. The choice is never switched during the match. Pairs are independent, so one match can
mix Direct and Relay pairs. When the pair with the host fails, that guest leaves; a failed pair between two guests does
not end the party.

Direct UDP is always tried first; the relay is the automatic fallback. There is one relay: the online service over the
WebSocket every member already has. Relay frames are binary messages (`"SR"`, version 2, type, source slot,
destination slot, token, sequence, length; at most 1472 bytes); the service checks the sender, destination, match
generation, token, size and rate before routing, and a frame never leaves its own match. A relayed pair does not survive
a lost service connection (30 s of seat grace in the match room). Every match has one 15 s establishment budget.

## Telemetry

Operational telemetry for support, only while Online play is on, and never in the way of the game: batched, bounded
(200 events, 8 KiB per batch, one batch per 10 s at most), best effort, dropped under pressure, never per packet. A
party is identified by a random `diag` id (12 hex characters) derived from nothing personal; events before any party
use a random id for the running process (`slot` 0). It carries session and per-peer metrics (state, transport,
Direct/Relay result and reason code, establishment time, round trip, jitter, loss, packet counts, reconnects,
disconnect reason) and a short list of events. It **never** carries friend codes, tokens, packet payloads, full IP
addresses, URLs, paths or free text; the client redacts such values and the service re-validates.

Reason codes come from one list, `tests/reason-codes.txt` (path, disconnect, invite, service and connection_test
reasons), kept identical in `src/online/telemetry.h` and `server/tmnt-lobby/src/reasons.js` by tests. Anything else
is reported as `other`, so a new code never breaks anything and is still counted.

Each fact has one authoritative source, so nothing is counted twice:

| Fact | Source |
|---|---|
| Connection Test outcome | the client's beacon, one `GET /telemetry/test?...` per finished test (works before any session and when the WebSocket cannot be opened; it cannot be delivered when the service itself is unreachable) |
| Party created, ended (including host left), member joined and left (guest disconnect), 4/4, invites sent, accepted, declined, expired | the online service |
| Invite received, join started, completed, failed, invite send failures, worker reconnect | the client |
| Game start attempted (the game calls XSessionStart), succeeded (every other member then exchanges game traffic), failed (a link failed, the party ended or 30 s passed) | the client |
| Per pair: Direct attempt and result, relay fallback and its reason, transport selected, link failure | the client, in the session report and peer reports |

Every client event carries a number that grows for the life of the process. A batch that failed is sent again once with
the same numbers, and the service ignores numbers it already has for that reporter, so a retry never inflates a count.
The service keeps that state only as long as the session row (7 days, 2000 rows). See `server/tmnt-lobby/README.md` for
the service side, retention and its private admin dashboard. Nothing is deployed.

## Diagnostics

Log lines marked `DEBUG_SAULO` are focused test-phase diagnostics (identity, invite flow, slot mapping, XSession
members, STUN, Direct establishment, relay readiness, fallback, packet routing counters, overlapped completion and the
Net-Z socket mapping). They never include tokens and do not log every packet.

## Testing on one PC

All instances connect to the production online service that is built into the game (`kDefaultServiceUrl`); no local
server, address, environment variable or command-line option is involved. `tools\online-instance.ps1` gives each of
the instances `a`..`d` its own folder (`build\online-instances\<name>\user_data` and `\logs`), friend code, settings,
presence and log, and the first free UDP port of 7001 to 7004 (then any free port). Start one per terminal:

```
tools\online-instance.ps1 -Name a
tools\online-instance.ps1 -Name b
tools\online-instance.ps1 -Name c
tools\online-instance.ps1 -Name d
```

In each launcher turn Online play on and run the Connection test. Then `a` adds the friend codes of `b`, `c` and `d`
and each accepts; `a` invites `b`, `c` and `d` and each accepts; everyone presses Play. `-PrepareOnly` sets the folders
up without starting anything.

When the online service is unreachable the Online page and the Connection test say so ("Cannot reach the online
service", "The online service needs a different version of the game", ...); the game never switches to another address
and offline play is unaffected.

Headless tests (implementation tests, not part of any manual procedure): `node --test "test/*.test.js"` in
`server/tmnt-lobby`, and `tools\test.ps1 -CppTests <build>`, which also runs the C++ online tests, including a
four-client run, against an isolated test server.

## What is not covered

- More than one online player per PC: the first controller carries the online profile.
- Host migration: the game has none; when the host leaves, the party ends and the guests return to the menu.
- The game's own timeouts are kept; its 15 s wait for the host's answer after joining is not tied to the
  establishment budget.
- Deployment: the online service must be published to Cloudflare by hand (`server/tmnt-lobby/README.md`) before the
  game can go online; until then Online play reports that the service is unavailable.
