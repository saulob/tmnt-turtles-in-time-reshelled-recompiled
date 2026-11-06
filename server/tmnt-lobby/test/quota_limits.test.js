// Budgets for writes that are real changes, so no dedupe can stop them, and the sign-in limiter's key table. Every claim about
// writes is measured in rows SQLite changed, not in replies.
import assert from 'node:assert/strict';
import { test } from 'node:test';

import { PROTECTION, RateLimiter } from '../src/abuse.js';
import { Hub } from '../src/hub.js';
import { BUCKETS, LIMITS, PROTOCOL, encodeRelayFrame, joinTokenHash } from '../src/protocol.js';
import { MatchRoom } from '../src/room.js';
import { LocalConn, LocalHost, sqliteStore } from '../local/host.js';
import { befriend, buildParty, greeted, identity, makeHub } from './harness.js';

const SIGNAL_GAP_MS = Math.ceil(1000 / BUCKETS.signal.perSecond);
const SOCIAL_GAP_MS = Math.ceil(1000 / BUCKETS.social.perSecond);

const rowsChanged = (host) => host.sql.get('SELECT total_changes() AS n').n;

function rowsDuring(hosts, fn) {
  const before = hosts.map(rowsChanged);
  fn();
  return hosts.reduce((sum, host, i) => sum + rowsChanged(host) - before[i], 0);
}

function abuseCounts(env) {
  env.hub.flushDiagnostics();
  return Object.fromEntries(env.hub.telemetrySummary(3).abuse.map((a) => [a.value, a.count]));
}

const roomHost = (env, match) => env.rooms.ensure(match).host;
const nextState = (socket) => {
  socket.flip = socket.flip === 'lost' ? 'connected' : 'lost';
  return socket.flip;
};

// Flips until refused; a refused state is sent again next time, as it was never applied.
function flipUntilLimited(env, socket, peer) {
  let accepted = 0;
  for (let i = 0; i < PROTECTION.linkChange.burst * 3; i++) {
    env.clock.now += SIGNAL_GAP_MS;
    const state = nextState(socket);
    const { code } = socket.call('match.link', { peer, state });
    if (code === 'limit') {
      socket.flip = state === 'lost' ? 'connected' : 'lost';
      return accepted;
    }
    assert.equal(code, 'ok');
    accepted += 1;
  }
  assert.fail('never limited');
}

function flipRefused(env, socket, peer) {
  env.clock.now += SIGNAL_GAP_MS;
  const { code } = socket.call('match.link', { peer, state: nextState(socket) });
  socket.flip = socket.flip === 'lost' ? 'connected' : 'lost';
  return code === 'limit';
}

test('the link budget is the agreed one', () => {
  assert.deepEqual(PROTECTION.linkChange, { burst: 40, perSecond: 1 / 5 });
});

test('match.link: a full party setting up, losing a peer and setting up again stays inside the budget', () => {
  const env = makeHub();
  const { rooms } = buildParty(env, 4);
  const peers = (slot) => [1, 2, 3, 4].filter((p) => p !== slot);
  for (let round = 0; round < 3; round++) {
    for (const room of rooms) {
      for (const peer of peers(room.data.slot)) {
        for (const state of ['connecting', 'connected']) {
          env.clock.now += SIGNAL_GAP_MS;
          assert.equal(room.call('match.link', { peer, state }).code, 'ok');
        }
      }
    }
  }
  for (const room of rooms) {
    for (const peer of peers(room.data.slot)) {
      env.clock.now += SIGNAL_GAP_MS;
      assert.equal(room.call('match.link', { peer, state: 'lost' }).code, 'ok');
    }
  }
  assert.equal(abuseCounts(env).match_link_limited, undefined);
});

test('match.link: flipping two valid states is limited, counted, and comes back at the refill rate', () => {
  const env = makeHub();
  const { rooms } = buildParty(env, 2);
  assert.equal(rooms[0].call('match.link', { peer: 2, state: 'connected' }).code, 'ok');
  rooms[0].flip = 'connected';
  const accepted = flipUntilLimited(env, rooms[0], 2);
  assert.ok(accepted >= PROTECTION.linkChange.burst && accepted < PROTECTION.linkChange.burst * 1.2, `${accepted} accepted`);
  env.advance(PROTECTION.flushMs + 1000);
  assert.ok(abuseCounts(env).match_link_limited >= 1);
});

