// Abuse protection of the directory and the match rooms: limits that a reconnect cannot reset, writes that only happen
// when something changed, relay senders, alarms and the aggregate diagnostics.
import assert from 'node:assert/strict';
import { test } from 'node:test';

import { DIAGNOSTICS, PROTECTION } from '../src/abuse.js';
import { Hub } from '../src/hub.js';
import { CAPABILITIES, GAME_ID, LIMITS, PROTOCOL, RELAY, tokenHash } from '../src/protocol.js';
import { befriend, buildParty, greeted, identity, makeHub } from './harness.js';

const bytes = (...values) => Uint8Array.from(values);
const DIAG = 'abcdef012345';

function recordWrites(host) {
  const writes = [];
  const run = host.sql.run;
  host.sql.run = (query, ...args) => {
    writes.push({ query: query.replace(/\s+/g, ' ').trim(), args });
    return run(query, ...args);
  };
  return writes;
}

const writesTo = (writes, pattern) => writes.filter((w) => pattern.test(w.query));

function recordAlarms(host) {
  const calls = [];
  const set = host.setAlarm.bind(host);
  host.setAlarm = (ms) => {
    calls.push(ms);
    set(ms);
  };
  return calls;
}

function signIn(env, n, options) {
  const client = env.connect(identity(n), options);
  return { client, reply: client.hello() };
}

const abuseCounts = (env) => Object.fromEntries(env.hub.telemetrySummary(3).abuse.map((a) => [a.value, a.count]));

const telemetryBatch = () => ({ diag: DIAG, ctx: { game: GAME_ID, proto: PROTOCOL, version: '0.4.2', platform: 'Windows', os: '10',
  arch: 'x64', fp: 'ab', slot: 0, host: false, party: 0 }, events: [] });

test('a reconnect flood is limited per account; a normal reconnect and other accounts are not', () => {
  const env = makeHub();
  const codes = [];
  for (let i = 0; i < 14; i++) {
    const { client, reply } = signIn(env, 1);
    codes.push(reply.code);
    client.drop();
  }
  assert.deepEqual(codes, [...Array(12).fill('ok'), 'limit', 'limit']);
  assert.equal(signIn(env, 2).reply.code, 'ok');
  env.clock.now += 15000;
  assert.equal(signIn(env, 1).reply.code, 'ok');
  assert.equal(abuseCounts(env).hello_limited, 2);
});

test('a client reconnecting at its own backoff pace is never limited', () => {
  const env = makeHub();
  let delay = 1000;
  for (let i = 0; i < 60; i++) {
    const { client, reply } = signIn(env, 1);
    assert.equal(reply.code, 'ok', `reconnect ${i}`);
    client.drop();
    env.clock.now += delay;
    delay = Math.min(delay * 2, 20000);
  }
});

test('sign-ins that fail the token check never spend the account budget', () => {
  const env = makeHub();
  const { client } = signIn(env, 1);
  client.drop();
  const player = identity(1).player;
  const left = env.hub.guards.hello.tokens(player, env.clock.now);
  env.host.sql.run('UPDATE players SET token_hash = ? WHERE player = ?', 'f'.repeat(64), player);
  for (let i = 0; i < 40; i++) {
    const attempt = signIn(env, 1);
    assert.equal(attempt.reply.code, 'unauthorized');
    attempt.client.drop();
  }
  const other = env.connect(identity(1));
  assert.equal(other.hello({ player: identity(2).player }).code, 'unauthorized');
  other.drop();
  assert.equal(env.hub.guards.hello.tokens(player, env.clock.now), left);
  assert.equal(env.hub.guards.hello.buckets.has(identity(2).player), false);
  env.host.sql.run('UPDATE players SET token_hash = ? WHERE player = ?', tokenHash(identity(1).token), player);
  assert.equal(signIn(env, 1).reply.code, 'ok');
  assert.equal(abuseCounts(env).hello_limited, undefined);
});

test('development sign-ins from loopback are not limited', () => {
  const env = makeHub({ local: true });
  for (let i = 0; i < 30; i++) {
    const { client, reply } = signIn(env, 1, { ip: '127.0.0.1' });
    assert.equal(reply.code, 'ok');
    client.drop();
  }
});

