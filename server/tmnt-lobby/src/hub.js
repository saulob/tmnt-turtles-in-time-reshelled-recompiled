// The online directory: players, friends, presence, parties (up to four players), invites and the advertisement of a
// party's game session. A party is only metadata here; its sockets, endpoints and relay live in a match room
// (room.js), one per party. The hub tells a room its roster (`apply`, `end`); the room reports member states and
// endings back (roomMember, roomLeft, roomEnded). Both directions are one-way commands, so each side holds the state it
// needs and the two converge through events.
// State lives in SQL tables and in each socket's attachment, so a hibernated Durable Object resumes as if it had never
// stopped. The same class runs in the development server (local/server.js).
import { PROTECTION, RateLimiter, checkedCounts } from './abuse.js';
import { Metrics, SAMPLE_MS, countryCode, platformCategory, sampleBucket, versionCategory } from './metrics.js';
import { parsePartyReport } from './party.js';
import { hmacSha256Hex, sha256Hex, toHex } from './sha256.js';
import {
  BUCKETS, CAPABILITIES, LIMITS, MAX_PARTY, PROTOCOL, ProtocolError, compatibleGuests, fail, ipGroup, isInt, isLocalAddress, isObject,
  joinTokenHash, normalizeFriendCode, parseHello, takeToken,
} from './protocol.js';
import { TelemetryStore, isDiag, parseBatch } from './telemetry.js';

const DAY_MS = 24 * 3600 * 1000;
const SOCIAL_OPS = new Set(['friends.add', 'party.create', 'party.kick', 'invite.send', 'invite.accept', 'pm.host', 'pm.join']);
const RULE_KEY = /^[a-z_]{1,16}$/;
const QOS_DATA = /^(?:[0-9a-f]{2}){0,256}$/;
const MEMBER_STATES = ['joined', 'connecting', 'connected', 'disconnected'];
const HELD_STATES = ['connecting', 'connected'];
// Who hears an invite ended, and which endings an accept still answers precisely.
const INVITE_AUDIENCE = {
  declined: ['sender'], accepted: ['sender'], expired: ['sender', 'target'], accepted_elsewhere: ['sender', 'target'],
  host_left: ['target'], party_full: ['target'], cancelled: ['target'],
};
const KEEP_ENDED = new Set(['host_left', 'party_full']);

const SCHEMA = [
  `CREATE TABLE IF NOT EXISTS meta (k TEXT PRIMARY KEY, v TEXT NOT NULL)`,
  `CREATE TABLE IF NOT EXISTS players (player TEXT PRIMARY KEY, token_hash TEXT NOT NULL, name TEXT NOT NULL,
     created INTEGER NOT NULL, seen INTEGER NOT NULL)`,
  `CREATE TABLE IF NOT EXISTS friends (player TEXT NOT NULL, friend TEXT NOT NULL, since INTEGER NOT NULL,
     PRIMARY KEY (player, friend))`,
  `CREATE TABLE IF NOT EXISTS friend_requests (sender TEXT NOT NULL, target TEXT NOT NULL, ms INTEGER NOT NULL,
     PRIMARY KEY (sender, target))`,
  `CREATE TABLE IF NOT EXISTS registrations (ip TEXT NOT NULL, ms INTEGER NOT NULL)`,
  `DROP TABLE IF EXISTS challenges`,
  `DROP TABLE IF EXISTS matches`,
  `DROP TABLE IF EXISTS invites`,
  `CREATE TABLE IF NOT EXISTS player_matches (id TEXT PRIMARY KEY, owner TEXT NOT NULL UNIQUE, guest TEXT NOT NULL,
     build INTEGER NOT NULL, slots INTEGER NOT NULL, rules TEXT NOT NULL, qos TEXT NOT NULL DEFAULT '',
     created INTEGER NOT NULL, refreshed INTEGER NOT NULL, party TEXT NOT NULL DEFAULT '')`,
  `CREATE TABLE IF NOT EXISTS parties (id TEXT PRIMARY KEY, diag TEXT NOT NULL, host TEXT NOT NULL UNIQUE,
     guest TEXT NOT NULL, build INTEGER NOT NULL, created INTEGER NOT NULL, activity INTEGER NOT NULL,
     revision INTEGER NOT NULL DEFAULT 0, room_seq INTEGER NOT NULL DEFAULT 0)`,
  `CREATE TABLE IF NOT EXISTS party_members (party TEXT NOT NULL, slot INTEGER NOT NULL, player TEXT NOT NULL UNIQUE,
     join_hash TEXT NOT NULL, state TEXT NOT NULL DEFAULT 'joined', generation INTEGER NOT NULL DEFAULT 0,
     in_session INTEGER NOT NULL DEFAULT 0, offline_since INTEGER, platform TEXT NOT NULL DEFAULT '',
     version TEXT NOT NULL DEFAULT '', PRIMARY KEY (party, slot))`,
  `CREATE TABLE IF NOT EXISTS party_invites (id TEXT PRIMARY KEY, party TEXT NOT NULL, sender TEXT NOT NULL,
     target TEXT NOT NULL, created INTEGER NOT NULL, ended TEXT NOT NULL DEFAULT '', UNIQUE (party, target))`,
];
// Added after the table first shipped: CREATE TABLE above only runs on a fresh database.
const ADDED_COLUMNS = [
  ['player_matches', 'party', "TEXT NOT NULL DEFAULT ''"],
  ['parties', 'reached4', 'INTEGER NOT NULL DEFAULT 0'],
  ['parties', 'peak', 'INTEGER NOT NULL DEFAULT 0'],
  ['parties', 'relayed', 'INTEGER NOT NULL DEFAULT 0'],
  ['players', 'session', 'INTEGER NOT NULL DEFAULT 0'],
];

function playerOf(value) {
  if (typeof value !== 'string' || !/^[A-Z0-9]{4}-[A-Z0-9]{4}$/.test(value)) fail('bad_request', 'player expected');
  return value;
}

export class Hub {
  // host: { sql: { all(q, ...a), get(q, ...a), run(q, ...a) }, sockets(): Conn[], now(): ms, setAlarm(ms), log(text),
  //         room(match, method, ...args) }
  // options: { local, telemetry, alarmAt, metricsSecret } - alarmAt is the alarm the host already has stored, if any;
  // metricsSecret keys the players' pseudonyms instead of the stored salt.
  // Conn: { send(text), close(code, reason), attachment(), setAttachment(object) }
  constructor(host, options = {}) {
    this.host = host;
    this.local = Boolean(options.local);
    this.alarmAt = options.alarmAt ?? null;
    this.guards = {
      hello: new RateLimiter(PROTECTION.hello),
      createAccount: new RateLimiter(PROTECTION.createAccount),
      createAddress: new RateLimiter(PROTECTION.createAddress),
      joinAccount: new RateLimiter(PROTECTION.joinAccount),
      inviteAccount: new RateLimiter(PROTECTION.inviteAccount),
      inviteAddress: new RateLimiter(PROTECTION.inviteAddress),
      friendAccount: new RateLimiter(PROTECTION.friendAccount),
      telemetryAccount: new RateLimiter(PROTECTION.telemetryAccount),
      partyReportAll: new RateLimiter(PROTECTION.partyReportAll),
      partyReportAccount: new RateLimiter(PROTECTION.partyReportAccount),
      partyReportAddress: new RateLimiter(PROTECTION.partyReportAddress),
      rejects: new RateLimiter({ burst: 1, perSecond: 1000 / PROTECTION.rejectWindowMs }),
    };
    for (const statement of SCHEMA) host.sql.run(statement);
    for (const [table, column, definition] of ADDED_COLUMNS) {
      try { host.sql.run(`ALTER TABLE ${table} ADD COLUMN ${column} ${definition}`); } catch { /* already there */ }
    }
    if (!this.meta('salt')) this.setMeta('salt', this.randomHex(16));
    this.telemetryBucket = null;
    this.telemetryFailures = 0;
    try {
      this.telemetry = options.telemetry ?? new TelemetryStore(host.sql, () => host.now(), (text) => host.log(text));
    } catch (error) {
      this.telemetryFailed(error);
      this.telemetry = null;
    }
    this.metricsSecret = options.metricsSecret || null;
    try {
      this.metrics = new Metrics(host.sql, () => host.now());
    } catch (error) {
      this.telemetryFailed(error);
      this.metrics = null;
    }
    if (this.telemetry) this.telemetry.onAbuse = (entries) => this.track(() => this.metrics.addAbuse(entries));
    this.reconcile();
  }

