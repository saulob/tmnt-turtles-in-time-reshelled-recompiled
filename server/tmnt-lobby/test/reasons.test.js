import assert from 'node:assert/strict';
import { readFileSync, readdirSync } from 'node:fs';
import { test } from 'node:test';

import { LIMITS, RELAY, encodeRelayFrame } from '../src/protocol.js';
import { REASON_CATEGORIES, REASON_GROUP, REASON_TABLE, isReason, normalizeReason, reasonCategory } from '../src/reasons.js';
import { befriend, buildParty, greeted, makeHub } from './harness.js';

const repo = new URL('../../../', import.meta.url);
const shared = readFileSync(new URL('tests/reason-codes.txt', repo), 'utf8')
  .split(/\r?\n/)
  .map((line) => line.trim())
  .filter((line) => line && !line.startsWith('#'))
  .map((line) => line.split(/\s+/));

test('the reason table is exactly tests/reason-codes.txt, in the same order', () => {
  assert.deepEqual(REASON_TABLE.map((row) => [...row]), shared);
  const codes = shared.map(([code]) => code);
  assert.equal(new Set(codes).size, codes.length, 'duplicate code');
  for (const [code, category] of shared) {
    assert.match(code, /^[a-z]+(_[a-z]+)*$/, code);
    assert.ok(REASON_CATEGORIES.includes(category), `${code}: ${category}`);
    assert.equal(shared.filter((row) => row.length !== 2).length, 0);
  }
  assert.deepEqual(new Set(shared.map(([, category]) => category)), new Set(REASON_CATEGORIES));
  assert.deepEqual(Object.keys(REASON_GROUP).sort(), [...REASON_CATEGORIES].sort());
});

test('known codes keep their code and category, anything else is "other"', () => {
  for (const [code, category] of shared) {
    assert.equal(normalizeReason(code), code);
    assert.equal(reasonCategory(code), category);
    assert.equal(isReason(code), true);
  }
  for (const value of ['', 'Direct_Timeout', 'direct timeout', 'nope', 'redacted', undefined, null, 5, {}, [], ['host_left'], '__proto__',
    'constructor', 'toString']) {
    assert.equal(normalizeReason(value), 'other', String(value));
    assert.equal(isReason(value), false);
    assert.equal(reasonCategory(value), 'other');
  }
});

