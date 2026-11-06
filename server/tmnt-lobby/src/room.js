// One match room: the sockets of the (up to four) members of one party, their published endpoints, the transport chosen
// for each pair and the relay between them. State lives in SQL tables and in each socket's attachment, so a hibernated
// Durable Object resumes as if it had never stopped; relay budgets and drop counters are memory only.
// The directory hub (hub.js) talks to a room with `apply` and `end`; the room reports back with roomMember, roomLeft and
// roomEnded, all one-way. The same class runs in the Durable Object (worker.js), the development server and the tests
// (local/rooms.js).
import { AbuseCounts, PROTECTION, RateLimiter, storedBudget } from './abuse.js';
import { toHex } from './sha256.js';
import {
  BUCKETS, CAPABILITIES, LIMITS, MAX_PARTY, ProtocolError, RELAY, fail, ipGroup, isInt, isLocalAddress, isObject,
  joinTokenHash, parseEndpoint, parseMatchHello, parseRelayFrame, takeRelayBudget, takeToken,
} from './protocol.js';

const HOST_SLOT = 1;
const SIGNAL_OPS = new Set(['match.endpoint', 'match.transport', 'match.link']);
const LINK_STATES = ['connecting', 'connected', 'lost'];
const TOLD_DROPS = new Set(['src_mismatch', 'dst_invalid', 'dst_unavailable', 'bad_token', 'pair_direct', 'relay_closed']);
const HASH = /^[0-9a-f]{64}$/;

const SCHEMA = [
  `CREATE TABLE IF NOT EXISTS room_meta (k TEXT PRIMARY KEY, v TEXT NOT NULL)`,
  `CREATE TABLE IF NOT EXISTS members (slot INTEGER PRIMARY KEY, name TEXT NOT NULL, hash TEXT NOT NULL,
     seen INTEGER NOT NULL DEFAULT 0, grace INTEGER, relay INTEGER NOT NULL DEFAULT 0,
     reported TEXT NOT NULL DEFAULT 'joined', ip TEXT NOT NULL DEFAULT '', port INTEGER NOT NULL DEFAULT 0,
     lan TEXT NOT NULL DEFAULT '[]', pub_ip TEXT NOT NULL DEFAULT '', pub_port INTEGER NOT NULL DEFAULT 0,
     endpoint_gen INTEGER NOT NULL DEFAULT 0)`,
  // Kept when a member leaves, so a later member of the same slot never reuses a generation an old socket still holds.
  `CREATE TABLE IF NOT EXISTS slot_generations (slot INTEGER PRIMARY KEY, generation INTEGER NOT NULL)`,
  `CREATE TABLE IF NOT EXISTS pairs (a INTEGER NOT NULL, b INTEGER NOT NULL, transport TEXT NOT NULL,
     chooser INTEGER NOT NULL, PRIMARY KEY (a, b))`,
  `CREATE TABLE IF NOT EXISTS links (slot INTEGER NOT NULL, peer INTEGER NOT NULL, state TEXT NOT NULL,
     PRIMARY KEY (slot, peer))`,
];
const MEMBER_SQL = `SELECT m.*, COALESCE(g.generation, 0) AS generation FROM members m
  LEFT JOIN slot_generations g ON g.slot = m.slot`;

const pairKey = (a, b) => (a < b ? `${a}-${b}` : `${b}-${a}`);

export class MatchRoom {
  // host: { sql: { all, get, run }, sockets(): Conn[], now(): ms, setAlarm(ms), log(text), hub(method, ...args),
  //         destroy?() }
  // options: { local, alarmAt } - alarmAt is the alarm the host already has stored, if any.
  // Conn: { send(text), sendBinary(bytes), bufferedAmount, close(code, reason), attachment(), setAttachment(object),
  //         lastActive?() }
  constructor(host, options = {}) {
    this.host = host;
    this.local = Boolean(options.local);
    this.alarmAt = options.alarmAt ?? null;
    this.depth = 0;
    this.gone = false;
    this.resetMemory();
    this.createSchema();
    this.reconcile();
  }

  // After the object restarted: a socket that did not survive sends no close, so its seat starts the usual grace here.
  reconcile() {
    if (!this.id || this.getMeta('ended') !== null) return;
    const held = new Set(this.sockets().filter((s) => s.att.hello).map((s) => s.att.slot));
    const now = this.host.now();
    for (const member of this.members()) {
      if (!member.seen || member.grace !== null || held.has(member.slot)) continue;
      this.host.sql.run('UPDATE members SET grace = ? WHERE slot = ?', now + LIMITS.graceMs, member.slot);
      this.host.sql.run('DELETE FROM links WHERE slot = ?', member.slot);
    }
    if (!this.liveSockets().length && this.num('ever') && this.getMeta('empty_since') === null) this.setMeta('empty_since', now);
    this.scheduleAlarm();
  }

