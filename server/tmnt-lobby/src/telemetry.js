// Operational telemetry: what clients report about their sessions (`telemetry.batch`) and what the Worker itself sees
// of a party, kept in bounded SQL tables and read back as aggregates by the private admin routes (admin.js).
// Nothing here names a player: a session row is keyed by the party's random `diag`, the reporting slot and a salted hash
// of the reporter that is never shown. Values are re-validated like the client does (SanitizeTelemetryValue), whatever
// the client claims to have done.
import { MAX_PARTY, fail, isObject } from './protocol.js';
import { REASON_CATEGORIES, REASON_GROUP, isReason, normalizeReason, reasonCategory } from './reasons.js';

const DAY_MS = 24 * 3600 * 1000;

export const TELEMETRY_LIMITS = {
  maxEvents: 200,
  maxPeers: 8,
  maxFields: 8,
  valueChars: 48,
  keyChars: 24,
  scanChars: 4096,
  ringEvents: 5000,
  ringMs: 7 * DAY_MS,
  sessionRows: 2000,
  sessionMs: 7 * DAY_MS,
  rowsPerDiag: 8,
  counterDays: 35,
  keysPerDay: 512,
  recentSessions: 50,
  eventsPerSession: 1000,
  maxDays: 30,
  defaultDays: 7,
  pruneMs: 5 * 60 * 1000,
  rateFlushMs: 10 * 1000,
  latencySamples: 1000,
};

// What only a client can see. What the Worker records itself (SERVER_EVENTS) is never taken from a client.
export const CLIENT_EVENTS = new Set([
  'invite_received', 'invite_failed', 'join_started', 'join_completed', 'join_failed', 'game_start_attempt',
  'game_start_success', 'game_start_failure', 'worker_reconnect', 'direct_attempt', 'direct_result', 'relay_fallback',
  'relay_result', 'transport_selected', 'peer_disconnect', 'session_end',
]);

export const SERVER_EVENTS = new Set([
  'party_created', 'party_reached_4', 'party_ended', 'member_joined', 'member_left', 'invite_sent', 'invite_accepted',
  'invite_declined', 'invite_expired', 'invite_ended', 'party_full', 'rate_limited', 'room_joined', 'room_left',
  'room_peer_lost', 'room_transport', 'relay_closed', 'room_ended',
]);

// Counted but not kept in the event ring: a client in a loop must not be able to churn it.
const COUNT_ONLY = new Set(['rate_limited']);
const DIMENSION_FIELDS = ['reason', 'result', 'transport', 'code', 'scope'];
const REASON_FIELDS = new Set(['reason', 'fallback']);
const MAX_EVENT_NUMBER = 0xffffffff;
const DIAG = /^[0-9a-f]{12}$/;
const REDACTED = 'redacted';
const SESSION_FLAGS = ['ct_usable', 'worker', 'stun', 'direct', 'relay'];

export const isDiag = (value) => typeof value === 'string' && DIAG.test(value);

// ---- the client's value sanitizer, ported ----
const isDigit = (c) => c >= '0' && c <= '9';
const isAlnum = (c) => isDigit(c) || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z');
const isUpperAlnum = (c) => isDigit(c) || (c >= 'A' && c <= 'Z');
const isHex = (c) => isDigit(c) || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
const isAllowed = (c) => isAlnum(c) || c === '_' || c === '.' || c === ':' || c === '-';

function hasFriendCode(s) {
  for (let i = 0; i + 9 <= s.length; i++) {
    if (s[i + 4] !== '-') continue;
    let ok = true;
    for (let k = 0; k < 4 && ok; k++) ok = isUpperAlnum(s[i + k]) && isUpperAlnum(s[i + 5 + k]);
    if (ok) return true;
  }
  return false;
}

function hasLongHex(s) {
  let run = 0;
  for (const c of s) {
    if (isHex(c)) {
      if (++run >= 32) return true;
    } else if (c !== '-') {
      run = 0;
    }
  }
  return false;
}

function hasIpv4(s) {
  let i = 0;
  while (i < s.length) {
    if (!isDigit(s[i]) && s[i] !== '.') {
      i++;
      continue;
    }
    let j = i;
    while (j < s.length && (isDigit(s[j]) || s[j] === '.')) j++;
    let groups = 0;
    let digits = 0;
    for (let k = i; k <= j; k++) {
      if (k === j || s[k] === '.') {
        groups = digits >= 1 && digits <= 3 ? groups + 1 : 0;
        digits = 0;
        if (groups >= 4) return true;
      } else {
        digits++;
      }
    }
    i = j;
  }
  return false;
}

function hasIpv6(s) {
  let i = 0;
  while (i < s.length) {
    if (!isHex(s[i]) && s[i] !== ':' && s[i] !== '.') {
      i++;
      continue;
    }
    let j = i;
    let colons = 0;
    while (j < s.length && (isHex(s[j]) || s[j] === ':' || s[j] === '.')) colons += s[j++] === ':' ? 1 : 0;
    if (colons >= 2) return true;
    i = j;
  }
  return false;
}

