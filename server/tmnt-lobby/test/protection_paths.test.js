// Protections the other suites leave unproved: the paths added after the first hardening pass, and the numbers, bindings and
// dashboard wiring that fail silently when they drift.
import assert from 'node:assert/strict';
import { readFileSync } from 'node:fs';
import { test } from 'node:test';

import { DIAGNOSTICS, PROTECTION } from '../src/abuse.js';
import { ADMIN_HTML, ADMIN_JS } from '../src/admin_page.js';
import { sampleBucket, SAMPLE_MS } from '../src/metrics.js';
import { BUCKETS, LIMITS, PROTOCOL, RELAY, joinTokenHash } from '../src/protocol.js';
import { MatchRoom } from '../src/room.js';
import { LocalConn, LocalHost, sqliteStore } from '../local/host.js';
import { befriend, buildParty, greeted, identity, makeHub } from './harness.js';

const bytes = (...values) => Uint8Array.from(values);
const MINUTE = 60 * 1000;

function recordWrites(host) {
  const writes = [];
  const run = host.sql.run;
  host.sql.run = (query, ...args) => {
    if (/^\s*(INSERT|UPDATE|DELETE)/.test(query)) writes.push({ query: query.replace(/\s+/g, ' ').trim(), args });
    return run(query, ...args);
  };
  return writes;
}

const rowsChanged = (host) => host.sql.get('SELECT total_changes() AS n').n;

function rowsDuring(host, fn) {
  const before = rowsChanged(host);
  fn();
  return rowsChanged(host) - before;
}

const abuseCounts = (env) => Object.fromEntries(env.hub.telemetrySummary(3).abuse.map((a) => [a.value, a.count]));
const roomOf = (env, match) => env.rooms.ensure(match);

function signIn(env, n, options, extra) {
  const client = env.connect(identity(n), options);
  return { client, reply: client.hello(extra) };
}

test('every number of the protection policy is the agreed one', () => {
  assert.equal(PROTECTION.seenRefreshMs, 15 * MINUTE);
  assert.equal(PROTECTION.sessionWindowMs, 10 * MINUTE);
  assert.equal(PROTECTION.rejectWindowMs, 10 * MINUTE);
  assert.equal(PROTECTION.advertiseRefreshMs, 45 * 1000);
  assert.equal(PROTECTION.flushMs, 5 * MINUTE);
  assert.deepEqual(PROTECTION.inviteAccount, { burst: 20, perSecond: 1 / 3 });
  assert.deepEqual(PROTECTION.inviteAddress, { burst: 60, perSecond: 1 / 2 });
  assert.deepEqual(PROTECTION.friendAccount, { burst: 30, perSecond: 1 / 2 });
  assert.deepEqual(PROTECTION.telemetryAccount, { burst: 12, perSecond: 0.2 });
  assert.deepEqual(RELAY.invalid, { burst: 200, perSecond: 5 });
  assert.deepEqual(BUCKETS.social, { burst: 10, perSecond: 1 });
  assert.deepEqual(BUCKETS.signal, { burst: 40, perSecond: 5 });
});

test('the deployed binding is the limiter the code reads, with its agreed namespace and rate', () => {
  const toml = readFileSync(new URL('../wrangler.toml', import.meta.url), 'utf8');
  assert.match(toml, /\[\[ratelimits\]\]\s+name = "WS_UPGRADE_LIMITER"\s+namespace_id = "3003"\s+simple = \{ limit = 30, period = 60 \}/);
  const edge = readFileSync(new URL('../src/edge.js', import.meta.url), 'utf8');
  assert.match(edge, /env\.WS_UPGRADE_LIMITER/);
});

test('sign-ins share one account budget across open sockets and connection tests', () => {
  const env = makeHub();
  const codes = [];
  for (let i = 0; i < 13; i++) codes.push(signIn(env, 1, undefined, { presence: i % 2 === 0 }).reply.code);
  assert.deepEqual(codes, [...Array(12).fill('ok'), 'limit']);
});