test('an unchanged player record is not rewritten; a rename and the refresh interval are', () => {
  const env = makeHub();
  const writes = recordWrites(env.host);
  signIn(env, 1).client.drop();
  writes.length = 0;
  signIn(env, 1).client.drop();
  signIn(env, 1).client.drop();
  assert.equal(writesTo(writes, /UPDATE players/).length, 0);
  env.clock.now += PROTECTION.seenRefreshMs - 1000;
  signIn(env, 1).client.drop();
  assert.equal(writesTo(writes, /UPDATE players/).length, 0);
  env.clock.now += 1000;
  signIn(env, 1).client.drop();
  assert.equal(writesTo(writes, /UPDATE players/).length, 1);
  const renamed = env.connect(identity(1), { name: 'Raphael' });
  renamed.hello();
  assert.equal(writesTo(writes, /UPDATE players/).length, 2);
  assert.equal(env.hub.playerRow(identity(1).player).name, 'Raphael');
});

test('a member signing in again writes its party row only when it was away or changed', () => {
  const env = makeHub();
  const host = greeted(env, 1);
  host.call('party.create');
  const writes = recordWrites(env.host);
  const second = env.connect(identity(1));
  second.hello();
  assert.equal(writesTo(writes, /UPDATE party_members/).length, 0);
  host.drop();
  second.drop();
  const back = env.connect(identity(1));
  back.hello();
  assert.equal(writesTo(writes, /UPDATE party_members SET offline_since = NULL/).length, 1);
  const upgraded = env.connect(identity(1), { version: '0.4.3' });
  upgraded.hello();
  assert.equal(writesTo(writes, /UPDATE party_members SET offline_since = NULL/).length, 2);
});

test('a flood of limited sign-ins counts in memory and does not write a counter per event', () => {
  const env = makeHub();
  const writes = recordWrites(env.host);
  for (let i = 0; i < 120; i++) signIn(env, 1).client.drop();
  assert.ok(writesTo(writes, /tel_counters/).length <= 8, `${writesTo(writes, /tel_counters/).length} counter writes`);
  assert.equal(abuseCounts(env).hello_limited, 108);
  assert.equal(abuseCounts(env).hello_accepted, 12);
});

test('creating parties in a loop is limited per account, also across reconnects, and the refusal writes nothing', () => {
  const env = makeHub();
  const writes = recordWrites(env.host);
  let host = greeted(env, 1);
  const codes = [];
  for (let i = 0; i < 8; i++) {
    env.clock.now += 1100;
    codes.push(host.call('party.create').code);
    if (codes.at(-1) === 'ok') host.call('party.leave');
  }
  assert.deepEqual(codes, [...Array(6).fill('ok'), 'limit', 'limit']);
  host.drop();
  host = greeted(env, 1);
  writes.length = 0;
  assert.equal(host.call('party.create').code, 'limit');
  assert.equal(host.call('pm.host', { slots: 2 }).code, 'limit');
  assert.deepEqual(writes, []);
  assert.equal(host.call('party.leave').code, 'ok');
  assert.equal(host.call('pm.cancel').code, 'ok');
  assert.equal(env.hub.stats().parties, 0);
  assert.equal(abuseCounts(env).party_create_limited, 4);
  assert.equal(abuseCounts(env).party_create_accepted, 6);
});

test('creating parties is limited per address across accounts, by IPv6 /64 and IPv4-mapped alike', () => {
  const env = makeHub();
  const spelled = (n) => [`2001:db8:1:2::${n.toString(16)}`, `2001:0db8:0001:0002:0000:0000:0000:${n.toString(16).padStart(4, '0')}`][n % 2];
  const codes = [];
  for (let n = 0; n < 23; n++) signIn(env, 100 + n, { ip: `198.18.0.${n + 1}` }).client.drop();
  for (let n = 0; n < 23; n++) {
    const client = env.connect(identity(100 + n), { ip: spelled(n + 1) });
    client.hello();
    codes.push(client.call('party.create').code);
    client.call('party.leave');
    client.drop();
  }
  assert.equal(codes.filter((c) => c === 'ok').length, PROTECTION.createAddress.burst);
  assert.deepEqual(codes.slice(20), ['limit', 'limit', 'limit']);
  const elsewhere = env.connect(identity(500), { ip: '2001:db8:1:3::1' });
  elsewhere.hello();
  assert.equal(elsewhere.call('party.create').code, 'ok');
  const mapped = env.connect(identity(501), { ip: '::ffff:198.51.100.9' });
  mapped.hello();
  const plain = env.connect(identity(502), { ip: '198.51.100.9' });
  plain.hello();
  assert.equal(env.hub.guards.createAddress.buckets.has('198.51.100.9'), false);
  assert.equal(mapped.call('party.create').code, 'ok');
  assert.equal(plain.call('party.create').code, 'ok');
  assert.equal(env.hub.guards.createAddress.buckets.size, 3);
});

