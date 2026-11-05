import assert from 'node:assert/strict';
import { mkdtempSync, rmSync } from 'node:fs';
import { tmpdir } from 'node:os';
import { join } from 'node:path';
import { DatabaseSync } from 'node:sqlite';
import { test } from 'node:test';

import { historyDays } from '../src/admin.js';
import { PROTECTION } from '../src/abuse.js';
import { DAILY_RETENTION_DAYS, HISTORY_MAX_DAYS, PLAYER_RETENTION_DAYS, SAMPLE_DAYS, SAMPLE_MS, countryCode, metricsDay, percentChange, platformCategory,
  versionCategory } from '../src/metrics.js';
import { PROTOCOL } from '../src/protocol.js';
import { hmacSha256Hex } from '../src/sha256.js';
import { buildParty, greeted, identity, makeHub } from './harness.js';

const DAY_MS = 24 * 3600 * 1000;
const MINUTE = 60 * 1000;
const ip = (n) => `198.51.100.${n}`;

const today = (env) => env.hub.metricsSummary().today;
const dimension = (env, name, day = metricsDay(env.clock.now)) => Object.fromEntries(env.host.sql.all(
  'SELECT value, count FROM metrics_daily_dimensions WHERE day = ? AND dimension = ?', day, name).map((r) => [r.value, r.count]));

function play(env, n, options) {
  const client = greeted(env, n, options);
  client.drop();
}

// Moves the clock without the alarm, so a test can leave a player's rows behind and come back days later.
const later = (env, ms) => { env.clock.now += ms; };

test('hmac: RFC 4231 test cases', () => {
  assert.equal(hmacSha256Hex('Jefe', 'what do ya want for nothing?'),
    '5bdcc146bf60754e6a042426089575c75a003f089d2739839dec58b964ec3843');
  const long = String.fromCharCode(...new Array(131).fill(0x61));
  assert.equal(hmacSha256Hex(long, 'Test Using Larger Than Block-Size Key - Hash Key First'),
    hmacSha256Hex(long, 'Test Using Larger Than Block-Size Key - Hash Key First'));
  assert.notEqual(hmacSha256Hex('a', 'x'), hmacSha256Hex('b', 'x'));
});

test('a sign-in with presence is one session and one unique player', () => {
  const env = makeHub();
  greeted(env, 1);
  assert.equal(today(env).sessions, 1);
  assert.equal(today(env).unique_players, 1);
});

test('a reconnect inside the window is neither a session nor a new player, and is counted as deduplicated', () => {
  const env = makeHub();
  play(env, 1);
  for (let i = 0; i < 5; i++) {
    later(env, MINUTE);
    play(env, 1);
  }
  assert.equal(today(env).sessions, 1);
  assert.equal(today(env).unique_players, 1);
  env.hub.flushDiagnostics();
  assert.equal(dimension(env, 'abuse').hello_reconnect_deduped, 5);
});

test('sessions and unique players stay separate: a later return is a session, not another player', () => {
  const env = makeHub();
  play(env, 1);
  later(env, PROTECTION.sessionWindowMs);
  play(env, 1);
  assert.equal(today(env).sessions, 2);
  assert.equal(today(env).unique_players, 1);
});

test('a session mark costs one write per window, not one per sign-in', () => {
  const env = makeHub();
  const writes = [];
  const run = env.host.sql.run;
  env.host.sql.run = (query, ...args) => { writes.push(query); return run.call(env.host.sql, query, ...args); };
  for (let i = 0; i < 5; i++) play(env, 1);
  assert.equal(writes.filter((q) => /UPDATE players SET session/.test(q)).length, 1);
});

test('different players count independently', () => {
  const env = makeHub();
  for (let n = 1; n <= 3; n++) play(env, n, { ip: ip(n) });
  assert.equal(today(env).unique_players, 3);
  assert.equal(today(env).sessions, 3);
});

test('a connection test is neither a session nor a player', () => {
  const env = makeHub();
  play(env, 1, { presence: false });
  assert.equal(today(env).sessions, 0);
  assert.equal(today(env).unique_players, 0);
});

