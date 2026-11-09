// Aggregate service metrics for the private dashboard (README.md, Service metrics): daily counters, daily counts per
// dimension, 15-minute usage samples and one keyed pseudonym per player per day for distinct-player counts. No row names a
// player, an account, an address or a party; live numbers come from the hub's sockets and tables.
import { MAX_PARTY } from './protocol.js';
import { PARTY_DIMENSIONS, partyCounts } from './party.js';

export const METRICS_TIME_ZONE = 'America/Sao_Paulo';
export const SAMPLE_MS = 15 * 60 * 1000;
export const SAMPLE_DAYS = 90;
export const HISTORY_MAX_DAYS = 365;
// The longest range plus the same length before it, which the period comparison reads.
export const PLAYER_RETENTION_DAYS = 2 * HISTORY_MAX_DAYS;
export const DAILY_RETENTION_DAYS = 3 * HISTORY_MAX_DAYS;
export const DIMENSIONS = ['platform', 'version', 'country', 'reject', 'abuse', 'occupancy', 'lifetime', 'incident_reject', ...PARTY_DIMENSIONS];
export const LIFETIMES = [['under_1m', 60e3], ['1_5m', 5 * 60e3], ['5_15m', 15 * 60e3], ['15_60m', 60 * 60e3], ['over_1h', Infinity]];
export const PLATFORMS = ['Windows', 'Linux', 'Steam Deck'];

const DAY_MS = 24 * 3600 * 1000;
// Bounds what a client can add to the table: further new versions in a day count as Other.
const VALUES_PER_DIMENSION_PER_DAY = 64;
const CLEANUP_ROWS = 500;
const INCIDENT_COLUMNS = ['incidents_filed', 'incidents_rejected', 'incident_status_checks', 'incident_reads',
  'incident_messages', 'incidents_deleted', 'incident_errors'];
const LOBBY_COLUMNS = ['lobby_list_requests', 'lobby_publishes', 'lobby_visible_peak'];
const COUNTERS = new Set(['sessions', 'rooms_created', 'room_joins', 'incompatible_rejects', ...INCIDENT_COLUMNS,
  'lobby_list_requests', 'lobby_publishes']);

const ROOM_COLUMNS = ['rooms_closed', 'rooms_with_guest', 'rooms_relayed', 'party_total'];
const DAY_COLUMNS = ['sessions', 'rooms_created', 'room_joins', 'incompatible_rejects', 'peak_online', 'peak_in_rooms',
  'peak_rooms', ...ROOM_COLUMNS, ...INCIDENT_COLUMNS, ...LOBBY_COLUMNS];
const SUMMED = ['sessions', 'rooms_created', 'room_joins', 'incompatible_rejects', ...ROOM_COLUMNS, ...INCIDENT_COLUMNS,
  'lobby_list_requests', 'lobby_publishes'];
const PEAKS = ['peak_online', 'peak_in_rooms', 'peak_rooms', 'lobby_visible_peak'];

export const METRICS_SCHEMA = [
  `CREATE TABLE IF NOT EXISTS metrics_daily (day TEXT PRIMARY KEY, sessions INTEGER NOT NULL DEFAULT 0,
     rooms_created INTEGER NOT NULL DEFAULT 0, room_joins INTEGER NOT NULL DEFAULT 0,
     incompatible_rejects INTEGER NOT NULL DEFAULT 0, peak_online INTEGER NOT NULL DEFAULT 0,
     peak_in_rooms INTEGER NOT NULL DEFAULT 0, peak_rooms INTEGER NOT NULL DEFAULT 0,
     rooms_closed INTEGER NOT NULL DEFAULT 0, rooms_with_guest INTEGER NOT NULL DEFAULT 0,
     rooms_relayed INTEGER NOT NULL DEFAULT 0, party_total INTEGER NOT NULL DEFAULT 0,
     incidents_filed INTEGER NOT NULL DEFAULT 0, incidents_rejected INTEGER NOT NULL DEFAULT 0,
     incident_status_checks INTEGER NOT NULL DEFAULT 0, incident_reads INTEGER NOT NULL DEFAULT 0,
     incident_messages INTEGER NOT NULL DEFAULT 0, incidents_deleted INTEGER NOT NULL DEFAULT 0,
     incident_errors INTEGER NOT NULL DEFAULT 0,
     lobby_list_requests INTEGER NOT NULL DEFAULT 0, lobby_publishes INTEGER NOT NULL DEFAULT 0,
     lobby_visible_peak INTEGER NOT NULL DEFAULT 0)`,
  `CREATE TABLE IF NOT EXISTS metrics_daily_dimensions (day TEXT NOT NULL, dimension TEXT NOT NULL, value TEXT NOT NULL,
     count INTEGER NOT NULL, PRIMARY KEY (day, dimension, value))`,
  `CREATE TABLE IF NOT EXISTS metrics_samples (bucket INTEGER PRIMARY KEY, online INTEGER NOT NULL,
     in_rooms INTEGER NOT NULL, rooms INTEGER NOT NULL)`,
  `CREATE TABLE IF NOT EXISTS metrics_players (day TEXT NOT NULL, player TEXT NOT NULL, PRIMARY KEY (day, player))`,
];

