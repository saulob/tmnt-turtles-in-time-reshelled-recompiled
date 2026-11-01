import assert from 'node:assert/strict';
import { after, before, test } from 'node:test';

import { startLobbyServer } from '../local/server.js';
import { GAME_ID, PROTOCOL, RELAY, SERVICE, WORKER_BUILD, encodeRelayFrame } from '../src/protocol.js';
import { identity } from './harness.js';

let server;
let base;

before(async () => {
  server = await startLobbyServer({ port: 0 });
  base = `127.0.0.1:${server.port}`;
});

after(async () => {
  await server.close();
});

class Socket {
  static async open(path = '/') {
    const s = new Socket();
    s.ws = new WebSocket(`ws://${base}${path}`);
    s.ws.binaryType = 'arraybuffer';
    s.messages = [];
    s.seq = 0;
    s.closed = null;
    s.ws.addEventListener('message', (e) => {
      if (e.data instanceof ArrayBuffer) s.messages.push({ binary: new Uint8Array(e.data) });
      else s.messages.push(e.data === 'pong' ? 'pong' : JSON.parse(e.data));
    });
    s.ws.addEventListener('close', (e) => {
      s.closed = e.code;
    });
    await new Promise((resolve, reject) => {
      s.ws.addEventListener('open', resolve, { once: true });
      s.ws.addEventListener('error', () => reject(new Error('handshake failed')), { once: true });
    });
    return s;
  }

  async next(match, timeout = 2000) {
    const deadline = Date.now() + timeout;
    for (;;) {
      const i = this.messages.findIndex(match);
      if (i >= 0) return this.messages.splice(i, 1)[0];
      if (Date.now() > deadline) throw new Error('timed out');
      await new Promise((resolve) => setTimeout(resolve, 10));
    }
  }

  async call(op, body = {}) {
    const seq = ++this.seq;
    this.ws.send(JSON.stringify({ op, seq, ...body }));
    return this.next((m) => m.re === seq);
  }

  hello(who, extra = {}) {
    return this.call('hello', { proto: PROTOCOL, game: GAME_ID, guest: 'base', build: 1, player: who.player,
      token: who.token, name: 'Net', version: '0.2.2', platform: 'Windows', ...extra });
  }

  roomHello(data, extra = {}) {
    return this.call('match.hello', { proto: PROTOCOL, match: data.match, slot: data.slot, token: data.join_token, relay: true,
      generation_seen: 0, ...extra });
  }
}

async function player(n) {
  const socket = await Socket.open();
  const who = identity(n);
  assert.equal((await socket.hello(who)).code, 'ok');
  return { socket, who };
}

async function befriend(a, b) {
  await a.socket.call('friends.add', { code: b.who.player });
  await b.socket.next((m) => m.ev === 'friend.request');
  await b.socket.call('friends.accept', { player: a.who.player });
  await a.socket.next((m) => m.ev === 'friend.added');
}

test('health endpoint reports service, protocol, status and relay support only', async () => {
  const response = await fetch(`http://${base}/health`);
  assert.equal(response.status, 200);
  assert.deepEqual(await response.json(), { service: SERVICE, protocol: PROTOCOL, status: 'ok', worker_build: WORKER_BUILD,
    relay_supported: true, relay_max_payload: RELAY.maxPayload });
  assert.equal((await fetch(`http://${base}/nope`)).status, 404);
});

test('a probe-style session says whether the protocol is compatible', async () => {
  const probe = await Socket.open();
  assert.equal((await probe.hello(identity(1), { presence: false })).code, 'ok');
  const bad = await Socket.open();
  assert.equal((await bad.call('hello', { proto: 99 })).code, 'proto');
  assert.equal((await bad.call('hello', { proto: 3 })).code, 'proto');
  assert.equal((await bad.call('ping')).code, 'no_session');
});

