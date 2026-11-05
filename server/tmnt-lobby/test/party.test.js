import assert from 'node:assert/strict';
import { readFileSync } from 'node:fs';
import { join } from 'node:path';
import { test } from 'node:test';

import { handleAdmin } from '../src/admin.js';
import { PROTECTION } from '../src/abuse.js';
import { SAMPLE_MS, metricsDay } from '../src/metrics.js';
import { PARTY_CHARACTERS, PARTY_COMPOSITIONS, PARTY_DIMENSIONS, PARTY_ROWS_PER_DAY, PARTY_VALUES, parsePartyReport,
  partyCounts } from '../src/party.js';
import { MAX_PARTY } from '../src/protocol.js';
import { befriend, buildParty, greeted, identity, makeHub } from './harness.js';

const REPO = join(import.meta.dirname, '..', '..', '..');
const ip = (n) => `198.51.100.${n}`;

function hosting(env, n = 1, options) {
  const client = greeted(env, n, options);
  assert.equal(client.call('party.create').code, 'ok');
  return client;
}

const report = (client, body) => client.call('party.report', body);
const dimension = (env, name, day = metricsDay(env.clock.now)) => Object.fromEntries(env.host.sql.all(
  'SELECT value, count FROM metrics_daily_dimensions WHERE day = ? AND dimension = ?', day, name).map((r) => [r.value, r.count]));
const partyRows = (env) => env.host.sql.all(`SELECT * FROM metrics_daily_dimensions WHERE dimension IN
  (${PARTY_DIMENSIONS.map(() => '?').join(', ')})`, ...PARTY_DIMENSIONS);
const parse = (body) => {
  const msg = { op: 'party.report', seq: 1, ...body };
  return parsePartyReport(msg, JSON.stringify(msg));
};

test('the four turtles are the game actor indexes and the only valid values', () => {
  assert.deepEqual(PARTY_CHARACTERS, ['Leonardo', 'Michelangelo', 'Raphael', 'Donatello']);
  assert.deepEqual([...PARTY_VALUES.character], PARTY_CHARACTERS);
  for (let c = 0; c < PARTY_CHARACTERS.length; c++) {
    assert.deepEqual(parse({ characters: [c] }), { characters: [c], host: null }, `turtle ${c}`);
    assert.deepEqual(parse({ characters: [c], host: c }), { characters: [c], host: c });
  }
});

test('the roster matches the client table', () => {
  const header = readFileSync(join(REPO, 'src/online/party_report.h'), 'utf8');
  assert.match(header, new RegExp(`kTurtleCount = ${PARTY_CHARACTERS.length};`));
});

test('unknown turtles, sizes outside 1 to 4 and malformed fields are refused', () => {
  const bad = [
    { characters: [4] }, { characters: [-1] }, { characters: [1.5] }, { characters: ['1'] }, { characters: ['Leonardo'] },
    { characters: [null] }, { characters: [true] }, { characters: [[0]] }, { characters: [{}] }, { characters: [] },
    { characters: [0, 1, 2, 3, 0] }, { characters: Array(5000).fill(0) }, { characters: 0 }, { characters: '0' },
    { characters: { 0: 0 } }, {}, { characters: [0], host: 1 }, { characters: [0], host: 4 }, { characters: [0], host: -1 },
    { characters: [0], host: null }, { characters: [0], host: '0' }, { characters: [0], host: [0] }, { characters: [0], size: 1 },
    { characters: [0], name: 'Alice' }, { characters: [0], extra: {} },
  ];
  for (const body of bad) assert.equal(parse(body), null, JSON.stringify(body).slice(0, 80));
  assert.equal(parsePartyReport({ op: 'party.report', seq: 1, characters: [0] }, undefined), null);
});