test('a refused sign-in counts nothing', () => {
  const env = makeHub();
  const client = env.connect(identity(1));
  assert.equal(client.hello({ token: 'f'.repeat(64) }).code, 'unauthorized');
  assert.equal(today(env).sessions, 0);
  assert.equal(today(env).unique_players, 0);
});

test('a reconnect that crosses midnight is the same session but counts the player on the new day', () => {
  const env = makeHub();
  env.clock.now += DAY_MS - (env.clock.now % DAY_MS) + 3 * 3600 * 1000 - MINUTE;
  play(env, 1);
  const before = metricsDay(env.clock.now);
  later(env, 2 * MINUTE);
  assert.notEqual(metricsDay(env.clock.now), before);
  play(env, 1);
  assert.equal(today(env).sessions, 0);
  assert.equal(today(env).unique_players, 1);
  assert.equal(env.hub.metricsHistory(7).totals.unique_players, 1);
});

test('the same player on several days counts once in the period and once per day', () => {
  const env = makeHub();
  for (let i = 0; i < 4; i++) {
    play(env, 1);
    later(env, DAY_MS);
  }
  const history = env.hub.metricsHistory(30);
  assert.equal(history.totals.unique_players, 1);
  assert.equal(history.daily.filter((d) => d.unique_players === 1).length, 4);
  assert.equal(history.daily.reduce((a, d) => a + d.unique_players, 0), 4, 'daily counts are per day, never the period figure');
  assert.equal(history.totals.sessions, 4);
});

test('the period count follows the selected range for 7, 30, 90 and 365 days', () => {
  const env = makeHub();
  const seen = [[400, 1], [300, 2], [80, 3], [20, 4], [5, 5], [0, 1], [0, 5]];
  let at = 500;
  for (const [ago, n] of seen) {
    later(env, (at - ago) * DAY_MS);
    at = ago;
    play(env, n, { ip: ip(n) });
    later(env, MINUTE);
  }
  const unique = (days) => env.hub.metricsHistory(days).totals.unique_players;
  assert.equal(unique(7), 2);
  assert.equal(unique(30), 3);
  assert.equal(unique(90), 4);
  assert.equal(unique(365), 5);
});

test('history reaches 365 days and refuses more', () => {
  const env = makeHub();
  assert.equal(env.hub.metricsHistory(HISTORY_MAX_DAYS).daily.length, 365);
  assert.throws(() => env.hub.metricsHistory(HISTORY_MAX_DAYS + 1), RangeError);
  assert.equal(historyDays(new URLSearchParams('days=365')), 365);
  assert.equal(historyDays(new URLSearchParams('days=366')), null);
  assert.equal(historyDays(new URLSearchParams('')), 30);
});

test('the period comparison counts distinct players of each period, and copes with an empty one', () => {
  const env = makeHub();
  play(env, 1, { ip: ip(1) });
  play(env, 2, { ip: ip(2) });
  later(env, 7 * DAY_MS);
  play(env, 1, { ip: ip(1) });
  const { comparison } = env.hub.metricsHistory(7);
  assert.deepEqual(comparison.unique_players, { current: 1, previous: 2, change: -50 });
  assert.equal(comparison.sessions.change, -50);
  assert.deepEqual(Object.keys(comparison), ['unique_players', 'sessions', 'rooms_created', 'room_joins', 'rooms_with_guest',
    'peak_online', 'peak_in_rooms']);
  assert.equal(percentChange(5, 0), null);
  assert.equal(percentChange(0, 0), null);
  assert.equal(percentChange(15, 10), 50);
});

test('player rows are stored keyed, and a Worker secret changes the key', () => {
  const salted = makeHub();
  const keyed = makeHub({ metricsSecret: 'k'.repeat(40) });
  for (const env of [salted, keyed]) play(env, 1);
  const rowOf = (env) => env.host.sql.get('SELECT player FROM metrics_players').player;
  assert.match(rowOf(keyed), /^[0-9a-f]{32}$/);
  assert.equal(rowOf(keyed), hmacSha256Hex('k'.repeat(40), `metrics-player|${identity(1).player}`).slice(0, 32));
  assert.notEqual(rowOf(salted), rowOf(keyed));
  assert.equal(salted.hub.metricsHistory(7).player_key, 'salt');
  assert.equal(keyed.hub.metricsHistory(7).player_key, 'secret');
});

