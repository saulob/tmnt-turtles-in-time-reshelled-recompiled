// The Friends / public Lobby release gate: aliases, handles, the request flow and the privacy contract, in process with
// a fake clock and no network. Failure messages name the contract that broke.
import assert from 'node:assert/strict';

import { test } from 'node:test';

import { CAPABILITIES, PROTOCOL, WORKER_BUILD } from '../src/protocol.js';
import { greeted, identity, makeHub } from './harness.js';

const HANDLE = /^[0-9a-f]{32}$/;
const ALIAS = /^Player-[0-9A-F]{4}$/;
const SALT = '0123456789abcdef0123456789abcdef';

const visible = (client, on = true) => client.call('lobby.publish', { visible: on });
const rows = (viewer) => viewer.call('lobby.list').players;
const rowOf = (viewer, name) => rows(viewer).find((p) => p.name === name);
const compact = (code) => code.replace('-', '').toUpperCase();

function strings(value, out = []) {
  if (typeof value === 'string') out.push(value);
  else if (Array.isArray(value)) value.forEach((v) => strings(v, out));
  else if (value && typeof value === 'object') {
    for (const [k, v] of Object.entries(value)) {
      out.push(k);
      strings(v, out);
    }
  }
  return out;
}

const mentions = (traffic, code) => strings(traffic).some((s) => s.toUpperCase().replace(/-/g, '').includes(compact(code)));

// Everything one player has been sent: every answer, and every event, read before a test consumes it.
class Tap {
  constructor(client) {
    this.client = client;
    this.seen = [];
  }

  call(op, body) {
    const reply = this.client.call(op, body);
    this.seen.push(reply);
    return reply;
  }

  events(name) {
    this.seen.push(...this.client.inbox.map((m) => JSON.parse(JSON.stringify(m))));
    return this.client.events(name);
  }
}

function pair({ names = ['Player', 'Player'] } = {}) {
  const env = makeHub();
  env.hub.setMeta('salt', SALT);
  const a = new Tap(greeted(env, 1, { name: names[0] }));
  const b = new Tap(greeted(env, 2, { name: names[1] }));
  visible(a.client);
  visible(b.client);
  return { env, a, b, codeA: identity(1).player, codeB: identity(2).player };
}

function assertNoCodeBeforeAcceptance({ a, b, codeA, codeB }) {
  assert.equal(mentions(a.seen, codeB), false, 'Public Lobby leaked permanent friend code before acceptance (requester saw the target code)');
  assert.equal(mentions(b.seen, codeA), false, 'Public Lobby leaked permanent friend code before acceptance (receiver saw the sender code)');
}

test('the build identifier is the canonical constant and the protocol stays 4', () => {
  assert.equal(CAPABILITIES.worker_build, WORKER_BUILD);
  assert.match(WORKER_BUILD, /^\d{4}-\d{2}-\d{2}-lobby\d+$/);
  assert.equal(PROTOCOL, 4);
  assert.equal(CAPABILITIES.protocol_version, 4);
});

test('two default-name accounts get distinct, stable Player-XXXX aliases and custom names get none', () => {
  const { env, a, b } = pair();
  const seenByB = rows(b.client)[0].name;
  const seenByA = rows(a.client)[0].name;
  for (const alias of [seenByA, seenByB]) {
    assert.notEqual(alias, 'Player', 'a default name reached the Lobby without an alias');
    assert.match(alias, ALIAS, 'a default alias is not Player-XXXX');
  }
  assert.notEqual(seenByA, seenByB, 'Default Player aliases were not unique');

  a.client.drop();
  const back = greeted(env, 1, { name: 'Player' });
  visible(back);
  assert.equal(rows(b.client)[0].name, seenByB, 'the alias changed after a reconnect');

  const custom = pair({ names: ['Saulo', 'Player1'] });
  assert.deepEqual(rows(custom.b.client).map((p) => p.name), ['Saulo'], 'a custom name was changed in the Lobby');
  assert.deepEqual(rows(custom.a.client).map((p) => p.name), ['Player1'], 'a custom name was given an alias');
});