test('match.link: an unchanged report spends nothing and is answered as before', () => {
  const env = makeHub();
  const { rooms, match } = buildParty(env, 2);
  rooms[0].call('match.link', { peer: 2, state: 'connected' });
  for (let i = 0; i < 30; i++) {
    env.clock.now += SIGNAL_GAP_MS;
    assert.equal(rooms[0].call('match.link', { peer: 2, state: 'connected' }).code, 'ok');
  }
  assert.equal(roomHost(env, match).sql.get("SELECT v FROM room_meta WHERE k = 'link_budget_1'"), undefined);
});

for (const [name, leave] of [['a reconnect', (socket) => socket.drop()], ['replacing the socket', () => {}]]) {
  test(`match.link: ${name} keeps the seat budget, and the links it clears report again for free`, () => {
    const env = makeHub();
    const { rooms, match, data } = buildParty(env, 3);
    rooms[0].call('match.link', { peer: 2, state: 'connected' });
    rooms[0].flip = 'connected';
    flipUntilLimited(env, rooms[0], 2);
    leave(rooms[0]);
    const back = env.room(match);
    assert.equal(back.hello(data[0]).code, 'ok');
    for (const peer of [2, 3]) assert.equal(back.call('match.link', { peer, state: 'connected' }).code, 'ok');
    back.flip = 'connected';
    assert.equal(flipRefused(env, back, 2), true);
  });
}

test('match.link: the seat budget survives the object hibernating', () => {
  const env = makeHub();
  const { rooms, match } = buildParty(env, 2);
  rooms[0].call('match.link', { peer: 2, state: 'connected' });
  rooms[0].flip = 'connected';
  flipUntilLimited(env, rooms[0], 2);
  const host = roomHost(env, match);
  rooms[0].room = new MatchRoom(host, { alarmAt: host.alarmAt });
  assert.equal(flipRefused(env, rooms[0], 2), true);
});

test('match.link: another seat of the room has its own budget', () => {
  const env = makeHub();
  const { rooms } = buildParty(env, 3);
  rooms[0].call('match.link', { peer: 2, state: 'connected' });
  rooms[0].flip = 'connected';
  flipUntilLimited(env, rooms[0], 2);
  rooms[1].call('match.link', { peer: 1, state: 'connected' });
  for (const state of ['lost', 'connected', 'connecting']) {
    env.clock.now += SIGNAL_GAP_MS;
    assert.equal(rooms[1].call('match.link', { peer: 1, state }).code, 'ok');
  }
});

test('match.link: a refused flip changes no row anywhere and tells nobody', () => {
  const env = makeHub();
  const { rooms, match } = buildParty(env, 2);
  rooms[0].call('match.link', { peer: 2, state: 'connected' });
  rooms[0].flip = 'connected';
  flipUntilLimited(env, rooms[0], 2);
  rooms[1].inbox = [];
  const hosts = [roomHost(env, match), env.host];
  for (let i = 0; i < 5; i++) assert.equal(rowsDuring(hosts, () => assert.equal(flipRefused(env, rooms[0], 2), true)), 0);
  assert.deepEqual(rooms[1].inbox, []);
});

test('match.link: an exhausted seat still falls back to the relay and keeps relaying', () => {
  const env = makeHub();
  const { rooms, token } = buildParty(env, 2);
  rooms[0].call('match.link', { peer: 2, state: 'connecting' });
  rooms[0].flip = 'connected';
  flipUntilLimited(env, rooms[0], 2);
  env.clock.now += SIGNAL_GAP_MS;
  assert.equal(rooms[0].call('match.transport', { peer: 2, transport: 'relay' }).code, 'ok');
  rooms[0].relay(encodeRelayFrame(1, 2, token, 1, Uint8Array.from([1])));
  assert.equal(rooms[1].binary.length, 1);
});

test('match.link: an exhausted seat keeps its reconnect grace', () => {
  const env = makeHub();
  const { rooms, match, data } = buildParty(env, 2);
  rooms[1].call('match.link', { peer: 1, state: 'connected' });
  rooms[1].flip = 'connected';
  flipUntilLimited(env, rooms[1], 1);
  rooms[1].drop();
  assert.equal(rooms[0].events('match.peer_lost').length, 1);
  env.advance(LIMITS.graceMs - 1000);
  const back = env.room(match);
  assert.equal(back.hello(data[1]).code, 'ok');
  assert.equal(rooms[0].events('match.peer_back').length, 1);
});