test('a repeated key is refused even though JSON itself would take the last one', () => {
  const env = makeHub();
  const a = hosting(env);
  for (const text of ['{"op":"party.report","seq":5,"characters":[0],"characters":[1]}',
    '{"op":"party.report","seq":5,"characters":[0],"host":0,"host":0}',
    '{"op":"party.report","seq":5,"seq":6,"characters":[0]}',
    '{"op":"party.report","seq":5,"characters":[0],"\\u0063haracters":[1]}']) {
    assert.equal(a.raw(text).code, 'bad_request', text);
  }
  assert.equal(a.raw('{"op":"party.report","seq":5,"characters":[0]}').code, 'ok');
  assert.deepEqual(dimension(env, 'character'), { Leonardo: 1 });
  assert.deepEqual(dimension(env, 'party_size'), { 1: 1 });
});

test('a snapshot carries up to four turtles, ordering irrelevant', () => {
  assert.equal(MAX_PARTY, 4);
  assert.deepEqual(parse({ characters: [3, 0, 2, 1] }).characters, [0, 1, 2, 3]);
  assert.equal(parse({ characters: [0, 0, 0, 0] }).characters.length, 4);
  assert.equal(parse({ characters: [2, 2], host: 2 }).host, 2);
});

test('compositions are canonical: order never splits a party, repeated turtles stay distinct', () => {
  const composition = (characters) => partyCounts({ characters, host: null }).find((r) => r[0] === 'party_composition')[1];
  assert.equal(composition([2, 0]), 'Leonardo+Raphael');
  assert.equal(composition([0, 2]), 'Leonardo+Raphael');
  assert.equal(composition([2, 0, 0]), 'Leonardo+Leonardo+Raphael');
  assert.notEqual(composition([0, 0, 2]), composition([0, 2]));
  assert.equal(composition([3, 2, 1, 0]), 'Leonardo+Michelangelo+Raphael+Donatello');
});

test('the composition space is finite: 4 + 10 + 20 + 35 values, every one reachable and nothing else', () => {
  assert.equal(PARTY_COMPOSITIONS.length, 69);
  assert.equal(new Set(PARTY_COMPOSITIONS).size, 69);
  assert.equal(PARTY_ROWS_PER_DAY, 4 + 4 + 4 + 69 + 2);
  const seen = new Set();
  const walk = (characters, from) => {
    if (characters.length) seen.add(partyCounts({ characters, host: null }).find((r) => r[0] === 'party_composition')[1]);
    if (characters.length === MAX_PARTY) return;
    for (let c = from; c < PARTY_CHARACTERS.length; c++) walk([...characters, c], c);
  };
  walk([], 0);
  assert.deepEqual([...seen].sort(), [...PARTY_COMPOSITIONS].sort());
});

test('counts are derived from indexes only: seats per turtle, host, size, composition and repeats', () => {
  assert.deepEqual(partyCounts({ characters: [2, 0, 0], host: 2 }), [['character', 'Leonardo', 2], ['character', 'Raphael', 1],
    ['party_size', '3', 1], ['party_composition', 'Leonardo+Leonardo+Raphael', 1], ['party_duplicate', 'yes', 1],
    ['host_character', 'Raphael', 1]]);
  assert.deepEqual(partyCounts({ characters: [0, 2], host: null }).map((r) => r[0]),
    ['character', 'character', 'party_size', 'party_composition', 'party_duplicate']);
  for (const bad of [{ characters: ['Leonardo'], host: null }, { characters: [0], host: 'Leonardo' }, { characters: [0], host: 1 },
    { characters: [], host: null }, { characters: [0, 1, 2, 3, 0], host: null }, { characters: [7], host: null }]) {
    assert.throws(() => partyCounts(bad), RangeError, JSON.stringify(bad));
  }
});

test('the host reports a snapshot and every figure lands in the daily dimensions', () => {
  const env = makeHub();
  const { clients } = buildParty(env, 3);
  const reply = report(clients[0], { characters: [2, 0, 3], host: 3 });
  assert.deepEqual([reply.code, reply.counted], ['ok', true]);
  assert.deepEqual(dimension(env, 'character'), { Leonardo: 1, Raphael: 1, Donatello: 1 });
  assert.deepEqual(dimension(env, 'host_character'), { Donatello: 1 });
  assert.deepEqual(dimension(env, 'party_size'), { 3: 1 });
  assert.deepEqual(dimension(env, 'party_composition'), { 'Leonardo+Raphael+Donatello': 1 });
  assert.deepEqual(dimension(env, 'party_duplicate'), { no: 1 });
});

