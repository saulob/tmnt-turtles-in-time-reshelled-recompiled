import assert from 'node:assert/strict';
import { test } from 'node:test';

import { RELAY, encodeRelayFrame, parseRelayFrame } from '../src/protocol.js';
import { buildParty, makeHub } from './harness.js';

const bytes = (...values) => Uint8Array.from(values);
const drops = (env, match) => env.rooms.ensure(match).room.stats().drops;
const received = (rooms) => rooms.map((r) => r.binary.length);
const clear = (rooms) => rooms.forEach((r) => {
  r.binary = [];
  r.inbox = [];
});

function chooseTransport(rooms, a, b, transport) {
  assert.equal(rooms[a - 1].call('match.transport', { peer: b, transport }).code, 'ok');
}

test('relay frames carry the sender and the destination slot in a 16-byte header; malformed ones say why', () => {
  const frame = encodeRelayFrame(2, 3, 0xdeadbeef, 7, bytes(1, 2, 3));
  assert.equal(RELAY.headerBytes, 16);
  assert.equal(RELAY.version, 2);
  assert.deepEqual([...frame.subarray(0, 6)], [0x53, 0x52, 2, 1, 2, 3]);
  assert.deepEqual(parseRelayFrame(frame), { type: RELAY.data, src: 2, dst: 3, token: 0xdeadbeef, seq: 7, length: 3 });
  const bad = (edit) => {
    const copy = Uint8Array.from(frame);
    edit(copy);
    return parseRelayFrame(copy).error;
  };
  assert.equal(bad((f) => (f[0] = 0x7b)), 'bad_magic');
  assert.equal(bad((f) => (f[2] = 9)), 'bad_version');
  assert.equal(bad((f) => (f[2] = 1)), 'bad_version');
  assert.equal(bad((f) => (f[3] = 3)), 'bad_type');
  assert.equal(parseRelayFrame(frame.subarray(0, 12)).error, 'truncated');
  assert.equal(parseRelayFrame(frame.subarray(0, 3)).error, 'truncated');
  assert.equal(parseRelayFrame(frame.subarray(0, frame.length - 1)).error, 'bad_length');
  assert.equal(parseRelayFrame(encodeRelayFrame(1, 2, 1, 1, new Uint8Array(RELAY.maxPayload + 1))).error, 'oversized');
  assert.equal(parseRelayFrame(encodeRelayFrame(1, 2, 1, 1, new Uint8Array(0))).error, 'bad_length');
  assert.equal(parseRelayFrame(encodeRelayFrame(1, 2, 1, 1, new Uint8Array(RELAY.maxPayload))).length, RELAY.maxPayload);
  assert.equal(parseRelayFrame('text').error, 'not_binary');
});

test('each room has its own relay token', () => {
  const env = makeHub();
  const one = buildParty(env, 2, { first: 1 });
  const two = buildParty(env, 2, { first: 20 });
  assert.ok(Number.isInteger(one.token) && one.token > 0 && one.token <= 0xffffffff);
  assert.notEqual(one.token, two.token);
  assert.equal(one.rooms[1].helloReply.relay_token, one.token);
});

test('a frame goes from any member to any other, unchanged, and to nobody else', () => {
  const env = makeHub();
  const { rooms, token } = buildParty(env, 4);
  let seq = 0;
  for (let src = 1; src <= 4; src++) {
    for (let dst = 1; dst <= 4; dst++) {
      if (src === dst) continue;
      clear(rooms);
      const frame = encodeRelayFrame(src, dst, token, ++seq, bytes(0x53, 0x46, src, dst, 9));
      rooms[src - 1].relay(frame);
      assert.deepEqual(rooms.map((r) => r.binary.length), rooms.map((_, i) => (i === dst - 1 ? 1 : 0)), `${src}->${dst}`);
      assert.deepEqual(rooms[dst - 1].binary[0], frame);
      assert.equal(rooms[src - 1].inbox.length, 0);
    }
  }
  assert.deepEqual(drops(env, rooms[0].match), {});
});

