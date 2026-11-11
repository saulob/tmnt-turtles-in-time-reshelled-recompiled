// Headless public-Lobby probe: two default-name accounts and a viewer.
// Run: node tools/probe.js wss://host/   (or set TMNT_LOBBY_PROBE_URL)
// It signs in three throwaway players and publishes visibility; it creates no friends, parties or matches.
import { randomBytes } from 'node:crypto';

import { GAME_ID, PROTOCOL, playerIdFor } from '../src/protocol.js';

const url = process.argv[2] ?? process.env.TMNT_LOBBY_PROBE_URL;
if (!url) {
  console.error('usage: node tools/probe.js wss://host/ | local');
  process.exit(2);
}

let server = null;
let target = url;
if (url === 'local') {
  const { startLobbyServer } = await import('../local/server.js');
  server = await startLobbyServer({ port: 0 });
  target = `ws://127.0.0.1:${server.port}/`;
}

class Client {
  static async open(name) {
    const client = new Client(name);
    client.ws = new WebSocket(target);
    client.messages = [];
    client.seq = 0;
    client.ws.addEventListener('message', (event) => {
      if (typeof event.data === 'string' && event.data !== 'pong') client.messages.push(JSON.parse(event.data));
    });
    await new Promise((resolve, reject) => {
      client.ws.addEventListener('open', resolve, { once: true });
      client.ws.addEventListener('error', () => reject(new Error(`${name}: handshake failed`)), { once: true });
    });
    const token = randomBytes(32).toString('hex');
    const player = playerIdFor(token);
    const hello = await client.call('hello', {
      proto: PROTOCOL, game: GAME_ID, guest: 'base', build: 1, player, token, name, version: 'probe', platform: 'Windows',
      presence: true,
    });
    if (hello.code !== 'ok') throw new Error(`${name}: hello refused ${hello.code}`);
    client.hello = hello;
    return client;
  }

  constructor(name) {
    this.name = name;
  }

  async next(match, timeout = 5000) {
    const deadline = Date.now() + timeout;
    for (;;) {
      const index = this.messages.findIndex(match);
      if (index >= 0) return this.messages.splice(index, 1)[0];
      if (Date.now() > deadline) throw new Error(`${this.name}: timed out`);
      await new Promise((resolve) => setTimeout(resolve, 10));
    }
  }

  async call(op, body = {}) {
    const seq = ++this.seq;
    this.ws.send(JSON.stringify({ op, seq, ...body }));
    return this.next((message) => message.re === seq);
  }

  close() {
    this.ws.close();
  }
}

const clients = [];
try {
  const a = await Client.open('Player');
  clients.push(a);
  const b = await Client.open('Player');
  clients.push(b);
  const viewer = await Client.open('ProbeViewer');
  clients.push(viewer);

  for (const client of [a, b, viewer]) {
    const reply = await client.call('lobby.publish', { visible: true });
    if (reply.code !== 'ok' || reply.visible !== true) throw new Error(`${client.name}: publish refused ${reply.code}`);
  }
  const listed = await viewer.call('lobby.list');
  if (listed.code !== 'ok' || !Array.isArray(listed.players)) throw new Error(`list refused ${listed.code}`);

  const visible = (row) => row.display_name || row.name;
  console.log(`worker_build=${a.hello.capabilities?.worker_build ?? 'unknown'}`);
  for (const row of listed.players) {
    console.log(`lobby_player name=${visible(row)} raw_name=${row.name} handle=${row.player} state=${row.state}`);
  }

  const rows = listed.players.filter((row) => visible(row) === 'Player' || /^Player-[0-9A-F]{4}$/.test(visible(row)));
  if (rows.length !== 2) throw new Error(`expected two Player rows, got ${rows.length}`);
  if (rows.some((row) => visible(row) === 'Player')) throw new Error('raw default name returned in lobby.list');
  if (visible(rows[0]) === visible(rows[1])) throw new Error('default-name aliases are identical');
  if (!/^[0-9a-f]{32}$/.test(rows[0].player) || !/^[0-9a-f]{32}$/.test(rows[1].player)) {
    throw new Error('public handle is not 32 lowercase hex');
  }
  if (rows[0].player === rows[1].player) throw new Error('public handles are identical');

  if (server) {
    const aRows = await a.call('lobby.list');
    const bRows = await b.call('lobby.list');
    const aliasA = bRows.players.find((row) => visible(row).startsWith('Player-'));
    const aliasB = aRows.players.find((row) => visible(row).startsWith('Player-'));
    if (!aliasA || !aliasB) throw new Error('could not map the two default-name rows');
    const request = await a.call('friends.add', { player: aliasB.player });
    if (request.code !== 'ok' || request.display_name !== visible(aliasB)) throw new Error('friends.add lost the alias');
    const event = await b.next((message) => message.ev === 'friend.request');
    if (event.player !== a.hello.player || event.display_name !== visible(aliasA)) {
      throw new Error('friend.request lost the alias');
    }
    const requests = await a.call('friends.requests');
    if (!requests.outgoing?.some((row) => row.player === b.hello.player && row.display_name === visible(aliasB))) {
      throw new Error('friends.requests lost the alias');
    }
    const accepted = await b.call('friends.accept', { player: a.hello.player });
    if (accepted.friend?.display_name !== visible(aliasA)) throw new Error('friends.accept lost the alias');
    const friends = await a.call('friends.list');
    if (!friends.friends?.some((row) => row.player === b.hello.player && row.display_name === visible(aliasB))) {
      throw new Error('friends.list lost the alias');
    }
    await a.call('friends.remove', { player: b.hello.player });
    console.log(`friend_request alias=${event.display_name} request_row=${visible(aliasB)} friends_list=${visible(aliasB)}`);
  }
  console.log('probe=ok');
} finally {
  for (const client of clients) client.close();
  if (server) await server.close();
}