test('the pseudonym never leaves the object: summary and history carry counts only', () => {
  const env = makeHub();
  play(env, 1);
  const key = env.host.sql.get('SELECT player FROM metrics_players').player;
  const text = JSON.stringify([env.hub.metricsSummary(), env.hub.metricsHistory(30)]);
  assert.ok(!text.includes(key));
  assert.ok(!text.includes(identity(1).player));
  const reply = env.connect(identity(1)).hello();
  assert.ok(!JSON.stringify(reply).includes(key));
});

test('player rows older than the retention are dropped a day at a time as samples turn over', () => {
  const env = makeHub();
  const old = metricsDay(env.clock.now - (PLAYER_RETENTION_DAYS + 3) * DAY_MS);
  const kept = metricsDay(env.clock.now - (PLAYER_RETENTION_DAYS - 3) * DAY_MS);
  for (const day of [old, kept]) env.host.sql.run('INSERT INTO metrics_players VALUES (?, ?)', day, 'a'.repeat(32));
  greeted(env, 1);
  assert.equal(env.host.sql.get('SELECT COUNT(*) AS n FROM metrics_players WHERE day = ?', old).n, 0);
  assert.equal(env.host.sql.get('SELECT COUNT(*) AS n FROM metrics_players WHERE day = ?', kept).n, 1);
  assert.equal(PLAYER_RETENTION_DAYS, 2 * HISTORY_MAX_DAYS, 'covers the longest range and its comparison');
});

test('daily rows older than three years are dropped, newer ones kept', () => {
  const env = makeHub();
  const old = metricsDay(env.clock.now - (DAILY_RETENTION_DAYS + 3) * DAY_MS);
  const kept = metricsDay(env.clock.now - (DAILY_RETENTION_DAYS - 3) * DAY_MS);
  for (const day of [old, kept]) {
    env.host.sql.run('INSERT INTO metrics_daily (day, sessions) VALUES (?, 1)', day);
    env.host.sql.run(`INSERT INTO metrics_daily_dimensions VALUES (?, 'country', 'BR', 1)`, day);
  }
  greeted(env, 1);
  for (const table of ['metrics_daily', 'metrics_daily_dimensions']) {
    assert.equal(env.host.sql.get(`SELECT COUNT(*) AS n FROM ${table} WHERE day = ?`, old).n, 0, table);
    assert.equal(env.host.sql.get(`SELECT COUNT(*) AS n FROM ${table} WHERE day = ?`, kept).n, 1, table);
  }
  assert.ok(DAILY_RETENTION_DAYS >= PLAYER_RETENTION_DAYS);
});

test('room lifecycle: created, joined, closed, largest party and lifetime', () => {
  const env = makeHub();
  const { clients } = buildParty(env, 3);
  assert.equal(today(env).rooms_created, 1);
  assert.equal(today(env).room_joins, 2);
  later(env, 2 * MINUTE);
  clients[0].call('party.leave');
  const day = today(env);
  assert.deepEqual([day.rooms_closed, day.rooms_with_guest, day.party_total, day.rooms_relayed], [1, 1, 3, 0]);
  assert.deepEqual(dimension(env, 'occupancy'), { 3: 1 });
  assert.deepEqual(dimension(env, 'lifetime'), { '1_5m': 1 });
});

test('a room that never had a guest closes as a party of one, and a guest leaving keeps the largest party', () => {
  const env = makeHub();
  const solo = greeted(env, 1, { ip: ip(1) });
  solo.call('party.create');
  solo.call('party.leave');
  const { clients } = buildParty(env, 2);
  clients[1].call('party.leave');
  clients[0].call('party.leave');
  const day = today(env);
  assert.deepEqual([day.rooms_closed, day.rooms_with_guest, day.party_total], [2, 1, 3]);
  assert.deepEqual(dimension(env, 'occupancy'), { 1: 1, 2: 1 });
});

test('a party from before peaks were kept is not counted when it ends', () => {
  const env = makeHub();
  const { clients, match } = buildParty(env, 2);
  env.host.sql.run('UPDATE parties SET peak = 0 WHERE id = ?', match);
  clients[0].call('party.leave');
  assert.equal(today(env).rooms_closed, 0);
});