test('every turtle is accepted, as a host and as part of a party', () => {
  const env = makeHub();
  for (let c = 0; c < PARTY_CHARACTERS.length; c++) {
    const client = hosting(env, c + 1, { ip: ip(c + 1) });
    assert.equal(report(client, { characters: [c, (c + 1) % 4], host: c }).code, 'ok', `turtle ${c}`);
  }
  assert.deepEqual(Object.keys(dimension(env, 'host_character')).sort(), [...PARTY_CHARACTERS].sort());
  assert.ok(Object.values(dimension(env, 'character')).every((n) => n === 2));
});

test('a refused request writes nothing and leaves the connection usable', () => {
  const env = makeHub();
  const a = hosting(env);
  for (const body of [{ characters: [9] }, { characters: [] }, { characters: [0, 1, 2, 3, 0] }, { characters: [0], host: 3 },
    { characters: [0], name: 'x' }, {}]) {
    assert.equal(report(a, body).code, 'bad_request');
  }
  assert.equal(a.raw('{"op":"party.report","seq":9,"characters":').code, 'bad_request');
  assert.equal(partyRows(env).length, 0);
  assert.equal(a.closedWith, null);
  assert.equal(a.call('ping').code, 'ok');
});

test('a frame over the size limit is refused before it is read', () => {
  const env = makeHub();
  const a = hosting(env);
  assert.equal(a.raw(JSON.stringify({ op: 'party.report', seq: 3, characters: [0], pad: 'x'.repeat(40_000) })).code, 'too_large');
  assert.equal(a.raw(JSON.stringify({ op: 'party.report', seq: 3, characters: [0], pad: 'x'.repeat(5000) })).code, 'too_large');
  assert.equal(partyRows(env).length, 0);
});

test('nothing is accepted before sign-in, outside a party, from a guest or from a test connection', () => {
  const env = makeHub();
  assert.equal(report(env.connect(identity(1), { ip: ip(1) }), { characters: [0] }).code, 'no_session');
  assert.equal(report(greeted(env, 2, { ip: ip(2) }), { characters: [0] }).code, 'no_party');
  const probe = greeted(env, 3, { ip: ip(3), presence: false });
  assert.equal(report(probe, { characters: [0] }).code, 'no_party');

  const { clients } = buildParty(env, 2);
  assert.equal(report(clients[1], { characters: [0, 1] }).code, 'not_host');
  assert.equal(partyRows(env).length, 0);
  assert.equal(report(clients[0], { characters: [0, 1] }).code, 'ok');
});

test('a host on a connection test socket of the same account cannot report', () => {
  const env = makeHub();
  hosting(env);
  const probe = env.connect(identity(1), { presence: false });
  probe.hello();
  assert.equal(report(probe, { characters: [0, 1] }).code, 'no_party');
  assert.equal(partyRows(env).length, 0);
});

test('a party cannot be larger than its advertised places', () => {
  const env = makeHub();
  const a = greeted(env, 1);
  a.call('pm.host', { slots: 2 });
  assert.equal(report(a, { characters: [0, 1, 2] }).code, 'bad_request');
  assert.equal(report(a, { characters: [0, 1] }).code, 'ok');
});

test('the same snapshot from the same connection and party is counted once and spends no budget', () => {
  const env = makeHub();
  const a = hosting(env);
  assert.equal(report(a, { characters: [0, 2], host: 0 }).counted, true);
  const tokens = env.hub.guards.partyReportAccount.tokens(a.who.player, env.clock.now);
  for (let i = 0; i < 50; i++) assert.equal(report(a, { characters: [2, 0], host: 0 }).counted, false);
  assert.equal(env.hub.guards.partyReportAccount.tokens(a.who.player, env.clock.now), tokens);
  assert.deepEqual(dimension(env, 'party_size'), { 2: 1 });
});

