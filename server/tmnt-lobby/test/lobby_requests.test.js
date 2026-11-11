// A friend request made from the public Lobby never carries a friend code before it is accepted: it is named by an
// opaque request id. Requests made by friend code keep their wire shape for older clients.
import assert from 'node:assert/strict';
import { mkdtempSync, rmSync } from 'node:fs';
import { tmpdir } from 'node:os';
import { join } from 'node:path';
import { DatabaseSync } from 'node:sqlite';
import { test } from 'node:test';

import { sha256Hex } from '../src/sha256.js';
import { greeted, identity, makeHub } from './harness.js';

const HEX32 = /^[0-9a-f]{32}$/;
const visible = (client, on = true) => client.call('lobby.publish', { visible: on });
const handleOf = (viewer, name) => viewer.call('lobby.list').players.find((p) => p.name === name).player;
const holds = (value, code) => JSON.stringify(value).includes(code);

// Alice is the target, Bob the requester; both are listed.
function lobbyPair() {
  const env = makeHub();
  const alice = greeted(env, 1, { name: 'Alice' });
  const bob = greeted(env, 2, { name: 'Bob' });
  visible(alice);
  visible(bob);
  return { env, alice, bob, aliceCode: identity(1).player, bobCode: identity(2).player };
}

test('a Lobby handle cannot be exchanged for a friend code before the request is accepted', () => {
  const { alice, bob, aliceCode, bobCode } = lobbyPair();
  const added = bob.call('friends.add', { player: handleOf(bob, 'Alice') });
  assert.deepEqual([added.code, added.state, added.name], ['ok', 'pending', 'Alice']);
  assert.equal(Object.hasOwn(added, 'player'), false);
  assert.match(added.request, HEX32);

  const asked = [added, bob.call('friends.requests'), bob.call('lobby.list'), bob.call('friends.list'), bob.inbox];
  assert.equal(asked.some((reply) => holds(reply, aliceCode)), false, 'the requester learned the target code');
  const heard = [alice.call('friends.requests'), alice.call('lobby.list'), alice.call('friends.list'), alice.inbox];
  assert.equal(heard.some((reply) => holds(reply, bobCode)), false, 'the target learned the requester code');
  assert.deepEqual(bob.call('friends.list').friends, []);
  assert.deepEqual(alice.call('friends.list').friends, []);
});

test('the request is named by one id on both sides and is not derived from the codes', () => {
  const { alice, bob, aliceCode, bobCode } = lobbyPair();
  const added = bob.call('friends.add', { player: handleOf(bob, 'Alice') });
  const [event] = alice.events('friend.request');
  const [incoming] = alice.call('friends.requests').incoming;
  const [outgoing] = bob.call('friends.requests').outgoing;
  assert.equal(new Set([added.request, event.request, incoming.request, outgoing.request]).size, 1);
  assert.deepEqual([incoming.name, outgoing.name], ['Bob', 'Alice']);
  for (const row of [incoming, outgoing]) assert.equal(Object.hasOwn(row, 'player'), false);
  assert.equal(Object.hasOwn(event, 'player'), false);
  for (const guess of [`${bobCode}|${aliceCode}`, `${bobCode}${aliceCode}`, `${aliceCode}|${bobCode}`]) {
    assert.notEqual(added.request, sha256Hex(guess).slice(0, 32));
  }

});

test('the same two players get another request id from a service with another salt', () => {
  const ids = ['a', 'b'].map((digit) => {
    const { env, bob } = lobbyPair();
    env.hub.setMeta('salt', digit.repeat(32));
    return bob.call('friends.add', { player: handleOf(bob, 'Alice') }).request;
  });
  assert.match(ids[0], HEX32);
  assert.notEqual(ids[0], ids[1]);
});

test('accepting by request id makes friends, and only then do the friend codes appear', () => {
  const { alice, bob, aliceCode, bobCode } = lobbyPair();
  bob.call('friends.add', { player: handleOf(bob, 'Alice') });
  const [event] = alice.events('friend.request');

  const accepted = alice.call('friends.accept', { request: event.request });
  assert.equal(accepted.code, 'ok');
  assert.equal(accepted.friend.player, bobCode);
  assert.deepEqual(alice.call('friends.list').friends.map((f) => f.player), [bobCode]);
  assert.deepEqual(bob.call('friends.list').friends.map((f) => f.player), [aliceCode]);
  const [added] = bob.events('friend.added');
  assert.equal(added.friend.player, aliceCode);
  assert.deepEqual(alice.call('friends.requests'), { re: alice.seq, code: 'ok', incoming: [], outgoing: [] });
  assert.equal(bob.call('lobby.list').players.find((p) => p.name === 'Alice').state, 'friend');
});

test('the request and the friendship survive the handle being replaced', () => {
  const { alice, bob, aliceCode } = lobbyPair();
  const first = handleOf(bob, 'Alice');
  const added = bob.call('friends.add', { player: first });
  const [event] = alice.events('friend.request');

  visible(alice, false);
  visible(alice);
  const second = handleOf(bob, 'Alice');
  assert.notEqual(second, first);
  assert.equal(bob.call('lobby.list').players.find((p) => p.name === 'Alice').state, 'pending');
  assert.equal(bob.call('friends.requests').outgoing[0].request, added.request);
  assert.equal(bob.call('friends.add', { player: second }).request, added.request);
  assert.equal(alice.call('friends.accept', { request: event.request }).code, 'ok');
  assert.deepEqual(bob.call('friends.list').friends.map((f) => f.player), [aliceCode]);
});