test('a connection test writes no session, however long since the last one', () => {
  const env = makeHub();
  signIn(env, 1).client.drop();
  env.clock.now += PROTECTION.sessionWindowMs + 1000;
  const writes = recordWrites(env.host);
  const { reply } = signIn(env, 1, undefined, { presence: false });
  assert.equal(reply.code, 'ok');
  assert.deepEqual(writes.filter((w) => /UPDATE players SET session|metrics_daily/.test(w.query) && w.args[1] !== 'abuse'), []);
  assert.equal(env.hub.metricsHistory(1).totals.sessions, 1);
});

test('reconnecting writes no row inside a usage sample, and one sample row per bucket', () => {
  const env = makeHub();
  env.clock.now = sampleBucket(env.clock.now) + SAMPLE_MS;
  signIn(env, 1).client.drop();
  env.hub.flushDiagnostics();
  for (let i = 0; i < 5; i++) {
    env.clock.now += 16000;
    assert.equal(rowsDuring(env.host, () => signIn(env, 1).client.drop()), 0, `reconnect ${i}`);
  }
  const samples = () => env.host.sql.get('SELECT COUNT(*) AS n FROM metrics_samples').n;
  assert.equal(samples(), 1);
  env.clock.now = sampleBucket(env.clock.now) + SAMPLE_MS;
  signIn(env, 1).client.drop();
  assert.equal(samples(), 2);
  assert.equal(rowsDuring(env.host, () => {
    env.clock.now += 16000;
    signIn(env, 1).client.drop();
  }), 0);
});

test('a refused protocol is written once per socket, also after the refusal window', () => {
  const env = makeHub();
  const client = env.connect(identity(1));
  assert.equal(client.hello({ proto: PROTOCOL + 1 }).code, 'proto');
  env.clock.now += PROTECTION.rejectWindowMs + 1000;
  assert.equal(client.hello({ proto: PROTOCOL + 1 }).code, 'proto');
  assert.equal(env.hub.metricsHistory(1).totals.incompatible_rejects, 1);
});

test('development exemptions need the local flag: production loopback addresses are limited', () => {
  const env = makeHub();
  const codes = [];
  for (let i = 0; i < 13; i++) {
    const { client, reply } = signIn(env, 1, { ip: '127.0.0.1' });
    codes.push(reply.code);
    client.drop();
  }
  assert.equal(codes.at(-1), 'limit');

  const token = 'a'.repeat(32);
  const match = '0123456789abcdef';
  const host = new LocalHost(sqliteStore(':memory:'), { now: () => env.clock.now });
  host.hub = () => {};
  const room = new MatchRoom(host, {});
  room.apply({ match, rev: 1, members: [{ slot: 1, name: 'A', hash: joinTokenHash(token) }] });
  const replies = [];
  for (let i = 0; i < 22; i++) {
    const conn = new LocalConn({ send: (text) => replies.push(JSON.parse(text)), close() {} });
    conn.setAttachment(room.admit('127.0.0.1'));
    host.conns.add(conn);
    room.message(conn, JSON.stringify({ op: 'match.hello', seq: 1, proto: PROTOCOL, match, slot: 1, token, relay: true }));
    room.closed(conn);
    host.conns.delete(conn);
  }
  assert.equal(replies.filter((r) => 're' in r).at(-1).code, 'limit');
  host.sql.close();
});

test('the socket cap groups an IPv6 /64 whatever its spelling, and counts its refusals', () => {
  const env = makeHub();
  const spelled = (n) => [`2001:db8:1:2::${n.toString(16)}`, `2001:0DB8:0001:0002:0:0:0:${n.toString(16)}`][n % 2];
  const open = Array.from({ length: LIMITS.socketsPerIp }, (_, i) => env.connect(identity(i + 1), { ip: spelled(i + 1) }));
  assert.ok(open.every((c) => c.admitted));
  assert.equal(env.connect(identity(99), { ip: spelled(99) }).admitted, false);
  assert.equal(env.connect(identity(99), { ip: '2001:db8:1:3::1' }).admitted, true);
  assert.equal(abuseCounts(env).ws_upgrade_ip_cap, 1);
});