  resetMemory() {
    this.matchId = null;
    this.token = null;
    this.pending = [];
    this.drops = new Map();
    this.forwards = new Map();
    this.budgets = new Map();
    this.shared = {};
    this.overruns = new Map();
    this.stalled = new Map();
    this.closedPairs = new Set();
    this.told = new Set();
    this.activity = new Map();
    this.faults = new Map();
    this.resumeGuard = new RateLimiter(PROTECTION.resumeAccount);
    this.abuse = new AbuseCounts();
    this.lastAbuseFlush = 0;
  }

  createSchema() {
    for (const statement of SCHEMA) this.host.sql.run(statement);
  }

  stats() {
    return { members: this.members().length, drops: Object.fromEntries(this.drops), ended: this.getMeta('ended') };
  }

  // ---- state helpers ----
  getMeta(key) {
    return this.host.sql.get('SELECT v FROM room_meta WHERE k = ?', key)?.v ?? null;
  }

  setMeta(key, value) {
    this.host.sql.run('INSERT INTO room_meta (k, v) VALUES (?, ?) ON CONFLICT(k) DO UPDATE SET v = excluded.v', key, String(value));
  }

  clearMeta(key) {
    this.host.sql.run('DELETE FROM room_meta WHERE k = ?', key);
  }

  num(key, fallback = 0) {
    const value = this.getMeta(key);
    return value === null ? fallback : Number(value);
  }

  get id() {
    this.matchId ??= this.getMeta('match');
    return this.matchId;
  }

  relayToken() {
    this.token ??= this.num('relay_token');
    return this.token;
  }

  members() {
    return this.host.sql.all(`${MEMBER_SQL} ORDER BY m.slot`);
  }

  member(slot) {
    return this.host.sql.get(`${MEMBER_SQL} WHERE m.slot = ?`, slot) ?? null;
  }

  pair(a, b) {
    return this.host.sql.get('SELECT * FROM pairs WHERE a = ? AND b = ?', Math.min(a, b), Math.max(a, b)) ?? null;
  }

  // The member whose current socket this is, or a refusal for a socket that was replaced or whose seat is gone.
  current(att) {
    const member = this.member(att.slot);
    if (!member || member.generation !== att.generation) fail('stale');
    return member;
  }

  bumpRevision() {
    this.setMeta('rev', this.num('rev') + 1);
  }

  sockets() {
    const list = [];
    for (const conn of this.host.sockets()) {
      const att = conn.attachment();
      if (att) list.push({ conn, att });
    }
    return list;
  }

  // The socket holding a member's seat now; none while the seat is in grace.
  socketOf(member) {
    if (member.grace !== null) return null;
    return this.sockets().find((s) => s.att.hello && s.att.slot === member.slot && s.att.generation === member.generation) ?? null;
  }

  liveSockets() {
    const seats = new Map(this.members().map((m) => [m.slot, m]));
    return this.sockets().filter((s) => {
      const seat = seats.get(s.att.slot);
      return s.att.hello && seat && seat.grace === null && seat.generation === s.att.generation;
    });
  }

  send(conn, event) {
    try {
      conn.send(JSON.stringify(event));
    } catch {
      // A socket that is closing; its close handler runs next.
    }
  }

  closeSocket(conn, code, reason) {
    try {
      conn.close(code, reason);
    } catch {
      // Already closing.
    }
  }

  broadcast(event, exceptCid = null) {
    for (const s of this.liveSockets()) if (s.att.cid !== exceptCid) this.send(s.conn, event);
  }

  touch(cid) {
    if (cid) this.activity.set(cid, this.host.now());
  }

  lastActive(s) {
    return Math.max(this.activity.get(s.att.cid) ?? 0, s.conn.lastActive?.() ?? 0, s.att.opened ?? 0);
  }

  // Everything a request does is applied before its reply goes out; events and reports to the hub follow it.
  scoped(fn) {
    if (this.gone) {
      this.gone = false;
      this.resetMemory();
      this.createSchema();
    }
    this.depth += 1;
    try {
      return fn();
    } finally {
      this.depth -= 1;
      if (this.depth === 0) {
        this.flushAbuse();
        this.flush();
        if (!this.gone) this.scheduleAlarm();
      }
    }
  }

  later(fn) {
    this.pending.push(fn);
  }

  flush() {
    while (this.pending.length) {
      const batch = this.pending;
      this.pending = [];
      for (const fn of batch) {
        try {
          fn();
        } catch (error) {
          this.host.log(`room: internal error: ${error?.stack ?? error}`);
        }
      }
    }
  }

  // Abuse counters go to the directory once per window; a room never writes them itself.
  countAbuse(name) {
    this.abuse.add(name);
  }