function hasUrl(s) {
  const lower = s.toLowerCase();
  return lower.includes('://') || lower.includes('www.') || lower.includes('mailto:');
}

const forbidden = (s) => hasFriendCode(s) || hasLongHex(s) || hasIpv4(s) || hasIpv6(s) || hasUrl(s);

// Keeps [A-Za-z0-9_.:-] up to 48 characters; a friend code, a 32+ hex token, an address, a URL, a path, an e-mail or
// free text becomes "redacted" instead of being cleaned up.
export function sanitizeValue(value) {
  const text = String(value).slice(0, TELEMETRY_LIMITS.scanChars);
  for (const c of text) {
    const code = c.codePointAt(0);
    if (c === '/' || c === '\\' || c === '@' || code <= 0x20 || code === 0x7f) return REDACTED;
  }
  if (forbidden(text)) return REDACTED;
  let kept = '';
  for (const c of text) if (isAllowed(c)) kept += c;
  if (forbidden(kept)) return REDACTED;
  return kept.slice(0, TELEMETRY_LIMITS.valueChars);
}

// ---- validation ----
function number(value, low, high, what) {
  if (typeof value !== 'number' || !Number.isFinite(value)) fail('bad_request', what);
  return Math.min(high, Math.max(low, value));
}

const integer = (value, low, high, what) => Math.trunc(number(value, low, high, what));

function exact(value, low, high, what) {
  if (!Number.isInteger(value) || value < low || value > high) fail('bad_request', what);
  return value;
}
const optional = (value, fallback, read) => (value === undefined ? fallback : read(value));

class Tally {
  constructor() {
    this.redacted = 0;
    this.unknownReasons = 0;
  }

  // A code of the shared list, else "other"; empty stays empty (no reason given).
  reason(clean) {
    if (clean === '') return '';
    if (isReason(clean)) return clean;
    this.unknownReasons++;
    return normalizeReason(clean);
  }

  text(value, what) {
    if (value === undefined) return '';
    if (typeof value !== 'string') fail('bad_request', what);
    const clean = sanitizeValue(value);
    if (clean === REDACTED && value !== REDACTED) this.redacted++;
    return clean;
  }

  // A field value: strings and numbers; anything else is dropped.
  scalar(value) {
    if (typeof value === 'number' && Number.isFinite(value)) return sanitizeValue(String(value));
    if (typeof value === 'boolean') return String(value);
    if (typeof value !== 'string') return null;
    const clean = sanitizeValue(value);
    if (clean === REDACTED && value !== REDACTED) this.redacted++;
    return clean;
  }

  fields(source) {
    const out = {};
    if (source === undefined) return out;
    if (!isObject(source)) fail('bad_request', 'fields');
    let kept = 0;
    for (const [key, value] of Object.entries(source)) {
      if (kept >= TELEMETRY_LIMITS.maxFields) break;
      const name = sanitizeValue(key).slice(0, TELEMETRY_LIMITS.keyChars);
      let clean = this.scalar(value);
      if (!name || name === REDACTED || clean === null || Object.hasOwn(out, name)) continue;
      if (REASON_FIELDS.has(name)) clean = this.reason(clean);
      out[name] = clean;
      kept++;
    }
    return out;
  }
}

function parseContext(ctx, tally) {
  if (!isObject(ctx)) fail('bad_request', 'ctx expected');
  const slot = exact(ctx.slot, 0, MAX_PARTY, 'slot expected');
  const party = optional(ctx.party, slot === 0 ? 0 : 1, (v) => exact(v, slot === 0 ? 0 : 1, slot === 0 ? 0 : MAX_PARTY, 'party'));
  return {
    game: tally.text(ctx.game, 'game'), proto: optional(ctx.proto, 0, (v) => exact(v, 0, 1000, 'proto')),
    version: tally.text(ctx.version, 'version'), platform: tally.text(ctx.platform, 'platform'),
    os: tally.text(ctx.os, 'os'), arch: tally.text(ctx.arch, 'arch'), fp: tally.text(ctx.fp, 'fp'), slot,
    host: slot > 0 && ctx.host === true, party,
  };
}

function parseSession(session, tally) {
  if (!isObject(session)) fail('bad_request', 'session');
  const out = {
    dur_ms: optional(session.dur_ms, 0, (v) => integer(v, 0, 7 * DAY_MS, 'dur_ms')),
    compat: tally.text(session.compat, 'compat'), reason: tally.reason(tally.text(session.reason, 'reason')),
  };
  for (const flag of SESSION_FLAGS) out[flag] = session[flag] === true;
  return out;
}

