import assert from 'node:assert/strict';
import { test } from 'node:test';

import { CAPABILITIES, SNES_KEYS, combineSnes, normalizeSnes, snesTable, supportedSnes } from '../src/protocol.js';
import { buildParty, makeHub } from './harness.js';

const ID = '1a2b3c4d';

// One announcement: `supports` and `enabled` list the mechanics, `ids` overrides the data id of one.
const announce = ({ supports = SNES_KEYS, enabled = supports, ids = {} } = {}) => ({
  supports: Object.fromEntries(SNES_KEYS.map((key) => [key, supports.includes(key)])),
  enabled: Object.fromEntries(SNES_KEYS.map((key) => [key, enabled.includes(key)])),
  compat: Object.fromEntries(SNES_KEYS.filter((key) => supports.includes(key)).map((key) => [key, ids[key] ?? ID])),
});

const none = Object.fromEntries(SNES_KEYS.map((key) => [key, false]));
const only = (...keys) => Object.fromEntries(SNES_KEYS.map((key) => [key, keys.includes(key)]));

// A party whose members greet their room in order with the given announcements (undefined: an old client).
function party(announcements) {
  const env = makeHub();
  const built = buildParty(env, announcements.length, { connect: false });
  const rooms = built.data.map((data, i) => {
    const room = env.room(built.match, built.clients[i].who);
    room.hello(data, announcements[i] === undefined ? {} : { snes: announcements[i] });
    return room;
  });
  return { ...built, env, rooms };
}

const latest = (room) => room.inbox.filter((m) => m.ev === 'match.snes').at(-1)?.snes;
const clear = (room) => {
  room.inbox = [];
};
const tableOf = (view) => ({ supported: view.supported, players: view.players });

test('only the three gameplay mechanics are negotiated, never the soundtrack', () => {
  assert.deepEqual(SNES_KEYS, ['directional_combat', 'throw_control', 'dash']);
  assert.equal(CAPABILITIES.snes_features, true);
  const withSoundtrack = announce();
  withSoundtrack.supports.classic_soundtrack = true;
  withSoundtrack.enabled.classic_soundtrack = true;
  const { rooms } = party([withSoundtrack, announce()]);
  const view = rooms[1].helloReply.snes;
  assert.deepEqual(Object.keys(view.supported), SNES_KEYS);
  assert.deepEqual(Object.keys(view.players['1']), SNES_KEYS);
  assert.equal('classic_soundtrack' in view.peer.supports, false);
});

for (const feature of SNES_KEYS) {
  test(`${feature}: each player's choice applies to their own turtle, every member gets the same table`, () => {
    for (const [a, b] of [[true, true], [true, false], [false, true], [false, false]]) {
      const on = (value) => announce({ enabled: value ? [feature] : [] });
      const { rooms } = party([on(a), on(b)]);
      const fromB = tableOf(rooms[1].helloReply.snes);
      const fromA = tableOf(latest(rooms[0]));
      assert.deepEqual(fromA, fromB);
      assert.equal(fromB.supported[feature], true, 'an opponent switching it off never makes it unavailable');
      assert.equal(fromB.players['1'][feature], a, `A ${a} B ${b}`);
      assert.equal(fromB.players['2'][feature], b, `A ${a} B ${b}`);
    }
  });

  test(`${feature}: unavailable to everyone when one player does not support it`, () => {
    const without = SNES_KEYS.filter((key) => key !== feature);
    const { rooms } = party([announce(), announce({ supports: without, enabled: SNES_KEYS })]);
    const view = rooms[1].helloReply.snes;
    assert.equal(view.supported[feature], false);
    assert.equal(view.players['1'][feature], false);
    assert.equal(view.players['2'][feature], false);
    assert.equal(view.supported[without[0]], true);
    assert.equal(latest(rooms[0]).peer.supports[feature], false);
  });

  test(`${feature}: different data ids make it unavailable to everyone`, () => {
    const { rooms } = party([announce({ ids: { [feature]: '11111111' } }), announce({ ids: { [feature]: '22222222' } })]);
    for (const view of [rooms[1].helloReply.snes, latest(rooms[0])]) {
      assert.equal(view.supported[feature], false);
      assert.equal(view.players['1'][feature] || view.players['2'][feature], false);
    }
    assert.equal(rooms[1].helloReply.snes.peer.compat[feature], '11111111');
  });

  test(`${feature}: four players, one without support or an old client: unavailable to all four`, () => {
    const without = SNES_KEYS.filter((key) => key !== feature);
    for (const odd of [announce({ supports: without }), undefined]) {
      const { rooms } = party([announce(), announce(), announce(), odd]);
      for (const room of rooms.slice(0, 3)) assert.equal(latest(room).supported[feature], false);
      assert.equal(rooms[3].helloReply.snes.supported[feature], false);
    }
    const { rooms } = party([announce(), announce(), announce(), announce({ enabled: without })]);
    assert.equal(rooms[3].helloReply.snes.supported[feature], true);
    assert.equal(rooms[3].helloReply.snes.players['4'][feature], false);
    assert.equal(rooms[3].helloReply.snes.players['1'][feature], true);
  });
}