test('a party is created and advertised normally within the limits', () => {
  const env = makeHub();
  const host = greeted(env, 1);
  const created = host.call('pm.host', { slots: 3, rules: { stage: 2 } });
  assert.equal(created.code, 'ok');
  assert.equal(created.slot, 1);
  assert.equal(host.call('pm.host', { slots: 3, rules: { stage: 2 } }).slot, undefined);
  assert.equal(env.hub.stats().parties, 1);
  assert.equal(env.hub.stats().player_matches, 1);
});

test('joins: failed attempts cost nothing, a join loop is limited, leaving always works', () => {
  const env = makeHub();
  const host = greeted(env, 1);
  const { session } = host.call('pm.host', { slots: 2 });
  const guest = greeted(env, 2);
  befriend(host, guest);
  const writes = recordWrites(env.host);
  for (let i = 0; i < 60; i++) {
    env.clock.now += 2200;
    assert.equal(guest.call('pm.join', { session: 'f'.repeat(16) }).code, 'not_found');
    assert.equal(guest.call('invite.accept', { invite: 'f'.repeat(16) }).code, 'not_found');
  }
  assert.deepEqual(writesTo(writes, /^(INSERT|UPDATE|DELETE)/), []);

  let firstLimit = -1;
  for (let i = 0; i < 40 && firstLimit < 0; i++) {
    env.clock.now += 1100;
    host.call('pm.host', { slots: 2 });
    const reply = guest.call('pm.join', { session });
    if (reply.code === 'limit') firstLimit = i;
    else {
      assert.equal(reply.code, 'ok');
      assert.equal(guest.call('party.leave').code, 'ok');
    }
  }
  assert.ok(firstLimit >= PROTECTION.joinAccount.burst && firstLimit < 40, `limited at ${firstLimit}`);
  writes.length = 0;
  assert.equal(guest.call('pm.join', { session }).code, 'limit');
  assert.deepEqual(writes, []);
  assert.equal(guest.call('party.leave').code, 'ok');
  assert.equal(guest.call('pm.cancel').code, 'ok');
  assert.equal(abuseCounts(env).party_join_limited >= 2, true);
});

test('invites: the normal flow works, an invite loop is limited, declining stays available', () => {
  const env = makeHub();
  const host = greeted(env, 1);
  host.call('party.create');
  const guests = [2, 3, 4].map((n) => greeted(env, n));
  for (const guest of guests) befriend(host, guest);
  env.clock.now += 60000;
  const sent = guests.map((guest) => host.call('invite.send', { player: guest.who.player }));
  assert.deepEqual(sent.map((r) => r.code), ['ok', 'ok', 'ok']);
  const joined = guests.map((guest, i) => guest.call('invite.accept', { invite: sent[i].invite }));
  assert.deepEqual(joined.map((r) => [r.code, r.slot]), [['ok', 2], ['ok', 3], ['ok', 4]]);

  const loop = makeHub();
  const owner = greeted(loop, 10);
  owner.call('party.create');
  const friend = greeted(loop, 11);
  befriend(owner, friend);
  loop.clock.now += 60000;
  let firstLimit = -1;
  for (let i = 0; i < 60 && firstLimit < 0; i++) {
    loop.clock.now += 1100;
    const reply = owner.call('invite.send', { player: friend.who.player });
    if (reply.code === 'limit') firstLimit = i;
    else {
      assert.equal(reply.code, 'ok');
      assert.equal(friend.call('invite.decline', { invite: reply.invite }).code, 'ok');
    }
  }
  assert.ok(firstLimit >= PROTECTION.inviteAccount.burst, `limited at ${firstLimit}`);
  const writes = recordWrites(loop.host);
  assert.equal(owner.call('invite.send', { player: friend.who.player }).code, 'limit');
  assert.deepEqual(writes, []);
  assert.equal(friend.call('invite.decline', { invite: 'f'.repeat(16) }).code, 'ok');
  assert.equal(owner.call('party.leave').code, 'ok');
  assert.equal(abuseCounts(loop).invite_limited >= 1, true);
});