function parsePing(ping) {
  if (ping === undefined) ping = {};
  if (!isObject(ping)) fail('bad_request', 'ping');
  const count = (v) => optional(v, 0, (x) => integer(x, 0, 1e12, 'ping'));
  const signed = (v) => optional(v, -1, (x) => integer(x, -1, 1e9, 'ping'));
  const real = (v) => optional(v, 0, (x) => Math.round(number(x, 0, 1e6, 'ping') * 1000) / 1000);
  return {
    sent: count(ping.sent), recv: count(ping.recv), last: signed(ping.last), min: signed(ping.min), max: signed(ping.max),
    avg: real(ping.avg), jit: real(ping.jit), to: count(ping.to), ooo: count(ping.ooo), dup: count(ping.dup),
    loss: optional(ping.loss, 0, (x) => Math.round(number(x, 0, 1, 'ping') * 1000) / 1000),
  };
}

function parsePeers(peers, tally) {
  if (!Array.isArray(peers)) fail('bad_request', 'peers');
  if (peers.length > TELEMETRY_LIMITS.maxPeers) fail('too_large', 'peers');
  const seen = new Set();
  const out = [];
  for (const peer of peers) {
    if (!isObject(peer)) fail('bad_request', 'peer');
    const ls = exact(peer.ls, 1, MAX_PARTY, 'ls');
    const rs = exact(peer.rs, 1, MAX_PARTY, 'rs');
    if (seen.has(`${ls}-${rs}`)) continue;
    seen.add(`${ls}-${rs}`);
    out.push({
      ls, rs, state: tally.text(peer.state, 'state'), transport: tally.text(peer.transport, 'transport'),
      direct: tally.text(peer.direct, 'direct'), relay: tally.text(peer.relay, 'relay'),
      fallback: tally.reason(tally.text(peer.fallback, 'fallback')), conn_ms: optional(peer.conn_ms, 0, (v) => integer(v, 0, 7 * DAY_MS, 'conn_ms')),
      est_ms: optional(peer.est_ms, -1, (v) => integer(v, -1, 7 * DAY_MS, 'est_ms')), ping: parsePing(peer.ping),
      tx: optional(peer.tx, 0, (v) => integer(v, 0, 1e15, 'tx')), rx: optional(peer.rx, 0, (v) => integer(v, 0, 1e15, 'rx')),
      reconn: optional(peer.reconn, 0, (v) => integer(v, 0, 1e9, 'reconn')), reason: tally.reason(tally.text(peer.reason, 'reason')),
    });
  }
  return out;
}

// The checked content of one `telemetry.batch`, or a ProtocolError (`bad_request`, `too_large`).
export function parseBatch(msg) {
  if (!isDiag(msg.diag)) fail('bad_request', 'diag expected');
  const tally = new Tally();
  const ctx = parseContext(msg.ctx, tally);
  const session = msg.session === undefined ? null : parseSession(msg.session, tally);
  const peers = msg.peers === undefined ? null : parsePeers(msg.peers, tally);
  const snapshotUnknown = tally.unknownReasons;
  const source = msg.events ?? [];
  if (!Array.isArray(source)) fail('bad_request', 'events');
  if (source.length > TELEMETRY_LIMITS.maxEvents) fail('too_large', 'events');
  const events = [];
  const skipped = [];
  for (const event of source) {
    if (!isObject(event)) fail('bad_request', 'event');
    const n = Number.isInteger(event.n) && event.n >= 1 && event.n <= MAX_EVENT_NUMBER ? event.n : undefined;
    if (typeof event.e !== 'string' || !CLIENT_EVENTS.has(event.e)) {
      skipped.push(n);
      continue;
    }
    const before = tally.unknownReasons;
    const t = optional(event.t, 0, (v) => Math.round(number(v, 0, 7 * 24 * 3600, 't') * 100) / 100);
    const fields = tally.fields(event.f);
    events.push({ name: event.e, n, t, fields, unknown: tally.unknownReasons - before });
  }
  const dropped = optional(msg.dropped, 0, (v) => integer(v, 0, 1e9, 'dropped'));
  return { diag: msg.diag, ctx, session, peers, events, skipped, ignored: skipped.length, dropped, redacted: tally.redacted,
    snapshotUnknown };
}

// ---- statistics ----
export function percentile(sorted, p) {
  if (!sorted.length) return null;
  const rank = Math.ceil((p / 100) * sorted.length);
  return sorted[Math.min(sorted.length - 1, Math.max(0, rank - 1))];
}

export function distribution(values, decimals = 1) {
  const sorted = values.filter(Number.isFinite).sort((a, b) => a - b);
  const scale = 10 ** decimals;
  const round = (v) => (v === null ? null : Math.round(v * scale) / scale);
  return {
    n: sorted.length, min: round(sorted[0] ?? null), p50: round(percentile(sorted, 50)), p90: round(percentile(sorted, 90)),
    p95: round(percentile(sorted, 95)), p99: round(percentile(sorted, 99)), max: round(sorted.at(-1) ?? null),
  };
}