test('a new ad on an existing party spends the create budget, and the refusal writes nothing', () => {
  const env = makeHub();
  const host = greeted(env, 1);
  assert.equal(host.call('party.create').code, 'ok');
  const codes = [];
  for (let i = 0; i < 8; i++) {
    env.clock.now += 1100;
    const reply = host.call('pm.host', { slots: 2 });
    codes.push(reply.code);
    if (reply.code === 'ok') host.call('pm.cancel');
  }
  assert.deepEqual(codes, [...Array(5).fill('ok'), 'limit', 'limit', 'limit']);
  const writes = recordWrites(env.host);
  env.clock.now += 1100;
  assert.equal(host.call('pm.host', { slots: 2 }).code, 'limit');
  assert.deepEqual(writes, []);
  assert.equal(abuseCounts(env).party_create_limited >= 3, true);
  assert.equal(host.call('pm.cancel').code, 'ok');
  assert.equal(host.call('party.leave').code, 'ok');
});

test('an accept loop spends the join budget across reconnects; the refusal writes nothing and keeps the invite', () => {
  const env = makeHub();
  const host = greeted(env, 1);
  host.call('party.create');
  let guest = greeted(env, 2);
  befriend(host, guest);
  env.clock.now += 60000;
  let invite = null;
  let accepted = 0;
  for (let i = 0; i < 40 && !invite; i++) {
    env.clock.now += 1100;
    if (i === 10) {
      guest.drop();
      guest = greeted(env, 2);
    }
    const sent = host.call('invite.send', { player: guest.who.player });
    assert.equal(sent.code, 'ok', `invite ${i}`);
    const joined = guest.call('invite.accept', { invite: sent.invite });
    if (joined.code === 'limit') {
      invite = sent.invite;
    } else {
      assert.equal(joined.code, 'ok', `accept ${i}`);
      accepted += 1;
      assert.equal(guest.call('party.leave').code, 'ok');
    }
  }
  assert.ok(invite, 'never limited');
  assert.ok(accepted >= PROTECTION.joinAccount.burst);
  const writes = recordWrites(env.host);
  assert.equal(guest.call('invite.accept', { invite }).code, 'limit');
  assert.deepEqual(writes, []);
  assert.equal(env.hub.stats().members, 1);
  env.clock.now += 1000 / PROTECTION.joinAccount.perSecond;
  assert.equal(guest.call('invite.accept', { invite }).code, 'ok');
});

test('adding back a friend who asked first spends the friend budget too', () => {
  const env = makeHub();
  const targets = Array.from({ length: PROTECTION.friendAccount.burst }, (_, i) => identity(300 + i));
  signIn(env, 1).client.drop();
  targets.forEach((_, i) => signIn(env, 300 + i, { ip: `198.18.1.${i + 1}` }).client.drop());
  let socket = null;
  targets.forEach((target, i) => {
    if (i % 8 === 0) {
      socket?.drop();
      socket = greeted(env, 1);
    }
    assert.equal(socket.call('friends.add', { code: target.player }).code, 'ok', `request ${i}`);
  });
  socket.drop();
  const asker = greeted(env, 900, { ip: '198.18.2.1' });
  assert.equal(asker.call('friends.add', { code: identity(1).player }).code, 'ok');
  socket = greeted(env, 1);
  assert.equal(rowsDuring(env.host, () => {
    assert.equal(socket.call('friends.add', { code: asker.who.player }).code, 'limit');
  }), 0);
  assert.equal(env.hub.areFriends(identity(1).player, asker.who.player), false);
});

