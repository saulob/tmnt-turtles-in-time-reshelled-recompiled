# Online play

Play Turtles in Time Re-Shelled with up to three friends over the internet through the game's own online menus.
Players meet through a small online service (`server/tmnt-lobby`); the game's peer traffic runs directly between the
PCs over UDP whenever it can, and through the online service's relay for any pair that cannot.

Status: **experimental.** The online service is live. The service, the client, the Connection test, the relay and the party
model pass their headless tests here, including a four-client run on loopback against the local service.

## Playing

1. Launcher > **Online** > **Online play** On. The rest of the page appears only then. **Online name** is what other
   players see (up to 16 letters, digits, `-` or `_`).
2. Share **Your friend code** (`XXXX-XXXX`, **Copy** puts it on the clipboard). Paste or type a friend's code under
   **Friends** and **Send request**; they **Accept** or **Decline** it on their page.
3. **Connection test** > **Run test**. PLAY stays blocked until it reports **Overall usable** (see below).
4. Everyone presses **Play**. In the game, with the controller that will play (or the keyboard), the host uses **Create Match**
   and the guests **Quick Match** or **Custom Match**, which find only the friends' party.

In the game, the Multiplayer menu offers **Local** and **Online**; Online lists Quick Match, Create Match and Custom Match.
In the lobby of a match you created, **X Invite Friends** opens the friend list of the online service (the Xbox Guide is not
used): pick a friend with A, as many as the match has places, B closes it. Only the host can invite. A friend who is
online and not in a match sees an **Online match invite** box in the game (the launcher page shows the invite and can decline it) and
**Accept** puts them in your lobby through the same path as Quick Match; **Decline** or a full, started or ended match
leaves them where they were. An accepted invite the game has not joined after 60 s gives its place back.

There are no rooms to pick: the party is the match. Player 1 is the host; a guest takes the lowest free slot when it
joins and keeps it. The match takes as many players as the host chose (2 to 4); one more is refused (`party full`). A
party lasts one game session: when the game leaves its session everyone in it leaves the party, so the next match has
its own party, room, relay and endpoints, and a former host can join a friend's match. One local player per PC is online
(the controller that chose ONLINE); the game's own Live check keeps other local players out, because only that controller
gets the online profile.

While Online play is signed in, the controller that chose **ONLINE** (the first one until then) sees a title-local Xbox LIVE profile: signed in, multiplayer
allowed, the Online name as gamer name and a stable XUID derived from the friend code (never from the private token).
With Online play Off nothing changes: no sign-in, no service, no relay, no test and no telemetry, and every local feature
(local multiplayer, achievements, player names) behaves as before.

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

**F3** shows or hides it in game. The value is the worst round trip to the other players in the party, measured on
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
  `XNetGetTitleXnAddr`: the title-local LIVE profile of the controller that chose ONLINE. Without it these calls, and the
  title's profile settings, answer for the one local profile, owned by the controller that pressed START whichever slot
  it connected in. That needs no connection, so saving works offline.
- `XSessionCreate` (host) advertises the party's session to its members; `XSessionSearchEx` returns it;
  `XSessionCreate` with a listed session joins it; `XSessionJoinRemote`/`XSessionLeaveRemote` add and remove members;
  `XSessionGetDetails` reports the real member list (up to four, with slot counts and state); `XSessionDelete` leaves.
- XLIVEBASE `0x0005800E` is `XUserMuteListQuery`, behind the scripts' `PLT_IsPlayerMuted`: this port has no voice chat, so
  nobody is muted. `PLT_GetNumPartyMembers` reads the Xbox LIVE Party through `XamPartyGetUserList`, which the runtime
  answers with no party, so the lobby's Invite Friends always takes the `XamShowFriendsUI` branch, guests or not.