test('invites that fail validation do not spend the invite budget', () => {
  const env = makeHub();
  const host = greeted(env, 1);
  host.call('party.create');
  const stranger = greeted(env, 2);
  const friend = greeted(env, 3);
  befriend(host, friend);
  for (let i = 0; i < 40; i++) {
    env.clock.now += 1100;
    assert.equal(host.call('invite.send', { player: stranger.who.player }).code, 'not_friends');
  }
  assert.equal(env.hub.guards.inviteAccount.buckets.size, 0);
  assert.equal(host.call('invite.send', { player: friend.who.player }).code, 'ok');
});

test('friend requests are limited per account across reconnects', () => {
  const env = makeHub();
  const targets = Array.from({ length: 33 }, (_, i) => identity(300 + i));
  signIn(env, 1).client.drop();
  targets.forEach((_, i) => signIn(env, 300 + i, { ip: `198.18.1.${i + 1}` }).client.drop());
  const codes = [];
  let socket = null;
  for (let i = 0; i < targets.length; i++) {
    if (i % 8 === 0) {
      socket?.drop();
      socket = greeted(env, 1);
    }
    codes.push(socket.call('friends.add', { code: targets[i].player }).code);
  }
  assert.equal(codes.filter((c) => c === 'ok').length, PROTECTION.friendAccount.burst);
  assert.deepEqual(codes.slice(-3), ['limit', 'limit', 'limit']);
  assert.equal(abuseCounts(env).friend_request_limited, 3);
});

test('telemetry batches are limited per account across sockets', () => {
  const env = makeHub();
  const sockets = [1, 2, 3].map(() => {
    const client = env.connect(identity(1));
    client.hello();
    return client;
  });
  const codes = [];
  for (const client of sockets.slice(0, 2)) {
    for (let i = 0; i < 6; i++) codes.push(client.call('telemetry.batch', telemetryBatch()).code);
  }
  assert.deepEqual(codes, Array(12).fill('ok'));
  assert.equal(sockets[2].call('telemetry.batch', telemetryBatch()).code, 'limit');
  env.clock.now += 5000;
  assert.equal(sockets[2].call('telemetry.batch', telemetryBatch()).code, 'ok');
  assert.equal(abuseCounts(env).telemetry_limited, 1);
});

test('an unchanged advertisement is not rewritten inside the refresh interval; a change or the interval is', () => {
  const env = makeHub();
  const host = greeted(env, 1);
  host.call('pm.host', { slots: 2, rules: { stage: 1 } });
  const writes = recordWrites(env.host);
  for (let i = 0; i < 5; i++) {
    env.clock.now += 5000;
    assert.equal(host.call('pm.host', { slots: 2, rules: { stage: 1 } }).code, 'ok');
  }
  assert.equal(writesTo(writes, /player_matches/).length, 0);
  env.clock.now += PROTECTION.advertiseRefreshMs;
  host.call('pm.host', { slots: 2, rules: { stage: 1 } });
  assert.equal(writesTo(writes, /UPDATE player_matches/).length, 1);
  host.call('pm.host', { slots: 2, rules: { stage: 2 } });
  assert.equal(writesTo(writes, /UPDATE player_matches/).length, 2);
  assert.equal(env.hub.stats().player_matches, 1);
});

test('the advertisement stays alive for a client that refreshes every minute', () => {
  const env = makeHub();
  const host = greeted(env, 1);
  const friend = greeted(env, 2);
  befriend(host, friend);
  host.call('pm.host', { slots: 2 });
  for (let i = 0; i < 20; i++) {
    env.advance(60000);
    assert.equal(host.call('pm.host', { slots: 2 }).code, 'ok');
  }
  assert.equal(friend.call('pm.search').sessions.length, 1);
});