test('the public handle is stable while listed, replaced by a visibility cycle, and is never an identity', () => {
  const { a, b, codeA } = pair();
  const first = rows(b.client)[0];
  assert.match(first.player, HANDLE, 'the handle is not 32 lowercase hex characters');
  visible(a.client);
  assert.equal(rows(b.client)[0].player, first.player, 'the handle changed while the player stayed visible');

  visible(a.client, false);
  assert.deepEqual(rows(b.client), [], 'a player who turned the Lobby Off is still listed');
  visible(a.client);
  const second = rows(b.client)[0];
  assert.match(second.player, HANDLE);
  assert.notEqual(second.player, first.player, 'Lobby handle did not rotate after visibility cycle');

  for (const handle of [first.player, second.player]) {
    assert.notEqual(handle, codeA);
    assert.equal(mentions(handle, codeA), false, 'the handle contains the friend code');
    assert.notEqual(handle, first.name);
  }
  assert.deepEqual(Object.keys(second).sort(), ['name', 'player', 'state', 'version'], 'a Lobby row carries more than handle, name, version and relation');
});

test('send, pending, accept: the same request id on both sides and no friend code before acceptance', () => {
  const ctx = pair();
  const { a, b, codeA, codeB } = ctx;
  const handle = rows(a.client)[0].player;
  const added = a.call('friends.add', { player: handle });
  assert.deepEqual([added.code, added.state], ['ok', 'pending'], 'the Lobby friend request was refused');
  assert.equal(rowOf(a.client, rows(a.client)[0].name).state, 'pending', 'the requester does not see Pending');

  const [event] = b.events('friend.request');
  const incoming = b.call('friends.requests').incoming;
  const outgoing = a.call('friends.requests').outgoing;
  assert.equal(incoming.length, 1, 'the receiver has no incoming request');
  assert.equal(new Set([added.request, event.request, incoming[0].request, outgoing[0].request]).size, 1,
    'the request id differs between the two sides');
  assert.match(incoming[0].display_name, ALIAS, 'the receiver does not see the sender alias');
  assert.match(outgoing[0].display_name, ALIAS, 'the requester does not see the target alias');
  assert.equal(rows(b.client)[0].state, 'incoming');
  a.call('lobby.list');
  b.call('lobby.list');
  a.call('friends.list');
  b.call('friends.list');
  assertNoCodeBeforeAcceptance(ctx);

  const accepted = b.call('friends.accept', { request: event.request });
  assert.equal(accepted.code, 'ok', 'the request could not be accepted by its id');
  const listA = a.call('friends.list').friends.map((f) => f.player);
  const listB = b.call('friends.list').friends.map((f) => f.player);
  assert.deepEqual([listA, listB], [[codeB], [codeA]], 'accepted friends are missing from Friends');
  assert.equal(mentions(a.call('friends.list'), codeB), true, 'the leak detector cannot see a friend code that is present');
  assert.equal(a.events('friend.added')[0].friend.player, codeB);
  assert.equal(rowOf(a.client, rows(a.client)[0].name)?.state, 'friend', 'the Lobby does not show Friend');
  assert.deepEqual([a.call('friends.requests').outgoing, b.call('friends.requests').incoming], [[], []]);
});

test('decline clears both sides without a friendship or a leak', () => {
  const ctx = pair();
  const { a, b } = ctx;
  a.call('friends.add', { player: rows(a.client)[0].player });
  const [event] = b.events('friend.request');
  assert.equal(b.call('friends.decline', { request: event.request }).code, 'ok');
  assert.deepEqual([b.call('friends.requests').incoming, a.call('friends.requests').outgoing], [[], []],
    'a declined request is still listed');
  assert.deepEqual([a.call('friends.list').friends, b.call('friends.list').friends], [[], []], 'a declined request became a friendship');
  assert.equal(rows(a.client)[0].state, 'none', 'the requester is still Pending after a decline');
  assert.equal(rows(b.client)[0].state, 'none', 'the receiver is still Incoming after a decline');
  assertNoCodeBeforeAcceptance(ctx);
});

test('withdrawing a request removes it for both and its id stops working', () => {
  const ctx = pair();
  const { a, b } = ctx;
  const added = a.call('friends.add', { player: rows(a.client)[0].player });
  b.events('friend.request');
  assert.equal(a.call('friends.decline', { request: added.request }).code, 'ok');
  assert.deepEqual([b.call('friends.requests').incoming, a.call('friends.requests').outgoing], [[], []],
    'a withdrawn request is still listed');
  assert.equal(b.call('friends.accept', { request: added.request }).code, 'not_found', 'a withdrawn request id still works');
  assert.deepEqual([a.call('friends.list').friends, b.call('friends.list').friends], [[], []]);
  assertNoCodeBeforeAcceptance(ctx);
});

