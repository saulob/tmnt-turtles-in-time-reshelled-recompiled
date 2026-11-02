import assert from 'node:assert/strict';
import { test } from 'node:test';

import { Hub } from '../src/hub.js';
import { LIMITS } from '../src/protocol.js';
import { CLIENT_EVENTS, SERVER_EVENTS, TELEMETRY_LIMITS, TelemetryStore, distribution, parseBatch, percentile,
  sanitizeValue } from '../src/telemetry.js';
import { sqliteStore } from '../local/host.js';
import { befriend, buildParty, greeted, makeHub } from './harness.js';

const DIAG = 'abcdef012345';
const DAY = 24 * 3600 * 1000;

const context = (over = {}) => ({ game: 'tmnt-reshelled-pc', proto: 4, version: '0.4.2', platform: 'Windows', os: '10.0.26200',
  arch: 'x64', fp: '9f2c41aa7be0d311', slot: 2, host: false, party: 3, ...over });

const ping = (over = {}) => ({ sent: 40, recv: 38, last: 31, min: 22, max: 60, avg: 33.5, jit: 4.2, to: 2, ooo: 0, dup: 0,
  loss: 0.05, ...over });

const peer = (over = {}) => ({ ls: 2, rs: 1, state: 'connected', transport: 'direct', direct: 'ok', relay: 'none', fallback: '',
  conn_ms: 5000, est_ms: 850, ping: ping(), tx: 900, rx: 880, reconn: 0, reason: '', ...over });

const session = (over = {}) => ({ dur_ms: 60000, ct_usable: true, worker: true, stun: true, direct: true, relay: true, compat: 'ok',
  reason: 'host_left', ...over });

const batch = (over = {}) => ({ diag: DIAG, ctx: context(), session: session(), peers: [peer()],
  events: [{ e: 'direct_attempt', t: 1.5, f: { slot: '1' } }, { e: 'direct_result', t: 2.25, f: { result: 'ok', ms: '850', slot: '1' } },
    { e: 'transport_selected', t: 2.3, f: { transport: 'direct', ms: '850', slot: '1' } }], ...over });

const send = (client, body) => client.call('telemetry.batch', body);
const everything = (env) => ['tel_events', 'tel_sessions', "tel_counters WHERE k NOT LIKE 'abuse.%'"]
  .flatMap((t) => env.host.sql.all(`SELECT * FROM ${t}`));
const values = (list) => Object.fromEntries(list.map((i) => [i.value, i.count]));

test('a client batch is accepted and shows up in the aggregates and the session drill-down', () => {
  const env = makeHub();
  const a = greeted(env, 1);
  const reply = send(a, batch());
  assert.deepEqual([reply.code, reply.accepted, reply.ignored], ['ok', 3, 0]);
  const summary = env.hub.telemetrySummary(7);
  assert.equal(summary.totals.sessions, 1);
  assert.equal(summary.totals.batches, 1);
  assert.equal(summary.totals.client_events, 3);
  assert.deepEqual(values(summary.sessions_by.platform), { Windows: 1 });
  assert.deepEqual(values(summary.sessions_by.version), { '0.4.2': 1 });
  assert.deepEqual(values(summary.direct_results), { ok: 1 });
  assert.deepEqual(values(summary.selected), { direct: 1 });
  assert.equal(summary.transport.direct, 1);
  assert.equal(summary.connection_tests.usable, 1);
  assert.equal(summary.live.online, 1);
  assert.equal(summary.recent_sessions[0].diag, DIAG);

  const detail = env.hub.telemetrySession(DIAG);
  assert.equal(detail.reporters.length, 1);
  assert.equal(detail.reporters[0].slot, 2);
  assert.equal(detail.reporters[0].peers[0].ping.avg, 33.5);
  assert.deepEqual(detail.events.map((e) => e.name), ['direct_attempt', 'direct_result', 'transport_selected']);
  assert.equal(detail.events[1].fields.result, 'ok');
  assert.equal(env.hub.telemetrySession('000000000000'), null);

  send(a, batch({ events: [], session: undefined, peers: undefined }));
  const again = env.hub.telemetrySession(DIAG).reporters[0];
  assert.equal(again.batches, 2);
  assert.equal(again.session.reason, 'host_left');
  assert.equal(again.peers.length, 1);
  assert.equal(env.hub.telemetrySummary(7).totals.sessions, 1);
});

