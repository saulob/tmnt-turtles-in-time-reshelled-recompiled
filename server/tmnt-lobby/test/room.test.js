import assert from 'node:assert/strict';
import { test } from 'node:test';

import { CAPABILITIES, LIMITS, MAX_PARTY, PROTOCOL, encodeRelayFrame } from '../src/protocol.js';
import { MatchRoom } from '../src/room.js';
import { buildParty, greeted, makeHub } from './harness.js';

const memberStates = (env) => env.host.sql.all('SELECT slot, state FROM party_members ORDER BY slot').map((r) => `${r.slot}:${r.state}`);
const keepAlive = (rooms) => rooms.forEach((r) => r.call('ping'));
const link = (room, peer, state = 'connected') => room.call('match.link', { peer, state });

test('match.hello checks the protocol, the match, the slot and the token', () => {
  const env = makeHub();
  const { data, match } = buildParty(env, 2, { connect: false });
  const r = env.room(match);
  assert.equal(r.call('match.endpoint', { port: 7001 }).code, 'no_session');
  const hello = (extra) => r.call('match.hello', { proto: PROTOCOL, match, slot: 1, token: data[0].join_token, relay: true, ...extra });
  assert.equal(hello({ proto: 3 }).code, 'proto');
  assert.equal(hello({ match: 'zz' }).code, 'bad_request');
  assert.equal(hello({ match: '0123456789abcdef' }).code, 'not_found');
  for (const slot of [0, 'x', 1.5, 256]) assert.equal(hello({ slot }).code, 'bad_request');
  assert.equal(hello({ token: 'abcd' }).code, 'bad_request');
  assert.equal(hello({ token: 'a'.repeat(32) }).code, 'unauthorized');
  assert.equal(hello({ token: data[1].join_token }).code, 'unauthorized');
  assert.equal(hello({ slot: 2, token: data[0].join_token }).code, 'unauthorized');
  assert.equal(hello({ slot: 3, token: data[1].join_token }).code, 'not_found');
  assert.equal(hello({ generation_seen: -1 }).code, 'bad_request');
  const reply = hello({});
  assert.equal(reply.code, 'ok');
  assert.equal(reply.generation, 1);
  assert.ok(Number.isInteger(reply.relay_token) && reply.relay_token > 0 && reply.relay_token <= 0xffffffff);
  assert.deepEqual(reply.capabilities, CAPABILITIES);
  assert.deepEqual(reply.members.map((m) => [m.slot, m.state]), [[1, 'connecting'], [2, 'joined']]);
  assert.equal(hello({}).code, 'bad_request');
});

test('a hello that races ahead of the directory finds no room yet and can try again', () => {
  const env = makeHub();
  const r = env.room('0123456789abcdef');
  assert.equal(r.call('match.hello', { proto: PROTOCOL, match: '0123456789abcdef', slot: 1, token: 'a'.repeat(32) }).code, 'not_found');
  const { match, data } = buildParty(env, 2, { connect: false });
  const late = env.room(match);
  assert.equal(late.hello(data[1]).code, 'ok');
});

test('a room answers malformed requests without crashing', () => {
  const env = makeHub();
  const { rooms } = buildParty(env, 2);
  const [a] = rooms;
  assert.equal(a.raw('not json').code, 'bad_request');
  assert.equal(a.raw('[]').code, 'bad_request');
  assert.equal(a.raw('{"op":"match.link"}').code, 'bad_request');
  assert.equal(a.call('no.such.op').code, 'bad_request');
  assert.equal(a.call('friends.list').code, 'bad_request');
  assert.equal(a.raw('x'.repeat(LIMITS.frameBytes + 1)).code, 'too_large');
  assert.equal(a.call('ping').code, 'ok');
  assert.equal(a.call('match.link', { peer: 2, state: 'maybe' }).code, 'bad_request');
  assert.equal(a.call('match.link', { peer: 1, state: 'connected' }).code, 'bad_request');
  assert.equal(a.call('match.link', { peer: 9, state: 'connected' }).code, 'bad_request');
  assert.equal(a.call('match.link', { peer: 3, state: 'connected' }).code, 'not_found');
  assert.equal(env.logs.filter((l) => l.includes('internal')).length, 0);
});