const REASON_COUNTER = /^(?:client\.[a-z0-9_]+|server\.(?:party_ended|member_left|invite_ended|relay_closed))\.reason\.(.+)$/;
const INVITE_OUTCOMES = { 'server.invite_accepted': 'accepted', 'server.invite_declined': 'declined', 'server.invite_expired': 'expired' };
const GUEST_DISCONNECTS = ['disconnect', 'left', 'kicked', 'expired'];

const utcDay = (ms) => new Date(ms).toISOString().slice(0, 10);

const SCHEMA = [
  `CREATE TABLE IF NOT EXISTS tel_events (id INTEGER PRIMARY KEY, ms INTEGER NOT NULL, diag TEXT NOT NULL,
     src TEXT NOT NULL, name TEXT NOT NULL, slot INTEGER NOT NULL DEFAULT 0, t REAL NOT NULL DEFAULT 0,
     fields TEXT NOT NULL DEFAULT '{}')`,
  `CREATE INDEX IF NOT EXISTS tel_events_diag ON tel_events (diag, id)`,
  `CREATE INDEX IF NOT EXISTS tel_events_ms ON tel_events (ms)`,
  `CREATE TABLE IF NOT EXISTS tel_sessions (diag TEXT NOT NULL, slot INTEGER NOT NULL, who TEXT NOT NULL,
     first_ms INTEGER NOT NULL, last_ms INTEGER NOT NULL, batches INTEGER NOT NULL DEFAULT 0,
     events INTEGER NOT NULL DEFAULT 0, dropped INTEGER NOT NULL DEFAULT 0, ctx TEXT NOT NULL,
     report TEXT NOT NULL DEFAULT '', peers TEXT NOT NULL DEFAULT '', last_n INTEGER NOT NULL DEFAULT 0,
     PRIMARY KEY (diag, slot, who))`,
  `CREATE INDEX IF NOT EXISTS tel_sessions_last ON tel_sessions (last_ms)`,
  `CREATE TABLE IF NOT EXISTS tel_counters (day TEXT NOT NULL, k TEXT NOT NULL, n INTEGER NOT NULL,
     PRIMARY KEY (day, k))`,
  `CREATE TABLE IF NOT EXISTS tel_latency (id INTEGER PRIMARY KEY, ms INTEGER NOT NULL, v INTEGER NOT NULL)`,
];

const parseJson = (text, fallback) => {
  try {
    return text ? JSON.parse(text) : fallback;
  } catch {
    return fallback;
  }
};

export class TelemetryStore {
  // sql: { all, get, run } as the hub's; now(): ms; log(text)
  constructor(sql, now, log = () => {}) {
    this.sql = sql;
    this.now = now;
    this.log = log;
    this.lastPrune = 0;
    this.rates = new Map();
    this.lastRateFlush = 0;
    for (const statement of SCHEMA) sql.run(statement);
    // CREATE TABLE IF NOT EXISTS leaves an older tel_sessions without the column.
    if (!sql.all('PRAGMA table_info(tel_sessions)').some((c) => c.name === 'last_n')) {
      sql.run('ALTER TABLE tel_sessions ADD COLUMN last_n INTEGER NOT NULL DEFAULT 0');
    }
  }

  // ---- counters ----
  addCounts(counts) {
    const day = utcDay(this.now());
    for (const [key, n] of counts) {
      let name = key;
      const known = this.sql.get('SELECT 1 AS x FROM tel_counters WHERE day = ? AND k = ?', day, name);
      if (!known && this.sql.get('SELECT COUNT(*) AS n FROM tel_counters WHERE day = ?', day).n >= TELEMETRY_LIMITS.keysPerDay) {
        name = 'overflow';
      }
      this.sql.run(`INSERT INTO tel_counters (day, k, n) VALUES (?, ?, ?) ON CONFLICT(day, k) DO UPDATE SET n = n + excluded.n`,
        day, name, n);
    }
  }

  static tally(counts, key, n = 1) {
    counts.set(key, (counts.get(key) ?? 0) + n);
  }

  static countEvent(counts, source, name, fields) {
    TelemetryStore.tally(counts, `${source}.${name}`);
    for (const field of DIMENSION_FIELDS) {
      const value = fields[field];
      if (value) TelemetryStore.tally(counts, `${source}.${name}.${field}.${value}`);
    }
  }

  // ---- retention ----
  prune(now = this.now()) {
    if (now - this.lastPrune < TELEMETRY_LIMITS.pruneMs) return;
    this.lastPrune = now;
    this.sql.run('DELETE FROM tel_events WHERE ms < ?', now - TELEMETRY_LIMITS.ringMs);
    this.sql.run('DELETE FROM tel_latency WHERE ms < ?', now - TELEMETRY_LIMITS.ringMs);
    this.sql.run('DELETE FROM tel_sessions WHERE last_ms < ?', now - TELEMETRY_LIMITS.sessionMs);
    this.sql.run('DELETE FROM tel_counters WHERE day < ?', utcDay(now - TELEMETRY_LIMITS.counterDays * DAY_MS));
  }