test('the lifetime buckets are the fixed five', () => {
  const env = makeHub();
  const spans = [[30, 'under_1m'], [120, '1_5m'], [600, '5_15m'], [1800, '15_60m'], [7200, 'over_1h']];
  for (const [i, [seconds]] of spans.entries()) {
    const client = greeted(env, i + 1, { ip: ip(i + 1) });
    client.call('party.create');
    later(env, seconds * 1000);
    client.call('party.leave');
  }
  assert.deepEqual(dimension(env, 'lifetime'), Object.fromEntries(spans.map(([, name]) => [name, 1])));
});

test('a room where a pair chose the relay counts as relayed, and a direct one does not', () => {
  const env = makeHub();
  const relayed = buildParty(env, 2, { first: 100 });
  relayed.rooms[0].call('match.transport', { peer: 2, transport: 'relay' });
  const direct = buildParty(env, 2, { first: 200 });
  direct.rooms[0].call('match.transport', { peer: 2, transport: 'direct' });
  env.rooms.tick();
  relayed.clients[0].call('party.leave');
  direct.clients[0].call('party.leave');
  assert.equal(today(env).rooms_closed, 2);
  assert.equal(today(env).rooms_relayed, 1);
});

test('platform, version and country are counted per session, bounded, and never from free text', () => {
  const env = makeHub();
  const first = env.hub.admit(ip(1), 'BR');
  assert.equal(first.country, 'BR');
  assert.equal(env.hub.admit(ip(2), 'XX').country, 'Unknown');
  assert.equal(env.hub.admit(ip(3)).country, 'Unknown');
  assert.equal(countryCode('T1'), 'Unknown');
  assert.equal(countryCode('br'), 'Unknown');
  assert.equal(countryCode("'; DROP"), 'Unknown');
  assert.equal(platformCategory('Steam Deck'), 'Steam Deck');
  assert.equal(platformCategory('<script>'), 'Other');
  assert.equal(versionCategory('0.4.2'), '0.4.2');
  assert.equal(versionCategory('abc'), 'Other');
  assert.equal(versionCategory(''), 'Unknown');
  greeted(env, 1, { ip: ip(9), version: '1.2.3', platform: 'Linux' });
  assert.deepEqual(dimension(env, 'platform'), { Linux: 1 });
  assert.deepEqual(dimension(env, 'version'), { '1.2.3': 1 });
  assert.deepEqual(dimension(env, 'country'), { Unknown: 1 });
});

test('live numbers count one per account and follow the sockets and the party tables', () => {
  const env = makeHub();
  const a = greeted(env, 1);
  env.connect(identity(1)).hello();
  buildParty(env, 2, { connect: false });
  let live = env.hub.metricsSummary().live;
  assert.deepEqual([live.online, live.in_rooms, live.rooms], [3, 2, 1]);
  assert.deepEqual(live.platform, [{ value: 'Windows', count: 3 }]);
  a.drop();
  live = env.hub.metricsSummary().live;
  assert.equal(live.online, 3, 'the same account still has another socket');
  assert.equal(today(env).peak_online, 3);
});

test('peaks and samples are written only when they grow', () => {
  const env = makeHub();
  buildParty(env, 2, { connect: false });
  const peak = () => env.host.sql.get('SELECT peak_online, peak_in_rooms, peak_rooms FROM metrics_daily').peak_online;
  assert.equal(peak(), 2);
  const writes = [];
  const run = env.host.sql.run;
  env.host.sql.run = (query, ...args) => { writes.push(query); return run.call(env.host.sql, query, ...args); };
  env.hub.observeLive();
  env.hub.observeLive();
  assert.equal(writes.filter((q) => /metrics_samples|peak_online/.test(q) && !/DELETE/.test(q)).length, 0);
});