test('the first forwards of each direction are logged, never their payload', () => {
  const env = makeHub();
  const { rooms, token, match } = buildParty(env, 3);
  const secret = bytes(0xca, 0xfe, 0xba, 0xbe, 0x12, 0x34);
  for (let i = 0; i < 10; i++) rooms[0].relay(encodeRelayFrame(1, 3, token, i, secret));
  rooms[2].relay(encodeRelayFrame(3, 1, token, 1, secret));
  const forwards = env.logs.filter((l) => l.startsWith('relay forward'));
  assert.equal(forwards.filter((l) => l.includes('src=1 dst=3')).length, RELAY.loggedForwards);
  assert.equal(forwards.filter((l) => l.includes('src=3 dst=1')).length, 1);
  assert.ok(forwards.includes(`relay forward match=${match} src=1 dst=3 bytes=${RELAY.headerBytes + secret.length}`));
  const text = env.logs.join('\n').toLowerCase();
  assert.ok(!text.includes('cafebabe') && !text.includes('202,254'));
  const tables = env.rooms.ensure(match).host.sql.all("SELECT name FROM sqlite_master WHERE type = 'table'").map((r) => r.name);
  assert.deepEqual(tables.filter((n) => !['room_meta', 'members', 'slot_generations', 'pairs', 'links'].includes(n)), []);
});

test('a sender cannot claim another slot: the frame is dropped and the sender told once', () => {
  const env = makeHub();
  const { rooms, token, match } = buildParty(env, 4);
  const spoof = encodeRelayFrame(1, 3, token, 1, bytes(1, 2, 3));
  const told = rooms[1].relay(spoof);
  rooms[1].relay(spoof);
  assert.deepEqual(received(rooms), [0, 0, 0, 0]);
  assert.deepEqual(told, [{ ev: 'relay.closed', peer: 3, reason: 'unavailable' }]);
  assert.equal(rooms[1].events('relay.closed').length, 1);
  assert.equal(drops(env, match).src_mismatch, 2);
  assert.equal(env.logs.filter((l) => l.startsWith('relay drop reason=src_mismatch')).length, 2);
  rooms[1].relay(encodeRelayFrame(2, 3, token, 2, bytes(1)));
  assert.equal(rooms[2].binary.length, 1);
});

test('a destination that is not a participant, is the sender or is not connected is refused', () => {
  const env = makeHub();
  const { data, match } = buildParty(env, 4, { connect: false });
  const rooms = [0, 1, 2].map((i) => {
    const r = env.room(match);
    r.hello(data[i]);
    return r;
  });
  const token = rooms[0].helloReply.relay_token;
  for (const dst of [0, 5, 9, 255]) rooms[0].relay(encodeRelayFrame(1, dst, token, 1, bytes(1)));
  rooms[0].relay(encodeRelayFrame(1, 1, token, 1, bytes(1)));
  assert.equal(drops(env, match).dst_invalid, 5);
  rooms[0].relay(encodeRelayFrame(1, 4, token, 1, bytes(1)));
  assert.equal(drops(env, match).dst_unavailable, 1);
  rooms[2].drop();
  const told = rooms[0].relay(encodeRelayFrame(1, 3, token, 2, bytes(1)));
  assert.equal(drops(env, match).dst_unavailable, 2);
  assert.deepEqual(told, [{ ev: 'relay.closed', peer: 3, reason: 'unavailable' }]);
  assert.deepEqual(received(rooms), [0, 0, 0]);
});

test('a frame never crosses into another match, whatever slot or token it names', () => {
  const env = makeHub();
  const one = buildParty(env, 4, { first: 1 });
  const two = buildParty(env, 4, { first: 20 });
  assert.notEqual(one.match, two.match);
  for (let dst = 2; dst <= 4; dst++) one.rooms[0].relay(encodeRelayFrame(1, dst, one.token, dst, bytes(1)));
  assert.deepEqual(received(one.rooms), [0, 1, 1, 1]);
  assert.deepEqual(received(two.rooms), [0, 0, 0, 0]);
  clear(one.rooms);
  one.rooms[0].relay(encodeRelayFrame(1, 2, two.token, 1, bytes(1)));
  two.rooms[0].relay(encodeRelayFrame(1, 2, one.token, 1, bytes(1)));
  assert.deepEqual([...received(one.rooms), ...received(two.rooms)], [0, 0, 0, 0, 0, 0, 0, 0]);
  assert.equal(drops(env, one.match).bad_token, 1);
  assert.equal(drops(env, two.match).bad_token, 1);
  two.rooms[0].relay(encodeRelayFrame(1, 2, two.token, 1, bytes(1)));
  assert.deepEqual([received(one.rooms), received(two.rooms)], [[0, 0, 0, 0], [0, 1, 0, 0]]);
});

