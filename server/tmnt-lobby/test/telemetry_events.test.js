import assert from 'node:assert/strict';
import { test } from 'node:test';

import { LIMITS } from '../src/protocol.js';
import { CLIENT_EVENTS, SERVER_EVENTS, TELEMETRY_LIMITS, TelemetryStore, parseBatch } from '../src/telemetry.js';
import { sqliteStore } from '../local/host.js';
import { befriend, buildParty, greeted, makeHub } from './harness.js';

const DIAG = 'abcdef012345';
const LOBBY = '0123456789ab';
const DAY = 24 * 3600 * 1000;

const context = (over = {}) => ({ game: 'tmnt-reshelled-pc', proto: 4, version: '0.4.2', platform: 'Windows', os: '10.0.26200',
  arch: 'x64', fp: '9f2c41aa7be0d311', slot: 2, host: false, party: 3, ...over });

const peer = (over = {}) => ({ ls: 2, rs: 1, state: 'connected', transport: 'direct', direct: 'ok', relay: 'none', fallback: '',
  conn_ms: 5000, est_ms: 850, ping: { sent: 40, recv: 38, last: 31, min: 22, max: 60, avg: 33.5, jit: 4.2, to: 2, ooo: 0, dup: 0,
    loss: 0.05 }, tx: 900, rx: 880, reconn: 0, reason: '', ...over });

const session = (over = {}) => ({ dur_ms: 60000, ct_usable: true, worker: true, stun: true, direct: true, relay: true, compat: 'ok',
  reason: 'host_left', ...over });

const ev = (n, e = 'direct_result', f = { result: 'ok' }) => ({ e, n, t: n, f });
const send = (client, body) => client.call('telemetry.batch', body);
const batch = (events, over = {}) => ({ diag: DIAG, ctx: context(), session: session(), peers: [peer()], events, ...over });
const values = (list) => Object.fromEntries(list.map((i) => [i.value, i.count]));
const stored = (env) => env.host.sql.all("SELECT name, fields FROM tel_events WHERE src = 'client' ORDER BY id");

test('the client allow-list holds only what a client alone can see; the Worker records the rest', () => {
  assert.deepEqual([...CLIENT_EVENTS].sort(), ['direct_attempt', 'direct_result', 'game_start_attempt', 'game_start_failure',
    'game_start_success', 'invite_failed', 'invite_received', 'join_completed', 'join_failed', 'join_started', 'peer_disconnect',
    'relay_fallback', 'relay_result', 'session_end', 'transport_selected', 'worker_reconnect'].sort());
  for (const name of CLIENT_EVENTS) assert.equal(SERVER_EVENTS.has(name), false, name);
  for (const removed of ['invite_sent', 'invite_accepted', 'invite_declined', 'invite_expired', 'party_created', 'party_2_of_4',
    'party_3_of_4', 'party_4_of_4', 'player_joined', 'player_left', 'party_full', 'host_disconnect', 'guest_disconnect',
    'connection_test']) {
    assert.equal(CLIENT_EVENTS.has(removed), false, removed);
  }
  assert.equal(SERVER_EVENTS.has('party_reached_4'), true);

  const env = makeHub();
  const a = greeted(env, 1);
  const reply = send(a, batch([ev(1, 'invite_sent'), ev(2, 'party_created'), ev(3, 'connection_test'), ev(4, 'invite_received',
    { reason: 'offline' }), ev(5, 'host_disconnect')]));
  assert.deepEqual([reply.code, reply.accepted, reply.ignored, reply.duplicates], ['ok', 1, 4, 0]);
  assert.deepEqual(stored(env).map((e) => e.name), ['invite_received']);
});