  trimEvents() {
    this.sql.run('DELETE FROM tel_events WHERE id <= (SELECT MAX(id) FROM tel_events) - ?', TELEMETRY_LIMITS.ringEvents);
  }

  trimSessions() {
    const rows = this.sql.get('SELECT COUNT(*) AS n FROM tel_sessions').n;
    if (rows <= TELEMETRY_LIMITS.sessionRows) return;
    this.sql.run(`DELETE FROM tel_sessions WHERE rowid IN
      (SELECT rowid FROM tel_sessions ORDER BY last_ms LIMIT ?)`, rows - TELEMETRY_LIMITS.sessionRows);
  }

  insertEvent(ms, diag, source, name, slot, t, fields) {
    this.sql.run('INSERT INTO tel_events (ms, diag, src, name, slot, t, fields) VALUES (?, ?, ?, ?, ?, ?, ?)', ms, diag, source,
      name, slot, t, JSON.stringify(fields));
  }

  // ---- writes ----
  // One checked batch from the player `who` (an opaque key).
  ingest(who, batch) {
    const now = this.now();
    this.prune(now);
    const { diag, ctx } = batch;
    const mine = this.sql.get('SELECT last_n FROM tel_sessions WHERE diag = ? AND slot = ? AND who = ?', diag, ctx.slot, who);
    const counts = new Map();
    if (!mine) {
      const rows = this.sql.get('SELECT COUNT(*) AS n FROM tel_sessions WHERE diag = ?', diag).n;
      if (rows >= TELEMETRY_LIMITS.rowsPerDiag) fail('limit', 'too many reporters');
      if (ctx.slot === 0) {
        TelemetryStore.tally(counts, 'lobby_sessions');
      } else {
        TelemetryStore.tally(counts, 'sessions');
        for (const [dimension, value] of [['platform', ctx.platform], ['version', ctx.version], ['os', ctx.os],
          ['proto', String(ctx.proto)]]) {
          TelemetryStore.tally(counts, `sessions.${dimension}.${value || 'unknown'}`);
        }
      }
    }

    // An event numbered at or below the reporter's high-water mark was seen already (a retried batch).
    const known = mine?.last_n ?? 0;
    const seen = new Set();
    let high = known;
    const fresh = [];
    let duplicates = 0;
    let ignored = 0;
    const isNew = (n) => {
      if (n === undefined) return true;
      if (n <= known || seen.has(n)) return false;
      seen.add(n);
      high = Math.max(high, n);
      return true;
    };
    for (const event of batch.events) {
      if (isNew(event.n)) fresh.push(event);
      else duplicates++;
    }
    for (const n of batch.skipped) {
      if (isNew(n)) ignored++;
      else duplicates++;
    }
    const replay = duplicates > 0 && !fresh.length && !ignored;
    const dropped = replay ? 0 : batch.dropped;

    this.sql.run(`INSERT INTO tel_sessions (diag, slot, who, first_ms, last_ms, batches, events, dropped, ctx, report, peers, last_n)
      VALUES (?, ?, ?, ?, ?, 1, ?, ?, ?, ?, ?, ?)
      ON CONFLICT(diag, slot, who) DO UPDATE SET last_ms = excluded.last_ms, batches = batches + 1,
      events = events + excluded.events, dropped = dropped + excluded.dropped, ctx = excluded.ctx,
      report = CASE WHEN excluded.report = '' THEN report ELSE excluded.report END,
      peers = CASE WHEN excluded.peers = '' THEN peers ELSE excluded.peers END,
      last_n = MAX(last_n, excluded.last_n)`, diag, ctx.slot, who, now, now, fresh.length, dropped, JSON.stringify(ctx),
    batch.session ? JSON.stringify(batch.session) : '', batch.peers ? JSON.stringify(batch.peers) : '', high);
    if (!mine) this.trimSessions();
    let unknown = mine ? 0 : batch.snapshotUnknown;
    for (const event of fresh) {
      this.insertEvent(now, diag, 'client', event.name, ctx.slot, event.t, event.fields);
      TelemetryStore.countEvent(counts, 'client', event.name, event.fields);
      unknown += event.unknown;
    }
    if (fresh.length) this.trimEvents();
    TelemetryStore.tally(counts, 'tel.batches');
    TelemetryStore.tally(counts, 'tel.events', fresh.length);
    if (ignored) TelemetryStore.tally(counts, 'tel.ignored', ignored);
    if (duplicates) TelemetryStore.tally(counts, 'duplicate_events', duplicates);
    if (unknown) TelemetryStore.tally(counts, 'unknown_reason', unknown);
    if (batch.redacted) TelemetryStore.tally(counts, 'tel.redacted', batch.redacted);
    if (dropped) TelemetryStore.tally(counts, 'tel.client_dropped', dropped);
    this.addCounts(counts);
    return { accepted: fresh.length, ignored, duplicates };
  }