test('a socket that never says hello is closed after ten seconds', () => {
  const env = makeHub();
  const { match } = buildParty(env, 2, { connect: false });
  const idle = env.room(match);
  env.advance(LIMITS.helloMs - 1000);
  assert.equal(idle.closedWith, null);
  env.advance(2000);
  assert.equal(idle.closedWith.code, 4003);
  assert.ok((env.rooms.ensure(match).host.alarmAt ?? Infinity) > env.clock.now);
});

test('a fifth member cannot get in: a full roster answers party_full, and sockets are capped', () => {
  const env = makeHub();
  const { rooms, match, data } = buildParty(env, MAX_PARTY);
  const fifth = env.room(match);
  const hello = (extra) => fifth.call('match.hello', { proto: PROTOCOL, match, slot: 5, token: data[0].join_token, ...extra });
  assert.equal(hello({}).code, 'party_full');
  assert.equal(hello({ slot: 9 }).code, 'party_full');
  assert.equal(hello({ slot: 2 }).code, 'unauthorized');
  assert.equal(rooms.filter((r) => r.closedWith).length, 0);
  const crowd = [];
  while (crowd.length < 20) {
    const extra = env.room(match);
    if (!extra.admitted) break;
    crowd.push(extra);
  }
  assert.equal(crowd.length + 1 + MAX_PARTY, LIMITS.roomSockets);
});

test('the directory follows the room: connecting, connected, disconnected', () => {
  const env = makeHub();
  const { data, match, clients } = buildParty(env, 3, { connect: false });
  assert.deepEqual(memberStates(env), ['1:joined', '2:joined', '3:joined']);
  const rooms = data.map((d) => {
    const r = env.room(match);
    r.hello(d);
    return r;
  });
  assert.deepEqual(memberStates(env), ['1:connecting', '2:connecting', '3:connecting']);
  assert.equal(clients[1].events('party.update').at(-1).party.members[1].state, 'connecting');
  link(rooms[0], 2, 'connecting');
  assert.deepEqual(memberStates(env), ['1:connecting', '2:connecting', '3:connecting']);
  link(rooms[0], 2);
  link(rooms[0], 3, 'lost');
  assert.deepEqual(memberStates(env), ['1:connecting', '2:connecting', '3:connecting']);
  link(rooms[0], 3);
  link(rooms[1], 1);
  assert.deepEqual(memberStates(env), ['1:connected', '2:connected', '3:connecting']);
  rooms[1].drop();
  assert.deepEqual(memberStates(env), ['1:connected', '2:disconnected', '3:connecting']);
  assert.equal(clients[0].events('party.update').at(-1).party.members[1].state, 'disconnected');
  assert.equal(clients[0].call('friends.list').friends.find((f) => f.player === clients[1].who.player).state, 'party');
});

test('a member that joined the game session shows connecting, then in game once its links are up', () => {
  const env = makeHub();
  const { clients, rooms } = buildParty(env, 3);
  const [host, b] = clients;
  const hosted = host.call('pm.host', {});
  const session = hosted.session;
  assert.equal(b.call('pm.join', { session }).code, 'ok');
  const view = () => host.call('friends.list').friends.find((f) => f.player === b.who.player).state;
  assert.equal(view(), 'connecting');
  link(rooms[1], 1);
  link(rooms[1], 3);
  assert.equal(view(), 'in_game');
  assert.equal(host.call('friends.list').friends.find((f) => f.player === clients[2].who.player).state, 'party');
  rooms[1].drop();
  assert.equal(view(), 'connecting');
});