  flushAbuse({ force = false } = {}) {
    if (!this.abuse.pending.size) return;
    const now = this.host.now();
    if (!force && now - this.lastAbuseFlush < PROTECTION.flushMs) return;
    this.lastAbuseFlush = now;
    const entries = this.abuse.drain();
    this.later(() => this.host.hub?.('roomAbuse', entries));
  }

  tell(method, ...args) {
    const seq = this.num('seq') + 1;
    this.setMeta('seq', seq);
    this.later(() => this.host.hub(method, this.id, ...args, seq));
  }

  // An event for the party's telemetry, correlated by the party's `diag`; best effort and one-way like `tell`.
  report(event, fields) {
    try {
      this.host.hub?.('roomEvent', this.getMeta('diag') ?? '', event, fields);
    } catch {
      // Telemetry never changes what the room does.
    }
  }

  // ---- views ----
  memberState(member) {
    if (!member.seen) return 'joined';
    if (member.grace !== null) return 'disconnected';
    const links = this.host.sql.all('SELECT state FROM links WHERE slot = ?', member.slot);
    return links.length && links.every((l) => l.state === 'connected') ? 'connected' : 'connecting';
  }

  membersView() {
    const links = this.host.sql.all('SELECT * FROM links');
    const pairs = this.host.sql.all('SELECT * FROM pairs');
    return this.members().map((m) => {
      const peers = new Set([...links.filter((l) => l.slot === m.slot).map((l) => l.peer),
        ...pairs.filter((p) => p.a === m.slot || p.b === m.slot).map((p) => (p.a === m.slot ? p.b : p.a))]);
      return {
        slot: m.slot, state: this.memberState(m), name: m.name,
        links: [...peers].sort((x, y) => x - y).map((peer) => ({
          peer,
          transport: pairs.find((p) => p.a === Math.min(m.slot, peer) && p.b === Math.max(m.slot, peer))?.transport ?? 'unset',
          state: links.find((l) => l.slot === m.slot && l.peer === peer)?.state ?? 'connecting',
        })),
      };
    });
  }

  broadcastMembers(exceptCid = null) {
    this.bumpRevision();
    this.broadcast({ ev: 'match.members', members: this.membersView(), generation: this.num('rev') }, exceptCid);
  }

  // A member's LAN addresses are private and of use only behind the same public address.
  peerEvent(member, recipient) {
    const sameNetwork = member.ip === recipient.ip;
    const samePublic = Boolean(member.pub_port) && member.pub_ip === recipient.pub_ip;
    return {
      ev: 'match.peer', slot: member.slot, name: member.name, ip: member.ip, port: member.port,
      lan: sameNetwork || samePublic ? JSON.parse(member.lan) : [],
      ...(member.pub_port ? { public_ip: member.pub_ip, public_port: member.pub_port } : {}),
      same_network: sameNetwork, service_forwards_public: true,
    };
  }

  // A member's state as the hub shows it, reported only when it changes.
  reportState(slot) {
    const member = this.member(slot);
    if (!member) return;
    const state = this.memberState(member);
    if (state === member.reported) return;
    this.host.sql.run('UPDATE members SET reported = ? WHERE slot = ?', state, slot);
    this.tell('roomMember', slot, state, member.generation);
  }

  // ---- connection lifecycle ----
  admit(ip) {
    return this.scoped(() => {
      const open = this.sockets();
      if (open.length >= LIMITS.roomSockets) return null;
      if (!(this.local && isLocalAddress(ip)) &&
          open.filter((s) => ipGroup(s.att.ip) === ipGroup(ip)).length >= LIMITS.socketsPerIp) {
        this.countAbuse('ws_upgrade_ip_cap');
        return null;
      }
      const cid = toHex(crypto.getRandomValues(new Uint8Array(8)));
      this.touch(cid);
      return { cid, ip, opened: this.host.now(), hello: false, slot: 0, generation: 0 };
    });
  }

  // Called once the socket carries its attachment, so the hello deadline is scheduled.
  accepted() {
    this.scoped(() => {});
  }

  message(conn, text) {
    const att = conn.attachment();
    if (!att) return;
    this.scoped(() => {
      let seq = null;
      try {
        if (typeof text !== 'string') fail('bad_request', 'text frames only');
        if (text.length > LIMITS.frameBytes) fail('too_large');
        let msg;
        try {
          msg = JSON.parse(text);
        } catch {
          fail('bad_request', 'invalid JSON');
        }
        if (!isObject(msg) || typeof msg.op !== 'string' || !isInt(msg.seq, 0, 0x7fffffff)) fail('bad_request', 'envelope');
        seq = msg.seq;
        const now = this.host.now();
        this.touch(att.cid);
        const any = takeToken(att.ra, BUCKETS.any, now);
        if (!any) fail('limit', 'slow down');
        att.ra = any;
        if (SIGNAL_OPS.has(msg.op)) {
          const signal = takeToken(att.rs, BUCKETS.signal, now);
          if (!signal) fail('limit', 'slow down');
          att.rs = signal;
        }
        if (!Object.hasOwn(HANDLERS, msg.op)) fail('bad_request', 'unknown op');
        if (msg.op !== 'match.hello' && msg.op !== 'ping' && !att.hello) fail('no_session');
        const body = HANDLERS[msg.op].call(this, att, msg, conn) ?? {};
        conn.setAttachment(att);
        conn.send(JSON.stringify({ re: seq, code: 'ok', ...body }));
      } catch (error) {
        this.pending = [];
        conn.setAttachment(att);
        if (!(error instanceof ProtocolError)) {
          this.host.log(`room: internal error in request ${seq}: ${error?.stack ?? error}`);
          conn.send(JSON.stringify({ re: seq, code: 'internal' }));
        } else {
          conn.send(JSON.stringify({ re: seq, code: error.code }));
        }
      }
    });
  }