test('match.link: loopback is exempt only with the local flag', () => {
  const env = makeHub();
  const token = 'a'.repeat(32);
  const match = '0123456789abcdef';
  const host = new LocalHost(sqliteStore(':memory:'), { now: () => env.clock.now });
  host.hub = () => {};
  const room = new MatchRoom(host, {});
  const hash = joinTokenHash(token);
  room.apply({ match, rev: 1, members: [{ slot: 1, name: 'A', hash }, { slot: 2, name: 'B', hash }] });
  const replies = [];
  const conn = new LocalConn({ send: (text) => replies.push(JSON.parse(text)), close() {} });
  conn.setAttachment(room.admit('127.0.0.1'));
  host.conns.add(conn);
  const call = (op, body) => {
    room.message(conn, JSON.stringify({ op, seq: 1, ...body }));
    return replies.at(-1).code;
  };
  assert.equal(call('match.hello', { proto: PROTOCOL, match, slot: 1, token, relay: true }), 'ok');
  const codes = [];
  for (let i = 0; i < PROTECTION.linkChange.burst + 5; i++) {
    env.clock.now += SIGNAL_GAP_MS;
    codes.push(call('match.link', { peer: 2, state: i % 2 ? 'connected' : 'lost' }));
  }
  assert.equal(codes.at(-1), 'limit');
  host.sql.close();

  const local = makeHub({ local: true });
  const { match: localMatch, data } = buildParty(local, 2, { connect: false });
  const socket = local.room(localMatch, null, { ip: '127.0.0.1' });
  assert.equal(socket.hello(data[0]).code, 'ok');
  for (let i = 0; i < PROTECTION.linkChange.burst + 5; i++) {
    local.clock.now += SIGNAL_GAP_MS;
    assert.equal(socket.call('match.link', { peer: 2, state: i % 2 ? 'connected' : 'lost' }).code, 'ok');
  }
});

const sustained = (seconds, fn) => {
  for (let t = 0; t < seconds; t++) fn(t);
};

test('write amplification: ten minutes of match.link flipping cost no more than the budget allows', () => {
  const env = makeHub();
  const { rooms, match } = buildParty(env, 2);
  const hosts = [roomHost(env, match), env.host];
  rooms[1].call('match.link', { peer: 1, state: 'connected' });
  rooms[1].flip = 'connected';
  env.hub.flushDiagnostics();
  const perFlip = rowsDuring(hosts, () => rooms[1].call('match.link', { peer: 1, state: nextState(rooms[1]) }));
  assert.ok(perFlip > 0 && perFlip <= 12, `${perFlip} rows per flip`);

  flipUntilLimited(env, rooms[1], 1);
  const seconds = 600;
  let accepted = 0;
  const rows = rowsDuring(hosts, () => sustained(seconds * 10, () => {
    env.clock.now += 100;
    const state = rooms[1].flip === 'lost' ? 'connected' : 'lost';
    if (rooms[1].call('match.link', { peer: 1, state }).code === 'ok') {
      rooms[1].flip = state;
      accepted += 1;
    }
  }));
  assert.ok(accepted <= seconds * PROTECTION.linkChange.perSecond + 1, `${accepted} accepted in ${seconds} s`);
  const perSecond = rows / seconds;
  assert.ok(perSecond <= perFlip * PROTECTION.linkChange.perSecond + 1, `${perSecond} rows/s`);
});

test('sign-in: a refused new player takes no limiter key, and changes no row', () => {
  const env = makeHub();
  const ip = '198.51.100.2';
  for (let n = 0; n < LIMITS.registrationsPerIpPerDay; n++) greeted(env, 5000 + n, { ip }).drop();
  const keys = env.hub.guards.hello.buckets.size;
  const socket = env.connect(identity(9), { ip });
  for (let n = 0; n < 40; n++) {
    const who = identity(100000 + n);
    assert.equal(rowsDuring([env.host], () => {
      assert.equal(socket.hello({ player: who.player, token: who.token }).code, 'limit');
    }), 0);
  }
  assert.equal(env.hub.guards.hello.buckets.size, keys);
  assert.equal(env.hub.stats().players, LIMITS.registrationsPerIpPerDay);
});

test('sign-in: a registration spends the new account\'s budget, and the account limit still holds after it', () => {
  const env = makeHub();
  const { player } = identity(1);
  greeted(env, 1).drop();
  assert.equal(env.hub.guards.hello.tokens(player, env.clock.now), PROTECTION.hello.burst - 1);
  const codes = [];
  for (let i = 0; i < PROTECTION.hello.burst; i++) {
    const client = env.connect(identity(1));
    codes.push(client.hello().code);
    client.drop();
  }
  assert.deepEqual(codes, [...Array(PROTECTION.hello.burst - 1).fill('ok'), 'limit']);
});

