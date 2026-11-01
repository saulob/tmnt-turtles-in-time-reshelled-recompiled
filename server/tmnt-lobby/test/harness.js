// In-process hub and room clients for the protocol tests: no sockets, a clock the test moves.
import { Hub } from '../src/hub.js';
import { GAME_ID, PROTOCOL, encodeRelayFrame, playerIdFor } from '../src/protocol.js';
import { LocalConn, LocalHost, sqliteStore } from '../local/host.js';
import { LocalRooms } from '../local/rooms.js';

export function identity(n) {
  const token = n.toString(16).padStart(64, 'a');
  return { token, player: playerIdFor(token) };
}

export function makeHub({ local = false, db } = {}) {
  const clock = { now: 1_700_000_000_000 };
  const logs = [];
  const now = () => clock.now;
  const log = (text) => logs.push(text);
  const host = new LocalHost(sqliteStore(db), { now, log });
  const hub = new Hub(host, { local });
  const rooms = new LocalRooms({ now, log });
  rooms.install(host, hub);
  const env = {
    hub,
    host,
    rooms,
    clock,
    logs,
    advance(ms) {
      clock.now += ms;
      if (host.alarmAt !== null && host.alarmAt <= clock.now) {
        host.alarmAt = null;
        env.hub.alarm();
      }
      rooms.tick();
    },
    connect(who, options) {
      return new Client(env.hub, host, who, options);
    },
    room(match, who, options) {
      return new RoomClient(env, match, who, options);
    },
  };
  return env;
}

class Wire {
  fill(options) {
    this.inbox = [];
    this.binary = [];
    this.closedWith = null;
    this.buffered = 0;
    this.seq = 0;
    return new LocalConn({
      send: (text) => this.inbox.push(JSON.parse(text)),
      sendBinary: (bytes) => {
        if (this.failBinary) throw new Error('socket gone');
        this.binary.push(Uint8Array.from(bytes));
      },
      get bufferedAmount() {
        return options.owner.buffered;
      },
      close: (code, reason) => {
        this.closedWith = { code, reason };
      },
    });
  }

  events(name) {
    const taken = this.inbox.filter((m) => m.ev === name);
    this.inbox = this.inbox.filter((m) => m.ev !== name);
    return taken;
  }
}

export class Client extends Wire {
  constructor(hub, host, who, { ip = '203.0.113.7', guest = 'base', build = 1, name, version = '0.2.2', platform = 'Windows',
    presence = true } = {}) {
    super();
    this.hub = hub;
    this.host = host;
    this.who = who;
    this.fields = { guest, build, name: name ?? `P${who.player.slice(0, 3)}`, version, platform, presence };
    const conn = this.fill({ owner: this });
    const first = hub.admit(ip);
    this.admitted = Boolean(first);
    if (!first) return;
    this.conn = conn;
    this.conn.setAttachment(first);
    host.conns.add(this.conn);
  }

  raw(text) {
    const before = this.inbox.length;
    this.hub.message(this.conn, text);
    const replies = this.inbox.splice(before);
    const response = replies.find((m) => 're' in m);
    this.inbox.push(...replies.filter((m) => m !== response));
    return response;
  }

  call(op, body = {}) {
    return this.raw(JSON.stringify({ op, seq: ++this.seq, ...body }));
  }

  hello(extra = {}) {
    return this.call('hello', { proto: PROTOCOL, game: GAME_ID, player: this.who.player, token: this.who.token,
      ...this.fields, ...extra });
  }

  drop() {
    this.hub.closed(this.conn);
    this.host.conns.delete(this.conn);
  }
}

// One socket on a match room.
export class RoomClient extends Wire {
  constructor(env, match, who = null, { ip = '203.0.113.7' } = {}) {
    super();
    this.env = env;
    this.match = match;
    this.who = who;
    const { room, host } = env.rooms.ensure(match);
    this.room = room;
    this.host = host;
    const conn = this.fill({ owner: this });
    const first = room.admit(ip);
    this.admitted = Boolean(first);
    if (!first) return;
    this.conn = conn;
    this.conn.setAttachment(first);
    host.conns.add(this.conn);
    room.accepted();
  }

  raw(text) {
    const before = this.inbox.length;
    this.room.message(this.conn, text);
    const replies = this.inbox.splice(before);
    const response = replies.find((m) => 're' in m);
    this.inbox.push(...replies.filter((m) => m !== response));
    return response;
  }

  call(op, body = {}) {
    return this.raw(JSON.stringify({ op, seq: ++this.seq, ...body }));
  }

  // `data` is what the directory answered when this member joined (party.create / invite.accept).
  hello(data = this.data, extra = {}) {
    this.data = data;
    this.helloReply = this.call('match.hello', { proto: PROTOCOL, match: data.match, slot: data.slot, token: data.join_token,
      relay: true, generation_seen: 0, ...extra });
    return this.helloReply;
  }

  // Sends one relay frame; what this socket hears back (events such as relay.closed) is returned and stays in the inbox.
  relay(bytes) {
    const before = this.inbox.length;
    this.room.relay(this.conn, bytes);
    return this.inbox.slice(before);
  }

  frame(dst, token, seq = 1, payload = Uint8Array.from([7, 1, 2, 3, 4, 5, 6, 7, 8]), src = this.data.slot) {
    return encodeRelayFrame(src, dst, token, seq, payload);
  }

  drop() {
    this.room.closed(this.conn);
    this.host.conns.delete(this.conn);
  }
}

export function greeted(env, n, options) {
  const client = env.connect(identity(n), options);
  client.hello();
  return client;
}

export function befriend(a, b) {
  a.call('friends.add', { code: b.who.player });
  b.call('friends.accept', { player: a.who.player });
  a.inbox = [];
  b.inbox = [];
}

// A host and `size - 1` guests that joined through invites, each also connected to the room unless `connect` is false.
// `clients[i]` is the directory socket and `rooms[i]` the room socket of slot i + 1.
export function buildParty(env, size = 4, { connect = true, first = 100, relay = true } = {}) {
  const clients = [greeted(env, first)];
  const data = [clients[0].call('party.create')];
  for (let i = 1; i < size; i++) {
    const guest = greeted(env, first + i);
    befriend(clients[0], guest);
    const invite = clients[0].call('invite.send', { player: guest.who.player });
    data.push(guest.call('invite.accept', { invite: invite.invite }));
    clients.push(guest);
  }
  const match = data[0].match;
  const rooms = connect ? data.map((d, i) => {
    const room = env.room(match, clients[i].who);
    room.hello(d, { relay });
    return room;
  }) : [];
  for (const c of clients) c.inbox = [];
  for (const r of rooms) r.inbox = [];
  return { clients, data, rooms, match, token: rooms[0]?.helloReply.relay_token, diag: data[0].diag };
}