  // After the object or the server restarted: what only a live socket could keep alive is dropped, and parties wait for
  // their players the usual grace.
  reconcile() {
    const present = new Set(this.host.sockets().map((c) => c.attachment()).filter((a) => a?.hello && a.presence).map((a) => a.player));
    const now = this.host.now();
    for (const ad of this.host.sql.all('SELECT * FROM player_matches')) {
      if (!ad.party || !present.has(ad.owner) || !this.partyRow(ad.party)) this.host.sql.run('DELETE FROM player_matches WHERE id = ?', ad.id);
    }
    for (const invite of this.host.sql.all('SELECT * FROM party_invites')) {
      if (!present.has(invite.target) || !present.has(invite.sender)) this.host.sql.run('DELETE FROM party_invites WHERE id = ?', invite.id);
    }
    for (const member of this.host.sql.all('SELECT * FROM party_members WHERE offline_since IS NULL')) {
      if (!present.has(member.player)) this.host.sql.run('UPDATE party_members SET offline_since = ? WHERE player = ?', now, member.player);
    }
    this.scheduleAlarm();
  }

  stats() {
    const count = (table) => this.host.sql.get(`SELECT COUNT(*) AS n FROM ${table}`).n;
    return {
      players: count('players'),
      friendships: count('friends') / 2,
      parties: count('parties'),
      members: count('party_members'),
      invites: this.host.sql.get("SELECT COUNT(*) AS n FROM party_invites WHERE ended = ''").n,
      player_matches: count('player_matches'),
    };
  }

  // ---- connection lifecycle ----
  // `country` is the edge's, never the client's.
  admit(ip, country = '') {
    const group = ipGroup(ip);
    if (!(this.local && isLocalAddress(ip))) {
      const open = this.host.sockets().filter((c) => ipGroup(c.attachment()?.ip) === group).length;
      if (open >= LIMITS.socketsPerIp) {
        this.diagnose('ws_upgrade_ip_cap');
        return null;
      }
    }
    this.diagnose('ws_upgrade_accepted');
    return { cid: this.randomHex(8), ip, opened: this.host.now(), country: countryCode(country) };
  }

  // Upgrades the Worker refused before they reached this object.
  edgeLimited(count) {
    if (count > 0) this.diagnose('ws_upgrade_limited', count);
  }

  message(conn, text) {
    const att = conn.attachment();
    if (!att) return;
    let seq = null;
    this.current = att;
    this.keepDeferred = false;
    try {
      if (typeof text !== 'string') fail('bad_request', 'text frames only');
      if (text.length > LIMITS.telemetryFrameBytes ||
          (text.length > LIMITS.frameBytes && !text.includes('"op":"telemetry.batch"'))) {
        fail('too_large');
      }
      let msg;
      try {
        msg = JSON.parse(text);
      } catch {
        fail('bad_request', 'invalid JSON');
      }
      if (!isObject(msg) || typeof msg.op !== 'string' || !isInt(msg.seq, 0, 0x7fffffff)) fail('bad_request', 'envelope');
      seq = msg.seq;
      if (text.length > LIMITS.frameBytes && msg.op !== 'telemetry.batch') fail('too_large');
      const now = this.host.now();
      const any = takeToken(att.ra, BUCKETS.any, now);
      if (!any) {
        this.noteLimit('any');
        fail('limit', 'slow down');
      }
      att.ra = any;
      if (SOCIAL_OPS.has(msg.op)) {
        const social = takeToken(att.rs, BUCKETS.social, now);
        if (!social) {
          this.noteLimit('social');
          fail('limit', 'slow down');
        }
        att.rs = social;
      }
      // A search walks every friend's advertisement.
      if (msg.op === 'pm.search') {
        const search = takeToken(att.rq, BUCKETS.search, now);
        if (!search) {
          this.noteLimit('search');
          fail('limit', 'slow down');
        }
        att.rq = search;
      }
      if (!Object.hasOwn(HANDLERS, msg.op)) fail('bad_request', 'unknown op');
      if (msg.op !== 'hello' && !att.hello) fail('no_session');
      this.index = null;
      const body = HANDLERS[msg.op].call(this, att, msg, conn, text) ?? {};
      conn.setAttachment(att);
      conn.send(JSON.stringify({ re: seq, code: 'ok', ...body }));
      this.runDeferred();
    } catch (error) {
      if (this.keepDeferred) {
        this.keepDeferred = false;
      } else {
        this.deferred = null;
      }
      conn.setAttachment(att);
      if (!(error instanceof ProtocolError)) {
        this.host.log(`hub: internal error in request ${seq}: ${error?.stack ?? error}`);
        conn.send(JSON.stringify({ re: seq, code: 'internal' }));
      } else {
        conn.send(JSON.stringify({ re: seq, code: error.code }));
      }
      this.runDeferred();
    } finally {
      this.current = null;
      this.index = null;
    }
  }

  runDeferred() {
    while (this.deferred?.length) {
      const batch = this.deferred;
      this.deferred = null;
      for (const fn of batch) {
        try {
          fn();
        } catch (error) {
          this.host.log(`hub: internal error in follow-up: ${error?.stack ?? error}`);
        }
      }
    }
  }

  // A socket closed: presence ends with the player's last present socket. A party outlives it for the usual grace, so
  // the player can come back; their ad and the invites they sent or hold do not.
  closed(conn) {
    const att = conn.attachment();
    if (!att?.hello) return;
    this.index = null;
    att.hello = false;
    conn.setAttachment(att);
    this.index = null;
    if (att.presence && !this.presentSockets(att.player, att.cid).length) {
      const mine = this.partyOf(att.player);
      if (mine) {
        this.host.sql.run('UPDATE party_members SET offline_since = ? WHERE player = ?', this.host.now(), att.player);
        if (mine.member.slot === 1) {
          const ad = this.adOf(mine.party.id);
          if (ad) this.dropAd(ad);
          for (const invite of this.invitesOf(mine.party.id)) this.endInvite(invite, 'host_left');
        }
        this.changed(mine.party.id);
      }
      for (const invite of this.host.sql.all('SELECT * FROM party_invites WHERE target = ?', att.player)) {
        this.host.sql.run('DELETE FROM party_invites WHERE id = ?', invite.id);
        if (!invite.ended) this.changed(invite.party);
      }
      this.notifyFriends(att.player, { ev: 'friend.offline', player: att.player });
    }
    this.runDeferred();
    this.index = null;
  }

  alarm() {
    this.alarmAt = null;
    this.index = null;
    const now = this.host.now();
    for (const ad of this.host.sql.all('SELECT * FROM player_matches WHERE refreshed + ? <= ?', LIMITS.playerMatchMs, now)) {
      this.dropAd(ad);
    }
    for (const invite of this.host.sql.all('SELECT * FROM party_invites WHERE created + ? <= ?', LIMITS.inviteMs, now)) {
      this.endInvite(invite, 'expired');
      if (!invite.ended) this.changed(invite.party);
    }
    for (const party of this.host.sql.all('SELECT * FROM parties')) {
      const members = this.membersOf(party.id);
      if (party.created + LIMITS.matchMaxMs <= now || (members.length === 1 && party.activity + LIMITS.partyIdleMs <= now)) {
        this.endParty(party, 'expired');
        continue;
      }
      // Gone from the directory and not holding a room seat: nobody is coming back for it.
      for (const member of members) {
        if (member.offline_since === null || member.offline_since + LIMITS.graceMs > now) continue;
        if (HELD_STATES.includes(member.state)) continue;
        if (member.slot === 1) {
          this.endParty(party, 'host_left');
          break;
        }
        this.removeMember(party, member, { tell: 'expired', gone: 'expired', roster: true });
      }
    }
    this.runDeferred();
    this.index = null;
    this.observeLive();
    this.flushDiagnostics();
    this.scheduleAlarm();
  }