test('an unknown reason becomes "other" and is counted; known and empty reasons are kept; nothing is discarded', () => {
  const env = makeHub();
  const a = greeted(env, 1);
  const hostile = batch([
    ev(1, 'relay_fallback', { reason: 'made_up_reason' }),
    ev(2, 'direct_result', { result: 'failed', reason: 'user@example.com', fallback: 'zzz' }),
    ev(3, 'peer_disconnect', { reason: 'peer_disconnected' }),
    ev(4, 'session_end', { reason: '' }),
    ev(5, 'relay_result', { result: 'ok', reason: 'Q7K2-9ZXA' }),
  ], { session: session({ reason: 'weird' }), peers: [peer({ fallback: 'nonsense', reason: 'also_nonsense' }), peer({ ls: 3, rs: 1,
    fallback: 'direct_timeout', reason: '' })] });
  assert.equal(send(a, hostile).code, 'ok');
  const rows = stored(env).map((e) => ({ name: e.name, fields: JSON.parse(e.fields) }));
  assert.equal(rows.length, 5);
  assert.equal(rows[0].fields.reason, 'other');
  assert.deepEqual([rows[1].fields.reason, rows[1].fields.fallback], ['other', 'other']);
  assert.equal(rows[2].fields.reason, 'peer_disconnected');
  assert.equal(rows[3].fields.reason, '');
  assert.equal(rows[4].fields.reason, 'other');
  const detail = env.hub.telemetrySession(DIAG).reporters[0];
  assert.equal(detail.session.reason, 'other');
  assert.deepEqual(detail.peers.map((p) => [p.fallback, p.reason]), [['other', 'other'], ['direct_timeout', '']]);
  const s = env.hub.telemetrySummary(1);
  assert.equal(s.totals.unknown_reasons, 4 + 3);
  assert.equal(s.reasons.other, 3);
  assert.equal(JSON.stringify(s).includes('made_up_reason'), false);

  send(a, batch([], { session: session({ reason: 'weird' }), peers: [peer({ fallback: 'nonsense' })] }));
  assert.equal(env.hub.telemetrySummary(1).totals.unknown_reasons, 7, 'a repeated snapshot is not counted again');
});

test('the summary groups every reason by the category of the shared list, never under another', () => {
  const env = makeHub();
  const a = greeted(env, 1);
  send(a, batch([ev(1, 'relay_fallback', { reason: 'direct_timeout' }), ev(2, 'relay_fallback', { reason: 'stun_failed' }),
    ev(3, 'peer_disconnect', { reason: 'peer_disconnected' }), ev(4, 'session_end', { reason: 'host_left' }),
    ev(5, 'invite_failed', { reason: 'offline' }), ev(6, 'join_failed', { reason: 'version_mismatch' }),
    ev(7, 'session_end', { reason: 'direct_timeout' }), ev(8, 'game_start_failure', { reason: 'nope' })]));
  const { reasons } = env.hub.telemetrySummary(1);
  assert.deepEqual(Object.keys(reasons), ['fallback', 'disconnect', 'invite', 'service', 'connection_test', 'other']);
  assert.deepEqual(reasons.fallback, { direct_timeout: 2, stun_failed: 1 });
  assert.deepEqual(reasons.disconnect, { host_left: 1, peer_disconnected: 1 });
  assert.deepEqual(reasons.service, { offline: 1, version_mismatch: 1 });
  assert.deepEqual([reasons.invite, reasons.connection_test, reasons.other], [{}, {}, 1]);
});

test('event numbers: a retried batch, or the same event inside another batch, is counted once', () => {
  const env = makeHub();
  const a = greeted(env, 1);
  const first = batch([ev(1, 'direct_attempt'), ev(2, 'direct_result'), ev(3, 'transport_selected', { transport: 'direct' })]);

  const one = send(a, first);
  assert.deepEqual([one.code, one.accepted, one.ignored, one.duplicates], ['ok', 3, 0, 0]);
  env.advance(30000);
  const again = send(a, first);
  assert.deepEqual(again, { re: again.re, code: 'ok', accepted: 0, ignored: 0, duplicates: 3 });
  assert.equal(stored(env).length, 3);

  env.advance(30000);
  const mixed = send(a, batch([...first.events, ev(4, 'relay_fallback', { reason: 'direct_timeout' }), ev(5, 'session_end',
    { reason: 'host_left' })]));
  assert.deepEqual([mixed.accepted, mixed.duplicates], [2, 3]);
  assert.equal(stored(env).length, 5);

  env.advance(30000);
  const stale = send(a, batch([ev(2, 'direct_result')]));
  assert.deepEqual([stale.accepted, stale.duplicates], [0, 1], 'a smaller number after a larger one was seen already');

  env.advance(30000);
  const shuffled = send(a, batch([ev(9, 'direct_attempt'), ev(7, 'direct_attempt'), ev(9, 'direct_attempt')]));
  assert.deepEqual([shuffled.accepted, shuffled.duplicates], [2, 1], 'within one batch each number counts once');
  env.advance(30000);
  const late = send(a, batch([ev(8, 'direct_attempt')]));
  assert.deepEqual([late.accepted, late.ignored, late.duplicates], [0, 0, 1]);

  const s = env.hub.telemetrySummary(1);
  assert.equal(s.totals.client_events, 7);
  assert.equal(s.totals.duplicate_events, 3 + 3 + 1 + 1 + 1);
  assert.deepEqual(values(s.client_events), { direct_attempt: 3, direct_result: 1, transport_selected: 1, relay_fallback: 1, session_end: 1 });
  assert.equal(env.hub.telemetrySession(DIAG).reporters[0].events, 7);
  assert.equal(env.host.sql.get('SELECT last_n FROM tel_sessions').last_n, 9);
});