test('a changed composition or host is a new sample', () => {
  const env = makeHub();
  const a = hosting(env);
  assert.equal(report(a, { characters: [0, 2], host: 0 }).counted, true);
  assert.equal(report(a, { characters: [0, 2], host: 2 }).counted, true);
  assert.equal(report(a, { characters: [0, 3], host: 0 }).counted, true);
  assert.deepEqual(dimension(env, 'party_size'), { 2: 3 });
  assert.deepEqual(dimension(env, 'party_composition'), { 'Leonardo+Raphael': 2, 'Leonardo+Donatello': 1 });
});

test('a reconnect may be counted again: a small duplicate, with nothing stored to prevent it', () => {
  const env = makeHub();
  const a = hosting(env);
  report(a, { characters: [0, 2] });
  a.drop();
  const again = greeted(env, 1);
  assert.equal(report(again, { characters: [0, 2] }).counted, true);
  assert.deepEqual(dimension(env, 'party_composition'), { 'Leonardo+Raphael': 2 });
});

test('one account is limited, and the refusal is counted in memory and written with the other diagnostics', () => {
  const env = makeHub();
  const a = hosting(env);
  for (let c = 0; c < 3; c++) assert.equal(report(a, { characters: [c] }).code, 'ok', `report ${c}`);
  const rows = partyRows(env).length;
  for (let i = 0; i < 20; i++) assert.equal(report(a, { characters: [3], host: 3 }).code, 'limit', `report ${i}`);
  assert.equal(partyRows(env).length, rows);
  env.advance(PROTECTION.flushMs + 1000);
  env.hub.flushDiagnostics();
  assert.equal(dimension(env, 'abuse').party_report_limited, 20);
  env.advance(1000 / PROTECTION.partyReportAccount.perSecond);
  assert.equal(report(a, { characters: [3], host: 3 }).code, 'ok');
});

test('one address is limited whatever the accounts behind it', () => {
  const env = makeHub();
  const results = [];
  for (let i = 0; i < PROTECTION.partyReportAddress.burst + 3; i++) {
    results.push(report(hosting(env, i + 1, { ip: '203.0.113.9' }), { characters: [i % 4] }).code);
  }
  assert.equal(results.filter((c) => c === 'ok').length, PROTECTION.partyReportAddress.burst);
  assert.deepEqual([...new Set(results.slice(PROTECTION.partyReportAddress.burst))], ['limit']);
});

test('the service as a whole is limited, however many addresses and accounts report', () => {
  const env = makeHub();
  const results = [];
  for (let i = 0; i < PROTECTION.partyReportAll.burst + 5; i++) {
    results.push(report(hosting(env, i + 1, { ip: `203.0.${Math.floor(i / 200)}.${(i % 200) + 1}` }), { characters: [i % 4] }).code);
  }
  assert.equal(results.filter((c) => c === 'ok').length, PROTECTION.partyReportAll.burst);
  assert.ok(partyRows(env).length <= PARTY_ROWS_PER_DAY);
});

test('party reports use no create, join, sign-in or telemetry budget', () => {
  const env = makeHub();
  const a = hosting(env);
  const { guards } = env.hub;
  const create = guards.createAccount.tokens(a.who.player, env.clock.now);
  const join = guards.joinAccount.tokens(a.who.player, env.clock.now);
  const hello = guards.hello.tokens(a.who.player, env.clock.now);
  for (let i = 0; i < 10; i++) report(a, { characters: [i % 4] });
  assert.equal(guards.createAccount.tokens(a.who.player, env.clock.now), create);
  assert.equal(guards.joinAccount.tokens(a.who.player, env.clock.now), join);
  assert.equal(guards.hello.tokens(a.who.player, env.clock.now), hello);
});