test('members are told who is in the room and what their links and pairs look like', () => {
  const env = makeHub();
  const { data, match } = buildParty(env, 3, { connect: false });
  const rooms = [];
  for (const d of data) {
    const r = env.room(match);
    r.hello(d);
    rooms.push(r);
  }
  const first = rooms[0].events('match.members');
  assert.ok(first.length >= 2);
  assert.deepEqual(first.at(-1).members.map((m) => [m.slot, m.state]), [[1, 'connecting'], [2, 'connecting'], [3, 'connecting']]);
  assert.ok(first.at(-1).generation > first[0].generation);
  link(rooms[1], 3);
  rooms[1].call('match.transport', { peer: 3, transport: 'relay' });
  const members = rooms[2].events('match.members').at(-1).members;
  assert.deepEqual(members[1].links, [{ peer: 3, transport: 'relay', state: 'connected' }]);
  assert.deepEqual(members[2].links, [{ peer: 2, transport: 'relay', state: 'connecting' }]);
  assert.deepEqual(members[0].links, []);
});

test('each reconnect bumps the slot generation; the replaced socket is closed and refused', () => {
  const env = makeHub();
  const { rooms, data, match } = buildParty(env, 3);
  const old = rooms[1];
  const again = env.room(match);
  assert.equal(again.hello(data[1]).generation, 2);
  assert.deepEqual(old.closedWith, { code: 4001, reason: 'replaced' });
  assert.equal(old.call('match.link', { peer: 1, state: 'connected' }).code, 'stale');
  assert.equal(old.call('match.endpoint', { port: 7001 }).code, 'stale');
  assert.equal(link(again, 1).code, 'ok');
  rooms[0].events('match.peer_lost');
  old.drop();
  assert.equal(rooms[0].events('match.peer_lost').length, 0);
  assert.equal(memberStates(env)[1], '2:connected');
  const third = env.room(match);
  assert.equal(third.hello(data[1]).generation, 3);
  assert.equal(again.closedWith.code, 4001);
  assert.equal(rooms[2].events('match.peer_back').length, 0);
});

test('a seat is held for the grace period; the peers hear it lost and back and get the endpoint again', () => {
  const env = makeHub();
  const { rooms, data, match, clients } = buildParty(env, 3);
  rooms[0].call('match.endpoint', { port: 7001, lan: ['127.0.0.1'] });
  rooms[1].call('match.endpoint', { port: 7002 });
  for (const r of rooms) r.inbox = [];
  rooms[2].drop();
  assert.deepEqual(rooms[0].events('match.peer_lost'), [{ ev: 'match.peer_lost', slot: 3 }]);
  assert.equal(rooms[1].events('match.peer_lost').length, 1);
  assert.equal(rooms[0].events('match.members').at(-1).members[2].state, 'disconnected');
  assert.equal(memberStates(env)[2], '3:disconnected');
  env.advance(5000);
  const back = env.room(match);
  const reply = back.hello(data[2]);
  assert.equal(reply.generation, 2);
  assert.deepEqual(rooms[0].events('match.peer_back'), [{ ev: 'match.peer_back', slot: 3 }]);
  assert.deepEqual(back.events('match.peer').map((e) => e.slot).sort(), [1, 2]);
  keepAlive([rooms[0], rooms[1], back]);
  env.advance(LIMITS.graceMs + 1000);
  assert.equal(env.hub.stats().members, 3);
  assert.equal(clients[2].events('party.ended').length, 0);
});

test('a guest whose seat is not retaken within the grace period leaves the party', () => {
  const env = makeHub();
  const { rooms, clients, data } = buildParty(env, 3);
  rooms[2].drop();
  env.advance(LIMITS.graceMs + 1000);
  assert.deepEqual(rooms[0].events('match.left'), [{ ev: 'match.left', slot: 3, reason: 'disconnect' }]);
  assert.deepEqual(rooms[1].events('match.left'), [{ ev: 'match.left', slot: 3, reason: 'disconnect' }]);
  assert.deepEqual(rooms[0].events('match.members').at(-1).members.map((m) => m.slot), [1, 2]);
  assert.deepEqual(clients[0].events('party.update').at(-1).party.members.map((m) => m.slot), [1, 2]);
  assert.deepEqual(clients[2].events('party.ended'), [{ ev: 'party.ended', match: data[0].match, reason: 'expired' }]);
  assert.equal(env.hub.stats().parties, 1);
  const late = env.room(data[0].match);
  assert.equal(late.hello(data[2]).code, 'not_found');
});