  ping(conn) {
    this.touch(conn.attachment()?.cid);
  }

  // Counted once the join token matched, so nobody can lock a member out of its seat by guessing at it.
  limitResume(att, slot) {
    if ((this.local && isLocalAddress(att.ip)) || this.resumeGuard.take(slot, this.host.now())) return;
    this.countAbuse('room_resume_limited');
    fail('limit', 'reconnecting too fast');
  }

  // The link budget follows the player, not the slot or the join token, so leaving and rejoining refills nothing.
  spendLinkChange(att, member) {
    if (this.local && isLocalAddress(att.ip)) return;
    const key = `link_budget_${this.getMeta(`owner_${member.slot}`) ?? member.hash}`;
    const budget = takeToken(storedBudget(this.getMeta(key)), PROTECTION.linkChange, this.host.now());
    if (!budget) {
      this.countAbuse('match_link_limited');
      fail('limit', 'link changing too fast');
    }
    this.setMeta(key, JSON.stringify(budget));
  }

  // A socket closed: the seat is held for the grace period. A socket that was replaced or removed changes nothing.
  closed(conn) {
    const att = conn.attachment();
    if (!att) return;
    this.scoped(() => {
      this.activity.delete(att.cid);
      this.budgets.delete(att.cid);
      this.faults.delete(att.cid);
      for (const key of [...this.told]) if (key.startsWith(`${att.cid}:`)) this.told.delete(key);
      const greeted = att.hello;
      att.hello = false;
      att.done = true;
      conn.setAttachment(att);
      if (!greeted) return;
      const member = this.member(att.slot);
      if (!member || member.generation !== att.generation || member.grace !== null) return;
      this.host.sql.run('UPDATE members SET grace = ? WHERE slot = ?', this.host.now() + LIMITS.graceMs, member.slot);
      this.report('room_peer_lost', { slot: member.slot });
      this.host.sql.run('DELETE FROM links WHERE slot = ?', member.slot);
      if (!this.liveSockets().length && this.num('ever')) this.setMeta('empty_since', this.host.now());
      this.later(() => {
        this.broadcast({ ev: 'match.peer_lost', slot: member.slot });
        for (const p of this.host.sql.all('SELECT * FROM pairs WHERE (a = ? OR b = ?) AND transport = ?', member.slot,
          member.slot, 'relay')) {
          const other = this.socketOf(this.member(p.a === member.slot ? p.b : p.a) ?? { slot: 0, generation: -1 });
          if (other) this.send(other.conn, { ev: 'relay.closed', peer: member.slot, reason: 'peer_disconnected' });
        }
        this.broadcastMembers();
        this.reportState(member.slot);
      });
    });
  }

  alarm() {
    this.alarmAt = null;
    this.scoped(() => {
      const now = this.host.now();
      this.flushAbuse({ force: true });
      if (this.getMeta('ended') !== null) {
        if (this.num('ended_at') + LIMITS.tombstoneMs <= now) this.destroy();
        return;
      }
      for (const s of this.sockets()) {
        if (!s.att.hello && !s.att.done && (s.att.opened ?? 0) + LIMITS.helloMs <= now) {
          this.closeSocket(s.conn, 4003, 'hello timeout');
          s.att.done = true;
          s.conn.setAttachment(s.att);
        }
      }
      for (const s of this.liveSockets()) {
        if (this.lastActive(s) + LIMITS.silenceMs <= now) {
          this.closeSocket(s.conn, 4004, 'silent');
          this.closed(s.conn);
        }
      }
      if (!this.id) {
        // A connection to a room no party ever opened leaves nothing behind.
        if (this.sockets().every((s) => s.att.done)) this.destroy();
        return;
      }
      if (this.num('created') + LIMITS.matchMaxMs <= now) return this.endRoom('expired');
      const members = this.members();
      const host = members.find((m) => m.slot === HOST_SLOT);
      if (host?.grace !== null && host?.grace <= now) return this.endRoom('expired');
      const empty = this.getMeta('empty_since');
      if (empty && Number(empty) + LIMITS.roomIdleMs <= now) return this.endRoom('expired');
      for (const m of members) if (m.grace !== null && m.grace <= now) this.removeMember(m.slot, 'disconnect');
    });
  }