test('an ignored event is deduplicated like any other, and a batch of duplicates only counts its duplicates', () => {
  const env = makeHub();
  const a = greeted(env, 1);
  const body = batch([ev(1, 'direct_attempt'), ev(2, 'invite_sent'), ev(3, 'bogus')], { dropped: 4 });
  assert.deepEqual([send(a, body).accepted, send(a, body).duplicates], [1, 3]);
  const s = env.hub.telemetrySummary(1);
  assert.deepEqual([s.totals.ignored_events, s.totals.client_events, s.totals.client_dropped], [2, 1, 4]);
});

test('events without a usable number are never deduplicated', () => {
  const env = makeHub();
  const a = greeted(env, 1);
  const plain = [{ e: 'direct_attempt', t: 1 }, { e: 'direct_attempt', t: 1 }];
  assert.equal(send(a, batch(plain)).accepted, 2);
  env.advance(30000);
  assert.equal(send(a, batch(plain)).accepted, 2);
  env.advance(30000);
  const odd = [0, -1, 1.5, '7', 2 ** 32, null, {}].map((n) => ({ e: 'direct_attempt', n, t: 1 }));
  const reply = send(a, batch(odd));
  assert.deepEqual([reply.accepted, reply.duplicates], [7, 0]);
  env.advance(30000);
  assert.deepEqual([send(a, batch(odd)).accepted, env.host.sql.get('SELECT last_n FROM tel_sessions').last_n], [7, 0]);
  env.advance(30000);
  assert.equal(send(a, batch([ev(2 ** 32 - 1, 'direct_attempt')])).accepted, 1);
});

test('reporters are independent: the same numbers from two reporters, slots or parties are both kept', () => {
  const env = makeHub();
  const a = greeted(env, 1);
  const b = greeted(env, 2);
  const events = [ev(1, 'direct_attempt'), ev(2, 'direct_result')];
  assert.equal(send(a, batch(events)).accepted, 2);
  assert.equal(send(b, batch(events, { ctx: context({ slot: 3 }) })).accepted, 2);
  env.advance(30000);
  assert.equal(send(a, batch(events, { diag: '111111111111' })).accepted, 2);
  assert.equal(send(b, batch(events, { ctx: context({ slot: 3 }) })).duplicates, 2);
  assert.equal(env.host.sql.get('SELECT COUNT(*) AS n FROM tel_sessions').n, 3);
});

test('the high-water mark lives in the session row and goes with it when the row is pruned', () => {
  const clock = { now: 1_700_000_000_000 };
  const store = new TelemetryStore(sqliteStore(), () => clock.now);
  const tables = () => store.sql.all("SELECT name FROM sqlite_master WHERE type = 'table'").map((r) => r.name).sort();
  const before = tables();
  const body = () => parseBatch(batch([ev(1), ev(2), ev(3)]));
  assert.equal(store.ingest('w', body()).accepted, 3);
  assert.equal(store.ingest('w', body()).duplicates, 3);
  clock.now += 6 * DAY;
  assert.equal(store.ingest('w', body()).duplicates, 3, 'still within the retention');
  clock.now += 8 * DAY;
  assert.equal(store.ingest('w', body()).accepted, 3, 'the pruned row forgot the numbers');
  assert.deepEqual(tables(), before);

  for (let i = 0; i < TELEMETRY_LIMITS.sessionRows + 5; i++) {
    clock.now += 1000;
    store.ingest('w', parseBatch(batch([ev(1)], { diag: i.toString(16).padStart(12, '0') })));
  }
  assert.equal(store.sql.get('SELECT COUNT(*) AS n FROM tel_sessions').n, TELEMETRY_LIMITS.sessionRows);
  assert.equal(store.ingest('w', parseBatch(batch([ev(1)], { diag: '000000000000' }))).accepted, 1);
});