// Every code the Worker can answer or report must be in the shared list, found by reading the sources so a new one cannot
// slip in unnoticed.
test('every code and reason the Worker emits is in the shared list', () => {
  const found = new Map();
  const note = (code, where) => found.set(code, [...(found.get(code) ?? []), where]);
  const patterns = [
    /\bfail\('([a-z_]+)'/g,
    /\bcode: '([a-z_]+)'/g,
    /\b(?:endInvite|endParty|endRoom)\((?:[A-Za-z.]+, )?'([a-z_]+)'/g,
    /\bclosePair\([^,]+, [^,]+, '([a-z_]+)'\)/g,
    /\b(?:tell|gone|reason): '([a-z_]+)'/g,
  ];
  for (const file of readdirSync(new URL('src/', new URL('../', import.meta.url))).filter((f) => f.endsWith('.js'))) {
    const text = readFileSync(new URL(`../src/${file}`, import.meta.url), 'utf8');
    for (const pattern of patterns) for (const match of text.matchAll(pattern)) note(match[1], file);
  }
  found.delete('ok');
  for (const code of ['bad_request', 'too_large', 'limit', 'internal', 'host_left', 'party_full', 'peer_disconnected', 'rate_limit',
    'accepted_elsewhere', 'proto', 'version_mismatch', 'relay_unavailable']) {
    assert.ok(found.has(code), `the scan does not see ${code}`);
  }
  const missing = [...found].filter(([code]) => !isReason(code));
  assert.deepEqual(missing, []);
});

const events = (env) => env.host.sql.all('SELECT name, fields FROM tel_events').map((r) => ({ name: r.name, fields: JSON.parse(r.fields) }));

test('no reason the Worker records in a full set of flows is outside the shared list', () => {
  const env = makeHub();
  const note = () => { env.clock.now += 60000; };
  const guest = (n) => greeted(env, n, { ip: `198.51.100.${n % 250}` });

  const a = buildParty(env, 4);
  const [host, two, three, four] = a.clients;
  const big = encodeRelayFrame(2, 3, a.token, 1, new Uint8Array(RELAY.maxPayload));
  for (let i = 0; i < 10000 && !env.logs.some((l) => l.includes('relay close') && l.includes('reason=rate_limit')); i++) a.rooms[1].relay(big);
  a.rooms[3].buffered = RELAY.bufferedBytes + 1;
  a.rooms[1].relay(a.rooms[1].frame(4, a.token));
  a.rooms[3].buffered = 0;
  a.rooms[2].failBinary = true;
  a.rooms[0].relay(a.rooms[0].frame(3, a.token));

  const extra = [1, 2, 3, 4, 5, 6].map((n) => guest(200 + n));
  for (const friend of extra) befriend(host, friend);
  note();
  assert.equal(host.call('invite.send', { player: extra[0].who.player }).code, 'party_full');
  assert.equal(host.call('party.kick', { player: four.who.player }).code, 'ok');
  const declined = host.call('invite.send', { player: extra[1].who.player });
  assert.equal(extra[1].call('invite.decline', { invite: declined.invite }).code, 'ok');
  assert.equal(two.call('party.leave').code, 'ok');
  three.drop();
  a.rooms[2].drop();
  env.advance(LIMITS.graceMs + 1000);
  assert.equal(host.call('party.leave').code, 'ok');

  const g = guest(700);
  const w = guest(701);
  befriend(g, w);
  g.call('party.create');
  note();
  g.call('invite.send', { player: w.who.player });
  env.advance(LIMITS.inviteMs + 1000);

  const b = guest(300);
  const c = guest(301);
  const x = guest(302);
  befriend(b, x);
  befriend(c, x);
  b.call('party.create');
  c.call('party.create');
  const fromB = b.call('invite.send', { player: x.who.player });
  note();
  c.call('invite.send', { player: x.who.player });
  assert.equal(x.call('invite.accept', { invite: fromB.invite }).code, 'ok');

  const d = buildParty(env, 3, { first: 400, connect: false });
  const late = [guest(410), guest(411)];
  for (const friend of late) befriend(d.clients[0], friend);
  const first = d.clients[0].call('invite.send', { player: late[0].who.player });
  d.clients[0].call('invite.send', { player: late[1].who.player });
  assert.equal(late[0].call('invite.accept', { invite: first.invite }).code, 'ok');

  const e = guest(500);
  const z = guest(501);
  befriend(e, z);
  e.call('party.create');
  note();
  e.call('invite.send', { player: z.who.player });
  e.drop();
  env.advance(LIMITS.graceMs + 1000);
  env.advance(LIMITS.partyIdleMs + 1000);
  env.advance(LIMITS.matchMaxMs);
  for (let i = 0; i < 200; i++) z.call('ping');

  const seen = events(env);
  const reasons = new Set(seen.flatMap((e) => [e.fields.reason, e.fields.fallback]).filter(Boolean));
  for (const code of ['host_left', 'left', 'kicked', 'expired', 'accepted_elsewhere', 'party_full', 'rate_limit', 'peer_backpressure',
    'peer_unavailable']) {
    assert.ok(reasons.has(code), `${code} was not recorded`);
  }
  for (const name of ['party_created', 'party_reached_4', 'party_ended', 'member_joined', 'member_left', 'invite_sent', 'invite_accepted',
    'invite_declined', 'invite_expired', 'invite_ended', 'party_full', 'room_joined', 'room_left', 'room_ended', 'relay_closed']) {
    assert.ok(seen.some((s) => s.name === name), `${name} was not recorded`);
  }
  for (const reason of reasons) assert.ok(isReason(reason), reason);
  assert.equal(reasons.has('other'), false);

  const keys = env.host.sql.all('SELECT k FROM tel_counters').map((r) => r.k);
  assert.equal(keys.includes('unknown_reason'), false);
  assert.ok(keys.some((k) => k.startsWith('server.rate_limited.scope.')));
  for (const key of keys.filter((k) => k.includes('.reason.'))) assert.ok(isReason(key.split('.reason.')[1]), key);
  const summary = env.hub.telemetrySummary(7);
  assert.equal(summary.reasons.other, 0);
  assert.equal(summary.totals.unknown_reasons, 0);
});