  // An alarm that is already set for the same moment or earlier stays: it fires early at worst, and alarm() sets the next.
  scheduleAlarm() {
    const times = [];
    const now = this.host.now();
    if (this.abuse.pending.size) times.push(this.lastAbuseFlush + PROTECTION.flushMs);
    if (this.getMeta('ended') !== null) {
      times.push(this.num('ended_at') + LIMITS.tombstoneMs);
    } else {
      if (this.id) {
        times.push(this.num('created') + LIMITS.matchMaxMs);
        for (const m of this.members()) if (m.grace !== null) times.push(m.grace);
        const empty = this.getMeta('empty_since');
        if (empty) times.push(Number(empty) + LIMITS.roomIdleMs);
      }
      const live = new Set(this.liveSockets().map((s) => s.att.cid));
      for (const s of this.sockets()) {
        if (s.att.done) continue;
        if (!s.att.hello) times.push((s.att.opened ?? now) + LIMITS.helloMs);
        else if (live.has(s.att.cid)) times.push(this.lastActive(s) + LIMITS.silenceMs);
      }
    }
    if (!times.length) return;
    const at = Math.min(...times);
    if (this.alarmAt !== null && this.alarmAt <= at) return;
    this.host.setAlarm(at);
    this.alarmAt = at;
  }

  destroy() {
    this.alarmAt = null;
    this.resetMemory();
    this.gone = true;
    this.host.destroy?.();
  }

  // ---- commands from the hub ----
  // The full roster, newest revision wins; a member missing from it has left (`gone` says why).
  apply(spec) {
    this.scoped(() => {
      if (!isObject(spec) || typeof spec.match !== 'string' || !isInt(spec.rev, 0, Number.MAX_SAFE_INTEGER) ||
          !Array.isArray(spec.members) || spec.members.length > MAX_PARTY) {
        this.host.log('room: apply refused: malformed');
        return;
      }
      for (const m of spec.members) {
        if (!isObject(m) || !isInt(m.slot, 1, MAX_PARTY) || typeof m.name !== 'string' || !HASH.test(m.hash ?? '')) {
          this.host.log('room: apply refused: malformed member');
          return;
        }
      }
      if (this.getMeta('ended') !== null) return;
      if (!this.id) {
        const now = this.host.now();
        this.setMeta('match', spec.match);
        this.matchId = spec.match;
        this.setMeta('diag', typeof spec.diag === 'string' ? spec.diag : '');
        this.setMeta('relay_token', crypto.getRandomValues(new Uint32Array(1))[0] || 1);
        this.setMeta('created', now);
        this.setMeta('apply_rev', -1);
        this.host.log(`room open match=${spec.match} diag=${spec.diag ?? ''}`);
      } else if (spec.match !== this.id) {
        return;
      }
      if (spec.rev <= this.num('apply_rev')) return;
      this.setMeta('apply_rev', spec.rev);
      let changed = false;
      const wanted = new Set(spec.members.map((m) => m.slot));
      for (const existing of this.members()) {
        if (wanted.has(existing.slot)) continue;
        const reason = spec.gone?.[existing.slot];
        this.removeMember(existing.slot, ['kicked', 'expired'].includes(reason) ? reason : 'left', { tellHub: false });
        if (this.getMeta('ended') !== null) return;
        changed = true;
      }
      for (const m of spec.members) {
        const owner = m.owner ?? m.hash;
        if (this.getMeta(`owner_${m.slot}`) !== owner) this.setMeta(`owner_${m.slot}`, owner);
        const existing = this.member(m.slot);
        if (!existing) {
          this.host.sql.run('INSERT INTO members (slot, name, hash) VALUES (?, ?, ?)', m.slot, m.name, m.hash);
          changed = true;
        } else if (existing.name !== m.name) {
          this.host.sql.run('UPDATE members SET name = ? WHERE slot = ?', m.name, m.slot);
          changed = true;
        }
      }
      if (changed) this.later(() => this.broadcastMembers());
    });
  }

  // Also for a room that was never opened, so an `apply` that arrives late cannot bring it back.
  end(match, reason) {
    this.scoped(() => {
      if (typeof match !== 'string') return;
      if (!this.id) {
        this.setMeta('match', match);
        this.matchId = match;
        this.setMeta('created', this.host.now());
      }
      if (match === this.id) this.endRoom(['host_left', 'expired'].includes(reason) ? reason : 'closed', { tellHub: false });
    });
  }