test('a session table created before the high-water mark existed gains the column', () => {
  const sql = sqliteStore();
  sql.run(`CREATE TABLE tel_sessions (diag TEXT NOT NULL, slot INTEGER NOT NULL, who TEXT NOT NULL, first_ms INTEGER NOT NULL,
    last_ms INTEGER NOT NULL, batches INTEGER NOT NULL DEFAULT 0, events INTEGER NOT NULL DEFAULT 0,
    dropped INTEGER NOT NULL DEFAULT 0, ctx TEXT NOT NULL, report TEXT NOT NULL DEFAULT '', peers TEXT NOT NULL DEFAULT '',
    PRIMARY KEY (diag, slot, who))`);
  sql.run("INSERT INTO tel_sessions (diag, slot, who, first_ms, last_ms, ctx) VALUES ('aaaaaaaaaaaa', 2, 'w', 1, 2, '{}')");
  const store = new TelemetryStore(sql, () => 1_700_000_000_000);
  assert.equal(sql.get("SELECT last_n FROM tel_sessions WHERE diag = 'aaaaaaaaaaaa'").last_n, 0);
  assert.equal(store.ingest('w', parseBatch(batch([ev(5)], { diag: 'aaaaaaaaaaaa' }))).accepted, 1);
  assert.equal(store.ingest('w', parseBatch(batch([ev(5)], { diag: 'aaaaaaaaaaaa' }))).duplicates, 1);
  new TelemetryStore(sql, () => 1_700_000_000_000);
});

test('a summary after retries equals the summary without them', () => {
  const clean = makeHub();
  const retried = makeHub();
  const run = (env, retries) => {
    const party = buildParty(env, 3, { first: 100 });
    const [host, two, three] = party.clients;
    const extra = greeted(env, 300, { ip: '198.51.100.9' });
    befriend(host, extra);
    host.call('party.kick', { player: three.who.player });
    const sent = host.call('invite.send', { player: extra.who.player });
    extra.call('invite.decline', { invite: sent.invite });
    two.call('party.leave');
    const reporters = [greeted(env, 400, { ip: '198.51.100.10' }), greeted(env, 401, { ip: '198.51.100.11' })];
    const base = [
      ev(1, 'direct_attempt'), ev(2, 'direct_result', { result: 'ok' }), ev(3, 'transport_selected', { transport: 'direct' }),
      ev(4, 'relay_fallback', { reason: 'direct_timeout' }), ev(5, 'peer_disconnect', { reason: 'peer_disconnected' }),
      ev(6, 'invite_failed', { reason: 'offline' }), ev(7, 'session_end', { reason: 'host_left' }),
    ];
    reporters.forEach((client, i) => {
      const body = batch(base, { diag: i ? '222222222222' : DIAG, ctx: context({ slot: 1 + i }),
        peers: [peer({ ls: 1 + i, rs: 3, transport: i ? 'relay' : 'direct' })] });
      send(client, body);
      for (let r = 0; r < 3; r++) {
        env.advance(30000);
        if (retries) {
          send(client, body);
          send(client, { ...body, events: [...base.slice(3), ev(8, 'direct_attempt')] });
          send(client, { ...body, events: [ev(8, 'direct_attempt')] });
        } else if (r === 0) {
          send(client, { ...body, events: [ev(8, 'direct_attempt')] });
        }
      }
    });
    const lobby = greeted(env, 500, { ip: '198.51.100.12' });
    const lobbyBody = { diag: LOBBY, ctx: context({ slot: 0, party: 0 }), events: [ev(1, 'invite_received'), ev(2, 'join_started')] };
    send(lobby, lobbyBody);
    for (let r = 0; r < 3; r++) {
      env.advance(30000);
      if (retries) send(lobby, lobbyBody);
    }
  };
  run(clean, false);
  run(retried, true);
  const compare = (env) => {
    const s = JSON.parse(JSON.stringify(env.hub.telemetrySummary(7)));
    const { batches, duplicate_events: duplicates, ...totals } = s.totals;
    for (const key of ['generated', 'stored', 'recent_sessions', 'live', 'rejected_batches']) delete s[key];
    return { summary: { ...s, totals }, duplicates };
  };
  const a = compare(clean);
  const b = compare(retried);
  assert.deepEqual(b.summary, a.summary);
  assert.equal(a.duplicates, 0);
  assert.ok(b.duplicates > 0);
  assert.equal(a.summary.totals.client_events, 7 * 2 + 2 + 2);
  assert.equal(a.summary.transport.pairs, 2);
  assert.equal(a.summary.server.parties_started, 1);
});