test('samples are taken at the 15-minute alarm while someone is connected, and kept 90 days', () => {
  const env = makeHub();
  greeted(env, 1);
  const before = env.host.sql.get('SELECT COUNT(*) AS n FROM metrics_samples').n;
  assert.equal(before, 1);
  env.advance(SAMPLE_MS + 1000);
  assert.equal(env.host.sql.get('SELECT COUNT(*) AS n FROM metrics_samples').n, 2);
  env.host.sql.run('INSERT INTO metrics_samples VALUES (?, 1, 0, 0)', env.clock.now - (SAMPLE_DAYS + 2) * DAY_MS);
  env.advance(SAMPLE_MS + 1000);
  assert.equal(env.host.sql.get('SELECT COUNT(*) AS n FROM metrics_samples WHERE bucket < ?', env.clock.now - SAMPLE_DAYS * DAY_MS).n, 0);
});

test('with nobody connected no sampling alarm is kept', () => {
  const env = makeHub();
  assert.equal(env.hub.sampleDueAt(), null);
  const client = greeted(env, 1);
  assert.ok(env.hub.sampleDueAt() > env.clock.now);
  client.drop();
  assert.equal(env.hub.sampleDueAt(), null);
});

test('a refused protocol is counted once per connection and address window, and the socket stays', () => {
  const env = makeHub();
  const client = env.connect(identity(1), { ip: ip(1) });
  for (let i = 0; i < 3; i++) assert.equal(client.hello({ proto: PROTOCOL + 1 }).code, 'proto');
  const again = env.connect(identity(2), { ip: ip(1) });
  again.hello({ proto: PROTOCOL + 1 });
  assert.equal(today(env).incompatible_rejects, 1);
  assert.deepEqual(dimension(env, 'reject'), { lobby_protocol: 1 });
  assert.equal(client.closedWith, null);
});

test('protection counters reach the metrics dimension as well as the telemetry counters', () => {
  const env = makeHub();
  greeted(env, 1);
  env.hub.flushDiagnostics();
  assert.equal(dimension(env, 'abuse').hello_accepted, 1);
  assert.equal(env.hub.telemetrySummary(7).abuse.find((a) => a.value === 'hello_accepted').count, 1);
});

test('an empty service answers zeros, nulls and no division by zero', () => {
  const env = makeHub();
  const history = env.hub.metricsHistory(30);
  assert.equal(history.totals.unique_players, 0);
  assert.equal(history.metrics_first_day, null);
  assert.equal(history.players_first_day, null);
  assert.equal(history.party_first_day, null);
  assert.ok(!/NaN|Infinity/.test(JSON.stringify(history)));
  assert.ok(!/null,null/.test(JSON.stringify(history.daily)));
});

test('a database from before these metrics gains them without losing a row', () => {
  const folder = mkdtempSync(join(tmpdir(), 'tmnt-metrics-'));
  const file = join(folder, 'lobby.sqlite');
  const legacy = makeHub({ db: file });
  greeted(legacy, 1).call('party.create');
  legacy.host.sql.close();
  const db = new DatabaseSync(file);
  db.exec('DROP TABLE metrics_daily; DROP TABLE metrics_daily_dimensions; DROP TABLE metrics_samples; DROP TABLE metrics_players');
  db.exec('ALTER TABLE players DROP COLUMN session; ALTER TABLE parties DROP COLUMN peak; ALTER TABLE parties DROP COLUMN relayed');
  db.close();
  const upgraded = makeHub({ db: file });
  assert.equal(upgraded.hub.stats().players, 1);
  assert.equal(upgraded.hub.stats().parties, 1);
  assert.equal(upgraded.hub.partyOf(identity(1).player).party.peak, 0);
  const back = upgraded.connect(identity(1));
  assert.equal(back.hello().code, 'ok');
  assert.equal(upgraded.hub.metricsSummary().today.sessions, 1);
  back.call('party.leave');
  assert.equal(upgraded.hub.metricsSummary().today.rooms_closed, 0, 'a party already open when this shipped is not counted');
  upgraded.host.sql.close();
  rmSync(folder, { recursive: true, force: true });
});

test('a metrics store that cannot start leaves sign-in and parties working', () => {
  const env = makeHub();
  env.hub.metrics = null;
  const client = greeted(env, 1);
  assert.equal(client.call('party.create').code, 'ok');
  assert.equal(client.call('party.report', { characters: [0, 1] }).code, 'ok');
  assert.equal(client.call('party.leave').code, 'ok');
  assert.equal(env.hub.sampleDueAt(), null);
});