  // An alarm that is already set for the same moment or earlier stays: it fires early at worst, and alarm() sets the next.
  scheduleAlarm() {
    const times = [];
    const flush = this.diagnosticsDueAt();
    if (flush) times.push(flush);
    const sample = this.sampleDueAt();
    if (sample) times.push(sample);
    const advertised = this.host.sql.get('SELECT MIN(refreshed) AS t FROM player_matches');
    if (advertised?.t) times.push(advertised.t + LIMITS.playerMatchMs);
    const invited = this.host.sql.get('SELECT MIN(created) AS t FROM party_invites');
    if (invited?.t) times.push(invited.t + LIMITS.inviteMs);
    const oldest = this.host.sql.get('SELECT MIN(created) AS t FROM parties');
    if (oldest?.t) times.push(oldest.t + LIMITS.matchMaxMs);
    const idle = this.host.sql.get(`SELECT MIN(activity) AS t FROM parties WHERE id IN
      (SELECT party FROM party_members GROUP BY party HAVING COUNT(*) = 1)`);
    if (idle?.t) times.push(idle.t + LIMITS.partyIdleMs);
    const gone = this.host.sql.get(`SELECT MIN(offline_since) AS t FROM party_members
      WHERE offline_since IS NOT NULL AND state NOT IN ('connecting', 'connected')`);
    if (gone?.t) times.push(gone.t + LIMITS.graceMs);
    if (!times.length) return;
    const at = Math.min(...times);
    if (this.alarmAt !== null && this.alarmAt <= at) return;
    this.host.setAlarm(at);
    this.alarmAt = at;
  }

  // ---- reports from a room ----
  inbound(match, seq, fn) {
    this.index = null;
    const party = this.partyRow(match);
    if (party && isInt(seq, 1, Number.MAX_SAFE_INTEGER) && seq > party.room_seq) {
      this.host.sql.run('UPDATE parties SET room_seq = ? WHERE id = ?', seq, match);
      fn(party);
    }
    this.runDeferred();
    this.index = null;
  }

  roomMember(match, slot, state, generation, seq) {
    this.inbound(match, seq, () => {
      const member = this.host.sql.get('SELECT * FROM party_members WHERE party = ? AND slot = ?', match, slot);
      if (!member || !MEMBER_STATES.includes(state) || !isInt(generation, 0, 0x7fffffff)) return;
      if (member.state === state && member.generation === generation) return;
      this.host.sql.run('UPDATE party_members SET state = ?, generation = ? WHERE party = ? AND slot = ?', state, generation, match, slot);
      // A member away from the directory gets the grace from the moment its room seat is lost too.
      if (state === 'disconnected' && member.offline_since !== null) {
        this.host.sql.run('UPDATE party_members SET offline_since = ? WHERE party = ? AND slot = ?', this.host.now(), match, slot);
      }
      this.changed(match);
    });
  }

  roomLeft(match, slot, reason, seq) {
    this.inbound(match, seq, (party) => {
      const member = this.host.sql.get('SELECT * FROM party_members WHERE party = ? AND slot = ?', match, slot);
      if (!member) return;
      if (member.slot === 1) {
        this.endParty(party, 'host_left', { room: false });
      } else {
        this.removeMember(party, member, { tell: reason === 'left' ? 'closed' : 'expired', roster: false });
      }
    });
  }

  roomEnded(match, reason, seq) {
    this.inbound(match, seq, (party) => {
      this.endParty(party, ['host_left', 'expired'].includes(reason) ? reason : 'closed', { room: false });
    });
  }

  // A room's event for the party's telemetry (room.js `report`); one-way and without a sequence number.
  roomEvent(diag, event, fields) {
    if (typeof event === 'string' && (diag === '' || isDiag(diag))) this.record(event, fields, diag);
    if (event === 'room_transport' && fields?.transport === 'relay' && isDiag(diag)) {
      this.track(() => this.host.sql.run('UPDATE parties SET relayed = 1 WHERE diag = ? AND relayed = 0', diag));
    }
  }

  // ---- telemetry (telemetry.js); best effort, never changes what a request or a report does ----
  record(event, fields = {}, diag = '') {
    try {
      this.telemetry?.record(event, fields, diag);
    } catch (error) {
      this.telemetryFailed(error);
    }
  }

  noteLimit(scope) {
    try {
      this.telemetry?.noteLimit(scope);
    } catch (error) {
      this.telemetryFailed(error);
    }
  }

  // Aggregate abuse counters (abuse.js): counted in memory, written together, never tied to a player or an address.
  diagnose(name, amount = 1) {
    try {
      if (this.telemetry?.noteAbuse(name, amount)) this.scheduleAlarm();
    } catch (error) {
      this.telemetryFailed(error);
    }
  }

  flushDiagnostics() {
    try {
      this.telemetry?.flushPending();
    } catch (error) {
      this.telemetryFailed(error);
    }
  }

  diagnosticsDueAt() {
    try {
      return this.telemetry?.abuseDueAt() ?? null;
    } catch (error) {
      this.telemetryFailed(error);
      return null;
    }
  }

  // A match room's abuse counters (RPC).
  roomAbuse(entries) {
    for (const [name, count] of checkedCounts(entries)) this.diagnose(name, count);
  }

  telemetryFailed(error) {
    if (this.telemetryFailures++ < 3) this.host.log(`hub: telemetry unavailable: ${error?.message ?? error}`);
  }

  telemetryKey(player) {
    return sha256Hex(`${this.meta('salt')}|telemetry|${player}`).slice(0, 16);
  }

  telemetrySummary(days) {
    const summary = this.telemetry.summary(days);
    const online = new Set(this.sockets().filter((s) => s.att.presence).map((s) => s.att.player));
    return { ...summary, live: { ...this.stats(), online: online.size } };
  }

  telemetrySession(diag) {
    return this.telemetry.session(diag);
  }

  // ---- service metrics (metrics.js); best effort, never changes what a request does ----
  track(fn) {
    if (!this.metrics) return;
    try {
      fn();
    } catch (error) {
      this.telemetryFailed(error);
    }
  }

  // Keyed, so the stored value cannot be matched to a friend code without the key; never leaves this object.
  playerKey(player) {
    return hmacSha256Hex(this.metricsSecret ?? this.meta('salt'), `metrics-player|${player}`).slice(0, 32);
  }

  // Refusals a client can repeat at will are written once per reason and key per window.
  countReject(reason, key) {
    if (this.guards.rejects.take(`${reason}|${key}`, this.host.now())) this.track(() => this.metrics.reject(reason));
  }

  liveMetrics() {
    this.index = null;
    const present = new Map(this.sockets().filter((s) => s.att.presence).map((s) => [s.att.player, s.att]));
    this.index = null;
    const tally = (pick) => {
      const counts = new Map();
      for (const att of present.values()) counts.set(pick(att), (counts.get(pick(att)) ?? 0) + 1);
      return counts;
    };
    return {
      online: present.size,
      in_rooms: this.host.sql.get('SELECT COUNT(*) AS n FROM party_members WHERE offline_since IS NULL').n,
      rooms: this.host.sql.get('SELECT COUNT(*) AS n FROM parties').n,
      platform: tally((att) => platformCategory(att.platform)),
      version: tally((att) => versionCategory(att.version)),
      country: tally((att) => countryCode(att.country)),
    };
  }