test('the client batch sizes fit: a batch of up to 8 KB is accepted, larger frames are not', () => {
  const env = makeHub();
  const a = greeted(env, 1);
  const wide = 'z'.repeat(48);
  const events = Array.from({ length: 12 }, (_, i) => ({ e: 'direct_result', t: i,
    f: { k0: wide, k1: wide, k2: wide, k3: wide, k4: wide, k5: wide, k6: wide, k7: String(i) } }));
  const text = JSON.stringify({ op: 'telemetry.batch', seq: 1, ...batch({ events }) });
  assert.ok(text.length > LIMITS.frameBytes && text.length <= 8192 + 32);
  assert.deepEqual([a.raw(text).code, a.raw(text).accepted === undefined], ['ok', false]);
  const huge = JSON.stringify({ op: 'telemetry.batch', seq: 2, ...batch({ events }), pad: 'x'.repeat(LIMITS.telemetryFrameBytes) });
  assert.equal(a.raw(huge).code, 'too_large');
  assert.equal(a.raw(JSON.stringify({ op: 'ping', seq: 3, pad: 'x'.repeat(LIMITS.frameBytes) })).code, 'too_large');
  assert.equal(a.call('ping').code, 'ok');
});

test('only signed-in sockets can send telemetry', () => {
  const env = makeHub();
  const a = env.connect({ token: 'a'.repeat(64), player: 'AAAA-AAAA' });
  assert.equal(send(a, batch()).code, 'no_session');
  assert.equal(everything(env).length, 0);
});

test('malformed batches are refused with a clear code and store nothing', () => {
  const env = makeHub();
  const a = greeted(env, 1);
  const bad = (over, code = 'bad_request') => {
    env.advance(30000);
    assert.equal(send(a, over).code, code, JSON.stringify(over).slice(0, 400));
  };
  bad({});
  bad(batch({ diag: 'not-a-diag' }));
  bad(batch({ diag: 'ABCDEF012345' }));
  bad(batch({ diag: undefined }));
  bad(batch({ ctx: undefined }));
  bad(batch({ ctx: 'x' }));
  bad(batch({ ctx: context({ slot: 0 }) }));
  bad(batch({ ctx: context({ slot: 5 }) }));
  bad(batch({ ctx: context({ slot: 'two' }) }));
  bad(batch({ ctx: context({ platform: 7 }) }));
  bad(batch({ session: 'x' }));
  bad(batch({ peers: 'x' }));
  bad(batch({ peers: [null] }));
  bad(batch({ peers: [peer({ ls: 9 })] }));
  bad(batch({ peers: [peer({ ping: { sent: 'many' } })] }));
  bad(batch({ events: 'x' }));
  bad(batch({ events: [null] }));
  bad(batch({ events: [{ e: 'direct_attempt', t: 'soon' }] }));
  bad(batch({ events: [{ e: 'direct_attempt', f: 'x' }] }));
  bad(batch({ dropped: 'lots' }));
  bad(batch({ events: Array.from({ length: 201 }, () => ({ e: 'session_end' })) }), 'too_large');
  bad(batch({ peers: Array.from({ length: 9 }, (_, i) => peer({ rs: (i % 4) + 1, ls: 2 })) }), 'too_large');
  assert.equal(env.host.sql.get('SELECT COUNT(*) AS n FROM tel_sessions').n, 0);
  assert.equal(env.host.sql.get('SELECT COUNT(*) AS n FROM tel_events').n, 0);
  assert.ok(env.hub.telemetrySummary(1).totals.rejected_batches >= 20);
  assert.equal(a.raw('{"op":"telemetry.batch","seq":').code, 'bad_request');
  assert.equal(send(a, batch()).code, 'ok');
});