test('a wrong token is dropped and the sender told once; the right one still works', () => {
  const env = makeHub();
  const { rooms, token, match } = buildParty(env, 3);
  const told = rooms[0].relay(encodeRelayFrame(1, 2, token ^ 1, 1, bytes(1)));
  rooms[0].relay(encodeRelayFrame(1, 2, token ^ 1, 2, bytes(1)));
  assert.deepEqual(told, [{ ev: 'relay.closed', peer: 2, reason: 'unavailable' }]);
  assert.equal(rooms[0].events('relay.closed').length, 1);
  assert.equal(rooms[1].binary.length, 0);
  assert.equal(drops(env, match).bad_token, 2);
  rooms[0].relay(encodeRelayFrame(1, 2, token, 3, bytes(1)));
  assert.equal(rooms[1].binary.length, 1);
});

test('malformed and oversized frames are dropped without closing the socket; the first three of each kind are logged', () => {
  const env = makeHub();
  const { rooms, token, match } = buildParty(env, 2);
  const good = () => encodeRelayFrame(1, 2, token, 1, bytes(1, 2, 3));
  const wrongMagic = good();
  wrongMagic[1] = 0x46;
  for (let i = 0; i < 6; i++) rooms[0].relay(wrongMagic);
  rooms[0].relay(bytes(0x53, 0x52, 2));
  rooms[0].relay(encodeRelayFrame(1, 2, token, 1, new Uint8Array(RELAY.maxPayload + 1)));
  const v1 = good();
  v1[2] = 1;
  rooms[0].relay(v1);
  const wrongType = good();
  wrongType[3] = 2;
  rooms[0].relay(wrongType);
  rooms[0].relay(good().subarray(0, good().length - 1));
  rooms[0].relay(new Uint8Array(0));
  assert.equal(rooms[1].binary.length, 0);
  assert.equal(rooms[0].closedWith, null);
  assert.deepEqual(drops(env, match), { bad_magic: 6, truncated: 2, oversized: 1, bad_version: 1, bad_type: 1, bad_length: 1 });
  assert.equal(env.logs.filter((l) => l.startsWith('relay drop reason=bad_magic')).length, RELAY.loggedDrops);
  rooms[0].relay(good());
  assert.equal(rooms[1].binary.length, 1);
  rooms[0].relay(encodeRelayFrame(1, 2, token, 2, new Uint8Array(RELAY.maxPayload)));
  assert.equal(rooms[1].binary.length, 2);
});

test('a socket that has not said hello cannot relay', () => {
  const env = makeHub();
  const { rooms, token, match } = buildParty(env, 2);
  const stranger = env.room(match);
  stranger.relay(encodeRelayFrame(1, 2, token, 1, bytes(1)));
  assert.equal(rooms[1].binary.length, 0);
  assert.equal(drops(env, match).not_joined, 1);
});

test('a replaced socket is stale: its frames, even late ones after its close, reach nobody', () => {
  const env = makeHub();
  const { rooms, data, match, token } = buildParty(env, 3);
  const old = rooms[1];
  const fresh = env.room(match);
  fresh.hello(data[1]);
  assert.equal(old.closedWith.code, 4001);
  old.relay(encodeRelayFrame(2, 3, token, 1, bytes(1)));
  old.relay(encodeRelayFrame(2, 1, token, 1, bytes(1)));
  assert.deepEqual(received(rooms), [0, 0, 0]);
  assert.equal(drops(env, match).stale, 2);
  old.drop();
  old.relay(encodeRelayFrame(2, 3, token, 2, bytes(1)));
  assert.equal(drops(env, match).stale + (drops(env, match).not_joined ?? 0), 3);
  assert.equal(rooms[2].binary.length, 0);
  fresh.relay(encodeRelayFrame(2, 3, token, 3, bytes(1)));
  assert.equal(rooms[2].binary.length, 1);
  rooms[0].relay(encodeRelayFrame(1, 2, token, 1, bytes(1)));
  assert.deepEqual([old.binary.length, fresh.binary.length], [0, 1]);
});