  observeLive() {
    this.track(() => this.metrics.observe(this.liveMetrics()));
  }

  // The next 15-minute usage sample, while anyone is connected.
  sampleDueAt() {
    return this.metrics && this.host.sockets().length ? sampleBucket(this.host.now()) + SAMPLE_MS : null;
  }

  metricsSummary() {
    return this.metrics.summary(this.liveMetrics());
  }

  metricsHistory(days) {
    return { ...this.metrics.history(days), player_key: this.metricsSecret ? 'secret' : 'salt' };
  }

  // One Connection Test beacon (beacon.js); a store that fails throws to the caller, which answers regardless.
  telemetryBeacon(beacon) {
    if (!this.telemetry) return false;
    this.telemetry.recordConnectionTest(beacon);
    return true;
  }

  // ---- helpers ----
  meta(key) {
    return this.host.sql.get('SELECT v FROM meta WHERE k = ?', key)?.v ?? null;
  }

  setMeta(key, value) {
    this.host.sql.run('INSERT INTO meta (k, v) VALUES (?, ?) ON CONFLICT(k) DO UPDATE SET v = excluded.v', key, String(value));
  }

  randomHex(bytes) {
    return toHex(crypto.getRandomValues(new Uint8Array(bytes)));
  }

  ipKey(ip) {
    return sha256Hex(`${this.meta('salt')}|${ipGroup(ip)}`).slice(0, 32);
  }

  // Greeted sockets; the one whose message is being handled carries its live attachment, not a stored copy.
  sockets() {
    if (!this.index) {
      this.index = [];
      for (const conn of this.host.sockets()) {
        const stored = conn.attachment();
        const att = stored && this.current && stored.cid === this.current.cid ? this.current : stored;
        if (att?.hello) this.index.push({ conn, att });
      }
    }
    return this.index;
  }

  socketByCid(cid) {
    return cid ? this.sockets().find((s) => s.att.cid === cid) ?? null : null;
  }

  presentSockets(player, exceptCid = null) {
    return this.sockets().filter((s) => s.att.player === player && s.att.presence && s.att.cid !== exceptCid);
  }

  send(conn, event) {
    try {
      conn.send(JSON.stringify(event));
    } catch {
      // A socket that is closing; its close handler runs next.
    }
  }

  later(fn) {
    (this.deferred ??= []).push(fn);
  }

  notifyPlayer(player, event) {
    for (const s of this.sockets()) if (s.att.player === player) this.send(s.conn, event);
  }

  notifyPresent(player, event) {
    for (const s of this.presentSockets(player)) this.send(s.conn, event);
  }

  // ---- players and presence ----
  playerRow(player) {
    return this.host.sql.get('SELECT * FROM players WHERE player = ?', player) ?? null;
  }

  stateOf(player) {
    const mine = this.partyOf(player);
    if (!mine) return 'online';
    const inSession = mine.member.slot === 1
      ? this.host.sql.get('SELECT 1 AS x FROM party_members WHERE party = ? AND in_session = 1', mine.party.id)
      : mine.member.in_session;
    if (!inSession) return 'party';
    return mine.member.state === 'connected' ? 'in_game' : 'connecting';
  }

  friendView(player, viewer) {
    const row = this.playerRow(player);
    const view = { player, name: row?.name ?? '', online: false, state: 'offline' };
    const present = this.presentSockets(player);
    if (!present.length) return view;
    const att = present[0].att;
    return {
      ...view, online: true, state: this.stateOf(player), platform: att.platform, version: att.version, guest: att.guest,
      build: att.build, compatible: viewer ? compatibleGuests(viewer, att) : true,
    };
  }

  friendsOf(player) {
    return this.host.sql.all('SELECT friend FROM friends WHERE player = ? ORDER BY friend', player).map((r) => r.friend);
  }

  areFriends(a, b) {
    return Boolean(this.host.sql.get('SELECT 1 AS x FROM friends WHERE player = ? AND friend = ?', a, b));
  }

  notifyFriends(player, event) {
    for (const friend of this.friendsOf(player)) this.notifyPlayer(friend, event);
  }

  notifyStatus(player) {
    for (const friend of this.friendsOf(player)) {
      for (const s of this.presentSockets(friend)) {
        this.send(s.conn, { ev: 'friend.status', friend: this.friendView(player, s.att) });
      }
    }
  }

  // ---- abuse limits (abuse.js); in memory, checked once the request is valid and before it writes ----
  exempt(att) {
    return this.local && isLocalAddress(att.ip);
  }

  // Counted once the token matched, so nobody can lock out another player by guessing at the account.
  limitHello(att, player, now) {
    if (this.exempt(att) || this.guards.hello.take(player, now)) return;
    this.diagnose('hello_limited');
    fail('limit', 'too many sign-ins');
  }

  limitByAccountAndAddress(att, account, address, diagnostic, detail) {
    if (this.exempt(att)) return;
    const now = this.host.now();
    const key = ipGroup(att.ip);
    if (!account.allows(att.player, now) || !address.allows(key, now)) {
      this.diagnose(diagnostic);
      fail('limit', detail);
    }
    account.take(att.player, now);
    address.take(key, now);
  }

  limitCreate(att) {
    this.limitByAccountAndAddress(att, this.guards.createAccount, this.guards.createAddress, 'party_create_limited',
      'creating parties too fast');
  }

  limitJoin(att) {
    if (this.exempt(att) || this.guards.joinAccount.take(att.player, this.host.now())) return;
    this.diagnose('party_join_limited');
    fail('limit', 'joining too fast');
  }

  // Full-party refusals come before the join limit, so their telemetry is sampled on its own.
  recordPartyFull(att, scope, diag) {
    if (this.guards.rejects.take(`party_full|${att.player}`, this.host.now())) this.record('party_full', { scope }, diag);
  }

  // Before the sign-in limit: an identity refused here must not take a limiter key.
  requireRegistrationRoom(att, now) {
    if (this.local && isLocalAddress(att.ip)) return;
    this.host.sql.run('DELETE FROM registrations WHERE ms < ?', now - DAY_MS);
    const recent = this.host.sql.get('SELECT COUNT(*) AS n FROM registrations WHERE ip = ?', this.ipKey(att.ip)).n;
    if (recent >= LIMITS.registrationsPerIpPerDay) fail('limit', 'too many new players');
  }

  limitInvite(att) {
    this.limitByAccountAndAddress(att, this.guards.inviteAccount, this.guards.inviteAddress, 'invite_limited', 'inviting too fast');
  }

  // Party snapshots have their own budget, one service-wide and one per account and per address; a refusal spends none of
  // them, and none is shared with creating or joining parties.
  limitPartyReport(att) {
    if (this.exempt(att)) return;
    const now = this.host.now();
    const address = ipGroup(att.ip);
    const { partyReportAll, partyReportAccount, partyReportAddress } = this.guards;
    if (!partyReportAll.allows('all', now) || !partyReportAccount.allows(att.player, now) ||
        !partyReportAddress.allows(address, now)) {
      this.diagnose('party_report_limited');
      fail('limit', 'too many party reports');
    }
    partyReportAll.take('all', now);
    partyReportAccount.take(att.player, now);
    partyReportAddress.take(address, now);
  }

  limitFriendRequest(att) {
    if (this.exempt(att) || this.guards.friendAccount.take(att.player, this.host.now())) return;
    this.diagnose('friend_request_limited');
    fail('limit', 'adding friends too fast');
  }

  touchPlayer(row, name, now) {
    if (row.name !== name || now - row.seen >= PROTECTION.seenRefreshMs) {
      this.host.sql.run('UPDATE players SET name = ?, seen = ? WHERE player = ?', name, now, row.player);
    }
  }

  // ---- friends ----
  friendCount(player) {
    return this.host.sql.get('SELECT COUNT(*) AS n FROM friends WHERE player = ?', player).n;
  }