test('a request is declined by the receiver and withdrawn by the sender with its id', () => {
  const { alice, bob } = lobbyPair();
  bob.call('friends.add', { player: handleOf(bob, 'Alice') });
  const [event] = alice.events('friend.request');
  assert.equal(alice.call('friends.decline', { request: event.request }).code, 'ok');
  assert.deepEqual(alice.call('friends.requests').incoming, []);
  assert.deepEqual(bob.call('friends.requests').outgoing, []);
  assert.equal(bob.call('lobby.list').players.find((p) => p.name === 'Alice').state, 'none');
  assert.equal(alice.call('friends.decline', { request: event.request }).code, 'not_found');

  const sent = bob.call('friends.add', { player: handleOf(bob, 'Alice') });
  assert.equal(bob.call('friends.decline', { request: sent.request }).code, 'ok');
  assert.deepEqual(alice.call('friends.requests').incoming, []);
  assert.deepEqual(bob.call('friends.list').friends, []);
});

test('a request id only works for the two players of that request, and only as the receiver accepts', () => {
  const { env, alice, bob } = lobbyPair();
  const stranger = greeted(env, 3, { name: 'Cat' });
  const sent = bob.call('friends.add', { player: handleOf(bob, 'Alice') });
  alice.events('friend.request');

  for (const op of ['friends.accept', 'friends.decline']) {
    assert.equal(stranger.call(op, { request: sent.request }).code, 'not_found', op);
    assert.equal(stranger.call(op, { request: 'zz' }).code, 'bad_request', op);
    assert.equal(stranger.call(op, { request: 7 }).code, 'bad_request', op);
    assert.equal(alice.call(op, { request: 'f'.repeat(32) }).code, 'not_found', op);
  }
  assert.equal(bob.call('friends.accept', { request: sent.request }).code, 'not_found');
  assert.deepEqual(bob.call('friends.list').friends, []);
  assert.equal(alice.call('friends.requests').incoming.length, 1);
});

test('two Lobby players adding each other become friends without a code in the answer', () => {
  const { alice, bob, aliceCode, bobCode } = lobbyPair();
  bob.call('friends.add', { player: handleOf(bob, 'Alice') });
  const mutual = alice.call('friends.add', { player: handleOf(alice, 'Bob') });
  assert.deepEqual([mutual.code, mutual.state], ['ok', 'friends']);
  assert.equal(Object.hasOwn(mutual, 'player'), false);
  assert.deepEqual(alice.call('friends.list').friends.map((f) => f.player), [bobCode]);
  assert.deepEqual(bob.call('friends.list').friends.map((f) => f.player), [aliceCode]);
});

test('a custom name and a default alias both reach the request without a code', () => {
  const env = makeHub();
  env.hub.setMeta('salt', '0123456789abcdef0123456789abcdef');
  const plain = greeted(env, 1, { name: 'Player' });
  const named = greeted(env, 2, { name: 'Raphael' });
  visible(plain);
  visible(named);
  const alias = named.call('lobby.list').players[0].name;
  assert.match(alias, /^Player-[0-9A-F]{4}$/);
  const added = named.call('friends.add', { player: handleOf(named, alias) });
  assert.deepEqual([added.name, added.display_name], ['Player', alias]);
  const [event] = plain.events('friend.request');
  assert.deepEqual([event.name, event.display_name], ['Raphael', undefined]);
  assert.equal(Object.hasOwn(event, 'player'), false);
  assert.equal(plain.call('friends.requests').incoming[0].name, 'Raphael');
});

test('a request made by friend code keeps its code for older clients and still accepts by it', () => {
  const env = makeHub();
  const alice = greeted(env, 1, { name: 'Alice' });
  const bob = greeted(env, 2, { name: 'Bob' });
  const aliceCode = identity(1).player;
  const bobCode = identity(2).player;

  const added = bob.call('friends.add', { code: aliceCode });
  assert.deepEqual([added.state, added.player], ['pending', aliceCode]);
  const [event] = alice.events('friend.request');
  assert.deepEqual([event.player, event.name], [bobCode, 'Bob']);
  assert.equal(alice.call('friends.requests').incoming[0].player, bobCode);
  assert.equal(bob.call('friends.requests').outgoing[0].player, aliceCode);
  assert.equal(alice.call('friends.accept', { player: bobCode }).code, 'ok');
  assert.deepEqual(bob.call('friends.list').friends.map((f) => f.player), [aliceCode]);

  const cat = greeted(env, 3, { name: 'Cat' });
  cat.call('friends.add', { code: aliceCode });
  alice.events('friend.request');
  assert.equal(alice.call('friends.decline', { player: identity(3).player }).code, 'ok');
  assert.deepEqual(alice.call('friends.requests').incoming, []);
});

test('requests stored before the origin was recorded stay readable and acceptable', () => {
  const folder = mkdtempSync(join(tmpdir(), 'tmnt-requests-'));
  const file = join(folder, 'lobby.sqlite');
  const first = makeHub({ db: file });
  const bob = greeted(first, 2, { name: 'Bob' });
  greeted(first, 1, { name: 'Alice' });
  bob.call('friends.add', { code: identity(1).player });
  first.host.sql.close();
  const db = new DatabaseSync(file);
  db.exec('ALTER TABLE friend_requests DROP COLUMN lobby');
  db.close();

  const upgraded = makeHub({ db: file });
  const alice = greeted(upgraded, 1, { name: 'Alice' });
  const [incoming] = alice.call('friends.requests').incoming;
  assert.deepEqual([incoming.player, incoming.name], [identity(2).player, 'Bob']);
  assert.match(incoming.request, HEX32);
  assert.equal(alice.call('friends.accept', { request: incoming.request }).code, 'ok');
  assert.deepEqual(alice.call('friends.list').friends.map((f) => f.player), [identity(2).player]);
  upgraded.host.sql.close();
  rmSync(folder, { recursive: true, force: true });
});