test('a host whose seat is not retaken ends the match for everyone', () => {
  const env = makeHub();
  const { rooms, clients, data } = buildParty(env, 3);
  rooms[0].drop();
  env.advance(LIMITS.graceMs + 1000);
  for (const r of rooms.slice(1)) {
    assert.deepEqual(r.events('match.ended'), [{ ev: 'match.ended', reason: 'expired' }]);
    assert.equal(r.closedWith.code, 1000);
  }
  for (const c of clients.slice(1)) {
    assert.deepEqual(c.events('party.ended'), [{ ev: 'party.ended', match: data[0].match, reason: 'expired' }]);
  }
  assert.deepEqual([env.hub.stats().parties, env.hub.stats().members], [0, 0]);
  assert.equal(env.rooms.ensure(data[0].match).room.stats().ended, 'expired');
  assert.equal(env.room(data[0].match).hello(data[1]).code, 'not_found');
});

test('a silent socket is dropped after 45 seconds and a ping keeps it', () => {
  const env = makeHub();
  const { rooms, match } = buildParty(env, 2);
  env.advance(30000);
  rooms[0].call('ping');
  env.advance(30000);
  assert.equal(rooms[0].closedWith, null);
  assert.equal(rooms[1].closedWith.code, 4004);
  const { host } = env.rooms.ensure(match);
  assert.ok(host.alarmAt > env.clock.now, 'a closed socket schedules no alarm in the past');
  assert.equal(rooms[0].events('match.peer_lost').length, 1);
  assert.equal(memberStates(env)[1], '2:disconnected');
});

test('a room nobody is connected to for a minute ends, and so does one past its age limit', () => {
  const env = makeHub();
  const { match, data, clients } = buildParty(env, 2, { connect: false });
  const guest = env.room(match);
  guest.hello(data[1]);
  guest.drop();
  env.advance(LIMITS.roomIdleMs + 1000);
  assert.equal(env.rooms.ensure(match).room.stats().ended, 'expired');
  assert.equal(clients[0].events('party.ended').at(-1).reason, 'expired');

  const old = makeHub();
  const built = buildParty(old, 2, { first: 30 });
  old.clock.now += LIMITS.matchMaxMs + 1000;
  keepAlive(built.rooms);
  old.rooms.tick();
  assert.equal(old.rooms.ensure(built.match).room.stats().ended, 'expired');
  assert.deepEqual(built.rooms[1].events('match.ended'), [{ ev: 'match.ended', reason: 'expired' }]);
  assert.equal(old.hub.stats().parties, 0);
});

test('an ended room stays a tombstone, refuses a late roster and is erased after an hour', () => {
  const env = makeHub();
  const { match, data, clients } = buildParty(env, 2, { connect: false });
  clients[0].call('party.leave');
  const { room, host } = env.rooms.ensure(match);
  assert.equal(room.stats().ended, 'host_left');
  room.apply({ match, diag: data[0].diag, rev: 999, members: [{ slot: 1, name: 'x', hash: 'a'.repeat(64) }] });
  assert.equal(room.stats().members, 0);
  assert.equal(env.room(match).hello(data[0]).code, 'not_found');
  env.advance(LIMITS.tombstoneMs + 1000);
  assert.equal(env.rooms.rooms.has(match), false);
  void host;

  const never = '0123456789abcdef';
  env.rooms.ensure(never).room.end(never, 'closed');
  env.rooms.ensure(never).room.apply({ match: never, diag: '', rev: 0, members: [{ slot: 1, name: 'x', hash: 'a'.repeat(64) }] });
  assert.equal(env.rooms.ensure(never).room.stats().members, 0);
});

test('an older roster never undoes a newer one', () => {
  const env = makeHub();
  const { match, data } = buildParty(env, 3, { connect: false });
  const { room } = env.rooms.ensure(match);
  const stale = { match, diag: data[0].diag, rev: 0, members: [{ slot: 1, name: 'x', hash: 'a'.repeat(64) }] };
  room.apply(stale);
  assert.equal(room.stats().members, 3);
  room.apply({ ...stale, rev: 'x' });
  room.apply({ ...stale, members: 'no' });
  room.apply({ ...stale, rev: 1000, members: [{ slot: 9, name: 'x', hash: 'a'.repeat(64) }] });
  assert.equal(room.stats().members, 3);
  room.apply({ ...stale, match: '1111111111111111', rev: 1000 });
  assert.equal(room.stats().members, 3);
});