  // ---- ending ----
  removeMember(slot, reason, { tellHub = true } = {}) {
    const member = this.member(slot);
    if (!member) return;
    if (slot === HOST_SLOT) {
      this.endRoom('host_left', { tellHub });
      return;
    }
    const sockets = this.sockets().filter((s) => s.att.hello && s.att.slot === slot);
    this.host.sql.run('DELETE FROM members WHERE slot = ?', slot);
    this.host.sql.run('DELETE FROM links WHERE slot = ? OR peer = ?', slot, slot);
    this.host.sql.run('DELETE FROM pairs WHERE a = ? OR b = ?', slot, slot);
    for (const key of [...this.closedPairs]) if (key.split('-').includes(String(slot))) this.closedPairs.delete(key);
    this.host.log(`room left match=${this.id} slot=${slot} reason=${reason}`);
    this.report('room_left', { slot, reason });
    this.later(() => {
      for (const s of sockets) {
        if (reason !== 'left') this.send(s.conn, { ev: 'match.left', slot, reason });
        this.closeSocket(s.conn, reason === 'left' ? 1000 : 4002, reason === 'left' ? 'left' : 'removed');
      }
      this.broadcast({ ev: 'match.left', slot, reason });
      this.broadcastMembers();
    });
    if (tellHub && (reason === 'left' || reason === 'disconnect')) this.tell('roomLeft', slot, reason);
  }

  endRoom(reason, { tellHub = true } = {}) {
    if (this.getMeta('ended') !== null) return;
    const sockets = this.sockets().filter((s) => s.att.hello);
    this.host.sql.run('DELETE FROM members');
    this.host.sql.run('DELETE FROM pairs');
    this.host.sql.run('DELETE FROM links');
    this.flushAbuse({ force: true });
    this.setMeta('ended', reason);
    this.setMeta('ended_at', this.host.now());
    this.clearMeta('empty_since');
    this.host.log(`room close match=${this.id} reason=${reason}`);
    this.report('room_ended', { reason, drops: [...this.drops.values()].reduce((a, b) => a + b, 0) });
    this.budgets.clear();
    this.closedPairs.clear();
    this.later(() => {
      for (const s of sockets) {
        this.send(s.conn, { ev: 'match.ended', reason });
        this.closeSocket(s.conn, 1000, 'ended');
      }
    });
    if (tellHub) this.tell('roomEnded', reason);
  }

  // ---- relay ----
  // One binary frame: forwarded unchanged to the socket of the slot it names, only when its sender, destination, token
  // and pair all check out.
  relay(conn, bytes) {
    const att = conn.attachment();
    this.touch(att?.cid);
    if (!att) return this.drop('not_joined');
    if (this.faults.get(att.cid)?.abusive) return;
    if (!att.hello) return this.senderFault(conn, att, 'not_joined');
    const frame = parseRelayFrame(bytes);
    if (frame.error) return this.senderFault(conn, att, frame.error);
    const me = this.member(att.slot);
    if (!me || me.generation !== att.generation) return this.drop('stale');
    if (frame.src !== att.slot) return this.senderFault(conn, att, 'src_mismatch', frame);
    if (frame.dst === me.slot || !isInt(frame.dst, 1, MAX_PARTY)) return this.senderFault(conn, att, 'dst_invalid', frame);
    const dst = this.member(frame.dst);
    if (!dst) return this.drop('dst_invalid', att, frame);
    const target = dst.seen && dst.grace === null ? this.socketOf(dst) : null;
    if (!target) return this.drop('dst_unavailable', att, frame);
    if (frame.token !== this.relayToken()) return this.senderFault(conn, att, 'bad_token', frame);
    const key = pairKey(frame.src, frame.dst);
    if (this.pair(frame.src, frame.dst)?.transport === 'direct') return this.drop('pair_direct', att, frame);
    if (this.closedPairs.has(key)) return this.drop('relay_closed', att, frame);
    const now = this.host.now();
    let budget = this.budgets.get(att.cid);
    if (!budget) {
      budget = {};
      this.budgets.set(att.cid, budget);
    }
    if (!takeRelayBudget(budget, RELAY.connection, now, bytes.length) ||
        !takeRelayBudget(this.shared, RELAY.match, now, bytes.length)) {
      this.drop('rate_limit', att, frame);
      const overruns = (this.overruns.get(key) ?? 0) + 1;
      this.overruns.set(key, overruns);
      if (overruns >= RELAY.overrunsToClose) {
        this.countAbuse('relay_abuse_closed');
        this.closePair(frame.src, frame.dst, 'rate_limit');
      }
      return;
    }
    const buffered = target.conn.bufferedAmount;
    if (typeof buffered === 'number' && buffered > RELAY.bufferedBytes) {
      // A moment of congestion loses frames, as a network would; only a peer that stays behind ends the pair.
      this.drop('peer_backpressure', att, frame);
      const stalled = (this.stalled.get(key) ?? 0) + 1;
      this.stalled.set(key, stalled);
      if (stalled >= RELAY.overrunsToClose) this.closePair(frame.src, frame.dst, 'peer_backpressure');
      return;
    }
    this.stalled.delete(key);
    try {
      target.conn.sendBinary(bytes);
    } catch {
      this.closePair(frame.src, frame.dst, 'peer_unavailable');
      return;
    }
    const direction = `${frame.src}>${frame.dst}`;
    const forwards = (this.forwards.get(direction) ?? 0) + 1;
    this.forwards.set(direction, forwards);
    if (forwards <= RELAY.loggedForwards) {
      this.host.log(`relay forward match=${this.id} src=${frame.src} dst=${frame.dst} bytes=${bytes.length}`);
    }
  }