test('frames of an ended match go nowhere, and a new party gets a new room and token', () => {
  const env = makeHub();
  const first = buildParty(env, 3, { first: 1 });
  first.clients[0].call('party.leave');
  first.rooms[0].relay(encodeRelayFrame(1, 2, first.token, 1, bytes(1)));
  first.rooms[1].relay(encodeRelayFrame(2, 3, first.token, 1, bytes(1)));
  assert.deepEqual(received(first.rooms), [0, 0, 0]);
  assert.equal(drops(env, first.match).stale, 2);

  const created = first.clients[0].call('party.create');
  assert.notEqual(created.match, first.match);
  const next = env.room(created.match);
  next.hello(created);
  assert.notEqual(next.helloReply.relay_token, first.token);
  first.rooms[1].relay(encodeRelayFrame(1, 1, first.token, 1, bytes(1)));
  assert.equal(next.binary.length, 0);
});

test('a flood is dropped, then that pair\'s relay ends; the other pairs go on and a reconnect brings the pair back', () => {
  const env = makeHub();
  const { rooms, token, match, data } = buildParty(env, 4);
  const big = encodeRelayFrame(2, 3, token, 1, new Uint8Array(RELAY.maxPayload));
  let sent = 0;
  while (!env.logs.some((l) => l.includes(`relay close match=${match} pair=2-3 reason=rate_limit`)) && sent < 10000) {
    rooms[1].relay(big);
    sent += 1;
  }
  assert.ok(sent < 10000);
  assert.ok(rooms[2].binary.length > 100 && rooms[2].binary.length < sent);
  assert.deepEqual(rooms[1].events('relay.closed'), [{ ev: 'relay.closed', peer: 3, reason: 'rate_limit' }]);
  assert.deepEqual(rooms[2].events('relay.closed'), [{ ev: 'relay.closed', peer: 2, reason: 'rate_limit' }]);
  assert.equal(rooms[0].events('relay.closed').length + rooms[3].events('relay.closed').length, 0);
  assert.ok(drops(env, match).rate_limit >= RELAY.overrunsToClose);

  const before = rooms[2].binary.length;
  rooms[1].relay(encodeRelayFrame(2, 3, token, 2, bytes(1)));
  assert.equal(rooms[2].binary.length, before);
  assert.equal(drops(env, match).relay_closed, 1);

  rooms[3].relay(encodeRelayFrame(4, 1, token, 1, bytes(1)));
  assert.equal(rooms[0].binary.length, 1);
  env.advance(2000);
  rooms[1].relay(encodeRelayFrame(2, 4, token, 1, bytes(1)));
  assert.equal(rooms[3].binary.length, 1);
  rooms[2].relay(encodeRelayFrame(3, 4, token, 1, bytes(1)));
  assert.equal(rooms[3].binary.length, 2);

  const again = env.room(match);
  again.hello(data[2]);
  rooms[1].relay(encodeRelayFrame(2, 3, token, 3, bytes(1)));
  assert.equal(again.binary.length, 1);
});

test('normal gameplay rates pass for four members at once', () => {
  const env = makeHub();
  const { rooms, token, match } = buildParty(env, 4);
  for (let second = 0; second < 5; ++second) {
    for (let i = 0; i < 120; ++i) {
      for (let src = 1; src <= 4; src++) rooms[src - 1].relay(encodeRelayFrame(src, (src % 4) + 1, token, i, bytes(7, 1, 2, 3)));
    }
    env.advance(1000);
  }
  assert.deepEqual(received(rooms), [600, 600, 600, 600]);
  assert.deepEqual(drops(env, match), {});
});