test('a mixed table resolves the same for every member', () => {
  const a = announce({ enabled: ['directional_combat', 'throw_control'] });
  const b = announce({ enabled: ['throw_control', 'dash'] });
  const { rooms } = party([a, b]);
  const expected = { supported: only(...SNES_KEYS), players: { 1: only('directional_combat', 'throw_control'), 2: only('throw_control', 'dash') } };
  assert.deepEqual(tableOf(rooms[1].helloReply.snes), expected);
  assert.deepEqual(tableOf(latest(rooms[0])), expected);
});

test('a player without dash support takes dash away from everyone, the others stay', () => {
  const { rooms } = party([announce(), announce({ supports: ['directional_combat', 'throw_control'] })]);
  assert.deepEqual(rooms[1].helloReply.snes.supported, only('directional_combat', 'throw_control'));
  assert.deepEqual(rooms[1].helloReply.snes.players['1'], only('directional_combat', 'throw_control'));
});

test('no gameplay data or an old client on one side: nothing is available and the match still joins', () => {
  for (const odd of [announce({ supports: [] }), undefined]) {
    const { rooms } = party([announce(), odd]);
    assert.equal(rooms[1].helloReply.code, 'ok');
    assert.deepEqual(rooms[1].helloReply.snes.supported, none);
    assert.deepEqual(latest(rooms[0]).supported, none);
  }
});

test('a player alone, or waiting for another to greet the room, has no peer and an empty table', () => {
  assert.deepEqual(party([announce()]).rooms[0].helloReply.snes, { supported: none, players: {} });
  const env = makeHub();
  const built = buildParty(env, 3, { connect: false });
  const first = env.room(built.match, built.clients[0].who);
  first.hello(built.data[0], { snes: announce() });
  const second = env.room(built.match, built.clients[1].who);
  second.hello(built.data[1], { snes: announce() });
  assert.deepEqual(second.helloReply.snes, { supported: none, players: {} });
  const third = env.room(built.match, built.clients[2].who);
  third.hello(built.data[2], { snes: announce() });
  assert.deepEqual(third.helloReply.snes.supported, only(...SNES_KEYS));
  assert.deepEqual(Object.keys(latest(first).players), ['1', '2', '3']);
});

test('a choice changed before the lock reaches the others', () => {
  const { rooms } = party([announce(), announce()]);
  clear(rooms[0]);
  const answer = rooms[1].call('snes.update', { snes: announce({ enabled: ['dash'] }) });
  assert.equal(answer.code, 'ok');
  assert.deepEqual(answer.snes.players['2'], only('dash'));
  assert.deepEqual(latest(rooms[0]).players['2'], only('dash'));
  assert.deepEqual(latest(rooms[0]).players['1'], only(...SNES_KEYS));
  clear(rooms[0]);
  rooms[1].call('snes.update', { snes: announce({ enabled: ['dash'] }) });
  assert.equal(latest(rooms[0]), undefined);
});

test('updates and locks need a greeted socket and a round', () => {
  const env = makeHub();
  const built = buildParty(env, 2, { connect: false });
  const room = env.room(built.match, built.clients[0].who);
  assert.equal(room.call('snes.update', { snes: announce() }).code, 'no_session');
  assert.equal(room.call('snes.lock', { round: 1 }).code, 'no_session');
  const { rooms } = party([announce(), announce()]);
  for (const round of [undefined, 0, 'x', 1.5, 2000000]) assert.equal(rooms[0].call('snes.lock', { round }).code, 'bad_request');
});