  // Counted by reason, the first few of each logged (never the payload). A sender whose frame could not be routed hears
  // it once per destination.
  drop(reason, att = null, frame = null) {
    const count = (this.drops.get(reason) ?? 0) + 1;
    this.drops.set(reason, count);
    if (count <= RELAY.loggedDrops) this.host.log(`relay drop reason=${reason} match=${this.id ?? '-'} count=${count}`);
    if (!att || !TOLD_DROPS.has(reason)) return;
    const told = `${att.cid}:${frame.dst}`;
    if (this.told.has(told)) return;
    if (this.told.size >= 1024) this.told.clear();
    this.told.add(told);
    const own = this.sockets().find((s) => s.att.cid === att.cid);
    if (own) this.send(own.conn, { ev: 'relay.closed', peer: frame.dst, reason: 'unavailable' });
  }

  // Drops the frame; a sender that keeps producing frames no well-behaved client sends loses its connection, and only it.
  senderFault(conn, att, reason, frame = null) {
    this.drop(reason, att, frame);
    const state = this.faults.get(att.cid) ?? { bucket: null, abusive: false };
    state.bucket = takeToken(state.bucket, RELAY.invalid, this.host.now());
    this.faults.set(att.cid, state);
    if (state.bucket) return;
    state.abusive = true;
    this.countAbuse('relay_abuse_closed');
    this.host.log(`relay sender closed match=${this.id ?? '-'} reason=${reason}`);
    this.closeSocket(conn, 1008, 'relay abuse');
  }

  // One pair's relay ends; the other pairs of the room are untouched. It comes back when either end reconnects.
  closePair(a, b, reason) {
    const key = pairKey(a, b);
    if (this.closedPairs.has(key)) return;
    this.closedPairs.add(key);
    this.overruns.delete(key);
    this.stalled.delete(key);
    this.host.log(`relay close match=${this.id} pair=${key} reason=${reason}`);
    this.report('relay_closed', { a: Math.min(a, b), b: Math.max(a, b), reason });
    for (const [slot, peer] of [[a, b], [b, a]]) {
      const member = this.member(slot);
      const s = member && this.socketOf(member);
      if (s) this.send(s.conn, { ev: 'relay.closed', peer, reason });
    }
  }

  reopenPairs(slot) {
    for (const key of [...this.closedPairs]) {
      if (key.split('-').includes(String(slot))) {
        this.closedPairs.delete(key);
        this.overruns.delete(key);
      }
    }
  }
}

const slotArg = (value) => {
  if (!isInt(value, 1, MAX_PARTY)) fail('bad_request', 'slot expected');
  return value;
};