test('joining the party session again, or a repeated room report, writes nothing', () => {
  const env = makeHub();
  const host = greeted(env, 1);
  const { session } = host.call('pm.host', { slots: 2 });
  const guest = greeted(env, 2);
  befriend(host, guest);
  env.clock.now += 60000;
  const sent = host.call('invite.send', { player: guest.who.player });
  const joined = guest.call('invite.accept', { invite: sent.invite });
  const writes = recordWrites(env.host);
  assert.equal(guest.call('pm.join', { session }).code, 'ok');
  assert.equal(writesTo(writes, /UPDATE party_members SET in_session/).length, 1);
  assert.equal(guest.call('pm.join', { session }).code, 'ok');
  assert.equal(writesTo(writes, /UPDATE party_members SET in_session/).length, 1);

  writes.length = 0;
  env.hub.roomMember(joined.match, 2, 'connected', 7, 9000);
  env.hub.roomMember(joined.match, 2, 'connected', 7, 9001);
  env.hub.roomMember(joined.match, 2, 'connected', 7, 9002);
  assert.equal(writesTo(writes, /UPDATE party_members SET state/).length, 1);
});

test('room: unchanged link, endpoint and transport reports write nothing', () => {
  const env = makeHub();
  const { rooms, match } = buildParty(env, 2);
  const writes = recordWrites(env.rooms.ensure(match).host);
  assert.equal(rooms[0].call('match.link', { peer: 2, state: 'connected' }).code, 'ok');
  assert.ok(writes.length > 0);
  writes.length = 0;
  for (let i = 0; i < 5; i++) assert.equal(rooms[0].call('match.link', { peer: 2, state: 'connected' }).code, 'ok');
  assert.deepEqual(writes, []);
  assert.equal(rooms[0].call('match.link', { peer: 2, state: 'lost' }).code, 'ok');
  assert.ok(writes.length > 0);

  assert.equal(rooms[0].call('match.endpoint', { port: 40000 }).code, 'ok');
  writes.length = 0;
  for (let i = 0; i < 5; i++) assert.equal(rooms[0].call('match.endpoint', { port: 40000 }).code, 'already_set');
  assert.equal(rooms[0].call('match.endpoint', { port: 3 }).code, 'already_set');
  assert.equal(rooms[0].call('match.transport', { peer: 2, transport: 'direct' }).code, 'ok');
  writes.length = 0;
  for (let i = 0; i < 5; i++) assert.equal(rooms[0].call('match.transport', { peer: 2, transport: 'direct' }).code, 'already_set');
  assert.deepEqual(writes, []);
});

test('room: a reconnect does not rewrite an unchanged seat or the ever-joined mark', () => {
  const env = makeHub();
  const { data, match } = buildParty(env, 2);
  const writes = recordWrites(env.rooms.ensure(match).host);
  const again = env.room(match);
  assert.equal(again.hello(data[1]).code, 'ok');
  assert.equal(writesTo(writes, /UPDATE members SET seen/).length, 0);
  assert.equal(writesTo(writes, /slot_generations/).length, 1);
  assert.equal(writes.filter((w) => w.query.includes('room_meta') && w.args[0] === 'ever').length, 0);
});

test('room: a reconnect flood is limited per seat, wrong join tokens never lock the member out', () => {
  const env = makeHub();
  const { data, match } = buildParty(env, 2, { connect: false });
  const roomHost = env.rooms.ensure(match).host;
  const codes = [];
  for (let i = 0; i < 6; i++) {
    const room = env.room(match);
    codes.push(room.hello(data[1]).code);
    room.drop();
  }
  assert.deepEqual(codes, Array(6).fill('ok'));
  for (let i = 0; i < 40; i++) {
    const room = env.room(match);
    assert.equal(room.hello({ ...data[1], join_token: 'a'.repeat(32) }).code, 'unauthorized');
    room.drop();
  }
  for (let i = 0; i < 6; i++) {
    const room = env.room(match);
    codes.push(room.hello(data[1]).code);
    room.drop();
  }
  assert.deepEqual(codes.slice(6), Array(6).fill('ok'));
  const writes = recordWrites(roomHost);
  const room = env.room(match);
  assert.equal(room.hello(data[1]).code, 'limit');
  assert.deepEqual(writes, []);
  assert.equal(env.room(match).hello(data[0]).code, 'ok');
  env.clock.now += 10000;
  assert.equal(env.room(match).hello(data[1]).code, 'ok');
});