test('event names outside the allow-list are ignored, the rest of the batch is kept', () => {
  const env = makeHub();
  const a = greeted(env, 1);
  const events = [{ e: 'session_end', t: 1, f: { reason: 'host_left' } }, { e: 'Session_End' }, { e: 'drop table' }, { e: 'http://x.y' },
    { e: 5 }, { e: 'tel_events' }, { e: 'party_ended' }];
  const reply = send(a, batch({ events }));
  assert.deepEqual([reply.code, reply.accepted, reply.ignored], ['ok', 1, 6]);
  assert.deepEqual(env.hub.telemetrySession(DIAG).events.map((e) => e.name), ['session_end']);
  assert.equal(env.hub.telemetrySummary(1).totals.ignored_events, 6);
  assert.equal(CLIENT_EVENTS.size, 16);
  assert.equal(CLIENT_EVENTS.has('party_ended'), false);
  assert.equal(SERVER_EVENTS.has('direct_result'), false);
});

test('values that look like friend codes, tokens, addresses, URLs, paths or free text are redacted', () => {
  const token = 'ab'.repeat(32);
  const leaks = ['Q7K2-9ZXA', 'code=Q7K2-9ZXA!', token, `tok_${token.slice(0, 32)}`, '192.168.1.20', '8.8.8.8:3478', '2001:db8::1',
    '::ffff:1.2.3.4', 'http://evil.example/x', 'wss://host', 'www.example.com', 'user@example.com', 'C:\\Users\\Saulo\\game.exe',
    '/home/saulo/.config', 'John Smith', 'line\nbreak', 'AB!CD-EFGH', '01234567-89ab-cdef-0123-456789abcdef'];
  for (const leak of leaks) assert.equal(sanitizeValue(leak), 'redacted', leak);
  assert.equal(sanitizeValue('failed:hole_punch_failed'), 'failed:hole_punch_failed');
  assert.equal(sanitizeValue('1.2.3'), '1.2.3');
  assert.equal(sanitizeValue('a"b<c>{d}'), 'abcd');
  assert.equal(sanitizeValue('z'.repeat(500)), 'z'.repeat(48));
  assert.equal(sanitizeValue('<script>alert(1)</script>'), 'redacted');

  const env = makeHub();
  const a = greeted(env, 1);
  const f = {};
  leaks.forEach((leak, i) => { f[`k${i % 8}`] = leak; });
  const hostile = batch({
    ctx: context({ platform: '10.1.2.3', os: 'Q7K2-9ZXA', fp: token, version: 'http://x.y/z' }),
    session: session({ reason: 'user@example.com', compat: '/etc/passwd' }),
    peers: [peer({ state: '192.168.0.9', fallback: token, reason: 'Q7K2-9ZXA' })],
    events: leaks.map((leak, i) => ({ e: 'peer_disconnect', t: i, f: { reason: leak, slot: leak, [leak]: 'x', [`k${i}`]: leak } })),
  });
  const reply = send(a, hostile);
  assert.equal(reply.code, 'ok');
  const dump = JSON.stringify([everything(env), env.hub.telemetrySummary(7), env.hub.telemetrySession(DIAG)]);
  for (const leak of leaks.filter((l) => /[0-9A-Za-z]{4}/.test(l))) {
    assert.ok(!dump.includes(leak.replace(/\\/g, '\\\\').replace(/\n/g, '\\n')), `leaked ${leak}`);
  }
  for (const pattern of [/[A-Z0-9]{4}-[A-Z0-9]{4}/, /[0-9a-f]{32}/, /\d{1,3}(\.\d{1,3}){3}/, /:\/\//, /@/, /<script/i]) {
    assert.doesNotMatch(dump.replace(/"(diag|who)":"[0-9a-f]+"/g, ''), pattern);
  }
  const detail = env.hub.telemetrySession(DIAG);
  assert.equal(detail.reporters[0].ctx.platform, 'redacted');
  assert.ok(env.hub.telemetrySummary(1).totals.redacted_values > 10);
  assert.ok(detail.events.every((e) => Object.keys(e.fields).every((k) => k !== 'redacted' || e.fields[k])));
});

test('a field value that is not text is dropped, field counts and key lengths are bounded', () => {
  const batchOf = parseBatch(batch({ events: [{ e: 'direct_result', f: { a: 1, b: true, c: { x: 1 }, d: [1], e: null, f: 'ok',
    long_key_long_key_long_key_long_key: 'v', h: '1', i: '2', j: '3' } }] }));
  const fields = batchOf.events[0].fields;
  assert.equal(fields.a, '1');
  assert.equal(fields.b, 'true');
  assert.equal(fields.c, undefined);
  assert.equal(fields.d, undefined);
  assert.ok(Object.keys(fields).length <= TELEMETRY_LIMITS.maxFields);
  assert.ok(Object.keys(fields).every((k) => k.length <= TELEMETRY_LIMITS.keyChars));
});

test('telemetry is limited per player and for the whole service, and the limits recover', () => {
  const env = makeHub();
  const a = greeted(env, 1);
  const results = [];
  for (let i = 0; i < 9; i++) results.push(send(a, batch()).code);
  assert.deepEqual(results, ['ok', 'ok', 'ok', 'ok', 'ok', 'ok', 'limit', 'limit', 'limit']);
  assert.equal(a.call('ping').code, 'ok');
  env.advance(5000);
  assert.equal(send(a, batch()).code, 'ok');
  assert.equal(send(a, batch()).code, 'limit');
  env.advance(11000);
  assert.ok(env.hub.telemetrySummary(1).server.rate_limited.some((r) => r.value === 'telemetry' && r.count >= 3));

  const crowd = makeHub();
  let limited = 0;
  let ok = 0;
  for (let n = 1; n <= 40; n++) {
    const p = greeted(crowd, 100 + n, { ip: `203.0.113.${n}` });
    for (let i = 0; i < 6; i++) send(p, batch({ diag: n.toString(16).padStart(12, '0'), events: [] })).code === 'ok' ? ok++ : limited++;
  }
  assert.ok(limited > 0 && ok >= 120 && ok <= 140, `${ok} accepted, ${limited} limited`);
  assert.ok(crowd.hub.telemetrySummary(1).server.rate_limited.some((r) => r.value === 'telemetry_all'));
});

test('a flood of limited requests writes in groups, not once per request', () => {
  const env = makeHub();
  const a = greeted(env, 1);
  let writes = 0;
  const run = env.host.sql.run;
  env.host.sql.run = (query, ...args) => {
    if (query.includes('tel_counters')) writes++;
    return run(query, ...args);
  };
  for (let i = 0; i < 300; i++) a.call('ping');
  assert.ok(writes <= 3, `${writes} counter writes`);
});

test('the event ring, the reports and the counters are bounded and expire', () => {
  const clock = { now: 1_700_000_000_000 };
  const store = new TelemetryStore(sqliteStore(), () => clock.now);
  const count = (table) => store.sql.get(`SELECT COUNT(*) AS n FROM ${table}`).n;

  for (let i = 0; i < TELEMETRY_LIMITS.ringEvents + 300; i++) store.record('invite_sent', { n: i }, DIAG);
  assert.equal(count('tel_events'), TELEMETRY_LIMITS.ringEvents);
  assert.equal(JSON.parse(store.sql.get('SELECT fields FROM tel_events ORDER BY id LIMIT 1').fields).n, '300');
  assert.equal(store.summary(1).server.parties_started, 0);
  assert.equal(store.summary(1).invites.sent, TELEMETRY_LIMITS.ringEvents + 300);

  const parsed = (diag, slot = 2) => parseBatch(batch({ diag, ctx: context({ slot }) }));
  for (let i = 0; i < TELEMETRY_LIMITS.rowsPerDiag; i++) store.ingest(`who${i}`, parsed('111111111111', 1 + (i % 4)));
  assert.throws(() => store.ingest('whoX', parsed('111111111111')), { code: 'limit' });
  assert.equal(count('tel_sessions'), TELEMETRY_LIMITS.rowsPerDiag);

  for (let i = 0; i < TELEMETRY_LIMITS.sessionRows + 100; i++) {
    clock.now += 1000;
    store.ingest('w', parsed(i.toString(16).padStart(12, '0')));
  }
  assert.equal(count('tel_sessions'), TELEMETRY_LIMITS.sessionRows);
  assert.equal(store.sql.get("SELECT COUNT(*) AS n FROM tel_sessions WHERE diag = '111111111111'").n, 0);

  clock.now += 6 * DAY;
  store.record('invite_sent', {}, DIAG);
  assert.ok(count('tel_events') > 0);
  clock.now += 2 * DAY;
  store.record('invite_sent', {}, DIAG);
  assert.equal(count('tel_events'), 2);
  assert.equal(count('tel_sessions'), 0);
  clock.now += 40 * DAY;
  store.record('invite_sent', {}, DIAG);
  assert.ok(store.sql.all('SELECT DISTINCT day FROM tel_counters').length <= 2);
});

test('the number of distinct counters per day is bounded', () => {
  const clock = { now: 1_700_000_000_000 };
  const store = new TelemetryStore(sqliteStore(), () => clock.now);
  for (let i = 0; i < TELEMETRY_LIMITS.keysPerDay + 200; i++) store.record('room_left', { code: `r${i}` }, '');
  assert.ok(store.sql.get('SELECT COUNT(*) AS n FROM tel_counters').n <= TELEMETRY_LIMITS.keysPerDay + 1);
  assert.ok(store.sql.get("SELECT n FROM tel_counters WHERE k = 'overflow'").n >= 200);
  assert.equal(store.sql.get("SELECT n FROM tel_counters WHERE k = 'server.room_left'").n, TELEMETRY_LIMITS.keysPerDay + 200);
});

test('percentiles use the nearest rank and the aggregates count each link once', () => {
  const hundred = Array.from({ length: 100 }, (_, i) => i + 1);
  assert.deepEqual([50, 90, 95, 99, 100].map((p) => percentile(hundred, p)), [50, 90, 95, 99, 100]);
  assert.equal(percentile([], 50), null);
  assert.deepEqual(distribution([5]), { n: 1, min: 5, p50: 5, p90: 5, p95: 5, p99: 5, max: 5 });
  assert.deepEqual(distribution([]), { n: 0, min: null, p50: null, p90: null, p95: null, p99: null, max: null });
  assert.equal(distribution([3, 1, 2, NaN]).n, 3);

  const clock = { now: 1_700_000_000_000 };
  const store = new TelemetryStore(sqliteStore(), () => clock.now);
  const link = (ls, rs, over, avg) => peer({ ls, rs, ping: ping({ avg, jit: avg / 10, loss: avg / 1000 }), ...over });
  // Ten links in one party from the lower slot's side: avg 10, 20, ... 100 ms.
  for (let i = 0; i < 10; i++) {
    const diag = `00000000000${i % 10}`;
    store.ingest('w', parseBatch(batch({ diag, ctx: context({ slot: 1 }), peers: [link(1, 2, i < 8 ? {} : { transport: 'relay' }, (i + 1) * 10)] })));
  }
  // The same link seen from the other end is not counted twice for the mix.
  store.ingest('v', parseBatch(batch({ diag: '000000000000', ctx: context({ slot: 2 }), peers: [link(2, 1, { transport: 'direct' }, 11)] })));
  const s = store.summary(1);
  assert.deepEqual([s.transport.pairs, s.transport.direct, s.transport.relay], [10, 8, 2]);
  assert.deepEqual(s.latency.direct.rtt_ms.n, 9);
  assert.equal(s.latency.all.rtt_ms.n, 11);
  assert.equal(s.latency.all.rtt_ms.max, 100);
  assert.equal(s.latency.relay.rtt_ms.p50, 90);
  assert.equal(s.latency.all.jitter_ms.max, 10);
  assert.equal(s.latency.all.loss.n, 11);
  assert.equal(s.latency.all.loss.max, 0.1);
});

test('a link that never got a pong adds no round trip or jitter sample', () => {
  const clock = { now: 1_700_000_000_000 };
  const store = new TelemetryStore(sqliteStore(), () => clock.now);
  store.ingest('w', parseBatch(batch({ peers: [peer({ ping: ping({ sent: 0, recv: 0, avg: 0, jit: 0, loss: 0 }) })] })));
  const s = store.summary(1);
  assert.deepEqual([s.latency.all.rtt_ms.n, s.latency.all.jitter_ms.n, s.latency.all.loss.n], [0, 0, 0]);
});

test('connection test outcomes, compatibility and fallback reasons are aggregated', () => {
  const env = makeHub();
  const a = greeted(env, 1);
  const b = greeted(env, 2);
  send(a, batch({ session: session({ ct_usable: true }), events: [{ e: 'relay_fallback', f: { reason: 'direct_timeout', slot: '1' } },
    { e: 'peer_disconnect', f: { reason: 'peer_disconnected' } }, { e: 'session_end', f: { reason: 'host_left' } },
    { e: 'invite_failed', f: { reason: 'offline' } }] }));
  send(b, batch({ ctx: context({ slot: 3, platform: 'Linux' }), session: session({ ct_usable: false, stun: false, compat: 'version_mismatch' }),
    events: [{ e: 'relay_fallback', f: { reason: 'direct_timeout' } }, { e: 'relay_fallback', f: { reason: 'stun_failed' } }] }));
  const s = env.hub.telemetrySummary(7);
  assert.deepEqual(values(s.fallback_reasons), { direct_timeout: 2, stun_failed: 1 });
  assert.deepEqual(values(s.disconnect_reasons.peer), { peer_disconnected: 1 });
  assert.deepEqual(values(s.disconnect_reasons.session), { host_left: 1 });
  assert.deepEqual(values(s.invites.client_failed), { offline: 1 });
  assert.deepEqual([s.connection_tests.reports, s.connection_tests.usable, s.connection_tests.stun_ok], [2, 1, 1]);
  assert.deepEqual(values(s.connection_tests.compatibility), { ok: 1, version_mismatch: 1 });
  assert.deepEqual(values(s.sessions_by.platform), { Windows: 1, Linux: 1 });
});

test('the Worker records a full host and three guests party flow, correlated by the party diag', () => {
  const env = makeHub();
  const party = buildParty(env, 4);
  const [host, two, three, four] = party.clients;
  const { diag } = party;

  party.rooms[0].call('match.transport', { peer: 2, transport: 'direct' });
  party.rooms[1].call('match.transport', { peer: 3, transport: 'relay' });
  party.rooms[2].failBinary = true;
  party.rooms[1].relay(party.rooms[1].frame(3, party.token));
  party.rooms[3].drop();
  env.advance(1000);

  const extra = [greeted(env, 200), greeted(env, 201), greeted(env, 202)];
  for (const friend of extra) befriend(host, friend);
  env.clock.now += 60000;
  assert.equal(host.call('invite.send', { player: extra[0].who.player }).code, 'party_full');
  assert.equal(host.call('party.kick', { player: four.who.player }).code, 'ok');
  const sent = host.call('invite.send', { player: extra[1].who.player });
  assert.equal(extra[1].call('invite.decline', { invite: sent.invite }).code, 'ok');
  host.call('invite.send', { player: extra[2].who.player });
  env.advance(LIMITS.inviteMs + 1000);
  assert.equal(two.call('party.leave').code, 'ok');
  assert.equal(host.call('party.leave').code, 'ok');

  const s = env.hub.telemetrySummary(7);
  assert.equal(s.server.parties_started, 1);
  assert.equal(s.server.member_joined, 3);
  assert.equal(s.invites.sent, 5);
  assert.equal(s.invites.accepted, 3);
  assert.equal(s.invites.declined, 1);
  assert.equal(s.invites.expired, 1);
  assert.equal(s.invites.party_full, 1);
  assert.equal(s.server.fifth_member_rejected, 1);
  assert.equal(s.server.room_joined, 4);
  assert.deepEqual(values(s.server.room_transport), { direct: 1, relay: 1 });
  assert.deepEqual(values(s.server.relay_closed), { peer_unavailable: 1 });
  assert.deepEqual(values(s.server.member_left), { kicked: 1, left: 1 });
  assert.deepEqual(values(s.server.party_ended), { host_left: 1 });
  assert.deepEqual(values(s.server.room_ended), { host_left: 1 });
  assert.ok(values(s.server.room_left).kicked >= 1);
  assert.ok(s.server.room_peer_lost >= 1);

  const detail = env.hub.telemetrySession(diag);
  const names = detail.events.map((e) => e.name);
  assert.equal(names[0], 'party_created');
  assert.ok(detail.events.every((e) => e.src === 'server'));
  for (const name of ['member_joined', 'room_joined', 'room_transport', 'relay_closed', 'party_full', 'member_left', 'invite_declined',
    'invite_expired', 'party_ended', 'room_ended']) {
    assert.ok(names.includes(name), name);
  }
  assert.ok(names.indexOf('party_created') < names.indexOf('party_ended'));
  assert.equal(s.recent_sessions.find((r) => r.diag === diag).ended, 'host_left');
  assert.ok(three);
  const dump = JSON.stringify(everything(env));
  for (const c of party.clients) assert.ok(!dump.includes(c.who.player));
  assert.ok(!dump.includes(party.match));
  assert.ok(!dump.includes(party.data[0].join_token));
});

test('a party that expires, and a member dropped after the grace, are recorded', () => {
  const env = makeHub();
  const party = buildParty(env, 3);
  party.clients[2].drop();
  party.rooms[2].drop();
  env.advance(LIMITS.graceMs + 1000);
  assert.deepEqual(values(env.hub.telemetrySummary(1).server.member_left), { expired: 1 });
  env.advance(LIMITS.matchMaxMs);
  const s = env.hub.telemetrySummary(1);
  assert.equal(Object.values(values(s.server.party_ended)).reduce((a, b) => a + b, 0), 1);
});

test('a telemetry store that fails never changes party, invite or room behaviour', () => {
  const env = makeHub();
  const broken = new Proxy({}, { get: () => () => { throw new Error('store is down'); } });
  env.hub.telemetry = broken;
  const party = buildParty(env, 4);
  assert.equal(party.clients.length, 4);
  assert.equal(party.rooms[1].call('match.transport', { peer: 3, transport: 'relay' }).code, 'ok');
  assert.equal(party.rooms[1].call('match.leave').code, 'ok');
  assert.equal(party.clients[2].call('party.leave').code, 'ok');
  assert.equal(party.clients[0].call('friends.list').code, 'ok');
  const reply = send(party.clients[0], batch());
  assert.equal(reply.code, 'internal');
  env.clock.now += 60000;
  assert.equal(party.clients[0].call('party.kick', { player: party.clients[3].who.player }).code, 'ok');
  assert.equal(party.clients[0].call('party.leave').code, 'ok');
  assert.equal(env.hub.stats().parties, 0);
  assert.throws(() => env.hub.telemetrySummary(7), /store is down/);
  assert.ok(env.logs.filter((l) => l.includes('telemetry unavailable')).length <= 4);
});

test('a store that cannot be created leaves the directory working', () => {
  const env = makeHub();
  const real = env.host.sql;
  const guarded = { ...real, run: (query, ...args) => {
    if (query.includes('tel_')) throw new Error('no space');
    return real.run(query, ...args);
  } };
  const host = Object.assign(Object.create(env.host), { sql: guarded });
  const hub = new Hub(host, {});
  assert.equal(hub.telemetry, null);
  assert.equal(hub.stats().players, 0);
  assert.throws(() => hub.telemetrySummary(7));
});