const HANDLERS = {
  'match.hello'(att, msg, conn) {
    if (att.hello) fail('bad_request', 'already greeted');
    const hello = parseMatchHello(msg);
    if (this.getMeta('ended') !== null || hello.match !== this.id) fail('not_found');
    const roster = this.members();
    const member = roster.find((m) => m.slot === hello.slot);
    if (!member) fail(roster.length >= MAX_PARTY ? 'party_full' : 'not_found');
    if (member.hash !== joinTokenHash(hello.token)) fail('unauthorized');
    this.limitResume(att, member.slot);
    const generation = member.generation + 1;
    if (hello.generationSeen > member.generation) {
      this.host.log(`room hello match=${this.id} slot=${member.slot} generation_seen=${hello.generationSeen} ahead of ${member.generation}`);
    }
    this.host.sql.run(`INSERT INTO slot_generations (slot, generation) VALUES (?, ?)
      ON CONFLICT(slot) DO UPDATE SET generation = excluded.generation`, member.slot, generation);
    for (const old of this.sockets()) {
      if (old.att.hello && old.att.slot === member.slot && old.att.cid !== att.cid) this.closeSocket(old.conn, 4001, 'replaced');
    }
    const wasLost = member.seen && member.grace !== null;
    if (!member.seen || member.grace !== null || member.relay !== (hello.relay ? 1 : 0)) {
      this.host.sql.run('UPDATE members SET seen = 1, grace = NULL, relay = ? WHERE slot = ?', hello.relay ? 1 : 0, member.slot);
    }
    this.host.sql.run('DELETE FROM links WHERE slot = ?', member.slot);
    this.reopenPairs(member.slot);
    this.clearMeta('empty_since');
    if (this.getMeta('ever') === null) this.setMeta('ever', 1);
    Object.assign(att, { hello: true, slot: member.slot, generation });
    conn.setAttachment(att);
    this.host.log(`room hello match=${this.id} slot=${member.slot} generation=${generation}`);
    this.report('room_joined', { slot: member.slot, generation, reconnect: wasLost ? 1 : 0 });
    this.later(() => {
      if (wasLost) this.broadcast({ ev: 'match.peer_back', slot: member.slot }, att.cid);
      this.broadcastMembers(att.cid);
      for (const other of this.members()) {
        if (other.slot === member.slot || !other.endpoint_gen || other.endpoint_gen !== other.generation) continue;
        if (this.socketOf(other)) this.send(conn, this.peerEvent(other, { ip: att.ip, pub_ip: member.pub_ip }));
      }
      this.reportState(member.slot);
    });
    return { generation, relay_token: this.relayToken(), members: this.membersView(), capabilities: CAPABILITIES };
  },

  ping() {
    return { time: this.host.now() };
  },

  'match.endpoint'(att, msg, conn) {
    const member = this.current(att);
    if (member.endpoint_gen === member.generation) fail('already_set');
    const endpoint = parseEndpoint(msg);
    this.host.sql.run(`UPDATE members SET ip = ?, port = ?, lan = ?, pub_ip = ?, pub_port = ?, endpoint_gen = ? WHERE slot = ?`,
      att.ip, endpoint.port, JSON.stringify(endpoint.lan), endpoint.publicIp, endpoint.publicPort, member.generation,
      member.slot);
    this.later(() => {
      const fresh = this.member(member.slot);
      for (const s of this.liveSockets()) {
        if (s.att.slot === member.slot) continue;
        const other = this.member(s.att.slot);
        this.send(s.conn, this.peerEvent(fresh, { ip: s.att.ip, pub_ip: other?.pub_ip }));
        // Its LAN addresses were held back until this member's public address showed they share a network.
        const owes = other?.endpoint_gen === other?.generation && other?.pub_port && other.pub_ip === fresh.pub_ip && other.ip !== att.ip;
        if (owes) this.send(conn, this.peerEvent(other, { ip: att.ip, pub_ip: fresh.pub_ip }));
      }
    });
    return {};
  },

  // Only the lower slot of a pair chooses, once. The pair, and the host when it is not one of them, hear the result.
  'match.transport'(att, msg) {
    const member = this.current(att);
    const peer = slotArg(msg.peer);
    if (msg.transport !== 'direct' && msg.transport !== 'relay') fail('bad_request', 'transport expected');
    if (peer === member.slot) fail('bad_request', 'own slot');
    const other = this.member(peer);
    if (!other) fail('not_found');
    if (member.slot > peer) fail('not_owner');
    if (this.pair(member.slot, peer)) fail('already_set');
    if (msg.transport === 'relay' && !(member.relay && other.relay && this.socketOf(other))) fail('relay_unavailable');
    this.host.sql.run('INSERT INTO pairs (a, b, transport, chooser) VALUES (?, ?, ?, ?)', member.slot, peer, msg.transport,
      member.slot);
    this.host.log(`relay transport selected match=${this.id} a=${member.slot} b=${peer} transport=${msg.transport}`);
    this.report('room_transport', { a: member.slot, b: peer, transport: msg.transport });
    this.later(() => {
      const event = { ev: 'match.transport', a: member.slot, b: peer, transport: msg.transport, by: member.slot };
      for (const s of this.liveSockets()) {
        if (s.att.slot === member.slot || s.att.slot === peer || s.att.slot === HOST_SLOT) this.send(s.conn, event);
      }
      this.broadcastMembers();
    });
    return {};
  },

  'match.link'(att, msg) {
    const member = this.current(att);
    const peer = slotArg(msg.peer);
    if (!LINK_STATES.includes(msg.state)) fail('bad_request', 'state expected');
    if (peer === member.slot) fail('bad_request', 'own slot');
    if (!this.member(peer)) fail('not_found');
    const known = this.host.sql.get('SELECT state FROM links WHERE slot = ? AND peer = ?', member.slot, peer);
    if (known?.state === msg.state) return {};
    // A link's first report is free: a reconnect clears them all and the client sends them again.
    if (known) this.spendLinkChange(att, member);
    this.host.sql.run(`INSERT INTO links (slot, peer, state) VALUES (?, ?, ?)
      ON CONFLICT(slot, peer) DO UPDATE SET state = excluded.state`, member.slot, peer, msg.state);
    this.later(() => {
      this.broadcastMembers();
      this.reportState(member.slot);
    });
    return {};
  },

  'match.leave'(att) {
    this.removeMember(this.current(att).slot, 'left');
    return {};
  },
};