test('a party of four over real WebSockets: invites, rooms, endpoints, transport, relay both ways, leaving', async () => {
  const host = await player(21);
  const guests = [await player(22), await player(23), await player(24)];
  for (const g of guests) await befriend(host, g);

  const created = await host.socket.call('party.create');
  assert.equal(created.code, 'ok');
  const data = [created];
  for (const g of guests) {
    const sent = await host.socket.call('invite.send', { player: g.who.player });
    const invite = await g.socket.next((m) => m.ev === 'invite');
    assert.equal(invite.invite, sent.invite);
    data.push(await g.socket.call('invite.accept', { invite: sent.invite }));
  }
  assert.deepEqual(data.map((d) => d.slot), [1, 2, 3, 4]);
  assert.deepEqual(new Set(data.map((d) => d.match)), new Set([created.match]));

  const fifth = await player(25);
  await befriend(host, fifth);
  assert.equal((await host.socket.call('invite.send', { player: fifth.who.player })).code, 'party_full');

  const sockets = [];
  for (const d of data) {
    const s = await Socket.open(`/match/${created.match}`);
    const reply = await s.roomHello(d);
    assert.equal(reply.code, 'ok');
    assert.equal(reply.generation, 1);
    sockets.push({ s, token: reply.relay_token });
  }
  assert.equal(new Set(sockets.map((x) => x.token)).size, 1);
  const token = sockets[0].token;

  const extra = await Socket.open(`/match/${created.match}`);
  assert.equal((await extra.roomHello({ ...data[0], slot: 5 })).code, 'party_full');
  const wrong = await Socket.open(`/match/${created.match}`);
  assert.equal((await wrong.roomHello({ ...data[1], join_token: data[2].join_token })).code, 'unauthorized');
  const nowhere = await Socket.open('/match/0123456789abcdef');
  assert.equal((await nowhere.roomHello({ ...data[0], match: '0123456789abcdef' })).code, 'not_found');

  assert.equal((await sockets[0].s.call('match.endpoint', { port: 7001, lan: ['127.0.0.1'] })).code, 'ok');
  for (const x of sockets.slice(1)) {
    const peer = await x.s.next((m) => m.ev === 'match.peer');
    assert.deepEqual([peer.slot, peer.port, peer.same_network], [1, 7001, true]);
  }
  assert.equal((await sockets[1].s.call('match.transport', { peer: 3, transport: 'relay' })).code, 'ok');
  for (const i of [0, 1, 2]) assert.equal((await sockets[i].s.next((m) => m.ev === 'match.transport')).transport, 'relay');

  const toGuest = encodeRelayFrame(1, 3, token, 1, Uint8Array.from([7, 0, 0, 0, 1, 0, 0, 0, 2]));
  sockets[0].s.ws.send(toGuest);
  assert.deepEqual((await sockets[2].s.next((m) => m.binary)).binary, toGuest);
  const guestToGuest = encodeRelayFrame(3, 2, token, 1, new Uint8Array(256).fill(0x5a));
  sockets[2].s.ws.send(guestToGuest);
  assert.deepEqual((await sockets[1].s.next((m) => m.binary)).binary, guestToGuest);
  const spoof = encodeRelayFrame(1, 2, token, 1, Uint8Array.from([1]));
  sockets[2].s.ws.send(spoof);
  assert.equal((await sockets[2].s.next((m) => m.ev === 'relay.closed')).reason, 'unavailable');
  assert.equal((await sockets[1].s.call('ping')).code, 'ok');
  assert.equal(sockets[1].s.messages.filter((m) => m.binary).length, 0);

  const state = await host.socket.next((m) => m.ev === 'party.update' && m.party.members.every((x) => x.state !== 'joined'));
  assert.equal(state.party.members.length, 4);

  assert.equal((await sockets[3].s.call('match.leave')).code, 'ok');
  for (const i of [0, 1, 2]) assert.equal((await sockets[i].s.next((m) => m.ev === 'match.left')).slot, 4);
  assert.equal((await guests[2].socket.next((m) => m.ev === 'party.ended')).reason, 'closed');
  assert.equal((await host.socket.call('party.leave')).code, 'ok');
  for (const i of [1, 2]) assert.equal((await sockets[i].s.next((m) => m.ev === 'match.ended')).reason, 'host_left');
  assert.equal((await guests[0].socket.next((m) => m.ev === 'party.ended')).reason, 'host_left');
});

test('a dropped room socket keeps the seat and the same token resumes it over a new connection', async () => {
  const a = await player(51);
  const b = await player(52);
  await befriend(a, b);
  const created = await a.socket.call('party.create');
  const sent = await a.socket.call('invite.send', { player: b.who.player });
  const accepted = await b.socket.call('invite.accept', { invite: sent.invite });
  const ra = await Socket.open(`/match/${created.match}`);
  await ra.roomHello(created);
  const rb = await Socket.open(`/match/${created.match}`);
  await rb.roomHello(accepted);
  rb.ws.close();
  assert.equal((await ra.next((m) => m.ev === 'match.peer_lost')).slot, 2);
  const again = await Socket.open(`/match/${created.match}`);
  assert.equal((await again.roomHello(accepted)).generation, 2);
  assert.equal((await ra.next((m) => m.ev === 'match.peer_back')).slot, 2);
  const replaced = await Socket.open(`/match/${created.match}`);
  assert.equal((await replaced.roomHello(accepted)).generation, 3);
  for (let i = 0; i < 100 && again.closed === null; i++) await new Promise((r) => setTimeout(r, 10));
  assert.equal(again.closed, 4001);
});

test('closing a socket makes the friend go offline for the other player', async () => {
  const a = await player(31);
  const b = await player(32);
  await befriend(a, b);
  a.socket.ws.close();
  assert.equal((await b.socket.next((m) => m.ev === 'friend.offline')).player, a.who.player);
});