const dayFormat = new Intl.DateTimeFormat('en-CA', { timeZone: METRICS_TIME_ZONE, year: 'numeric', month: '2-digit',
  day: '2-digit' });

export function metricsDay(ms) {
  const parts = Object.fromEntries(dayFormat.formatToParts(ms).map((p) => [p.type, p.value]));
  return `${parts.year}-${parts.month}-${parts.day}`;
}

function daysBefore(day, n) {
  const [y, m, d] = day.split('-').map(Number);
  return new Date(Date.UTC(y, m - 1, d - n)).toISOString().slice(0, 10);
}

export const sampleBucket = (ms) => ms - (ms % SAMPLE_MS);

export const platformCategory = (text) => (PLATFORMS.includes(text) ? text : 'Other');

export function versionCategory(text) {
  if (!text) return 'Unknown';
  return /^\d{1,4}(\.\d{1,4}){1,3}$/.test(text) ? text : 'Other';
}

// Cloudflare's country code, or Unknown (XX is Cloudflare's own unknown, T1 is Tor).
export function countryCode(text) {
  return typeof text === 'string' && /^[A-Z]{2}$/.test(text) && text !== 'XX' ? text : 'Unknown';
}

const sorted = (counts) => [...counts].map(([value, count]) => ({ value, count }))
  .sort((a, b) => b.count - a.count || a.value.localeCompare(b.value));

export class Metrics {
  constructor(sql, now) {
    this.sql = sql;
    this.now = now;
    for (const statement of METRICS_SCHEMA) sql.run(statement);
    const have = new Set(sql.all('PRAGMA table_info(metrics_daily)').map((c) => c.name));
    for (const column of [...ROOM_COLUMNS, ...INCIDENT_COLUMNS, ...LOBBY_COLUMNS]) {
      if (!have.has(column)) sql.run(`ALTER TABLE metrics_daily ADD COLUMN ${column} INTEGER NOT NULL DEFAULT 0`);
    }
    this.peaks = null;
    this.sample = null;
    this.lobbyPeak = null;
  }

  incidentFiled() {
    this.count('incidents_filed');
  }

  incidentRejected(reason) {
    this.count('incidents_rejected');
    if (reason) this.addDimension(metricsDay(this.now()), 'incident_reject', String(reason).slice(0, 32));
  }

  incidentStatusCheck() {
    this.count('incident_status_checks');
  }

  incidentRead() {
    this.count('incident_reads');
  }

  incidentMessage() {
    this.count('incident_messages');
  }

  incidentDeleted() {
    this.count('incidents_deleted');
  }

  incidentError() {
    this.count('incident_errors');
  }

  lobbyListRequest() {
    this.count('lobby_list_requests');
  }

  lobbyPublish() {
    this.count('lobby_publishes');
  }