test('sign-in: wrong tokens take no key and cannot lock the real player out', () => {
  const env = makeHub();
  const real = identity(1);
  greeted(env, 1).drop();
  env.clock.now += 1000 / PROTECTION.hello.perSecond;
  const before = env.hub.guards.hello.tokens(real.player, env.clock.now);
  env.host.sql.run('UPDATE players SET token_hash = ? WHERE player = ?', 'f'.repeat(64), real.player);
  const keys = env.hub.guards.hello.buckets.size;
  for (let i = 0; i < 40; i++) {
    const attempt = env.connect(real);
    assert.equal(attempt.hello().code, 'unauthorized');
    attempt.drop();
  }
  assert.equal(env.hub.guards.hello.buckets.size, keys);
  assert.equal(env.hub.guards.hello.tokens(real.player, env.clock.now), before);
});

test('limiter: a throttled key outlives a flood of new keys, and keeps exactly what it had spent', () => {
  const limiter = new RateLimiter({ burst: 12, perSecond: 0 }, 64);
  for (let i = 0; i < 12; i++) limiter.take('account', 1000);
  assert.equal(limiter.take('account', 1000), false);
  for (let i = 0; i < 10000; i++) limiter.take(`disposable${i}`, 1000);
  assert.equal(limiter.buckets.has('account'), true);
  assert.equal(limiter.take('account', 1000), false);
  assert.ok(limiter.buckets.size <= 64);
});

test('limiter: several partly spent keys are kept in order of how little they have left', () => {
  const limiter = new RateLimiter({ burst: 12, perSecond: 0 }, 8);
  for (const [key, spent] of [['heavy', 12], ['middle', 6]]) for (let i = 0; i < spent; i++) limiter.take(key, 1000);
  for (let i = 0; i < 1000; i++) limiter.take(`disposable${i}`, 1000);
  assert.equal(limiter.tokens('heavy', 1000), 0);
  assert.equal(limiter.tokens('middle', 1000), 6);
});

test('limiter: idle keys are pruned before any busy key, and the table stays bounded', () => {
  const limiter = new RateLimiter({ burst: 2, perSecond: 1 }, 8);
  for (let i = 0; i < 7; i++) limiter.take(`idle${i}`, 1000);
  limiter.take('busy', 100000);
  limiter.take('busy', 100000);
  limiter.take('newest', 100000);
  assert.deepEqual([...limiter.buckets.keys()], ['busy', 'newest']);
  assert.ok(limiter.buckets.size <= 8);
});

test('sign-in: an account at its limit stays there through a flood of unrelated identities', () => {
  const env = makeHub();
  const account = identity(1);
  for (let i = 0; i < PROTECTION.hello.burst; i++) greeted(env, 1, { ip: '198.51.100.1' }).drop();
  const guard = env.hub.guards.hello;
  for (let n = 0; n < PROTECTION.trackedKeys * 3; n++) guard.take(identity(100000 + n).player, env.clock.now);
  assert.ok(guard.buckets.size <= PROTECTION.trackedKeys);
  assert.equal(env.connect(account, { ip: '198.51.100.1' }).hello().code, 'limit');
});

function spendLinkBudget(env, rooms, seat = 1) {
  rooms[seat].call('match.link', { peer: 1, state: 'connected' });
  rooms[seat].flip = 'connected';
  return flipUntilLimited(env, rooms[seat], 1);
}

// A reconnect clears the links, so the first report of each is free and only a change after it is charged.
function refusedAfterFirstReport(env, socket) {
  socket.call('match.link', { peer: 1, state: 'connected' });
  socket.flip = 'connected';
  return flipRefused(env, socket, 1);
}

function invitedInto(env, host, guest) {
  befriend(host, guest);
  env.clock.now += SOCIAL_GAP_MS;
  return guest.call('invite.accept', { invite: host.call('invite.send', { player: guest.who.player }).invite });
}

test('match.link: the budget belongs to the player, so reconnects keep it and a new occupant of the slot starts clean', () => {
  const env = makeHub();
  const { clients, rooms, data, match } = buildParty(env, 2);
  spendLinkBudget(env, rooms);
  rooms[1].drop();
  const back = env.room(match);
  assert.equal(back.hello(data[1]).code, 'ok');
  assert.equal(refusedAfterFirstReport(env, back), true);
  const replaced = env.room(match);
  assert.equal(replaced.hello(data[1]).code, 'ok');
  back.drop();
  assert.equal(refusedAfterFirstReport(env, replaced), true);

  replaced.drop();
  clients[1].call('party.leave');
  const joined = invitedInto(env, clients[0], greeted(env, 300));
  assert.equal(joined.slot, 2);
  const fresh = env.room(match);
  fresh.hello(joined);
  fresh.call('match.link', { peer: 1, state: 'connected' });
  fresh.flip = 'connected';
  assert.ok(flipUntilLimited(env, fresh, 1) >= PROTECTION.linkChange.burst);
});