test('room: a member reconnecting at its own pace within the grace is never limited', () => {
  const env = makeHub();
  const { data, match } = buildParty(env, 2, { connect: false });
  for (let i = 0; i < 40; i++) {
    const room = env.room(match);
    assert.equal(room.hello(data[1]).code, 'ok', `reconnect ${i}`);
    room.drop();
    env.advance(8000);
  }
});

test('room: loopback development reconnects are not limited', () => {
  const env = makeHub();
  const { data, match } = buildParty(env, 2, { connect: false });
  for (let i = 0; i < 30; i++) {
    const room = env.room(match, null, { ip: '127.0.0.1' });
    assert.equal(room.hello(data[1]).code, 'ok');
    room.drop();
  }
});

test('valid relay traffic is unaffected; occasional malformed frames never disconnect a player', () => {
  const env = makeHub();
  const { rooms, token } = buildParty(env, 3);
  for (let i = 1; i <= 300; i++) rooms[0].relay(rooms[0].frame(2, token, i));
  assert.equal(rooms[1].binary.length, 300);
  for (let i = 0; i < 60; i++) {
    rooms[0].relay(bytes(1, 2, 3));
    env.clock.now += 1000;
  }
  for (let i = 301; i <= 400; i++) rooms[0].relay(rooms[0].frame(2, token, i));
  assert.equal(rooms[1].binary.length, 400);
  assert.equal(rooms[0].closedWith, null);
});

test('a sender of sustained invalid frames is closed with 1008, and only that sender', () => {
  const faults = {
    malformed: (room) => bytes(1, 2, 3),
    badToken: (room, token) => room.frame(1, token + 1),
    wrongSource: (room, token) => room.frame(1, token, 1, bytes(7), 1),
    outOfRange: (room, token) => room.frame(9, token),
    toSelf: (room, token) => room.frame(3, token),
  };
  for (const [name, make] of Object.entries(faults)) {
    const env = makeHub();
    const { rooms, token } = buildParty(env, 3);
    let sent = 0;
    while (!rooms[2].closedWith && sent < 1000) {
      rooms[2].relay(make(rooms[2], token));
      sent += 1;
    }
    assert.deepEqual(rooms[2].closedWith, { code: 1008, reason: 'relay abuse' }, name);
    assert.equal(sent, RELAY.invalid.burst + 1, name);
    assert.equal(rooms[0].closedWith, null, name);
    assert.equal(rooms[1].closedWith, null, name);
    const before = env.rooms.ensure(rooms[0].match).room.stats().drops;
    rooms[2].relay(bytes(1, 2, 3));
    assert.deepEqual(env.rooms.ensure(rooms[0].match).room.stats().drops, before, name);
    rooms[0].relay(rooms[0].frame(2, token, 1));
    assert.equal(rooms[1].binary.length, 1, name);
  }
});

test('a sender with no hello is closed after sustained frames; others in the room are unaffected', () => {
  const env = makeHub();
  const { rooms, token, match } = buildParty(env, 2);
  const stranger = env.room(match);
  let sent = 0;
  while (!stranger.closedWith && sent < 1000) {
    stranger.relay(rooms[0].frame(2, token, 1));
    sent += 1;
  }
  assert.equal(stranger.closedWith.code, 1008);
  assert.equal(rooms[0].closedWith, null);
  assert.equal(rooms[1].closedWith, null);
});