  // The most players listed in the public Lobby at once today; writes only what grew and nothing for an empty Lobby.
  lobbyVisible(count) {
    if (!Number.isInteger(count) || count <= 0) return;
    const day = metricsDay(this.now());
    if (this.lobbyPeak?.day !== day) {
      const row = this.sql.get('SELECT lobby_visible_peak FROM metrics_daily WHERE day = ?', day);
      this.lobbyPeak = { day, count: row?.lobby_visible_peak ?? 0 };
    }
    if (count <= this.lobbyPeak.count) return;
    this.sql.run(`INSERT INTO metrics_daily (day, lobby_visible_peak) VALUES (?, ?)
      ON CONFLICT(day) DO UPDATE SET lobby_visible_peak = MAX(lobby_visible_peak, excluded.lobby_visible_peak)`, day, count);
    this.lobbyPeak.count = count;
  }

  count(column, day = metricsDay(this.now())) {
    if (!COUNTERS.has(column)) throw new Error(`unknown counter ${column}`);
    this.sql.run(`INSERT INTO metrics_daily (day, ${column}) VALUES (?, 1)
      ON CONFLICT(day) DO UPDATE SET ${column} = ${column} + 1`, day);
  }

  addDimension(day, dimension, value, amount = 1) {
    const known = this.sql.get('SELECT 1 AS x FROM metrics_daily_dimensions WHERE day = ? AND dimension = ? AND value = ?',
      day, dimension, value);
    if (!known) {
      const values = this.sql.get('SELECT COUNT(*) AS n FROM metrics_daily_dimensions WHERE day = ? AND dimension = ?',
        day, dimension).n;
      if (values >= VALUES_PER_DIMENSION_PER_DAY) value = 'Other';
    }
    this.sql.run(`INSERT INTO metrics_daily_dimensions (day, dimension, value, count) VALUES (?, ?, ?, ?)
      ON CONFLICT(day, dimension, value) DO UPDATE SET count = count + excluded.count`, day, dimension, value, amount);
  }

  // Protection counters as written by the telemetry store's flush, so the dashboard's ranges reach past its 35 days.
  addAbuse(entries) {
    const day = metricsDay(this.now());
    for (const [name, amount] of entries) this.addDimension(day, 'abuse', name, amount);
  }

  // A party that ended: `peak` seats at most were taken (0: unknown, not counted), `relayed` when a pair chose the relay.
  roomClosed({ peak, lifetimeMs, relayed }) {
    if (!Number.isInteger(peak) || peak < 1) return;
    const day = metricsDay(this.now());
    const party = Math.min(peak, MAX_PARTY);
    this.sql.run(`INSERT INTO metrics_daily (day, rooms_closed, rooms_with_guest, rooms_relayed, party_total)
      VALUES (?, 1, ?, ?, ?) ON CONFLICT(day) DO UPDATE SET rooms_closed = rooms_closed + 1,
      rooms_with_guest = rooms_with_guest + excluded.rooms_with_guest, rooms_relayed = rooms_relayed + excluded.rooms_relayed,
      party_total = party_total + excluded.party_total`, day, party > 1 ? 1 : 0, relayed ? 1 : 0, party);
    this.addDimension(day, 'occupancy', String(party));
    this.addDimension(day, 'lifetime', LIFETIMES.find(([, limit]) => (Number(lifetimeMs) || 0) < limit)[0]);
  }

  // One connection that became an online session; `client` is its platform, version and country as reported.
  session(client) {
    const day = metricsDay(this.now());
    this.count('sessions', day);
    this.addDimension(day, 'platform', platformCategory(client.platform));
    this.addDimension(day, 'version', versionCategory(client.version));
    this.addDimension(day, 'country', countryCode(client.country));
  }

  // One signed-in player today, by the keyed pseudonym the hub derives. Repeats write nothing.
  playerSeen(key) {
    this.sql.run('INSERT OR IGNORE INTO metrics_players (day, player) VALUES (?, ?)', metricsDay(this.now()), key);
  }

  uniquePlayers(from, to) {
    return this.sql.get('SELECT COUNT(DISTINCT player) AS n FROM metrics_players WHERE day >= ? AND day <= ?', from, to).n;
  }

  // One accepted party snapshot (party.js): bounded counts whose values the server derives from character indexes, never
  // from client text. Callers have applied the hub's limits; nothing about the sender is kept.
  partyReport(party) {
    const day = metricsDay(this.now());
    for (const [dimension, value, amount] of partyCounts(party)) {
      this.sql.run(`INSERT INTO metrics_daily_dimensions (day, dimension, value, count) VALUES (?, ?, ?, ?)
        ON CONFLICT(day, dimension, value) DO UPDATE SET count = count + excluded.count`, day, dimension, value, amount);
    }
  }