test('endpoints are published once per socket generation and reach every other member', () => {
  const env = makeHub();
  const { rooms } = buildParty(env, 4);
  assert.equal(rooms[0].call('match.endpoint', { port: 7001, lan: ['127.0.0.1', '192.168.1.5'] }).code, 'ok');
  assert.equal(rooms[0].events('match.peer').length, 0);
  for (const r of rooms.slice(1)) {
    const [peer] = r.events('match.peer');
    assert.deepEqual(peer, { ev: 'match.peer', slot: 1, name: peer.name, ip: '203.0.113.7', port: 7001,
      lan: ['127.0.0.1', '192.168.1.5'], same_network: true, service_forwards_public: true });
  }
  assert.equal(rooms[0].call('match.endpoint', { port: 7001 }).code, 'already_set');
  assert.equal(rooms[1].call('match.endpoint', { port: 7002, public_ip: '179.113.149.63', public_port: 54114 }).code, 'ok');
  for (const i of [0, 2, 3]) {
    const [peer] = rooms[i].events('match.peer');
    assert.deepEqual([peer.slot, peer.port, peer.public_ip, peer.public_port], [2, 7002, '179.113.149.63', 54114]);
  }
  assert.equal(rooms[1].events('match.peer').length, 0);
});

test('endpoint messages are validated and need a current socket', () => {
  const env = makeHub();
  const { rooms } = buildParty(env, 2);
  assert.equal(rooms[0].call('match.endpoint', { port: 80 }).code, 'bad_request');
  assert.equal(rooms[0].call('match.endpoint', { port: 7001, lan: ['8.8.8.8'] }).code, 'bad_request');
  assert.equal(rooms[0].call('match.endpoint', { port: 7001, lan: 'x' }).code, 'bad_request');
  assert.equal(rooms[0].call('match.endpoint', {}).code, 'bad_request');
});

// The STUN-mapped endpoint of the sender's gameplay socket: a public IPv4 address and its own port, both or neither.
test('public_ip and public_port are validated as one endpoint', () => {
  const env = makeHub();
  const { rooms } = buildParty(env, 2);
  const [a] = rooms;
  for (const bad of ['10.0.0.5', '192.168.1.5', '127.0.0.1', '2804:14d::1', 'not-an-ip', '', 1]) {
    assert.equal(a.call('match.endpoint', { port: 7001, public_ip: bad, public_port: 54114 }).code, 'bad_request');
  }
  for (const bad of [0, 65536, -1, 1.5, '54114', null]) {
    assert.equal(a.call('match.endpoint', { port: 7001, public_ip: '203.0.113.7', public_port: bad }).code, 'bad_request');
  }
  assert.equal(a.call('match.endpoint', { port: 7001, public_ip: '203.0.113.7' }).code, 'bad_request');
  assert.equal(a.call('match.endpoint', { port: 7001, public_port: 54114 }).code, 'bad_request');
  assert.equal(a.call('match.endpoint', { port: 7001, public_ip: '203.0.113.7', public_port: 54114 }).code, 'ok');
  const none = rooms[1];
  assert.equal(none.call('match.endpoint', { port: 7001 }).code, 'ok');  // omitted: STUN found nothing
});