test('advertising is held to the social bucket on one socket', () => {
  const env = makeHub();
  const host = greeted(env, 1);
  const codes = Array.from({ length: BUCKETS.social.burst + 2 }, () => host.call('pm.host', { slots: 2 }).code);
  assert.deepEqual(codes, [...Array(BUCKETS.social.burst).fill('ok'), 'limit', 'limit']);
});

test('room signalling is held to the signal bucket, and a change that is refused writes nothing', () => {
  const env = makeHub();
  const { rooms, match } = buildParty(env, 2);
  const codes = [];
  for (let i = 0; i < BUCKETS.signal.burst + 2; i++) {
    codes.push(rooms[0].call('match.link', { peer: 2, state: i % 2 ? 'connected' : 'lost' }).code);
  }
  assert.deepEqual(codes, [...Array(BUCKETS.signal.burst).fill('ok'), 'limit', 'limit']);
  const writes = recordWrites(roomOf(env, match).host);
  assert.equal(rooms[0].call('match.link', { peer: 2, state: 'connecting' }).code, 'limit');
  assert.deepEqual(writes, []);
});

test('a member state that did not change is not reported, however many of its links do', () => {
  const env = makeHub();
  const { rooms, match } = buildParty(env, 3);
  assert.equal(rooms[0].call('match.link', { peer: 2, state: 'connected' }).code, 'ok');
  const writes = recordWrites(roomOf(env, match).host);
  assert.equal(rooms[0].call('match.link', { peer: 3, state: 'connected' }).code, 'ok');
  assert.deepEqual(writes.filter((w) => /UPDATE members SET reported/.test(w.query)), []);
  assert.deepEqual(writes.filter((w) => w.args[0] === 'seq'), []);
  assert.equal(rooms[0].call('match.link', { peer: 3, state: 'lost' }).code, 'ok');
  assert.equal(writes.filter((w) => /UPDATE members SET reported/.test(w.query)).length, 1);
});

test('the relayed mark of a party is written once', () => {
  const env = makeHub();
  const { diag } = buildParty(env, 3, { connect: false });
  const report = () => rowsDuring(env.host, () => env.hub.roomEvent(diag, 'room_transport', { a: 1, b: 2, transport: 'relay' }));
  const first = report();
  const second = report();
  assert.equal(first - second, 1);
  assert.equal(report(), second);
});

test('waking a room writes each lost seat once, and never moves its grace or the empty mark', () => {
  const env = makeHub();
  const { match } = buildParty(env, 2);
  const { host } = roomOf(env, match);
  host.conns.clear();
  new MatchRoom(host, { alarmAt: host.alarmAt });
  const graces = () => host.sql.all('SELECT grace FROM members ORDER BY slot').map((m) => m.grace);
  const seats = graces();
  assert.ok(seats.every((g) => g !== null));
  const empty = host.sql.get("SELECT v FROM room_meta WHERE k = 'empty_since'").v;
  env.clock.now += 5000;
  const writes = recordWrites(host);
  new MatchRoom(host, { alarmAt: host.alarmAt });
  assert.deepEqual(writes, []);
  assert.deepEqual(graces(), seats);
  assert.equal(host.sql.get("SELECT v FROM room_meta WHERE k = 'empty_since'").v, empty);
});

test('a room left empty is closed on its own deadline', () => {
  const env = makeHub();
  const { data, match } = buildParty(env, 2, { connect: false });
  const guest = env.room(match);
  assert.equal(guest.hello(data[1]).code, 'ok');
  guest.drop();
  env.advance(LIMITS.graceMs + 1000);
  assert.equal(roomOf(env, match).room.stats().ended, null);
  env.advance(LIMITS.roomIdleMs);
  assert.equal(roomOf(env, match).room.stats().ended, 'expired');
});

test('counters of a room with no other deadline soon are still flushed on theirs', () => {
  const env = makeHub();
  const { rooms, match } = buildParty(env, 2);
  assert.equal(rooms[0].call('match.leave').code, 'ok');
  const { room, host } = roomOf(env, match);
  env.advance(LIMITS.silenceMs + 1000);
  assert.ok(host.alarmAt > env.clock.now + PROTECTION.flushMs);
  room.lastAbuseFlush = env.clock.now;
  room.countAbuse('relay_abuse_closed');
  room.accepted();
  assert.equal(host.alarmAt, env.clock.now + PROTECTION.flushMs);
});