  reject(code) {
    this.addCounts(new Map([[`tel.rejected.${sanitizeValue(code)}`, 1]]));
  }

  // One event the Worker itself saw. `diag` correlates it with the clients' reports of the same party.
  record(event, fields = {}, diag = '') {
    if (!SERVER_EVENTS.has(event) || (diag && !isDiag(diag))) return false;
    const tally = new Tally();
    const clean = tally.fields(isObject(fields) ? fields : {});
    const counts = new Map();
    TelemetryStore.countEvent(counts, 'server', event, clean);
    if (tally.unknownReasons) TelemetryStore.tally(counts, 'unknown_reason', tally.unknownReasons);
    this.addCounts(counts);
    if (!COUNT_ONLY.has(event)) {
      const now = this.now();
      this.prune(now);
      this.insertEvent(now, diag, 'server', event, 0, 0, clean);
      this.trimEvents();
    }
    return true;
  }

  // One Connection Test outcome from beacon.js, as daily counters and one latency sample; nothing identifies the sender.
  recordConnectionTest(beacon) {
    const counts = new Map();
    const tally = (key) => TelemetryStore.tally(counts, `ct.${key}`);
    tally('total');
    tally(beacon.usable ? 'usable' : 'unusable');
    if (beacon.fail) tally(`fail.${beacon.fail}`);
    for (const dimension of ['worker', 'ws', 'stun', 'router']) tally(`${dimension}.${beacon[dimension]}`);
    tally(`platform.${beacon.plat}`);
    tally(`version.${sanitizeValue(beacon.ver) || 'unknown'}`);
    if (beacon.direct) tally('direct');
    if (beacon.relay) tally('relay');
    const now = this.now();
    this.prune(now);
    if (beacon.ms !== null) {
      this.sql.run('INSERT INTO tel_latency (ms, v) VALUES (?, ?)', now, beacon.ms);
      this.sql.run('DELETE FROM tel_latency WHERE id <= (SELECT MAX(id) FROM tel_latency) - ?', TELEMETRY_LIMITS.latencySamples);
    }
    this.addCounts(counts);
  }

  // A rate limit hit is only counted in memory and written in groups, so a flood adds no write per message.
  noteLimit(scope) {
    this.rates.set(scope, (this.rates.get(scope) ?? 0) + 1);
    if (this.now() - this.lastRateFlush >= TELEMETRY_LIMITS.rateFlushMs) this.flushLimits();
  }

  flushLimits() {
    this.lastRateFlush = this.now();
    if (!this.rates.size) return;
    const counts = new Map();
    for (const [scope, n] of this.rates) {
      TelemetryStore.tally(counts, 'server.rate_limited', n);
      TelemetryStore.tally(counts, `server.rate_limited.scope.${sanitizeValue(scope)}`, n);
    }
    this.rates.clear();
    this.addCounts(counts);
  }