// Real test 84b7d2f1: the peer must get the mapped port (54114), not the local one (7001), and the WebSocket's observed
// address (here IPv6) is passed on untouched, never dressed up as a UDP endpoint.
test('match.peer carries the sender\'s mapped endpoint apart from its observed address', () => {
  const env = makeHub();
  const { data, match } = buildParty(env, 2, { connect: false });
  const a = env.room(match, null, { ip: '2804:14d::1' });
  const b = env.room(match, null, { ip: '203.0.113.9' });
  a.hello(data[0]);
  b.hello(data[1]);
  a.call('match.endpoint', { port: 7001, public_ip: '179.113.149.63', public_port: 54114 });
  b.call('match.endpoint', { port: 7002 });

  const toB = b.events('match.peer').at(-1);
  assert.deepEqual([toB.ip, toB.port, toB.public_ip, toB.public_port, toB.same_network],
    ['2804:14d::1', 7001, '179.113.149.63', 54114, false]);
  const toA = a.events('match.peer').at(-1);
  assert.deepEqual([toA.ip, toA.port, toA.public_ip, toA.public_port, toA.same_network], ['203.0.113.9', 7002, undefined, undefined, false]);
});

// Real two-PC test 6d9f1c2a: whichever member finishes STUN first, the others get its public endpoint, and a member that
// connects later gets every endpoint already published.
for (const first of [1, 2, 3, 4]) {
  test(`slot ${first} publishing first reaches the rest, and a late member still gets it`, () => {
    const env = makeHub();
    const { data, match } = buildParty(env, 4, { connect: false });
    const rooms = [];
    const connect = (i) => {
      const r = env.room(match);
      r.hello(data[i]);
      rooms[i] = r;
    };
    const others = [0, 1, 2, 3].filter((i) => i !== first - 1);
    connect(first - 1);
    connect(others[0]);
    rooms[first - 1].call('match.endpoint', { port: 7000 + first, public_ip: '177.18.65.229', public_port: 7001 });
    assert.equal(rooms[others[0]].events('match.peer').length, 1);
    connect(others[1]);
    connect(others[2]);
    for (const i of others.slice(1)) {
      const [peer] = rooms[i].events('match.peer');
      assert.deepEqual([peer.slot, peer.port, peer.public_ip, peer.public_port], [first, 7000 + first, '177.18.65.229', 7001]);
    }
    assert.equal(rooms[first - 1].events('match.peer').length, 0);
  });
}

test('after a reconnect an endpoint is stale until its owner publishes it again', () => {
  const env = makeHub();
  const { rooms, data, match } = buildParty(env, 3);
  rooms[1].call('match.endpoint', { port: 7002 });
  const again = env.room(match);
  again.hello(data[1]);
  const late = env.room(match);
  assert.equal(late.hello(data[2]).generation, 2);
  assert.equal(late.events('match.peer').length, 0);
  assert.equal(again.call('match.endpoint', { port: 7003 }).code, 'ok');
  assert.equal(late.events('match.peer').at(-1).port, 7003);
  assert.equal(rooms[0].events('match.peer').at(-1).port, 7003);
});

test('only the lower slot of a pair chooses its transport, once, and the pair and the host hear it', () => {
  const env = makeHub();
  const { rooms } = buildParty(env, 4);
  const choose = (i, peer, transport) => rooms[i].call('match.transport', { peer, transport });
  assert.equal(choose(2, 2, 'relay').code, 'not_owner');
  assert.equal(choose(1, 3, 'relay').code, 'ok');
  const event = { ev: 'match.transport', a: 2, b: 3, transport: 'relay', by: 2 };
  assert.deepEqual(rooms[1].events('match.transport'), [event]);
  assert.deepEqual(rooms[2].events('match.transport'), [event]);
  assert.deepEqual(rooms[0].events('match.transport'), [event]);
  assert.deepEqual(rooms[3].events('match.transport'), []);
  assert.equal(choose(1, 3, 'relay').code, 'already_set');
  assert.equal(choose(1, 3, 'direct').code, 'already_set');
  assert.equal(choose(2, 2, 'direct').code, 'not_owner');
  rooms.forEach((r) => r.events('match.transport'));

  assert.equal(choose(0, 2, 'direct').code, 'ok');
  assert.equal(rooms[0].events('match.transport').length, 1);
  assert.equal(rooms[1].events('match.transport').length, 1);
  assert.equal(rooms[2].events('match.transport').length, 0);
  assert.equal(rooms[3].events('match.transport').length, 0);
});