test('directory counters are flushed on their own deadline, with no reader and no other alarm', () => {
  const env = makeHub();
  env.hub.sampleDueAt = () => null;
  greeted(env, 1);
  assert.equal(env.host.alarmAt, env.hub.telemetry.lastAbuseFlush + PROTECTION.flushMs);
  env.advance(PROTECTION.flushMs + 1000);
  assert.deepEqual(env.hub.metricsHistory(1).dimensions.abuse.map((a) => a.value), ['hello_accepted', 'ws_upgrade_accepted']);
});

test('a room reports its counters once per window, none lost', () => {
  const env = makeHub();
  const { data, match } = buildParty(env, 2, { connect: false });
  const { host } = roomOf(env, match);
  const reports = [];
  const hub = host.hub;
  host.hub = (method, ...args) => {
    if (method === 'roomAbuse') reports.push(args[0]);
    return hub(method, ...args);
  };
  let refused = 0;
  for (let i = 0; i < 30; i++) {
    const socket = env.room(match);
    if (socket.hello(data[1]).code === 'limit') refused += 1;
    socket.drop();
  }
  assert.equal(refused, 18);
  assert.equal(reports.length, 1);
  env.advance(PROTECTION.flushMs + 1000);
  assert.equal(reports.length, 2);
  assert.equal(reports.flat().reduce((sum, [, n]) => sum + n, 0), refused);
});

test('frames to an empty seat, a direct pair, a closed pair or a stalled peer are never held against the sender', () => {
  const env = makeHub();
  const { rooms, token, match } = buildParty(env, 3);
  const { room } = roomOf(env, match);
  assert.equal(rooms[0].call('match.transport', { peer: 2, transport: 'direct' }).code, 'ok');
  room.closePair(1, 3, 'test');
  rooms[1].buffered = RELAY.bufferedBytes + 1;
  for (let i = 1; i <= RELAY.invalid.burst + 100; i++) {
    rooms[0].relay(rooms[0].frame(4, token, i));
    rooms[0].relay(rooms[0].frame(2, token, i));
    rooms[0].relay(rooms[0].frame(3, token, i));
    rooms[2].relay(rooms[2].frame(2, token, i));
  }
  assert.equal(rooms[0].closedWith, null);
  assert.equal(rooms[2].closedWith, null);
  assert.equal(room.faults.size, 0);
  assert.equal(rooms[1].binary.length, 0);
});

test('no relay frame writes a row, forwarded or dropped for any reason', () => {
  const env = makeHub();
  const { rooms, token, match, data } = buildParty(env, 4);
  const { room, host } = roomOf(env, match);
  rooms[0].call('match.transport', { peer: 2, transport: 'direct' });
  room.closePair(1, 3, 'test');
  const old = rooms[3];
  const fresh = env.room(match);
  fresh.hello(data[3]);
  const stranger = env.room(match);
  const writes = recordWrites(host);
  for (let i = 1; i <= 50; i++) {
    rooms[1].relay(rooms[1].frame(3, token, i));
    rooms[0].relay(rooms[0].frame(2, token, i));
    rooms[0].relay(rooms[0].frame(3, token, i));
    rooms[0].relay(rooms[0].frame(9, token, i));
    rooms[1].relay(rooms[1].frame(3, token + 1, i));
    rooms[1].relay(rooms[1].frame(3, token, i, bytes(1), 3));
    rooms[1].relay(bytes(1, 2, 3));
    old.relay(old.frame(1, token, i));
    stranger.relay(rooms[1].frame(3, token, i));
  }
  assert.equal(rooms[2].binary.length, 50);
  assert.deepEqual(writes, []);
});