test('the per-socket message budget still applies to the new operation', () => {
  const env = makeHub();
  const a = hosting(env);
  const codes = new Set();
  for (let i = 0; i < 100; i++) codes.add(report(a, { characters: [0] }).code);
  assert.ok(codes.has('ok') && codes.has('limit'));
});

test('a client that never sends party reports behaves exactly as before and adds no party rows', () => {
  const env = makeHub();
  const { clients } = buildParty(env, 2);
  assert.equal(clients[1].call('party.leave').code, 'ok');
  assert.equal(clients[0].call('party.leave').code, 'ok');
  assert.equal(partyRows(env).length, 0);
  const summary = env.hub.metricsSummary();
  assert.equal(summary.today.sessions, 2);
  assert.equal(summary.today.rooms_created, 1);
  assert.equal(summary.today.room_joins, 1);
  const history = env.hub.metricsHistory(30);
  for (const name of PARTY_DIMENSIONS) assert.deepEqual(history.dimensions[name], [], name);
  assert.equal(history.party_first_day, null);
});

test('the hello answer is unchanged: no capability announces party reports', () => {
  const env = makeHub();
  const reply = env.connect(identity(1)).hello();
  assert.deepEqual(Object.keys(reply).sort(), ['capabilities', 'code', 'friend_code', 'name', 'player', 're', 'time']);
  assert.ok(!JSON.stringify(reply.capabilities).includes('party_report'));
});

test('a metrics failure never fails the request', () => {
  const env = makeHub();
  const a = hosting(env);
  env.host.sql.run('DROP TABLE metrics_daily_dimensions');
  assert.equal(report(a, { characters: [0] }).code, 'ok');
  assert.equal(a.call('party.leave').code, 'ok');
  const b = greeted(env, 2);
  assert.equal(b.call('party.create').code, 'ok');
});

test('spam cannot grow the table beyond the finite party space', () => {
  const env = makeHub();
  for (const composition of PARTY_COMPOSITIONS) {
    const characters = composition.split('+').map((name) => PARTY_CHARACTERS.indexOf(name));
    for (const host of [null, ...new Set(characters)]) env.hub.metrics.partyReport({ characters, host });
  }
  for (let i = 0; i < 2000; i++) env.hub.metrics.partyReport({ characters: [i % 4, (i * 3) % 4], host: null });
  const rows = partyRows(env);
  assert.equal(rows.length, PARTY_ROWS_PER_DAY);
  for (const { dimension: name, value } of rows) assert.ok(PARTY_VALUES[name].has(value), `${name}=${value}`);
  assert.throws(() => env.hub.metrics.partyReport({ characters: ['Leonardo; DROP TABLE x'], host: null }), RangeError);
  assert.throws(() => env.hub.metrics.partyReport({ characters: [0], host: 'Raphael' }), RangeError);
});

test('a client cannot name a dimension, a value or a composition', () => {
  const env = makeHub();
  const a = hosting(env);
  for (const body of [{ characters: [0], dimension: 'platform' }, { characters: [0], value: 'x' },
    { characters: [0], composition: 'Leonardo+Raphael' }, { characters: ['Leonardo+Raphael'] }, { characters: [0], host: 'Leonardo' }]) {
    assert.equal(report(a, body).code, 'bad_request', JSON.stringify(body));
  }
  assert.equal(partyRows(env).length, 0);
});

test('history covers the selected range only and names the first day anything was reported', () => {
  const env = makeHub();
  const first = metricsDay(env.clock.now);
  report(hosting(env, 1, { ip: ip(1) }), { characters: [0, 2] });
  env.advance(10 * 24 * 3600 * 1000);
  report(hosting(env, 2, { ip: ip(2) }), { characters: [1, 3], host: 1 });
  const wide = env.hub.metricsHistory(30);
  assert.equal(wide.party_first_day, first);
  assert.deepEqual(wide.dimensions.party_size, [{ value: '2', count: 2 }]);
  const narrow = env.hub.metricsHistory(7);
  assert.equal(narrow.party_first_day, first);
  assert.deepEqual(narrow.dimensions.party_composition, [{ value: 'Michelangelo+Donatello', count: 1 }]);
  assert.deepEqual(narrow.dimensions.host_character, [{ value: 'Michelangelo', count: 1 }]);
  assert.deepEqual(narrow.dimensions.party_size, [{ value: '2', count: 1 }]);
});