test('match.link: leaving and joining again at another slot buys nothing', () => {
  const env = makeHub();
  const { clients, rooms, match } = buildParty(env, 3);
  spendLinkBudget(env, rooms);
  rooms[1].drop();
  clients[1].call('party.leave');
  invitedInto(env, clients[0], greeted(env, 300));
  const moved = invitedInto(env, clients[0], clients[1]);
  assert.equal(moved.slot, 4);
  const socket = env.room(match);
  assert.equal(socket.hello(moved).code, 'ok');
  socket.call('match.link', { peer: 1, state: 'connected' });
  socket.flip = 'connected';
  assert.ok(flipUntilLimited(env, socket, 1) < 5);
});

test('match.link: the key identifies nobody on the wire, and two seats have two budgets', () => {
  const env = makeHub();
  const { rooms, match } = buildParty(env, 3);
  spendLinkBudget(env, rooms, 1);
  assert.ok(spendLinkBudget(env, rooms, 2) >= PROTECTION.linkChange.burst);
  const { room } = env.rooms.ensure(match);
  const owner = room.getMeta('owner_2');
  assert.match(owner, /^[0-9a-f]{16}$/);
  const seen = JSON.stringify([rooms[0].inbox, rooms[1].inbox, rooms[2].inbox, rooms[1].helloReply]);
  assert.ok(!seen.includes(owner));
});

test('a refused join to a full party is recorded once per account and window, and writes nothing more', () => {
  const env = makeHub();
  const host = greeted(env, 1);
  const guests = [2, 3, 4, 5, 6].map((n) => greeted(env, n, { ip: '198.51.100.40' }));
  for (const guest of guests) {
    befriend(host, guest);
    env.clock.now += SOCIAL_GAP_MS;
  }
  const { session } = host.call('pm.host', { slots: 4 });
  for (const guest of guests.slice(0, 3)) {
    env.clock.now += SOCIAL_GAP_MS;
    assert.equal(guest.call('pm.join', { session }).code, 'ok');
  }
  const counted = () => env.host.sql.get("SELECT COALESCE(SUM(n), 0) AS n FROM tel_counters WHERE k = 'server.party_full'").n;
  const join = (who) => {
    env.clock.now += SOCIAL_GAP_MS;
    return who.call('pm.join', { session }).code;
  };
  assert.equal(join(guests[3]), 'party_full');
  assert.equal(counted(), 1);
  assert.equal(rowsDuring([env.host], () => {
    for (let i = 0; i < 20; i++) assert.equal(join(guests[3]), 'party_full');
  }), 0);
  assert.equal(join(guests[4]), 'party_full');
  assert.equal(counted(), 2);
  for (let t = 0; t < PROTECTION.rejectWindowMs; t += 60000) {
    env.clock.now += 60000;
    host.call('pm.host', { slots: 4 });
  }
  assert.equal(join(guests[3]), 'party_full');
  assert.equal(counted(), 3);
});

test('a four-player party forms, a declined friend is invited again, and nothing is refused on the way', () => {
  const env = makeHub();
  const host = greeted(env, 1);
  host.call('party.create');
  const guests = [2, 3, 4].map((n) => greeted(env, n, { ip: '198.51.100.40' }));
  for (const guest of guests) {
    befriend(host, guest);
    env.clock.now += SOCIAL_GAP_MS;
  }
  const sent = guests.map((guest) => host.call('invite.send', { player: guest.who.player }));
  assert.deepEqual(sent.map((r) => r.code), ['ok', 'ok', 'ok']);
  assert.equal(guests[2].call('invite.decline', { invite: sent[2].invite }).code, 'ok');
  env.clock.now += SOCIAL_GAP_MS;
  const again = host.call('invite.send', { player: guests[2].who.player });
  assert.equal(again.code, 'ok');
  const joined = [sent[0], sent[1], again].map((reply, i) => guests[i].call('invite.accept', { invite: reply.invite }));
  assert.deepEqual(joined.map((r) => [r.code, r.slot]), [['ok', 2], ['ok', 3], ['ok', 4]]);
  for (const guest of guests) assert.equal(guest.call('party.leave').code, 'ok');
  assert.equal(host.call('party.leave').code, 'ok');
});