test('a peer in its reconnect grace, a replaced socket or a closed pair never count as relay abuse', () => {
  const env = makeHub();
  const { rooms, token, match, data } = buildParty(env, 3);
  rooms[1].drop();
  for (let i = 0; i < 600; i++) rooms[0].relay(rooms[0].frame(2, token, i + 1));
  assert.equal(rooms[0].closedWith, null);
  const back = env.room(match);
  back.hello(data[1]);
  rooms[0].relay(rooms[0].frame(2, token, 1));
  assert.equal(back.binary.length, 1);

  const old = rooms[2];
  const fresh = env.room(match);
  fresh.hello(data[2]);
  assert.deepEqual(old.closedWith, { code: 4001, reason: 'replaced' });
  for (let i = 0; i < 600; i++) old.relay(old.frame(1, token, i + 1));
  assert.equal(old.closedWith.code, 4001);
  assert.equal(env.rooms.ensure(match).room.stats().drops.stale, 600);

  for (let i = 0; i < 600; i++) fresh.relay(fresh.frame(1, token, i + 1, bytes(1)));
  assert.equal(fresh.closedWith, null);
  assert.equal(rooms[0].binary.length, 600);
});

test('abuse seen by a room reaches the directory counters without ids', () => {
  const env = makeHub();
  const { rooms, match, data } = buildParty(env, 2);
  let sent = 0;
  while (!rooms[1].closedWith && sent < 1000) {
    rooms[1].relay(bytes(1, 2, 3));
    sent += 1;
  }
  rooms[1].drop();
  for (let i = 0; i < 14; i++) {
    const room = env.room(match);
    room.hello(data[1]);
    room.drop();
  }
  env.advance(PROTECTION.flushMs + 1000);
  const counts = abuseCounts(env);
  assert.equal(counts.relay_abuse_closed, 1);
  assert.equal(counts.room_resume_limited >= 1, true);
});

test('diagnostics are aggregate names and numbers: no address, token, player, party or session', () => {
  const env = makeHub();
  const writes = recordWrites(env.host);
  const ip = '203.0.113.77';
  for (let i = 0; i < 20; i++) signIn(env, 1, { ip }).client.drop();
  const host = greeted(env, 2, { ip });
  const party = host.call('party.create');
  for (let i = 0; i < 10; i++) host.call('party.create');
  env.hub.edgeLimited(4);
  env.advance(PROTECTION.flushMs + 1000);
  const summary = env.hub.telemetrySummary(3);
  assert.ok(summary.abuse.length >= 4);
  const text = JSON.stringify(summary.abuse);
  for (const secret of [identity(1).player, identity(1).token, identity(2).player, ip, '203.0.113', party.match, party.diag,
    party.join_token]) {
    assert.equal(text.includes(secret), false, secret);
  }
  assert.ok(summary.abuse.every((a) => DIAGNOSTICS.has(a.value) && Number.isInteger(a.count) && a.count > 0));
  const stored = env.host.sql.all("SELECT k FROM tel_counters WHERE k LIKE 'abuse.%'").map((r) => r.k.slice(6));
  assert.ok(stored.every((k) => DIAGNOSTICS.has(k)));
  for (const write of writesTo(writes, /tel_counters/)) {
    assert.equal(write.args.some((a) => typeof a === 'string' && (a.includes(ip) || a.includes(identity(1).player))), false);
  }
  assert.equal(abuseCounts(env).ws_upgrade_limited, 4);
});

test('the same abuse is counted again in a later window, never dropped', () => {
  const env = makeHub();
  for (let i = 0; i < 14; i++) signIn(env, 1).client.drop();
  env.advance(PROTECTION.flushMs + 1000);
  for (let i = 0; i < 14; i++) signIn(env, 1).client.drop();
  env.advance(PROTECTION.flushMs + 1000);
  assert.equal(abuseCounts(env).hello_limited, 4);
});

test('alarms: a repeated schedule with the same or a later deadline does not set the alarm again', () => {
  const env = makeHub();
  const host = greeted(env, 1);
  const friend = greeted(env, 2);
  host.call('party.create');
  befriend(host, friend);
  env.clock.now += 60000;
  const calls = recordAlarms(env.host);
  for (let i = 0; i < 10; i++) {
    env.clock.now += 1100;
    const sent = host.call('invite.send', { player: friend.who.player });
    assert.equal(sent.code, 'ok');
    friend.call('invite.decline', { invite: sent.invite });
  }
  assert.ok(calls.length <= 2, `${calls.length} alarm writes`);
  const next = env.host.alarmAt;
  assert.ok(next !== null);
  env.advance(LIMITS.inviteMs * 2);
  assert.equal(env.hub.stats().invites, 0);
  assert.ok(env.host.alarmAt === null || env.host.alarmAt > env.clock.now);
});