test('the match as a whole has a budget too: four fast senders exceed it where two do not', () => {
  const flood = (senders) => {
    const env = makeHub();
    const { rooms, token, match } = buildParty(env, 4);
    const perSender = Math.floor(RELAY.connection.packets.perSecond * 0.9);
    for (let second = 0; second < 20; ++second) {
      for (let i = 0; i < perSender; ++i) {
        for (let src = 1; src <= senders; src++) rooms[src - 1].relay(encodeRelayFrame(src, (src % 4) + 1, token, i, bytes(1, 2, 3, 4)));
      }
      env.advance(1000);
    }
    return drops(env, match).rate_limit ?? 0;
  };
  assert.equal(flood(2), 0);
  assert.ok(flood(4) > 0);
});

test('a peer that stops taking frames loses them, and only one that stays behind ends that pair\'s relay', () => {
  const env = makeHub();
  const { rooms, token, match } = buildParty(env, 4);
  rooms[2].buffered = RELAY.bufferedBytes + 1;
  rooms[1].relay(encodeRelayFrame(2, 3, token, 1, bytes(1)));
  assert.equal(rooms[2].binary.length, 0);
  assert.deepEqual(rooms[1].events('relay.closed'), []);  // a moment of congestion is not the end of the pair
  rooms[2].buffered = 0;
  rooms[1].relay(encodeRelayFrame(2, 3, token, 2, bytes(1)));
  assert.equal(rooms[2].binary.length, 1);
  rooms[2].binary.length = 0;
  rooms[2].buffered = RELAY.bufferedBytes + 1;
  for (let seq = 3; seq < RELAY.overrunsToClose + 3; seq++) rooms[1].relay(encodeRelayFrame(2, 3, token, seq, bytes(1)));
  assert.deepEqual(rooms[1].events('relay.closed'), [{ ev: 'relay.closed', peer: 3, reason: 'peer_backpressure' }]);
  assert.deepEqual(rooms[2].events('relay.closed'), [{ ev: 'relay.closed', peer: 2, reason: 'peer_backpressure' }]);
  rooms[2].buffered = 0;
  rooms[1].relay(encodeRelayFrame(2, 3, token, 2, bytes(1)));
  assert.equal(rooms[2].binary.length, 0);
  rooms[1].relay(encodeRelayFrame(2, 4, token, 1, bytes(1)));
  assert.equal(rooms[3].binary.length, 1);
  assert.ok(env.logs.includes(`relay close match=${match} pair=2-3 reason=peer_backpressure`));

  rooms[3].failBinary = true;
  const told = rooms[0].relay(encodeRelayFrame(1, 4, token, 1, bytes(1)));
  assert.ok(env.logs.includes(`relay close match=${match} pair=1-4 reason=peer_unavailable`));
  assert.deepEqual(told, [{ ev: 'relay.closed', peer: 4, reason: 'peer_unavailable' }]);
  assert.deepEqual(rooms[3].events('relay.closed'), [{ ev: 'relay.closed', peer: 1, reason: 'peer_unavailable' }]);
  rooms[0].relay(encodeRelayFrame(1, 3, token, 1, bytes(1)));
  assert.equal(rooms[2].binary.length, 1);
});

test('a disconnect ends the relay of the pairs that chose it and a returning seat keeps the same token', () => {
  const env = makeHub();
  const { rooms, token, match, data } = buildParty(env, 4);
  chooseTransport(rooms, 1, 3, 'relay');
  chooseTransport(rooms, 2, 3, 'direct');
  clear(rooms);
  rooms[2].drop();
  assert.deepEqual(rooms[0].events('relay.closed'), [{ ev: 'relay.closed', peer: 3, reason: 'peer_disconnected' }]);
  assert.equal(rooms[1].events('relay.closed').length, 0);
  assert.equal(rooms[3].events('relay.closed').length, 0);
  rooms[0].relay(encodeRelayFrame(1, 3, token, 1, bytes(1)));
  assert.equal(drops(env, match).dst_unavailable, 1);
  env.advance(3000);
  const back = env.room(match);
  assert.equal(back.hello(data[2]).relay_token, token);
  rooms[0].relay(encodeRelayFrame(1, 3, token, 2, bytes(1)));
  back.relay(encodeRelayFrame(3, 1, token, 1, bytes(1)));
  assert.deepEqual([back.binary.length, rooms[0].binary.length], [1, 1]);
});