test('a pending request survives the target replacing its handle', () => {
  const ctx = pair();
  const { env, a, b } = ctx;
  const aliasOfB = rows(a.client)[0].name;
  const oldHandle = rows(a.client)[0].player;
  a.call('friends.add', { player: oldHandle });
  const [event] = b.events('friend.request');

  visible(b.client, false);
  visible(b.client);
  const fresh = rows(a.client)[0];
  assert.notEqual(fresh.player, oldHandle, 'Lobby handle did not rotate after visibility cycle');
  assert.equal(fresh.name, aliasOfB, 'the alias changed when the handle rotated');
  assert.equal(fresh.state, 'pending', 'Pending request was lost after Lobby handle rotation');
  assert.equal(a.call('friends.requests').outgoing.length, 1);
  a.call('lobby.list');
  b.call('lobby.list');
  assertNoCodeBeforeAcceptance(ctx);
  assert.equal(b.call('friends.accept', { request: event.request }).code, 'ok', 'the request id stopped working after the handle rotated');
  assert.equal(env.hub.areFriends(identity(1).player, identity(2).player), true, 'the friendship did not survive the handle rotation');
});

test('requests carry the exact custom names', () => {
  const { a, b } = pair({ names: ['Saulo', 'Player1'] });
  const added = a.call('friends.add', { player: rowOf(a.client, 'Player1').player });
  assert.deepEqual([added.name, added.display_name], ['Player1', undefined]);
  const [event] = b.events('friend.request');
  assert.deepEqual([event.name, event.display_name], ['Saulo', undefined]);
  assert.deepEqual(b.call('friends.requests').incoming.map((r) => r.name), ['Saulo']);
  assert.deepEqual(a.call('friends.requests').outgoing.map((r) => r.name), ['Player1']);
});

test('players who are already friends show Friend and cannot gain a duplicate request or relationship', () => {
  const { env, a, b } = pair({ names: ['Saulo', 'Player1'] });
  a.call('friends.add', { player: rowOf(a.client, 'Player1').player });
  b.call('friends.accept', { request: b.events('friend.request')[0].request });
  assert.equal(rowOf(a.client, 'Player1').state, 'friend', 'the Lobby does not show Friend for a friend');
  assert.equal(rowOf(b.client, 'Saulo').state, 'friend');

  const again = a.call('friends.add', { player: rowOf(a.client, 'Player1').player });
  assert.deepEqual([again.code, again.state], ['ok', 'friends']);
  assert.equal(b.events('friend.request').length, 0, 'a friend got a new request event');
  assert.equal(env.host.sql.get('SELECT COUNT(*) AS n FROM friend_requests').n, 0, 'a duplicate request was stored');
  assert.equal(env.host.sql.get('SELECT COUNT(*) AS n FROM friends').n, 2, 'the friends table gained a duplicate relationship');
});

test('a player who turns the Lobby Off disappears and the old handle is refused', () => {
  const { a, b } = pair();
  const old = rows(a.client)[0].player;
  visible(b.client, false);
  assert.deepEqual(rows(a.client), [], 'the Lobby still lists a player who turned it Off');
  assert.equal(a.call('friends.add', { player: old }).code, 'not_found', 'an old handle still reaches a hidden player');
  assert.equal(a.call('friends.decline', { player: old }).code, 'not_found');
});

test('a request made by friend code keeps its legacy shape beside the request id', () => {
  const env = makeHub();
  const a = new Tap(greeted(env, 1, { name: 'Alice' }));
  const b = new Tap(greeted(env, 2, { name: 'Bob' }));
  const added = b.call('friends.add', { code: identity(1).player });
  assert.deepEqual([added.state, added.player], ['pending', identity(1).player]);
  const [event] = a.events('friend.request');
  assert.deepEqual([event.player, event.name], [identity(2).player, 'Bob']);
  assert.equal(a.call('friends.accept', { player: identity(2).player }).code, 'ok');
});