test('alarms: an earlier deadline replaces a later alarm, and expiry still fires', () => {
  const env = makeHub();
  const host = greeted(env, 1);
  const friend = greeted(env, 2);
  host.call('party.create');
  befriend(host, friend);
  env.clock.now += 60000;
  const idle = env.host.alarmAt;
  const sent = host.call('invite.send', { player: friend.who.player });
  assert.ok(env.host.alarmAt < idle);
  assert.equal(env.host.alarmAt, env.clock.now + LIMITS.inviteMs);
  env.advance(LIMITS.inviteMs + 1000);
  assert.equal(friend.events('invite.ended').at(-1).invite, sent.invite);
});

// What the aggregates must find, whatever the other parties look like.
test('alarms: the next deadline is the earliest of every party', () => {
  const env = makeHub();
  env.hub.diagnosticsDueAt = () => null;
  const next = () => {
    env.hub.alarmAt = null;
    env.host.alarmAt = null;
    env.hub.scheduleAlarm();
    return env.host.alarmAt;
  };
  assert.equal(next(), null);
  const started = env.clock.now;
  const { clients } = buildParty(env, 2, { first: 700, connect: false });
  env.host.sql.run('DELETE FROM party_invites');
  assert.equal(next(), started + LIMITS.matchMaxMs);

  env.clock.now += 1000;
  const solo = greeted(env, 710);
  solo.call('party.create');
  assert.equal(next(), env.clock.now + LIMITS.partyIdleMs);

  // Only a seat nobody holds counts as gone.
  clients[1].drop();
  assert.equal(next(), env.clock.now + LIMITS.graceMs);
  env.host.sql.run("UPDATE party_members SET state = 'connected' WHERE offline_since IS NOT NULL");
  assert.equal(next(), env.clock.now + LIMITS.partyIdleMs);
});

test('alarms: a woken hub that knows its stored alarm does not write it again', () => {
  const env = makeHub();
  const host = greeted(env, 1);
  host.call('party.create');
  const stored = env.host.alarmAt;
  assert.ok(stored !== null);
  const calls = recordAlarms(env.host);
  new Hub(env.host, { alarmAt: stored });
  assert.equal(calls.length, 0);
  new Hub(env.host, {});
  assert.equal(calls.length, 1);
});

test('alarms: a room that is busy does not set its alarm on every message', () => {
  const env = makeHub();
  const { rooms, match } = buildParty(env, 2);
  const calls = recordAlarms(env.rooms.ensure(match).host);
  for (let i = 0; i < 40; i++) {
    env.clock.now += 1000;
    assert.equal(rooms[0].call('ping').code, 'ok');
  }
  assert.ok(calls.length <= 1, `${calls.length} alarm writes`);
  env.advance(LIMITS.silenceMs * 2);
  assert.equal(rooms[0].closedWith.code, 4004);
});

test('compatibility: protocol, capabilities, replies and relay numbers are unchanged', () => {
  const env = makeHub();
  assert.equal(PROTOCOL, 4);
  assert.deepEqual(Object.keys(CAPABILITIES).sort(), ['max_party', 'protocol_version', 'public_endpoints', 'relay_frame_version',
    'relay_max_payload', 'relay_supported', 'worker_build']);
  const client = env.connect(identity(1));
  const hello = client.hello();
  assert.deepEqual(Object.keys(hello).sort(), ['capabilities', 'code', 'friend_code', 'name', 'player', 're', 'time']);
  const created = client.call('party.create');
  assert.deepEqual(Object.keys(created).sort(), ['code', 'diag', 'generation', 'join_token', 'match', 'party', 're', 'slot']);
  assert.deepEqual(RELAY.connection, { packets: { burst: 1600, perSecond: 800 }, bytes: { burst: 512 * 1024, perSecond: 256 * 1024 } });
  assert.deepEqual(RELAY.match, { packets: { burst: 4800, perSecond: 2400 }, bytes: { burst: 1536 * 1024, perSecond: 768 * 1024 } });
  assert.equal(RELAY.overrunsToClose, 256);
  assert.equal(RELAY.maxPayload, 1472);
  assert.equal(RELAY.version, 2);
});