test('a pair that chose direct carries no relay frames, the others still do', () => {
  const env = makeHub();
  const { rooms, token, match } = buildParty(env, 4);
  chooseTransport(rooms, 2, 4, 'direct');
  clear(rooms);
  rooms[1].relay(encodeRelayFrame(2, 4, token, 1, bytes(1)));
  rooms[3].relay(encodeRelayFrame(4, 2, token, 1, bytes(1)));
  assert.equal(drops(env, match).pair_direct, 2);
  assert.deepEqual(received(rooms), [0, 0, 0, 0]);
  rooms[1].relay(encodeRelayFrame(2, 3, token, 1, bytes(1)));
  rooms[3].relay(encodeRelayFrame(4, 3, token, 1, bytes(1)));
  assert.equal(rooms[2].binary.length, 2);
});

test('pairs of one room can be direct, relayed and undecided at once, each on its own', () => {
  const env = makeHub();
  const { rooms, token, match } = buildParty(env, 4);
  chooseTransport(rooms, 1, 2, 'direct');
  chooseTransport(rooms, 1, 3, 'relay');
  chooseTransport(rooms, 1, 4, 'direct');
  chooseTransport(rooms, 2, 3, 'relay');
  chooseTransport(rooms, 2, 4, 'direct');
  chooseTransport(rooms, 3, 4, 'relay');
  assert.deepEqual(rooms[0].events('match.transport').map((e) => [e.a, e.b, e.transport]),
    [[1, 2, 'direct'], [1, 3, 'relay'], [1, 4, 'direct'], [2, 3, 'relay'], [2, 4, 'direct'], [3, 4, 'relay']]);
  assert.deepEqual(rooms[3].events('match.transport').map((e) => [e.a, e.b]), [[1, 4], [2, 4], [3, 4]]);
  const expect = { '1-2': false, '2-1': false, '1-3': true, '3-1': true, '1-4': false, '4-1': false, '2-3': true, '3-2': true,
    '2-4': false, '4-2': false, '3-4': true, '4-3': true };
  for (const [key, delivered] of Object.entries(expect)) {
    const [src, dst] = key.split('-').map(Number);
    clear(rooms);
    rooms[src - 1].relay(encodeRelayFrame(src, dst, token, 1, bytes(1)));
    assert.equal(rooms[dst - 1].binary.length, delivered ? 1 : 0, key);
  }
  assert.equal(drops(env, match).pair_direct, 6);

  rooms[2].buffered = RELAY.bufferedBytes + 1;
  clear(rooms);
  for (let seq = 2; seq < RELAY.overrunsToClose + 2; seq++) rooms[0].relay(encodeRelayFrame(1, 3, token, seq, bytes(1)));
  assert.deepEqual(rooms[0].events('relay.closed'), [{ ev: 'relay.closed', peer: 3, reason: 'peer_backpressure' }]);
  rooms[2].buffered = 0;
  clear(rooms);
  rooms[1].relay(encodeRelayFrame(2, 3, token, 3, bytes(1)));
  rooms[3].relay(encodeRelayFrame(4, 3, token, 3, bytes(1)));
  assert.equal(rooms[2].binary.length, 2);
});

test('signalling keeps working while frames are relayed', () => {
  const env = makeHub();
  const { rooms, token } = buildParty(env, 3);
  rooms[0].relay(encodeRelayFrame(1, 2, token, 1, bytes(1)));
  assert.equal(rooms[0].call('match.endpoint', { port: 7001 }).code, 'ok');
  assert.equal(rooms[1].call('match.endpoint', { port: 7002 }).code, 'ok');
  assert.equal(rooms[0].events('match.peer').length, 1);
  assert.equal(rooms[2].events('match.peer').length, 2);
  assert.equal(rooms[1].call('ping').code, 'ok');
  rooms[1].relay(encodeRelayFrame(2, 1, token, 1, bytes(1)));
  assert.deepEqual([rooms[0].binary.length, rooms[1].binary.length], [1, 1]);
});