  expireRequests() {
    this.host.sql.run('DELETE FROM friend_requests WHERE ms < ?', this.host.now() - LIMITS.requestDays * DAY_MS);
  }

  makeFriends(a, b) {
    const now = this.host.now();
    this.host.sql.run('INSERT OR IGNORE INTO friends (player, friend, since) VALUES (?, ?, ?)', a, b, now);
    this.host.sql.run('INSERT OR IGNORE INTO friends (player, friend, since) VALUES (?, ?, ?)', b, a, now);
    this.host.sql.run('DELETE FROM friend_requests WHERE (sender = ? AND target = ?) OR (sender = ? AND target = ?)', a, b, b, a);
  }

  requireFriendRoom(a, b) {
    if (this.friendCount(a) >= LIMITS.friends || this.friendCount(b) >= LIMITS.friends) fail('limit', 'friend list full');
  }

  // ---- parties ----
  partyRow(id) {
    return typeof id === 'string' ? this.host.sql.get('SELECT * FROM parties WHERE id = ?', id) ?? null : null;
  }

  memberOf(player) {
    return this.host.sql.get('SELECT * FROM party_members WHERE player = ?', player) ?? null;
  }

  membersOf(id) {
    return this.host.sql.all('SELECT * FROM party_members WHERE party = ? ORDER BY slot', id);
  }

  partyOf(player) {
    const member = this.memberOf(player);
    const party = member && this.partyRow(member.party);
    return party ? { party, member } : null;
  }

  invitesOf(id) {
    return this.host.sql.all("SELECT * FROM party_invites WHERE party = ? AND ended = '' ORDER BY created", id);
  }

  snapshot(party) {
    const ad = this.adOf(party.id);
    const members = this.membersOf(party.id).map((m) => {
      const att = this.presentSockets(m.player)[0]?.att;
      return { slot: m.slot, player: m.player, name: this.playerRow(m.player)?.name ?? '', platform: att?.platform ?? m.platform,
        version: att?.version ?? m.version, state: m.state };
    });
    const invited = this.invitesOf(party.id).map((i) => ({ player: i.target, name: this.playerRow(i.target)?.name ?? '' }));
    return { match: party.id, diag: party.diag, host: party.host, generation: party.revision, session: ad?.id ?? '',
      slots: this.capacityOf(party.id), members, invited };
  }

  // Every change to a party bumps its revision and, once the request is answered, tells its members and their friends.
  // Only what a player did counts as activity for the idle expiry.
  changed(id, { roster = false, gone = {}, active = false } = {}) {
    if (active) this.host.sql.run('UPDATE parties SET revision = revision + 1, activity = ? WHERE id = ?', this.host.now(), id);
    else this.host.sql.run('UPDATE parties SET revision = revision + 1 WHERE id = ?', id);
    if (roster) this.later(() => this.pushRoster(id, gone));
    this.later(() => this.broadcastParty(id));
    this.later(() => {
      for (const m of this.membersOf(id)) this.notifyStatus(m.player);
    });
    this.scheduleAlarm();
  }

  pushRoster(id, gone) {
    const party = this.partyRow(id);
    if (!party) return;
    const members = this.membersOf(id).map((m) => ({ slot: m.slot, name: this.playerRow(m.player)?.name ?? '', hash: m.join_hash,
      owner: sha256Hex(`${this.meta('salt')}|seat|${id}|${m.player}`).slice(0, 16) }));
    this.host.room?.(id, 'apply', { match: id, diag: party.diag, rev: party.revision, members, gone });
  }

  broadcastParty(id) {
    const party = this.partyRow(id);
    if (!party) return;
    const event = { ev: 'party.update', party: this.snapshot(party) };
    for (const m of this.membersOf(id)) this.notifyPresent(m.player, event);
  }

  createParty(att) {
    this.limitCreate(att);
    this.diagnose('party_create_accepted');
    const id = this.randomHex(8);
    const diag = this.randomHex(6);
    const token = this.randomHex(16);
    const now = this.host.now();
    this.host.sql.run('INSERT INTO parties (id, diag, host, guest, build, created, activity, peak) VALUES (?, ?, ?, ?, ?, ?, ?, 1)', id,
      diag, att.player, att.guest, att.build, now, now);
    this.host.sql.run('INSERT INTO party_members (party, slot, player, join_hash, platform, version) VALUES (?, 1, ?, ?, ?, ?)', id,
      att.player, joinTokenHash(token), att.platform, att.version);
    this.host.log(`party create diag=${diag} match=${id}`);
    this.record('party_created', {}, diag);
    this.track(() => this.metrics.count('rooms_created'));
    this.changed(id, { roster: true, active: true });
    this.observeLive();
    return { match: id, diag, slot: 1, join_token: token, generation: 0, party: this.snapshot(this.partyRow(id)) };
  }

  // A party from before peaks were kept (peak 0) stays uncounted, as metrics.roomClosed drops it.
  countJoin(party, seats) {
    this.host.sql.run('UPDATE parties SET peak = ? WHERE id = ? AND peak > 0 AND peak < ?', seats, party.id, seats);
    this.track(() => this.metrics.count('room_joins'));
    this.observeLive();
  }

  endParty(party, reason, { actor = null, room = true } = {}) {
    const members = this.membersOf(party.id);
    const lifetimeMs = this.host.now() - party.created;
    this.track(() => this.metrics.roomClosed({ peak: party.peak, lifetimeMs, relayed: party.relayed === 1 }));
    this.host.sql.run('DELETE FROM player_matches WHERE party = ?', party.id);
    for (const invite of this.host.sql.all('SELECT * FROM party_invites WHERE party = ?', party.id)) this.endInvite(invite, 'host_left');
    this.host.sql.run('DELETE FROM party_members WHERE party = ?', party.id);
    this.host.sql.run('DELETE FROM parties WHERE id = ?', party.id);
    this.host.log(`party end diag=${party.diag} match=${party.id} reason=${reason}`);
    this.record('party_ended', { reason, members: members.length }, party.diag);
    for (const m of members) {
      if (m.player !== actor) this.notifyPresent(m.player, { ev: 'party.ended', match: party.id, reason });
    }
    if (room) this.later(() => this.host.room?.(party.id, 'end', party.id, reason));
    this.later(() => {
      for (const m of members) this.notifyStatus(m.player);
    });
    this.scheduleAlarm();
  }

  // tell: why the member hears the party ended for them (none when they left on their own); gone: why the room closes
  // their seat; roster: whether the room needs telling at all (it does not when it reported the departure itself).
  removeMember(party, member, { tell = null, gone = 'left', roster = true } = {}) {
    this.host.sql.run('DELETE FROM party_members WHERE party = ? AND slot = ?', party.id, member.slot);
    this.record('member_left', { slot: member.slot, reason: gone }, party.diag);
    if (tell) this.notifyPresent(member.player, { ev: 'party.ended', match: party.id, reason: tell });
    this.changed(party.id, { roster, gone: { [member.slot]: gone }, active: true });
    this.later(() => this.notifyStatus(member.player));
  }

  // ---- advertisements ----
  adOf(partyId) {
    return this.host.sql.get('SELECT * FROM player_matches WHERE party = ?', partyId) ?? null;
  }

  // The places of a party: its game session's slots once there is one.
  capacityOf(partyId) {
    const ad = this.adOf(partyId);
    return ad ? Math.min(ad.slots, MAX_PARTY) : MAX_PARTY;
  }

  // Invites still out for a party that just filled up can no longer be accepted.
  endInvitesIfFull(party) {
    if (this.membersOf(party.id).length < this.capacityOf(party.id)) return;
    for (const pending of this.invitesOf(party.id)) this.endInvite(pending, 'party_full');
  }