test('slot 0 rows are lobby rows: their events count, they never count as a party session', () => {
  const env = makeHub();
  const a = greeted(env, 1);
  const b = greeted(env, 2);
  const lobby = { diag: LOBBY, ctx: context({ slot: 0, party: 0, host: false }),
    events: [ev(1, 'invite_received'), ev(2, 'join_started'), ev(3, 'join_failed', { reason: 'not_found' }), ev(4, 'worker_reconnect')],
    session: session({ ct_usable: false }), peers: [peer({ transport: 'relay' })] };
  const reply = send(a, lobby);
  assert.deepEqual([reply.code, reply.accepted, reply.duplicates], ['ok', 4, 0]);
  send(b, batch([ev(1, 'direct_attempt')], { ctx: context({ slot: 2 }) }));
  const s = env.hub.telemetrySummary(1);
  assert.deepEqual([s.totals.sessions, s.totals.lobby_sessions, s.totals.client_events], [1, 1, 5]);
  assert.deepEqual(values(s.sessions_by.platform), { Windows: 1 });
  assert.equal(s.transport.pairs, 1);
  assert.equal(s.transport.direct, 1);
  assert.equal(s.latency.all.rtt_ms.n, 1);
  assert.equal(s.connection_tests.reports, 1);
  assert.equal(s.connection_tests.usable, 1);
  assert.deepEqual(values(s.client_events), { invite_received: 1, join_started: 1, join_failed: 1, worker_reconnect: 1, direct_attempt: 1 });
  assert.deepEqual(s.reasons.service, { not_found: 1 });
  const row = env.hub.telemetrySession(LOBBY).reporters[0];
  assert.deepEqual([row.slot, row.ctx.party, row.ctx.host], [0, 0, false]);
  assert.equal(env.hub.telemetrySession(LOBBY).events.every((e) => e.slot === 0), true);
  assert.ok(s.recent_sessions.some((r) => r.diag === LOBBY));
});

test('slot validation stays strict: 1 to 4 with a party of 1 to 4, or 0 with party 0', () => {
  const env = makeHub();
  const a = greeted(env, 1);
  const code = (ctx) => {
    env.advance(30000);
    return send(a, batch([], { ctx })).code;
  };
  assert.equal(code(context({ slot: 0, party: 0 })), 'ok');
  assert.equal(code({ ...context({ slot: 0 }), party: undefined }), 'ok');
  assert.equal(code(context({ slot: 0, party: 0, host: true })), 'ok');
  assert.equal(env.hub.telemetrySession(DIAG).reporters.find((r) => r.slot === 0).ctx.host, false);
  for (const bad of [{ slot: 0, party: 1 }, { slot: 0, party: 3 }, { slot: 1, party: 0 }, { slot: 5, party: 3 }, { slot: -1, party: 3 },
    { slot: 1.5, party: 3 }, { slot: '0', party: 0 }, { slot: 2, party: 5 }, { slot: 0, party: 5 }]) {
    assert.equal(code(context(bad)), 'bad_request', JSON.stringify(bad));
  }
  assert.equal(code(context({ slot: 4, party: 4 })), 'ok');
});

test('party_reached_4 is recorded once per party, and the disconnects are derived from the Worker endings', () => {
  const env = makeHub();
  const party = buildParty(env, 4, { connect: false });
  const [host, two, three, four] = party.clients;
  assert.equal(env.hub.telemetrySummary(1).server.parties_reached_4, 1);
  env.advance(60000);
  assert.equal(host.call('party.kick', { player: four.who.player }).code, 'ok');
  const again = greeted(env, 900, { ip: '198.51.100.20' });
  befriend(host, again);
  env.advance(60000);
  const sent = host.call('invite.send', { player: again.who.player });
  assert.equal(again.call('invite.accept', { invite: sent.invite }).code, 'ok');
  assert.equal(two.call('party.leave').code, 'ok');
  three.drop();
  env.advance(LIMITS.graceMs + 1000);
  host.call('party.leave');
  const s = env.hub.telemetrySession(party.diag).events.filter((e) => e.name === 'party_reached_4');
  assert.equal(s.length, 1);
  assert.deepEqual(s[0].fields, {});
  const server = env.hub.telemetrySummary(1).server;
  assert.deepEqual([server.parties_started, server.parties_reached_4, server.host_disconnects], [1, 1, 1]);
  assert.equal(server.guest_disconnects, 3);
  assert.deepEqual(values(server.member_left), { expired: 1, kicked: 1, left: 1 });
});