- `XNetXnAddrToInAddr` maps a participant's XNADDR to a synthetic address (10.83.x.y), hashed from its XUID; a member whose
  address is taken moves to the next free one, in slot order so every console agrees, and keeps it until it leaves. QoS
  answers with the host's advertised listen data.
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
generation, token, size and rate before routing, and a frame never leaves its own match. Congestion loses frames as a
network would; only a peer that stays behind ends its pair. A relayed pair waits up to 30 s (the room's seat grace) for a
lost room connection to return, its datagrams lost meanwhile, and goes on over the new connection; it never outlives the
party. A member's LAN addresses reach only members behind the same address. A game running over Direct links does not need the service once it has started: if the
service ends the party, the game plays on and the party ends with the game's session. Every match has one 15 s
establishment budget.

## SNES gameplay online

Four SNES gameplay mechanics can be used online: **Directional Combat**, **SNES Throw Control**, **Disable Double Jump**
and **SNES Dash** ([snes-gameplay.md](snes-gameplay.md)).
Support is global, choices are per player. A mechanic is available in a match only when every PC supports it with the
same data; once it is, each player's own On or Off applies to the turtle that player controls, and another player's Off
never takes it away. **Classic Soundtrack** is not part of this: it is each PC's own presentation, follows that player's
F9 and launcher choice at once, online too, and needs nothing from the other players. Cheats stay off online, and F10
only says **Cheats are unavailable while Online play is enabled.**.

- **Supports.** A mechanic needs the gameplay data of a Ready import. Without it, or with an older import that only has
  the soundtrack, it is unsupported, so nobody in that match has it; the match plays normally.
- **Data identity.** Directional Combat is identified by the facing table, Throw Control by that table and the throw hold
  time, Disable Double Jump by its rule version, Dash by its modes and the dash hold and tap times; the hold and tap times carry the 50 or 60 Hz clock of the imported ROM. A PAL and
  an NTSC import share Directional Combat and Disable Double Jump; Throw Control and Dash are unavailable
  for that match (F9 shows **Incompatible SNES data**).
- **Dash mode.** Dash is one choice per player with three modes, Off, Auto and Manual, announced as `dash` plus
  `dash_mode`; the room's table gives each seat its own mode. A service without the `snes_modes` capability is never sent
  Manual (that player's Dash is Off for the match, F9 says so) and has no Disable Double Jump.
- **Table.** Every member announces `supports`, `enabled` and the identities to the match room. The room answers each
  member with the same table: the `supported` mechanics and, per party slot, that player's choice among them. Every
  client checks the supported set against its own rule; it can only shrink.
- **Who uses which row.** One online player per PC: the PC's party slot is its seat, and its controller drives its turtle.
  Each PC applies only its own seat's row, and only to the turtle its controller drives. The game's replication master
  for a turtle is its owner's PC, which decides that turtle's facing, throws and dash; the other PCs receive the result,
  so every screen shows each turtle with its owner's choices.
- **Lock per gameplay match.** When the game starts a match (`XSessionStart`) the client asks the room to lock that
  match's round. The first request fixes the table for everyone, and each client uses it whole or, if its own check
  cannot back the supported set, not at all (every client runs the same check). The table holds until the match ends
  (`XSessionEnd`); F9 gameplay changes during the match are announced afterwards and the next match of the same party
  locks a new table. A room reconnect does not change a locked table.
- **No answer, no mechanics.** Until the locked table arrives every mechanic is off. Without an answer within 5 s, or with
  a service that does not negotiate, the client locks all off and, when the room is reachable, asks the room to lock that
  round all off for everyone. A client that locked all off and then learns the room locked something on leaves the party
  (reason `snes_mismatch`).
- **F9 online.** Classic Soundtrack shows the local On or Off and switches at once. A gameplay row shows the player's own
  On or Off while the match supports it (with **Waiting for players** until everyone has announced), and **Unavailable**
  with the reason otherwise: another player does not support it, the online service does not support it, or
  **Incompatible SNES data**. During a locked match **Applies to the next online match** is shown under the gameplay rows.
- **Older peers and services.** A client or service without this negotiation announces or answers nothing, which reads as
  unsupported: those mechanics are unavailable to everyone and the match is otherwise unchanged. A client from before
  the Dash modes has another Dash identity, so Dash is unavailable between it and a new client.

## Privacy and usage statistics

The online service stores anonymous aggregate usage statistics: player counts, sessions, unique players, rooms created
and joined, how many players each room reached and how long it lasted, how many rooms chose the relay, the port version,
the platform (Windows, Linux, Steam Deck), the country, and which turtles appear in online parties. Only the maintainer
can see them, on a private, token-protected page of the service.

- Game traffic goes directly between the players' PCs (P2P) whenever it can. A pair that cannot connect directly is
  relayed by the service, which forwards that pair's packets unread and unstored.
- The statistics store no player names, Online names, friend codes, tokens, addresses or room ids. The country is worked
  out by Cloudflare from your connection and kept only as a count per day; the game never sends one, and the address is
  not kept for these statistics.
- **Unique players** need one thing per player: for each day you played, the service stores an anonymous key it derives
  from your account with a keyed hash. The key cannot be turned back into your account without the service's secret, is
  never shown on the dashboard or sent to any client, and is deleted after two years.
- **Turtle statistics**: when a match starts, the host's game sends the numbers of the turtles in the party. The service
  keeps only daily counts of turtles, host turtles, party sizes and party compositions, with nothing about who played
  them. Nothing is sent for offline play, local multiplayer, a party of one, or with Online play Off. The report goes over
  the connection the game already has, is sent once, and a refused or lost report changes nothing in the game.
- Saves, scores, kills, deaths, gameplay inputs, Ping and FPS are not part of these statistics.

The service still needs your account, Online name, friends and, while you are in a party, your address and port to
connect you to other players; that is not part of these statistics. Definitions, tables and retention are in
[server/tmnt-lobby/README.md](../server/tmnt-lobby/README.md).

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

A normal log keeps the online connection state, sign-in, party and room changes, transport selection for each peer,
relay fallback, peers leaving and session joins and refusals. They never include tokens and do not log every packet.
Detailed logging adds the rest of what the game and ReXGlue write.

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

- More than one online player per PC: one controller carries the online profile.
- Host migration: the game has none; when the host leaves, the party ends and the guests return to the menu.
- The game's own timeouts are kept; its 15 s wait for the host's answer after joining is not tied to the
  establishment budget. A join the service refuses ends in the game's own failure dialog (session full, session gone,
  unable to connect or its generic error), and the game's own join stays pending until the service has answered.
- The UPnP check only proves the router would open a port: no mapping is kept during a match. Direct is hole punching
  from the gameplay socket, and the relay takes over any pair that cannot punch through.
- Closing the window ends the process at once: friends see the player offline immediately, and the room gives the seat
  the usual 30 s grace.
- Deployment: the online service must be published to Cloudflare by hand (`server/tmnt-lobby/README.md`) before the
  game can go online; until then Online play reports that the service is unavailable.