  // What a party member's search shows; never the host's address.
  joinFriendsSession(att, session) {
    const ad = typeof session === 'string' ? this.host.sql.get('SELECT * FROM player_matches WHERE id = ?', session) : null;
    if (!ad || !this.areFriends(att.player, ad.owner)) fail('not_found');
    if (!this.liveAd(ad)) {
      this.keepDeferred = true;
      this.dropAd(ad);
      fail('offline');
    }
    if (!compatibleGuests(att, ad)) {
      this.countReject('online_protocol', att.player);
      fail('version_mismatch');
    }
    const party = this.partyRow(ad.party);
    if (!party) fail('not_found');
    const members = this.membersOf(party.id);
    if (members.length >= this.capacityOf(party.id)) {
      this.recordPartyFull(att, 'accept', party.diag);
      fail('party_full');
    }
    this.limitJoin(att);
    let slot = 2;
    while (members.some((m) => m.slot === slot)) slot += 1;
    const token = this.randomHex(16);
    this.host.sql.run('INSERT INTO party_members (party, slot, player, join_hash, platform, version, in_session) VALUES (?, ?, ?, ?, ?, ?, 1)',
      party.id, slot, att.player, joinTokenHash(token), att.platform, att.version);
    for (const other of this.host.sql.all('SELECT * FROM party_invites WHERE target = ?', att.player)) {
      this.endInvite(other, 'accepted_elsewhere');
      if (!other.ended) this.changed(other.party);
    }
    this.endInvitesIfFull(party);
    this.record('member_joined', { slot }, party.diag);
    if (members.length + 1 === MAX_PARTY && !party.reached4) {
      this.host.sql.run('UPDATE parties SET reached4 = 1 WHERE id = ?', party.id);
      this.record('party_reached_4', {}, party.diag);
    }
    this.countJoin(party, members.length + 1);
    this.changed(party.id, { roster: true, active: true });
    return { session: ad.id, match: party.id, diag: party.diag, slot, join_token: token, generation: 0,
      party: this.snapshot(this.partyRow(party.id)) };
  }

  adView(ad) {
    const owner = this.presentSockets(ad.owner)[0]?.att;
    return { session: ad.id, player: ad.owner, name: this.playerRow(ad.owner)?.name ?? '', guest: ad.guest, build: ad.build,
      slots: ad.slots, rules: JSON.parse(ad.rules), qos: ad.qos, platform: owner?.platform ?? '', version: owner?.version ?? '' };
  }

  // Live while its owner is present and keeps refreshing it.
  liveAd(ad) {
    return ad.refreshed + LIMITS.playerMatchMs > this.host.now() && this.presentSockets(ad.owner).length > 0;
  }

  dropAd(ad) {
    this.host.sql.run('DELETE FROM player_matches WHERE id = ?', ad.id);
    if (this.partyRow(ad.party)) this.changed(ad.party);
  }

  // ---- invites ----
  endInvite(invite, reason) {
    if (invite.ended) {
      this.host.sql.run('DELETE FROM party_invites WHERE id = ?', invite.id);
      return;
    }
    const known = ['accepted', 'declined', 'expired'].includes(reason);
    this.record(known ? `invite_${reason}` : 'invite_ended', known ? {} : { reason }, this.partyRow(invite.party)?.diag ?? '');
    if (KEEP_ENDED.has(reason)) {
      this.host.sql.run('UPDATE party_invites SET ended = ? WHERE id = ?', reason, invite.id);
    } else {
      this.host.sql.run('DELETE FROM party_invites WHERE id = ?', invite.id);
    }
    const event = { ev: 'invite.ended', invite: invite.id, reason };
    const audience = INVITE_AUDIENCE[reason];
    if (audience.includes('sender')) this.notifyPresent(invite.sender, event);
    if (audience.includes('target')) this.notifyPresent(invite.target, event);
  }
}