test('transport requests are validated, and relay needs both ends relay-capable and present', () => {
  const env = makeHub();
  const { data, match } = buildParty(env, 4, { connect: false });
  const rooms = data.map((d, i) => {
    const r = env.room(match);
    r.hello(d, { relay: i !== 3 });
    return r;
  });
  const choose = (i, extra) => rooms[i].call('match.transport', extra);
  for (const peer of [0, 5, 'x', null, 1.5]) assert.equal(choose(0, { peer, transport: 'relay' }).code, 'bad_request');
  assert.equal(choose(0, { peer: 2, transport: 'udp' }).code, 'bad_request');
  assert.equal(choose(0, { peer: 2 }).code, 'bad_request');
  assert.equal(choose(0, { peer: 1, transport: 'relay' }).code, 'bad_request');
  assert.equal(choose(0, { peer: 4, transport: 'relay' }).code, 'relay_unavailable');
  assert.equal(choose(0, { peer: 4, transport: 'direct' }).code, 'ok');
  rooms[2].drop();
  assert.equal(choose(0, { peer: 3, transport: 'relay' }).code, 'relay_unavailable');
  assert.equal(choose(0, { peer: 3, transport: 'direct' }).code, 'ok');
  assert.equal(choose(1, { peer: 3, transport: 'direct' }).code, 'ok');
});

test('links are informational: stored, forwarded and limited', () => {
  const env = makeHub();
  const { rooms } = buildParty(env, 3);
  assert.equal(link(rooms[0], 2, 'connecting').code, 'ok');
  assert.equal(rooms[1].events('match.members').at(-1).members[0].links[0].state, 'connecting');
  assert.equal(link(rooms[0], 2).code, 'ok');
  assert.equal(rooms[1].events('match.members').at(-1).members[0].links[0].state, 'connected');
  let limited = 0;
  for (let i = 0; i < 60; i++) if (link(rooms[1], 3, i % 2 ? 'lost' : 'connected').code === 'limit') limited++;
  assert.ok(limited > 0 && limited < 60);
  assert.equal(link(rooms[2], 1).code, 'ok');
});

test('leaving frees the seat at once; the slot, and its generations, are reused by the next member', () => {
  const env = makeHub();
  const { rooms, clients, data, match } = buildParty(env, 3);
  assert.equal(rooms[1].call('match.leave').code, 'ok');
  assert.equal(rooms[1].closedWith.code, 1000);
  assert.deepEqual(rooms[0].events('match.left'), [{ ev: 'match.left', slot: 2, reason: 'left' }]);
  assert.deepEqual(rooms[2].events('match.members').at(-1).members.map((m) => m.slot), [1, 3]);
  assert.deepEqual(clients[1].events('party.ended'), [{ ev: 'party.ended', match, reason: 'closed' }]);
  assert.deepEqual(clients[0].events('party.update').at(-1).party.members.map((m) => m.slot), [1, 3]);
  assert.equal(env.rooms.ensure(match).room.stats().members, 2);

  const newcomer = greeted(env, 200);
  env.clock.now += 60000;
  keepAlive([rooms[0], rooms[2]]);
  const { befriend } = { befriend: (a, b) => { a.call('friends.add', { code: b.who.player }); b.call('friends.accept', { player: a.who.player }); } };
  befriend(clients[0], newcomer);
  const accepted = newcomer.call('invite.accept', { invite: clients[0].call('invite.send', { player: newcomer.who.player }).invite });
  assert.equal(accepted.slot, 2);
  const fresh = env.room(match);
  assert.equal(fresh.hello(accepted).generation, 2);
  assert.equal(fresh.call('match.hello', {}).code, 'bad_request');
  void data;
});

test('the host leaving the room ends the match for everyone', () => {
  const env = makeHub();
  const { rooms, clients, match } = buildParty(env, 3);
  assert.equal(rooms[0].call('match.leave').code, 'ok');
  for (const r of rooms.slice(1)) {
    assert.deepEqual(r.events('match.ended'), [{ ev: 'match.ended', reason: 'host_left' }]);
    assert.equal(r.closedWith.code, 1000);
  }
  for (const c of clients.slice(1)) assert.deepEqual(c.events('party.ended'), [{ ev: 'party.ended', match, reason: 'host_left' }]);
  assert.deepEqual([env.hub.stats().parties, env.hub.stats().members], [0, 0]);
  assert.equal(clients[0].call('party.create').code, 'ok');
});