test('match 1 locks one table for everyone, a change during it waits, match 2 locks the new table', () => {
  const { rooms } = party([announce({ enabled: ['dash'] }), announce({ enabled: [] })]);
  rooms.forEach(clear);
  const first = rooms[0].call('snes.lock', { round: 1 });
  const match1 = { round: 1, supported: only(...SNES_KEYS), players: { 1: only('dash'), 2: none } };
  assert.deepEqual(first.snes.locked, match1);
  assert.deepEqual(latest(rooms[1]).locked, match1);
  assert.deepEqual(rooms[1].call('snes.lock', { round: 1 }).snes.locked, match1);

  rooms[1].call('snes.update', { snes: announce({ enabled: ['dash'] }) });
  assert.deepEqual(latest(rooms[0]).locked, match1);
  assert.deepEqual(rooms[0].call('snes.lock', { round: 1, off: true }).snes.locked, match1);

  const second = rooms[1].call('snes.lock', { round: 2 });
  assert.deepEqual(second.snes.locked, { round: 2, supported: only(...SNES_KEYS), players: { 1: only('dash'), 2: only('dash') } });
  assert.deepEqual(latest(rooms[0]).locked.round, 2);
  assert.deepEqual(rooms[0].call('snes.lock', { round: 1 }).snes.locked, match1);
});

test('a member that gave up waiting locks its round all Off for everyone when nothing is fixed yet', () => {
  const { rooms } = party([announce(), announce()]);
  clear(rooms[1]);
  assert.deepEqual(rooms[0].call('snes.lock', { round: 1, off: true }).snes.locked, { round: 1, supported: none, players: {} });
  assert.deepEqual(latest(rooms[1]).locked, { round: 1, supported: none, players: {} });
  assert.deepEqual(rooms[1].call('snes.lock', { round: 1 }).snes.locked.supported, none);
});

test('a member that greets the room after the lock is told the locked table', () => {
  const env = makeHub();
  const built = buildParty(env, 2, { connect: false });
  const first = env.room(built.match, built.clients[0].who);
  first.hello(built.data[0], { snes: announce() });
  const second = env.room(built.match, built.clients[1].who);
  second.hello(built.data[1], { snes: announce() });
  first.call('snes.lock', { round: 1 });
  const again = env.room(built.match, built.clients[1].who);
  again.hello(built.data[1], { snes: announce({ enabled: [] }) });
  assert.deepEqual(again.helloReply.snes.locked.players['2'], only(...SNES_KEYS));
  assert.deepEqual(again.helloReply.snes.players['2'], none);
});

test('a member that leaves takes its announcement with it', () => {
  const { rooms } = party([announce(), announce(), announce({ supports: [] })]);
  assert.deepEqual(latest(rooms[0]).supported, none);
  clear(rooms[0]);
  rooms[2].call('match.leave');
  assert.deepEqual(latest(rooms[0]).supported, only(...SNES_KEYS));
  assert.deepEqual(Object.keys(latest(rooms[0]).players), ['1', '2']);
});

test('announcements are normalized: nothing is enabled or identified without being supported', () => {
  const messy = normalizeSnes({
    supports: { directional_combat: true, dash: 'yes', classic_soundtrack: true },
    enabled: { directional_combat: true, dash: true, throw_control: true, classic_soundtrack: true },
    compat: { directional_combat: ID, dash: 'XYZ', throw_control: ID },
  });
  assert.deepEqual(messy.supports, only('directional_combat'));
  assert.deepEqual(messy.enabled, only('directional_combat'));
  assert.deepEqual(messy.compat, { directional_combat: ID, throw_control: '', dash: '' });
  for (const value of [undefined, null, 5, 'x', [], { supports: 'all' }]) {
    assert.deepEqual(normalizeSnes(value).supports, none);
  }
});

test('the rules are pure: support is global, choices stay per slot', () => {
  const a = normalizeSnes(announce({ enabled: ['dash'] }));
  const b = normalizeSnes(announce({ enabled: [] }));
  assert.deepEqual(supportedSnes([a, b]), only(...SNES_KEYS));
  assert.deepEqual(supportedSnes([a]), none);
  assert.equal(supportedSnes([a, normalizeSnes(announce({ supports: [] }))]).dash, false);
  assert.equal(supportedSnes([a, normalizeSnes(announce({ ids: { dash: '99999999' } }))]).dash, false);
  assert.deepEqual(snesTable([[1, a], [2, b]]).players, { 1: only('dash'), 2: none });
  assert.equal('enabled' in combineSnes([a, b]), false);
});