  reject(reason) {
    const day = metricsDay(this.now());
    this.count('incompatible_rejects', day);
    this.addDimension(day, 'reject', reason);
  }

  // Raises today's peaks and the current sample to `live` ({ online, in_rooms, rooms }); writes only what grew.
  observe(live) {
    const now = this.now();
    const day = metricsDay(now);
    if (this.peaks?.day !== day) {
      const row = this.sql.get('SELECT peak_online, peak_in_rooms, peak_rooms FROM metrics_daily WHERE day = ?', day);
      this.peaks = row ? { day, online: row.peak_online, in_rooms: row.peak_in_rooms, rooms: row.peak_rooms }
        : { day, online: -1, in_rooms: -1, rooms: -1 };
    }
    if (grew(this.peaks, live)) {
      this.sql.run(`INSERT INTO metrics_daily (day, peak_online, peak_in_rooms, peak_rooms) VALUES (?, ?, ?, ?)
        ON CONFLICT(day) DO UPDATE SET peak_online = MAX(peak_online, excluded.peak_online),
        peak_in_rooms = MAX(peak_in_rooms, excluded.peak_in_rooms), peak_rooms = MAX(peak_rooms, excluded.peak_rooms)`,
      day, live.online, live.in_rooms, live.rooms);
      raise(this.peaks, live);
    }
    const bucket = sampleBucket(now);
    if (this.sample?.bucket !== bucket) {
      const row = this.sql.get('SELECT online, in_rooms, rooms FROM metrics_samples WHERE bucket = ?', bucket);
      this.sample = row ? { bucket, ...row } : { bucket, online: -1, in_rooms: -1, rooms: -1 };
      this.dropOldRows(now);
    }
    if (grew(this.sample, live)) {
      this.sql.run(`INSERT INTO metrics_samples (bucket, online, in_rooms, rooms) VALUES (?, ?, ?, ?)
        ON CONFLICT(bucket) DO UPDATE SET online = MAX(online, excluded.online),
        in_rooms = MAX(in_rooms, excluded.in_rooms), rooms = MAX(rooms, excluded.rooms)`,
      bucket, live.online, live.in_rooms, live.rooms);
      raise(this.sample, live);
    }
  }

  dropOldRows(now) {
    this.sql.run(`DELETE FROM metrics_samples WHERE bucket IN
      (SELECT bucket FROM metrics_samples WHERE bucket < ? ORDER BY bucket LIMIT ?)`, now - SAMPLE_DAYS * DAY_MS,
    CLEANUP_ROWS);
    const today = metricsDay(now);
    this.sql.run(`DELETE FROM metrics_players WHERE day =
      (SELECT MIN(day) FROM metrics_players WHERE day < ?)`, daysBefore(today, PLAYER_RETENTION_DAYS));
    this.sql.run('DELETE FROM metrics_daily WHERE day < ?', daysBefore(today, DAILY_RETENTION_DAYS));
    this.sql.run(`DELETE FROM metrics_daily_dimensions WHERE day =
      (SELECT MIN(day) FROM metrics_daily_dimensions WHERE day < ?)`, daysBefore(today, DAILY_RETENTION_DAYS));
  }

  // `live`: the counts plus Maps of platform, version and country to connected players.
  summary(live) {
    const now = this.now();
    const day = metricsDay(now);
    const today = this.sql.get(`SELECT ${DAY_COLUMNS.join(', ')} FROM metrics_daily WHERE day = ?`, day) ?? emptyDay();
    return {
      generated: now,
      time_zone: METRICS_TIME_ZONE,
      day,
      live: { online: live.online, in_rooms: live.in_rooms, rooms: live.rooms, platform: sorted(live.platform),
        version: sorted(live.version), country: sorted(live.country) },
      today: { ...today, unique_players: this.uniquePlayers(day, day), peak_online: Math.max(today.peak_online, live.online),
        peak_in_rooms: Math.max(today.peak_in_rooms, live.in_rooms), peak_rooms: Math.max(today.peak_rooms, live.rooms) },
    };
  }