  // ---- reads ----
  summary(days = TELEMETRY_LIMITS.defaultDays) {
    if (!Number.isInteger(days) || days < 1 || days > TELEMETRY_LIMITS.maxDays) throw new RangeError('days out of range');
    this.flushLimits();
    const now = this.now();
    this.prune(now);
    const first = utcDay(now - (days - 1) * DAY_MS);
    const counters = new Map(this.sql.all('SELECT k, SUM(n) AS n FROM tel_counters WHERE day >= ? GROUP BY k', first)
      .map((r) => [r.k, r.n]));
    const count = (key) => counters.get(key) ?? 0;
    const list = (prefix) => [...counters].filter(([k]) => k.startsWith(prefix) && k.length > prefix.length)
      .map(([k, n]) => ({ value: k.slice(prefix.length), count: n }))
      .sort((a, b) => b.count - a.count || a.value.localeCompare(b.value));
    const sinceMs = now - days * DAY_MS;
    const serverReasons = (event, codes) => codes.reduce((sum, code) => sum + count(`server.${event}.reason.${code}`), 0);
    const rows = this.sql.all('SELECT * FROM tel_sessions WHERE last_ms >= ? ORDER BY last_ms DESC', sinceMs)
      .map((r) => ({ ...r, ctx: parseJson(r.ctx, {}), report: parseJson(r.report, null), peers: parseJson(r.peers, []) }));
    // Rows of slot 0 are lobby rows (events before any party): they carry no party session, link or test report.
    const partyRows = rows.filter((r) => r.slot > 0);

    const pairs = new Map();
    for (const row of partyRows) {
      for (const peer of row.peers) {
        const key = `${row.diag}|${Math.min(peer.ls, peer.rs)}-${Math.max(peer.ls, peer.rs)}`;
        if (!pairs.has(key) || peer.ls < peer.rs) pairs.set(key, peer);
      }
    }
    const transport = { pairs: pairs.size, direct: 0, relay: 0, failed: 0, pending: 0, other: 0 };
    for (const peer of pairs.values()) {
      transport[['direct', 'relay', 'failed', 'pending'].includes(peer.transport) ? peer.transport : 'other']++;
    }

    const quality = { rtt_ms: [], jitter_ms: [], loss: [] };
    const byTransport = { direct: { rtt_ms: [], jitter_ms: [], loss: [] }, relay: { rtt_ms: [], jitter_ms: [], loss: [] } };
    for (const row of partyRows) {
      for (const peer of row.peers) {
        const targets = [quality, byTransport[peer.transport]].filter(Boolean);
        for (const target of targets) {
          if (peer.ping.recv > 0) {
            target.rtt_ms.push(peer.ping.avg);
            target.jitter_ms.push(peer.ping.jit);
          }
          if (peer.ping.sent > 0) target.loss.push(peer.ping.loss);
        }
      }
    }
    const latency = (set) => ({ rtt_ms: distribution(set.rtt_ms), jitter_ms: distribution(set.jitter_ms),
      loss: distribution(set.loss, 3) });

    const reports = partyRows.filter((r) => r.report);
    const connection = { reports: reports.length };
    for (const [key, flag] of [['usable', 'ct_usable'], ['worker_reachable', 'worker'], ['stun_ok', 'stun'],
      ['direct_available', 'direct'], ['relay_available', 'relay']]) {
      connection[key] = reports.filter((r) => r.report[flag]).length;
    }
    const compat = new Map();
    for (const r of reports) compat.set(r.report.compat || 'unknown', (compat.get(r.report.compat || 'unknown') ?? 0) + 1);

    const diags = new Map();
    for (const row of rows) {
      const entry = diags.get(row.diag) ?? { diag: row.diag, reporters: 0, last_ms: 0, platforms: new Set() };
      entry.reporters++;
      entry.last_ms = Math.max(entry.last_ms, row.last_ms);
      entry.platforms.add(row.ctx.platform || 'unknown');
      diags.set(row.diag, entry);
    }
    const recent = this.sql.all(`SELECT diag, MIN(ms) AS first_ms, MAX(ms) AS last_ms, COUNT(*) AS events FROM tel_events
      WHERE diag != '' GROUP BY diag ORDER BY last_ms DESC LIMIT ?`, TELEMETRY_LIMITS.recentSessions);
    for (const r of recent) {
      const entry = diags.get(r.diag) ?? { diag: r.diag, reporters: 0, last_ms: 0, platforms: new Set() };
      entry.events = r.events;
      entry.first_ms = r.first_ms;
      entry.last_ms = Math.max(entry.last_ms, r.last_ms);
      diags.set(r.diag, entry);
    }
    const ended = new Map(this.sql.all(`SELECT diag, fields FROM tel_events WHERE src = 'server' AND name = 'party_ended'
      ORDER BY id`).map((r) => [r.diag, parseJson(r.fields, {}).reason ?? '']));
    const sessions = [...diags.values()].sort((a, b) => b.last_ms - a.last_ms).slice(0, TELEMETRY_LIMITS.recentSessions)
      .map((e) => ({ diag: e.diag, reporters: e.reporters, events: e.events ?? 0, first_ms: e.first_ms ?? null, last_ms: e.last_ms,
        platforms: [...e.platforms].sort(), ended: ended.get(e.diag) ?? '' }));

    const reasons = Object.fromEntries([...REASON_CATEGORIES.map((c) => [REASON_GROUP[c], {}]), ['other', 0]]);
    const countReason = (code, n) => {
      if (!isReason(code) || code === 'other') reasons.other += n;
      else reasons[REASON_GROUP[reasonCategory(code)]][code] = (reasons[REASON_GROUP[reasonCategory(code)]][code] ?? 0) + n;
    };
    for (const [key, n] of counters) {
      const found = REASON_COUNTER.exec(key);
      if (found) countReason(found[1], n);
      else if (Object.hasOwn(INVITE_OUTCOMES, key)) countReason(INVITE_OUTCOMES[key], n);
    }
    for (const group of REASON_CATEGORIES.map((c) => REASON_GROUP[c])) {
      reasons[group] = Object.fromEntries(Object.entries(reasons[group]).sort((a, b) => b[1] - a[1] || a[0].localeCompare(b[0])));
    }

    const testMs = this.sql.all('SELECT v FROM tel_latency WHERE ms >= ?', sinceMs).map((r) => r.v);
    const testTotal = count('ct.total');
    const connectionBeacon = {
      total: testTotal, usable: count('ct.usable'), unusable: count('ct.unusable'), direct_available: count('ct.direct'),
      relay_available: count('ct.relay'), failures: list('ct.fail.'), worker: list('ct.worker.'), ws: list('ct.ws.'),
      stun: list('ct.stun.'), router: list('ct.router.'), platform: list('ct.platform.'), version: list('ct.version.'),
      latency_ms: distribution(testMs, 0),
    };

    return {
      generated: now,
      days,
      since_day: first,
      time_zone: 'UTC',
      retention: { events: TELEMETRY_LIMITS.ringEvents, events_days: TELEMETRY_LIMITS.ringMs / DAY_MS,
        sessions: TELEMETRY_LIMITS.sessionRows, sessions_days: TELEMETRY_LIMITS.sessionMs / DAY_MS,
        counters_days: TELEMETRY_LIMITS.counterDays },
      stored: { events: this.sql.get('SELECT COUNT(*) AS n FROM tel_events').n,
        sessions: this.sql.get('SELECT COUNT(*) AS n FROM tel_sessions').n },
      totals: { parties_started: count('server.party_created'), sessions: count('sessions'), lobby_sessions: count('lobby_sessions'),
        batches: count('tel.batches'), client_events: count('tel.events'), ignored_events: count('tel.ignored'),
        duplicate_events: count('duplicate_events'), unknown_reasons: count('unknown_reason'), redacted_values: count('tel.redacted'),
        client_dropped: count('tel.client_dropped'), rejected_batches: list('tel.rejected.').reduce((a, b) => a + b.count, 0) },
      sessions_by: { platform: list('sessions.platform.'), version: list('sessions.version.'), os: list('sessions.os.'),
        proto: list('sessions.proto.') },
      transport,
      latency: { all: latency(quality), direct: latency(byTransport.direct), relay: latency(byTransport.relay) },
      direct_results: list('client.direct_result.result.'),
      relay_results: list('client.relay_result.result.'),
      selected: list('client.transport_selected.transport.'),
      fallback_reasons: list('client.relay_fallback.reason.'),
      direct_failures: list('client.direct_result.reason.'),
      reasons,
      disconnect_reasons: { peer: list('client.peer_disconnect.reason.'), session: list('client.session_end.reason.') },
      rejected_batches: list('tel.rejected.'),
      invites: {
        sent: count('server.invite_sent'), accepted: count('server.invite_accepted'), declined: count('server.invite_declined'),
        expired: count('server.invite_expired'), ended_other: list('server.invite_ended.reason.'),
        client_failed: list('client.invite_failed.reason.'), party_full: count('server.party_full'),
      },
      connection_beacon: connectionBeacon,
      connection_tests: { ...connection, compatibility: [...compat].map(([value, n]) => ({ value, count: n })),
      },
      server: {
        parties_started: count('server.party_created'), parties_reached_4: count('server.party_reached_4'),
        host_disconnects: serverReasons('party_ended', ['host_left']),
        guest_disconnects: serverReasons('member_left', GUEST_DISCONNECTS), party_ended: list('server.party_ended.reason.'),
        member_joined: count('server.member_joined'), member_left: list('server.member_left.reason.'),
        room_joined: count('server.room_joined'), room_left: list('server.room_left.reason.'),
        room_peer_lost: count('server.room_peer_lost'), room_ended: list('server.room_ended.reason.'),
        room_transport: list('server.room_transport.transport.'), relay_closed: list('server.relay_closed.reason.'),
        rate_limited: list('server.rate_limited.scope.'), fifth_member_rejected: count('server.party_full'),
      },
      client_events: [...counters].filter(([k]) => /^client\.[a-z0-9_]+$/.test(k))
        .map(([k, n]) => ({ value: k.slice(7), count: n })).sort((a, b) => b.count - a.count || a.value.localeCompare(b.value)),
      recent_sessions: sessions,
    };
  }

  // Everything kept about one party (`diag`), or null.
  session(diag) {
    if (!isDiag(diag)) throw new RangeError('diag expected');
    const rows = this.sql.all('SELECT * FROM tel_sessions WHERE diag = ? ORDER BY slot, who', diag);
    const events = this.sql.all(`SELECT id, ms, src, name, slot, t, fields FROM tel_events WHERE diag = ? ORDER BY id LIMIT ?`, diag,
      TELEMETRY_LIMITS.eventsPerSession);
    if (!rows.length && !events.length) return null;
    return {
      diag,
      reporters: rows.map((r) => ({ slot: r.slot, first_ms: r.first_ms, last_ms: r.last_ms, batches: r.batches, events: r.events,
        dropped: r.dropped, ctx: parseJson(r.ctx, {}), session: parseJson(r.report, null), peers: parseJson(r.peers, []) })),
      events: events.map((e) => ({ id: e.id, ms: e.ms, src: e.src, name: e.name, slot: e.slot, t: e.t, fields: parseJson(e.fields, {}) })),
    };
  }
}