test('the host leaving the party at the directory ends the room too', () => {
  const env = makeHub();
  const { rooms, clients } = buildParty(env, 3);
  clients[0].call('party.leave');
  for (const r of rooms.slice(1)) assert.deepEqual(r.events('match.ended'), [{ ev: 'match.ended', reason: 'host_left' }]);
  assert.equal(rooms[1].closedWith.code, 1000);
});

test('a kicked member is told, its socket closed and its seat refused afterwards', () => {
  const env = makeHub();
  const { rooms, clients, data, match } = buildParty(env, 4);
  assert.equal(clients[0].call('party.kick', { player: clients[2].who.player }).code, 'ok');
  assert.deepEqual(rooms[2].events('match.left'), [{ ev: 'match.left', slot: 3, reason: 'kicked' }]);
  assert.deepEqual(rooms[2].closedWith, { code: 4002, reason: 'removed' });
  for (const i of [0, 1, 3]) assert.deepEqual(rooms[i].events('match.left'), [{ ev: 'match.left', slot: 3, reason: 'kicked' }]);
  assert.deepEqual(rooms[3].events('match.members').at(-1).members.map((m) => m.slot), [1, 2, 4]);
  assert.equal(env.room(match).hello(data[2]).code, 'not_found');
  assert.equal(rooms[2].call('match.link', { peer: 1, state: 'connected' }).code, 'stale');
});

test('rate limits apply per connection', () => {
  const env = makeHub();
  const { rooms } = buildParty(env, 2);
  let limited = 0;
  for (let i = 0; i < 100; i++) if (rooms[0].call('ping').code === 'limit') limited++;
  assert.ok(limited > 0);
  assert.equal(rooms[1].call('ping').code, 'ok');
  env.advance(2000);
  assert.equal(rooms[0].call('ping').code, 'ok');
});

test('no join token or player code appears in an event or a log of the room', () => {
  const env = makeHub();
  const { rooms, clients, data } = buildParty(env, 3);
  rooms[0].call('match.endpoint', { port: 7001 });
  rooms[1].call('match.transport', { peer: 3, transport: 'relay' });
  rooms[2].drop();
  const everything = JSON.stringify([rooms.map((r) => r.inbox), env.logs]);
  for (const d of data) assert.ok(!everything.includes(d.join_token));
  for (const c of clients) assert.ok(!everything.includes(c.who.player));
  for (const c of clients) assert.ok(!everything.includes(c.who.token));
});

test('a hibernated room wakes with its seats, pairs, generations and sockets intact', () => {
  const env = makeHub();
  const { rooms, match, token, data } = buildParty(env, 3);
  rooms[0].call('match.transport', { peer: 2, transport: 'direct' });
  rooms[1].call('match.transport', { peer: 3, transport: 'relay' });
  link(rooms[0], 2);
  const entry = env.rooms.ensure(match);
  const woke = new MatchRoom(entry.host, { local: true });
  entry.room = woke;
  for (const r of rooms) r.room = woke;
  rooms[1].relay(encodeRelayFrame(2, 3, token, 1, Uint8Array.from([1])));
  assert.equal(rooms[2].binary.length, 1);
  rooms[0].relay(encodeRelayFrame(1, 2, token, 1, Uint8Array.from([1])));
  assert.equal(rooms[1].binary.length, 0);
  assert.equal(woke.stats().drops.pair_direct, 1);
  assert.equal(rooms[0].call('match.transport', { peer: 2, transport: 'relay' }).code, 'already_set');
  const again = env.room(match);
  again.room = woke;
  assert.equal(again.hello(data[1]).generation, 2);
  assert.equal(rooms[1].closedWith.code, 4001);
  assert.equal(rooms[1].call('match.link', { peer: 1, state: 'connected' }).code, 'stale');
  assert.equal(memberStates(env)[0], '1:connected');
});