test('a closed sender leaves no fault state behind', () => {
  const env = makeHub();
  const { rooms, match } = buildParty(env, 2);
  const { room } = roomOf(env, match);
  while (!rooms[1].closedWith) rooms[1].relay(bytes(1, 2, 3));
  assert.equal(room.faults.size, 1);
  rooms[1].drop();
  assert.equal(room.faults.size, 0);
});

test('one connection has its own budget inside the match budget, and a pair ended by it is counted', () => {
  const env = makeHub();
  const { rooms, token, match } = buildParty(env, 3);
  const burst = RELAY.connection.packets.burst;
  for (let i = 1; i <= burst + RELAY.overrunsToClose; i++) rooms[0].relay(rooms[0].frame(2, token, i, bytes(1)));
  assert.equal(rooms[1].binary.length, burst);
  rooms[2].relay(rooms[2].frame(1, token, 1, bytes(1)));
  assert.equal(rooms[0].binary.length, 1);
  assert.equal(rooms[0].closedWith, null);
  roomOf(env, match).room.flushAbuse({ force: true });
  roomOf(env, match).room.flush();
  assert.equal(abuseCounts(env).relay_abuse_closed, 1);
});

test('the Abuse protection panel exists and names every counter the service can write', () => {
  assert.match(ADMIN_HTML, /<div id="abuse"><\/div>/);
  const labels = ADMIN_JS.slice(ADMIN_JS.indexOf('var ABUSE = {'), ADMIN_JS.indexOf('var REJECTS'));
  for (const name of DIAGNOSTICS) assert.match(labels, new RegExp(`\\b${name}: '`), name);
});

test('counters of the directory and of a room reach the dashboard source, and nothing stored there identifies anyone', () => {
  const env = makeHub();
  const addresses = ['203.0.113.7', '203.0.113.77'];
  for (let i = 0; i < 14; i++) signIn(env, 1, { ip: addresses[1] }).client.drop();
  const { rooms, match, data, clients } = buildParty(env, 2);
  const session = clients[0].call('pm.host', { slots: 2 }).session;
  env.hub.edgeLimited(4);
  while (!rooms[1].closedWith) rooms[1].relay(bytes(1, 2, 3));
  rooms[1].drop();
  for (let i = 0; i < 22; i++) {
    const socket = env.room(match);
    socket.hello(data[1]);
    socket.drop();
  }
  env.advance(PROTECTION.flushMs + 1000);
  env.hub.flushDiagnostics();

  const shown = Object.fromEntries(env.hub.metricsHistory(1).dimensions.abuse.map((a) => [a.value, a.count]));
  for (const name of ['hello_limited', 'ws_upgrade_limited', 'relay_abuse_closed', 'room_resume_limited']) assert.ok(shown[name] > 0, name);
  assert.ok(Object.keys(shown).every((name) => DIAGNOSTICS.has(name)));

  const stored = JSON.stringify(['tel_counters', 'metrics_daily', 'metrics_daily_dimensions', 'metrics_samples', 'metrics_players']
    .map((table) => env.host.sql.all(`SELECT * FROM ${table}`)));
  const sockets = [...clients.map((c) => c.conn), ...rooms.map((r) => r.conn)].map((c) => c.attachment().cid);
  const secrets = [...addresses, '203.0.113', identity(1).player, identity(1).token, match, session, ...sockets,
    ...data.flatMap((d) => [d.diag, d.join_token]), ...clients.map((c) => c.who.player), ...clients.map((c) => c.who.token)];
  for (const secret of secrets) assert.equal(stored.includes(secret), false, secret);
});

test('a search refusal is the plain limit reply every client already handles', () => {
  const env = makeHub();
  const host = greeted(env, 1);
  for (let i = 0; i < BUCKETS.search.burst; i++) assert.equal(host.call('pm.search').code, 'ok');
  const limited = host.call('pm.search');
  assert.deepEqual(Object.keys(limited).sort(), ['code', 're']);
  assert.equal(limited.code, 'limit');
});
