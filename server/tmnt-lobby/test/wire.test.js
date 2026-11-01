import assert from 'node:assert/strict';
import { readFileSync } from 'node:fs';
import { test } from 'node:test';

import { MAX_PARTY, PROTOCOL, RELAY, encodeRelayFrame, parseRelayFrame } from '../src/protocol.js';

// The relay frames and constants the game clients use (tests/wire-vectors.txt, src/netplay/relay_frame.h,
// src/online/protocol.h), so the Worker and every client platform agree on one layout.
const repo = new URL('../../../', import.meta.url);
const read = (path) => readFileSync(new URL(path, repo), 'utf8');

// TMNT_WIRE_VECTORS names the vectors another platform's build printed (tools/test.ps1 runs the Worker on each).
const vectors = new Map(
  (process.env.TMNT_WIRE_VECTORS ? readFileSync(process.env.TMNT_WIRE_VECTORS, 'utf8') : read('tests/wire-vectors.txt'))
    .split('\n')
    .map((line) => line.trim())
    .filter((line) => line && !line.startsWith('#'))
    .map((line) => line.split(/\s+/))
    .map(([name, hex]) => [name, Uint8Array.from(hex.match(/../g).map((byte) => parseInt(byte, 16)))]),
);

function cppConstant(path, name) {
  const match = read(path).match(new RegExp(`constexpr \\w+ ${name} = (\\d+);`));
  assert.ok(match, `${name} not found in ${path}`);
  return Number(match[1]);
}

test('the Worker parses the relay frames every client platform sends', () => {
  assert.deepEqual(parseRelayFrame(vectors.get('relay.data')), { type: RELAY.data, src: 1, dst: 2, token: 0x89abcdef, seq: 7, length: 5 });
  assert.deepEqual(parseRelayFrame(vectors.get('relay.game')), { type: RELAY.data, src: 1, dst: 4, token: 1, seq: 0xffffffff, length: 24 });
  assert.deepEqual(parseRelayFrame(vectors.get('relay.v2.slot2to3')),
    { type: RELAY.data, src: 2, dst: 3, token: 0x12345678, seq: 0x42, length: 8 });
});

test('the Worker encodes relay frames byte-for-byte like the clients', () => {
  const data = vectors.get('relay.data');
  assert.deepEqual(encodeRelayFrame(1, 2, 0x89abcdef, 7, data.subarray(RELAY.headerBytes)), data);
  const game = vectors.get('relay.game');
  assert.deepEqual(encodeRelayFrame(1, 4, 1, 0xffffffff, game.subarray(RELAY.headerBytes)), game);
  const slot = vectors.get('relay.v2.slot2to3');
  assert.deepEqual(encodeRelayFrame(2, 3, 0x12345678, 0x42, slot.subarray(RELAY.headerBytes)), slot);
  assert.equal(new TextDecoder().decode(slot.subarray(RELAY.headerBytes)), 'slot2to3');
  assert.equal(slot[2], 2);
});

test('relay and lobby protocol constants match the client', () => {
  assert.equal(RELAY.version, cppConstant('src/netplay/relay_frame.h', 'kRelayVersion'));
  assert.equal(RELAY.data, cppConstant('src/netplay/relay_frame.h', 'kRelayData'));
  assert.equal(RELAY.headerBytes, cppConstant('src/netplay/relay_frame.h', 'kRelayHeaderBytes'));
  assert.equal(RELAY.maxPayload, cppConstant('src/netplay/relay_frame.h', 'kRelayMaxPayload'));
  assert.equal(PROTOCOL, cppConstant('src/online/protocol.h', 'kLobbyProtocol'));
  assert.equal(MAX_PARTY, cppConstant('src/online/protocol.h', 'kMaxParty'));
});