  history(days) {
    if (!Number.isInteger(days) || days < 1 || days > HISTORY_MAX_DAYS) throw new RangeError('days out of range');
    const now = this.now();
    const last = metricsDay(now);
    const first = daysBefore(last, days - 1);
    const rows = new Map(this.sql.all(`SELECT day, ${DAY_COLUMNS.join(', ')} FROM metrics_daily WHERE day >= ? AND day <= ?
      ORDER BY day`, first, last)
      .map((r) => [r.day, r]));
    const players = new Map(this.sql.all(`SELECT day, COUNT(*) AS n FROM metrics_players WHERE day >= ? AND day <= ?
      GROUP BY day`, first, last).map((r) => [r.day, r.n]));
    const daily = [];
    for (let i = days - 1; i >= 0; i--) {
      const day = daysBefore(last, i);
      daily.push({ ...(rows.get(day) ?? { day, ...emptyDay() }), unique_players: players.get(day) ?? 0 });
    }
    const dimensions = Object.fromEntries(DIMENSIONS.map((d) => [d, []]));
    for (const r of this.sql.all(`SELECT dimension, value, SUM(count) AS count FROM metrics_daily_dimensions
      WHERE day >= ? AND day <= ? GROUP BY dimension, value ORDER BY count DESC, value`, first, last)) {
      dimensions[r.dimension]?.push({ value: r.value, count: r.count });
    }
    const since = Math.max(now - days * DAY_MS, now - SAMPLE_DAYS * DAY_MS);
    const samples = this.sql.all('SELECT bucket, online, in_rooms, rooms FROM metrics_samples WHERE bucket >= ? ORDER BY bucket',
      sampleBucket(since));
    const totals = this.totals(first, last);
    const previous = this.totals(daysBefore(first, days), daysBefore(first, 1));
    return { generated: now, time_zone: METRICS_TIME_ZONE, days, first, last, sample_ms: SAMPLE_MS, daily, dimensions,
      metrics_first_day: this.firstDay('metrics_daily'), players_first_day: this.firstDay('metrics_players'),
      party_first_day: this.sql.get(`SELECT MIN(day) AS day FROM metrics_daily_dimensions WHERE dimension = 'party_size'`).day ?? null,
      samples, totals, previous, comparison: compare(totals, previous) };
  }

  firstDay(table) {
    return this.sql.get(`SELECT MIN(day) AS day FROM ${table}`).day ?? null;
  }

  totals(from, to) {
    const select = [...SUMMED.map((c) => `COALESCE(SUM(${c}), 0) AS ${c}`), ...PEAKS.map((c) => `COALESCE(MAX(${c}), 0) AS ${c}`)];
    const row = this.sql.get(`SELECT ${select.join(', ')} FROM metrics_daily WHERE day >= ? AND day <= ?`, from, to);
    return { ...row, unique_players: this.uniquePlayers(from, to) };
  }
}

const emptyDay = () => Object.fromEntries(DAY_COLUMNS.map((c) => [c, 0]));

// Relative change in percent; null when there was nothing before to compare with.
export const percentChange = (current, previous) => (previous > 0 ? Math.round(1000 * (current - previous) / previous) / 10 : null);

export const COMPARED = ['unique_players', 'sessions', 'rooms_created', 'room_joins', 'rooms_with_guest',
  'peak_online', 'peak_in_rooms', 'incidents_filed', 'incidents_rejected', 'lobby_list_requests', 'lobby_publishes'];

export function compare(current, previous) {
  return Object.fromEntries(COMPARED.map((c) => [c, { current: current[c], previous: previous[c],
    change: percentChange(current[c], previous[c]) }]));
}

const grew = (seen, live) => live.online > seen.online || live.in_rooms > seen.in_rooms || live.rooms > seen.rooms;

function raise(seen, live) {
  seen.online = Math.max(seen.online, live.online);
  seen.in_rooms = Math.max(seen.in_rooms, live.in_rooms);
  seen.rooms = Math.max(seen.rooms, live.rooms);
}