const HANDLERS = {
  hello(att, msg) {
    if (att.hello) fail('bad_request', 'already greeted');
    if (msg.proto !== PROTOCOL) {
      if (!att.protoRejected) this.countReject('lobby_protocol', ipGroup(att.ip));
      att.protoRejected = true;
    }
    const hello = parseHello(msg);
    const now = this.host.now();
    const row = this.playerRow(hello.player);
    if (row && row.token_hash !== hello.hash) fail('unauthorized');
    if (!row) this.requireRegistrationRoom(att, now);
    this.limitHello(att, hello.player, now);
    const newSession = hello.presence && now - (row?.session ?? 0) >= PROTECTION.sessionWindowMs;
    let renamed = false;
    if (!row) {
      this.host.sql.run('INSERT INTO players (player, token_hash, name, created, seen) VALUES (?, ?, ?, ?, ?)', hello.player,
        hello.hash, hello.name, now, now);
      this.host.sql.run('INSERT INTO registrations (ip, ms) VALUES (?, ?)', this.ipKey(att.ip), now);
    } else {
      renamed = row.name !== hello.name;
      this.touchPlayer(row, hello.name, now);
    }
    if (newSession) this.host.sql.run('UPDATE players SET session = ? WHERE player = ?', now, hello.player);
    else if (hello.presence) this.diagnose('hello_reconnect_deduped');
    const wasOnline = hello.presence && this.presentSockets(hello.player).length > 0;
    Object.assign(att, { hello: true, player: hello.player, name: hello.name, presence: hello.presence, guest: hello.guest,
      build: hello.build, version: hello.version, platform: hello.platform, relay: hello.relay });
    this.diagnose('hello_accepted');
    this.index = null;
    if (hello.presence) {
      this.track(() => this.metrics.playerSeen(this.playerKey(hello.player)));
      if (newSession) this.track(() => this.metrics.session(att));
      this.observeLive();
    }
    const mine = hello.presence ? this.partyOf(hello.player) : null;
    if (mine) {
      const { offline_since: offline, platform, version } = mine.member;
      if (offline !== null || platform !== hello.platform || version !== hello.version) {
        this.host.sql.run('UPDATE party_members SET offline_since = NULL, platform = ?, version = ? WHERE player = ?',
          hello.platform, hello.version, hello.player);
      }
      this.later(() => {
        const s = this.socketByCid(att.cid);
        const party = this.partyRow(mine.party.id);
        if (s && party) this.send(s.conn, { ev: 'party.update', party: this.snapshot(party) });
      });
    }
    if (hello.presence ? !wasOnline || renamed : renamed) this.later(() => this.notifyStatus(hello.player));
    return { player: hello.player, name: hello.name, friend_code: hello.player, time: now, capabilities: CAPABILITIES };
  },

  ping() {
    return { time: this.host.now() };
  },

  'friends.list'(att) {
    return { friends: this.friendsOf(att.player).map((f) => this.friendView(f, att)) };
  },

  'friends.requests'(att) {
    this.expireRequests();
    const named = (rows, key) => rows.map((r) => ({ player: r[key], name: this.playerRow(r[key])?.name ?? '', ms: r.ms }));
    return {
      incoming: named(this.host.sql.all('SELECT sender, ms FROM friend_requests WHERE target = ? ORDER BY ms', att.player), 'sender'),
      outgoing: named(this.host.sql.all('SELECT target, ms FROM friend_requests WHERE sender = ? ORDER BY ms', att.player), 'target'),
    };
  },

  'friends.add'(att, msg) {
    const code = normalizeFriendCode(msg.code);
    const target = this.playerRow(code);
    if (!target) fail('not_found');
    if (target.player === att.player) fail('bad_request', 'own code');
    const result = { player: target.player, name: target.name };
    if (this.areFriends(att.player, target.player)) return { ...result, state: 'friends' };
    this.expireRequests();
    if (this.host.sql.get('SELECT 1 AS x FROM friend_requests WHERE sender = ? AND target = ?', target.player, att.player)) {
      this.requireFriendRoom(att.player, target.player);
      this.limitFriendRequest(att);
      this.makeFriends(att.player, target.player);
      for (const s of this.presentSockets(target.player)) {
        this.send(s.conn, { ev: 'friend.added', friend: this.friendView(att.player, s.att) });
      }
      return { ...result, state: 'friends' };
    }
    if (this.host.sql.get('SELECT 1 AS x FROM friend_requests WHERE sender = ? AND target = ?', att.player, target.player)) {
      return { ...result, state: 'pending' };
    }
    const outgoing = this.host.sql.get('SELECT COUNT(*) AS n FROM friend_requests WHERE sender = ?', att.player).n;
    const incoming = this.host.sql.get('SELECT COUNT(*) AS n FROM friend_requests WHERE target = ?', target.player).n;
    if (outgoing >= LIMITS.pendingOut || incoming >= LIMITS.pendingIn) fail('limit', 'too many pending requests');
    this.requireFriendRoom(att.player, target.player);
    this.limitFriendRequest(att);
    this.host.sql.run('INSERT INTO friend_requests (sender, target, ms) VALUES (?, ?, ?)', att.player, target.player, this.host.now());
    this.notifyPlayer(target.player, { ev: 'friend.request', player: att.player, name: att.name });
    return { ...result, state: 'pending' };
  },

  'friends.accept'(att, msg) {
    const sender = playerOf(msg.player);
    this.expireRequests();
    if (!this.host.sql.get('SELECT 1 AS x FROM friend_requests WHERE sender = ? AND target = ?', sender, att.player)) fail('not_found');
    this.requireFriendRoom(att.player, sender);
    this.makeFriends(att.player, sender);
    for (const s of this.presentSockets(sender)) {
      this.send(s.conn, { ev: 'friend.added', friend: this.friendView(att.player, s.att) });
    }
    return { friend: this.friendView(sender, att) };
  },

  'friends.decline'(att, msg) {
    const other = playerOf(msg.player);
    const pair = [other, att.player, att.player, other];
    if (!this.host.sql.get(`SELECT 1 AS x FROM friend_requests WHERE (sender = ? AND target = ?) OR (sender = ? AND target = ?)`, ...pair)) {
      fail('not_found');
    }
    this.host.sql.run('DELETE FROM friend_requests WHERE (sender = ? AND target = ?) OR (sender = ? AND target = ?)', ...pair);
    return {};
  },

  'friends.remove'(att, msg) {
    const other = playerOf(msg.player);
    const were = this.areFriends(att.player, other);
    this.host.sql.run('DELETE FROM friends WHERE (player = ? AND friend = ?) OR (player = ? AND friend = ?)', att.player, other,
      other, att.player);
    this.host.sql.run('DELETE FROM friend_requests WHERE (sender = ? AND target = ?) OR (sender = ? AND target = ?)', other,
      att.player, att.player, other);
    if (were) this.notifyPlayer(other, { ev: 'friend.removed', player: att.player });
    return {};
  },

  // Client telemetry (telemetry.js): signed-in sockets only, rate limited per player and for the whole service. A store
  // that fails answers `internal` and nothing else changes.
  'telemetry.batch'(att, msg) {
    const now = this.host.now();
    const mine = takeToken(att.rt, BUCKETS.telemetry, now);
    if (!mine) {
      this.noteLimit('telemetry');
      fail('limit', 'slow down');
    }
    const all = takeToken(this.telemetryBucket, BUCKETS.telemetryAll, now);
    if (!all) {
      this.noteLimit('telemetry_all');
      fail('limit', 'slow down');
    }
    if (!this.exempt(att) && !this.guards.telemetryAccount.take(att.player, now)) {
      this.diagnose('telemetry_limited');
      fail('limit', 'slow down');
    }
    att.rt = mine;
    this.telemetryBucket = all;
    if (!this.telemetry) fail('internal', 'telemetry unavailable');
    let result;
    try {
      result = this.telemetry.ingest(this.telemetryKey(att.player), parseBatch(msg));
    } catch (error) {
      if (error instanceof ProtocolError) {
        try {
          this.telemetry.reject(error.code);
        } catch (inner) {
          this.telemetryFailed(inner);
        }
        throw error;
      }
      this.telemetryFailed(error);
      fail('internal', 'telemetry unavailable');
    }
    return { accepted: result.accepted, ignored: result.ignored, duplicates: result.duplicates };
  },

  // Anonymous party statistics (party.js): optional, sent by the party's host, so a party is counted once. Only character
  // indexes are read; the sender and the party are used to check the request and are not kept with the counts. An identical
  // repeat on the same connection is not counted again.
  'party.report'(att, msg, conn, text) {
    const report = parsePartyReport(msg, text);
    if (!report) fail('bad_request', 'party');
    const mine = att.presence ? this.partyOf(att.player) : null;
    if (!mine) fail('no_party');
    if (mine.member.slot !== 1) fail('not_host');
    if (report.characters.length > this.capacityOf(mine.party.id)) fail('bad_request', 'party');
    const signature = `${mine.party.id}:${report.characters.join('')}:${report.host ?? '-'}`;
    if (att.partyReport === signature) return { counted: false };
    this.limitPartyReport(att);
    this.track(() => this.metrics.partyReport(report));
    att.partyReport = signature;
    return { counted: true };
  },

  'party.create'(att) {
    if (!att.presence) fail('bad_request', 'presence required');
    if (this.partyOf(att.player)) fail('busy');
    return this.createParty(att);
  },

  // A member leaves; the host leaving ends the party for everyone.
  'party.leave'(att) {
    const mine = this.partyOf(att.player);
    if (!mine) return {};
    if (mine.member.slot === 1) {
      this.endParty(mine.party, 'host_left', { actor: att.player });
    } else {
      this.removeMember(mine.party, mine.member, { roster: true });
    }
    return {};
  },

  'party.kick'(att, msg) {
    const mine = this.partyOf(att.player);
    if (!mine) fail('no_party');
    if (mine.member.slot !== 1) fail('not_host');
    const target = playerOf(msg.player);
    if (target === att.player) fail('bad_request', 'own code');
    const member = this.memberOf(target);
    if (!member || member.party !== mine.party.id) fail('not_found');
    this.removeMember(mine.party, member, { tell: 'kicked', gone: 'kicked', roster: true });
    return {};
  },

  // Invites one online friend to the sender's party; it never reserves a slot.
  'invite.send'(att, msg) {
    if (!att.presence) fail('bad_request', 'presence required');
    const mine = this.partyOf(att.player);
    if (!mine) fail('no_party');
    if (mine.member.slot !== 1) fail('not_host');
    const target = playerOf(msg.player);
    if (target === att.player) fail('bad_request', 'own code');
    if (!this.areFriends(att.player, target)) fail('not_friends');
    const recipient = this.presentSockets(target)[0];
    if (!recipient) fail('offline');
    if (this.partyOf(target)) fail('busy');
    const pending = this.invitesOf(mine.party.id);
    if (pending.some((i) => i.target === target)) fail('already_invited');
    if (!compatibleGuests(recipient.att, mine.party)) fail('version_mismatch');
    if (this.membersOf(mine.party.id).length >= this.capacityOf(mine.party.id)) {
      this.recordPartyFull(att, 'invite', mine.party.diag);
      fail('party_full');
    }
    if (pending.length >= LIMITS.pendingInvites) fail('limit', 'too many pending invites');
    this.limitInvite(att);
    const now = this.host.now();
    const id = this.randomHex(8);
    this.host.sql.run('DELETE FROM party_invites WHERE party = ? AND target = ?', mine.party.id, target);
    this.host.sql.run('INSERT INTO party_invites (id, party, sender, target, created) VALUES (?, ?, ?, ?, ?)', id, mine.party.id,
      att.player, target, now);
    this.record('invite_sent', {}, mine.party.diag);
    this.changed(mine.party.id, { active: true });
    const ad = this.adOf(mine.party.id);
    this.notifyPresent(target, { ev: 'invite', invite: id, from: { player: att.player, name: att.name }, expires_ms: LIMITS.inviteMs,
      session: ad && this.liveAd(ad) ? ad.id : '', slots: this.capacityOf(mine.party.id) });
    return { invite: id };
  },

  'invite.accept'(att, msg) {
    if (!att.presence) fail('bad_request', 'presence required');
    const invite = typeof msg.invite === 'string'
      ? this.host.sql.get('SELECT * FROM party_invites WHERE id = ? AND target = ?', msg.invite, att.player) : null;
    if (!invite) fail('not_found');
    if (invite.ended === 'party_full') this.recordPartyFull(att, 'accept', this.partyRow(invite.party)?.diag ?? '');
    if (invite.ended === 'host_left' || invite.ended === 'party_full') fail(invite.ended);
    if (invite.ended) fail('not_found');
    if (invite.created + LIMITS.inviteMs <= this.host.now()) {
      this.keepDeferred = true;
      this.endInvite(invite, 'expired');
      this.changed(invite.party);
      fail('expired');
    }
    const party = this.partyRow(invite.party);
    if (!party || !this.presentSockets(party.host).length) {
      this.host.sql.run('UPDATE party_invites SET ended = ? WHERE id = ?', 'host_left', invite.id);
      fail('host_left');
    }
    if (this.partyOf(att.player)) fail('busy');
    if (!compatibleGuests(att, party)) {
      this.countReject('online_protocol', att.player);
      fail('version_mismatch');
    }
    const members = this.membersOf(party.id);
    if (members.length >= this.capacityOf(party.id)) {
      this.recordPartyFull(att, 'accept', party.diag);
      fail('party_full');
    }
    this.limitJoin(att);
    let slot = 2;
    while (members.some((m) => m.slot === slot)) slot += 1;
    const token = this.randomHex(16);
    this.host.sql.run('INSERT INTO party_members (party, slot, player, join_hash, platform, version) VALUES (?, ?, ?, ?, ?, ?)', party.id,
      slot, att.player, joinTokenHash(token), att.platform, att.version);
    this.endInvite(invite, 'accepted');
    for (const other of this.host.sql.all('SELECT * FROM party_invites WHERE target = ?', att.player)) {
      this.endInvite(other, 'accepted_elsewhere');
      if (!other.ended) this.changed(other.party);
    }
    this.endInvitesIfFull(party);
    this.host.log(`party join diag=${party.diag} match=${party.id} slot=${slot}`);
    this.record('member_joined', { slot }, party.diag);
    if (members.length + 1 === MAX_PARTY && !party.reached4) {
      this.host.sql.run('UPDATE parties SET reached4 = 1 WHERE id = ?', party.id);
      this.record('party_reached_4', {}, party.diag);
    }
    this.countJoin(party, members.length + 1);
    this.changed(party.id, { roster: true, active: true });
    return { match: party.id, diag: party.diag, slot, join_token: token, generation: 0, party: this.snapshot(this.partyRow(party.id)) };
  },

  'invite.decline'(att, msg) {
    const invite = typeof msg.invite === 'string'
      ? this.host.sql.get('SELECT * FROM party_invites WHERE id = ? AND target = ?', msg.invite, att.player) : null;
    if (!invite) return {};
    this.endInvite(invite, 'declined');
    if (!invite.ended) this.changed(invite.party, { active: true });
    return {};
  },

  // Advertises (or keeps alive) the sender's party game session to its members; the party is created when there is none.
  'pm.host'(att, msg) {
    if (!att.presence) fail('bad_request', 'presence required');
    const slots = msg.slots ?? 2;
    if (!isInt(slots, 2, MAX_PARTY)) fail('bad_request', 'slots expected');
    const rules = msg.rules ?? {};
    if (!isObject(rules) || Object.keys(rules).length > LIMITS.ruleFields) fail('bad_request', 'rules expected');
    for (const [key, value] of Object.entries(rules)) {
      if (!RULE_KEY.test(key) || !isInt(value, 0, 255)) fail('bad_request', 'rule expected');
    }
    const qos = msg.qos ?? '';
    if (typeof qos !== 'string' || !QOS_DATA.test(qos)) fail('bad_request', 'qos expected');
    const existing = this.partyOf(att.player);
    if (existing && existing.member.slot !== 1) fail('not_host');
    const created = existing ? null : this.createParty(att);
    const party = existing?.party ?? this.partyRow(created.match);
    const now = this.host.now();
    const ad = this.adOf(party.id);
    const rulesText = JSON.stringify(rules);
    if (ad) {
      const unchanged = ad.guest === att.guest && ad.build === att.build && ad.slots === slots && ad.rules === rulesText &&
        ad.qos === qos;
      if (!unchanged || now - ad.refreshed >= PROTECTION.advertiseRefreshMs) {
        this.host.sql.run('UPDATE player_matches SET guest = ?, build = ?, slots = ?, rules = ?, qos = ?, refreshed = ? WHERE id = ?',
          att.guest, att.build, slots, rulesText, qos, now, ad.id);
      }
    } else {
      if (existing) this.limitCreate(att);
      this.host.sql.run(`INSERT INTO player_matches (id, owner, guest, build, slots, rules, qos, created, refreshed, party)
        VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?)`, this.randomHex(8), att.player, att.guest, att.build, slots, rulesText, qos,
      now, now, party.id);
      this.changed(party.id, { active: true });
    }
    const session = this.adOf(party.id).id;
    return created ? { session, ...created, party: this.snapshot(this.partyRow(party.id)) } : { session };
  },

  'pm.cancel'(att) {
    const mine = this.partyOf(att.player);
    const ad = mine && mine.member.slot === 1 ? this.adOf(mine.party.id) : null;
    if (!ad) return {};
    for (const invite of this.invitesOf(mine.party.id)) this.endInvite(invite, 'cancelled');
    // Without a session nobody in the party is in a game any more.
    this.host.sql.run('UPDATE party_members SET in_session = 0 WHERE party = ?', mine.party.id);
    this.dropAd(ad);
    return {};
  },

  // A party member sees its own party's advertisement; a player in no party sees the open sessions of online friends.
  'pm.search'(att) {
    const mine = this.partyOf(att.player);
    if (mine) {
      if (mine.member.slot === 1) return { sessions: [] };
      const ad = this.adOf(mine.party.id);
      return { sessions: ad && this.liveAd(ad) && compatibleGuests(att, ad) ? [this.adView(ad)] : [] };
    }
    const sessions = [];
    for (const friend of this.friendsOf(att.player)) {
      const ad = this.host.sql.get('SELECT * FROM player_matches WHERE owner = ?', friend);
      if (ad && this.liveAd(ad) && compatibleGuests(att, ad) && this.membersOf(ad.party).length < Math.min(ad.slots, MAX_PARTY)) {
        sessions.push(this.adView(ad));
        if (sessions.length >= LIMITS.searchResults) break;
      }
    }
    return { sessions };
  },

  // A member joins the party's game session; the connection data is what it already has from the party.
  'pm.join'(att, msg) {
    if (!att.presence) fail('bad_request', 'presence required');
    const mine = this.partyOf(att.player);
    if (!mine) return this.joinFriendsSession(att, msg.session);
    const ad = typeof msg.session === 'string' ? this.adOf(mine.party.id) : null;
    if (!ad || ad.id !== msg.session) fail('not_found');
    if (mine.member.slot === 1) fail('bad_request', 'own session');
    if (!this.liveAd(ad)) {
      this.keepDeferred = true;
      this.dropAd(ad);
      fail('offline');
    }
    if (!compatibleGuests(att, ad)) {
      this.countReject('online_protocol', att.player);
      fail('version_mismatch');
    }
    if (!mine.member.in_session) {
      this.host.sql.run('UPDATE party_members SET in_session = 1 WHERE party = ? AND slot = ?', mine.party.id, mine.member.slot);
      this.changed(mine.party.id, { active: true });
    }
    return { session: ad.id, match: mine.party.id, slot: mine.member.slot, generation: mine.member.generation };
  },
};