test('the signature that stops repeats lives in the connection only: no SQL column or table holds it', () => {
  const env = makeHub();
  const a = hosting(env);
  report(a, { characters: [0, 1, 2], host: 0 });
  const columns = env.host.sql.all(`SELECT name FROM sqlite_master WHERE type = 'table'`).flatMap((t) =>
    env.host.sql.all(`PRAGMA table_info(${t.name})`).map((c) => c.name));
  assert.ok(!columns.some((c) => /character|signature|composition/i.test(c)), columns.join());
  assert.ok(a.conn.attachment().partyReport);
});

test('there is no HTTP route for reports and no extra socket: they ride the directory connection', async () => {
  const read = (file) => readFileSync(join(REPO, file), 'utf8');
  const worker = read('server/tmnt-lobby/src/worker.js');
  const admin = read('server/tmnt-lobby/src/admin.js');
  const edge = read('server/tmnt-lobby/src/edge.js');
  for (const [name, text] of [['worker.js', worker], ['admin.js', admin], ['edge.js', edge]]) {
    assert.ok(!/party\.report|partyReport|PARTY_CHARACTERS/.test(text), name);
  }
  assert.equal([...worker.matchAll(/acceptWebSocket\(/g)].length, 2, 'the directory and the room, nothing more');
  assert.equal([...worker.matchAll(/new WebSocketPair\(/g)].length, 2);
  const key = 'x'.repeat(32);
  const ask = (path, init = {}) => handleAdmin(new Request(`https://x.example${path}`, init), { ADMIN_TOKEN: key }, {});
  assert.equal((await ask('/admin/api/party', { method: 'POST', body: '{}' })).status, 405);
  assert.equal((await ask('/admin/api/party', { headers: { Authorization: `Bearer ${key}` } })).status, 404);
  const service = read('src/online/online_service.cpp');
  assert.ok(service.includes('Send(Purpose::kPartyReport'));
  assert.ok(!/OpenLobbyLink|open_channel/.test(read('src/online/party_report.h')));
});

test('analytics hold no player, address, party or diag after a played party', () => {
  const env = makeHub();
  const host = greeted(env, 1, { ip: '203.0.113.77', name: 'SecretAlice' });
  const created = host.call('party.create');
  const guest = greeted(env, 2, { ip: '198.51.100.66', name: 'HiddenBob' });
  befriend(host, guest);
  const invite = host.call('invite.send', { player: guest.who.player });
  guest.call('invite.accept', { invite: invite.invite });
  assert.equal(report(host, { characters: [0, 2], host: 0 }).code, 'ok');
  host.call('party.leave');
  env.advance(SAMPLE_MS);
  const dump = JSON.stringify(['metrics_daily', 'metrics_daily_dimensions', 'metrics_samples', 'metrics_players']
    .map((table) => env.host.sql.all(`SELECT * FROM ${table}`)));
  for (const secret of ['SecretAlice', 'HiddenBob', host.who.player, guest.who.player, host.who.player.replace('-', ''),
    '203.0.113.77', '198.51.100.66', created.match, created.diag, host.who.token]) {
    assert.ok(!dump.includes(secret), `analytics contain ${secret}`);
  }
  assert.deepEqual(env.host.sql.all('PRAGMA table_info(metrics_daily_dimensions)').map((c) => c.name),
    ['day', 'dimension', 'value', 'count']);
  assert.deepEqual(env.host.sql.all('PRAGMA table_info(metrics_players)').map((c) => c.name), ['day', 'player']);
});
