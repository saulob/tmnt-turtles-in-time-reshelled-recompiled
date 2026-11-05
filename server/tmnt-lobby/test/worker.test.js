// The Worker and its two Durable Object classes, run in Node with a stand-in for the Cloudflare runtime: the glue that
// the other tests do not reach (routing, the SQL adapter, hibernated sockets, the RPC between the objects).
import assert from 'node:assert/strict';
import { registerHooks } from 'node:module';
import { DatabaseSync } from 'node:sqlite';
import { test } from 'node:test';

import { COUNTRY_HEADER, edgeCounts, withEdgeLimited } from '../src/edge.js';
import { GAME_ID, PROTOCOL, encodeRelayFrame } from '../src/protocol.js';
import { identity } from './harness.js';

const skip = typeof registerHooks !== 'function' ? 'node:module registerHooks is not available' : false;

class FakeWs {
  constructor() {
    this.att = null;
    this.sent = [];
    this.closedWith = null;
    this.bufferedAmount = 0;
  }

  serializeAttachment(value) {
    this.att = structuredClone(value);
  }

  deserializeAttachment() {
    return this.att ? structuredClone(this.att) : null;
  }

  send(data) {
    this.sent.push(typeof data === 'string' ? JSON.parse(data) : { binary: Uint8Array.from(data) });
  }

  close(code, reason) {
    this.closedWith = { code, reason };
  }
}

class FakeResponse {
  constructor(body, init = {}) {
    this.body = body;
    this.status = init.status ?? 200;
    this.webSocket = init.webSocket;
    this.headers = new Map(Object.entries(init.headers ?? {}));
  }

  get ok() {
    return this.status >= 200 && this.status < 300;
  }

  async json() {
    return JSON.parse(this.body);
  }

  async text() {
    return this.body;
  }

  static json(body, init = {}) {
    return new FakeResponse(JSON.stringify(body), init);
  }
}

function fakeContext(waits) {
  const db = new DatabaseSync(':memory:');
  const sockets = [];
  const alarms = [];
  let ready = Promise.resolve();
  return {
    sockets,
    alarms,
    get ready() {
      return ready;
    },
    ctx: {
      storage: {
        sql: {
          exec: (query, ...args) => {
            const rows = db.prepare(query).all(...args).map((r) => ({ ...r }));
            return { toArray: () => rows };
          },
        },
        setAlarm(ms) {
          alarms.push(ms);
        },
        getAlarm: async () => alarms.at(-1) ?? null,
        deleteAll() {},
      },
      blockConcurrencyWhile: (fn) => {
        ready = fn();
        return ready;
      },
      setWebSocketAutoResponse() {},
      getWebSocketAutoResponseTimestamp: () => null,
      getWebSockets: () => sockets,
      acceptWebSocket: (ws) => sockets.push(ws),
      waitUntil: (promise) => waits.push(promise),
    },
  };
}

let sequence = 0;

async function setup() {
  Object.assign(globalThis, {
    Response: FakeResponse,
    WebSocketPair: class {
      constructor() {
        this[0] = new FakeWs();
        this[1] = new FakeWs();
      }
    },
    WebSocketRequestResponsePair: class {},
  });
  const unique = `?${++sequence}`;
  const { TMNTLobby, TMNTMatch, default: worker } = await import(`../src/worker.js${unique}`);
  const waits = [];
  const objects = { LOBBY: new Map(), MATCH: new Map() };
  const env = {};
  const binding = (kind, Class) => ({
    idFromName: (name) => name,
    get: (name) => {
      // Like a stub: the object only exists once something is called on it.
      const instance = () => {
        if (!objects[kind].has(name)) {
          const context = fakeContext(waits);
          objects[kind].set(name, { object: new Class(context.ctx, env), context });
        }
        return objects[kind].get(name).object;
      };
      return new Proxy({}, { get: (_, method) => (...args) => Promise.resolve().then(async () => {
        const object = instance();
        await objects[kind].get(name).context.ready;
        return object[method](...args);
      }) });
    },
  });
  env.LOBBY = binding('LOBBY', TMNTLobby);
  env.MATCH = binding('MATCH', TMNTMatch);
  const settle = async () => {
    for (let i = 0; i < 5; i++) {
      await new Promise((resolve) => setImmediate(resolve));
      await Promise.all(waits.splice(0));
    }
  };
  const open = async (path) => {
    const response = await worker.fetch(new Request(`https://lobby.example${path}`, { headers: { Upgrade: 'websocket' } }), env);
    assert.equal(response.status, 101);
    return response;
  };
  const lobby = () => objects.LOBBY.get('global');
  return { worker, env, objects, settle, open, lobby, classes: { TMNTLobby, TMNTMatch } };
}

function directory(ctxOwner, who, n) {
  const ws = ctxOwner.context.sockets.at(-1);
  let seq = 0;
  return {
    ws,
    who,
    n,
    call(op, body = {}) {
      const before = ws.sent.length;
      ctxOwner.object.webSocketMessage(ws, JSON.stringify({ op, seq: ++seq, ...body }));
      return ws.sent.slice(before).find((m) => m.re === seq);
    },
    events: (name) => ws.sent.filter((m) => m.ev === name),
  };
}

let hooked = false;

function stubCloudflare() {
  if (hooked) return;
  hooked = true;
  registerHooks({
    resolve(specifier, context, next) {
      if (specifier === 'cloudflare:workers') {
        return { url: 'data:text/javascript,export class DurableObject { constructor(ctx, env) { this.ctx = ctx; this.env = env; } }',
          shortCircuit: true };
      }
      return next(specifier, context);
    },
  });
}

test('routes, the SQL adapter and the RPC between the directory and a match room', { skip }, async () => {
  stubCloudflare();
  const quiet = console.log;
  console.log = () => {};
  const rig = await setup();

  const health = await rig.worker.fetch(new Request('https://lobby.example/health'), rig.env);
  assert.equal(JSON.parse(health.body).protocol, PROTOCOL);
  assert.equal((await rig.worker.fetch(new Request('https://lobby.example/nope'), rig.env)).status, 404);

  const open = async (n) => {
    await rig.open('/');
    const owner = rig.lobby();
    const player = directory(owner, identity(n), n);
    const reply = player.call('hello', { proto: PROTOCOL, game: GAME_ID, guest: 'base', build: 1, player: player.who.player,
      token: player.who.token, name: `W${n}`, version: '0.2.2', platform: 'Windows' });
    assert.equal(reply.code, 'ok');
    return player;
  };
  const host = await open(1);
  const guest = await open(2);
  host.call('friends.add', { code: guest.who.player });
  guest.call('friends.accept', { player: host.who.player });

  const created = host.call('party.create');
  const sent = host.call('invite.send', { player: guest.who.player });
  const joined = guest.call('invite.accept', { invite: sent.invite });
  assert.deepEqual([created.code, joined.code, joined.slot, joined.match], ['ok', 'ok', 2, created.match]);
  await rig.settle();
  assert.equal(rig.objects.MATCH.has(created.match), true);

  const connectRoom = async (data) => {
    await rig.open(`/match/${data.match}`);
    const room = rig.objects.MATCH.get(data.match);
    const ws = room.context.sockets.at(-1);
    let seq = 0;
    const call = (op, body = {}) => {
      const before = ws.sent.length;
      room.object.webSocketMessage(ws, JSON.stringify({ op, seq: ++seq, ...body }));
      return ws.sent.slice(before).find((m) => m.re === seq);
    };
    const reply = call('match.hello', { proto: PROTOCOL, match: data.match, slot: data.slot, token: data.join_token, relay: true,
      generation_seen: 0 });
    return { ws, call, reply, room };
  };
  const rh = await connectRoom(created);
  const rg = await connectRoom(joined);
  assert.equal(rh.reply.code, 'ok');
  assert.equal(rg.reply.code, 'ok');
  assert.equal(rh.reply.relay_token, rg.reply.relay_token);
  await rig.settle();
  assert.equal(host.events('party.update').at(-1).party.members[1].state, 'connecting');

  const frame = encodeRelayFrame(1, 2, rh.reply.relay_token, 1, Uint8Array.from([1, 2, 3]));
  rh.room.object.webSocketMessage(rh.ws, frame.buffer.slice(frame.byteOffset, frame.byteOffset + frame.byteLength));
  assert.deepEqual(rg.ws.sent.at(-1).binary, frame);

  rg.room.object.webSocketClose(rg.ws, 1006);
  await rig.settle();
  assert.equal(host.events('party.update').at(-1).party.members[1].state, 'disconnected');
  assert.equal(rh.ws.sent.filter((m) => m.ev === 'match.peer_lost').length, 1);

  rh.call('match.leave');
  await rig.settle();
  assert.equal(host.events('party.ended').at(-1).reason, 'host_left');
  assert.equal(guest.events('party.ended').at(-1).reason, 'host_left');
  assert.equal(rig.lobby().object.hub.stats().parties, 0);
  const worldwide = rig.lobby().object.hub.telemetrySummary(1);
  assert.equal(worldwide.server.parties_started, 1);
  assert.deepEqual(worldwide.server.room_ended.map((r) => r.value), ['host_left']);

  const wrong = await rig.open('/match/zz');
  assert.equal(wrong.status, 101);
  assert.equal(rig.objects.MATCH.has('zz'), false);
  console.log = quiet;
});

test('the Worker takes telemetry frames up to the batch limit and serves the admin routes only with the secret', { skip }, async () => {
  stubCloudflare();
  const quiet = console.log;
  console.log = () => {};
  const rig = await setup();
  await rig.open('/');
  const player = directory(rig.lobby(), identity(5), 5);
  assert.equal(player.call('hello', { proto: PROTOCOL, game: GAME_ID, guest: 'base', build: 1, player: player.who.player,
    token: player.who.token, name: 'W5', version: '0.2.2', platform: 'Windows' }).code, 'ok');
  const wide = 'z'.repeat(48);
  const events = Array.from({ length: 12 }, (_, i) => ({ e: 'direct_result', t: i, f: { k0: wide, k1: wide, k2: wide, k3: wide, k4: wide,
    k5: wide, k6: wide, k7: String(i) } }));
  const body = { diag: 'abcdef012345', ctx: { game: GAME_ID, proto: 4, version: '0.4.2', platform: 'Windows', os: '10', arch: 'x64',
    fp: 'ab', slot: 1, host: true, party: 1 }, events };
  assert.ok(JSON.stringify(body).length > 4096);
  const reply = player.call('telemetry.batch', body);
  assert.deepEqual([reply.code, reply.accepted, player.ws.closedWith], ['ok', 12, null]);

  const request = (path, headers = {}) => new Request(`https://lobby.example${path}`, { headers });
  const secret = 'a-long-enough-admin-secret';
  for (const path of ['/admin', '/admin/telemetry/summary', '/admin/telemetry/session?diag=abcdef012345']) {
    assert.equal((await rig.worker.fetch(request(path, { Authorization: `Bearer ${secret}` }), rig.env)).status, 404, path);
  }
  rig.env.ADMIN_TOKEN = secret;
  assert.equal((await rig.worker.fetch(request('/admin'), rig.env)).status, 200);
  assert.equal((await rig.worker.fetch(request('/admin/telemetry/summary'), rig.env)).status, 401);
  assert.equal((await rig.worker.fetch(request('/admin/telemetry/summary', { Authorization: 'Bearer wrong' }), rig.env)).status, 401);
  const summary = await rig.worker.fetch(request('/admin/telemetry/summary?days=1', { Authorization: `Bearer ${secret}` }), rig.env);
  assert.equal(summary.status, 200);
  assert.equal((await summary.json()).totals.client_events, 12);
  const session = await rig.worker.fetch(request('/admin/telemetry/session?diag=abcdef012345', { Authorization: `Bearer ${secret}` }), rig.env);
  assert.equal((await session.json()).events.length, 12);
  const missing = await rig.worker.fetch(request('/admin/telemetry/session?diag=000000000000', { Authorization: `Bearer ${secret}` }), rig.env);
  assert.equal(missing.status, 404);
  console.log = quiet;
});

test('the Worker serves the metrics routes only with the token, and keeps them off the public surface', { skip }, async () => {
  stubCloudflare();
  const quiet = console.log;
  console.log = () => {};
  const rig = await setup();
  await rig.open('/');
  const player = directory(rig.lobby(), identity(6), 6);
  assert.equal(player.call('hello', { proto: PROTOCOL, game: GAME_ID, guest: 'base', build: 1, player: player.who.player,
    token: player.who.token, name: 'W6', version: '0.2.2', platform: 'Windows' }).code, 'ok');
  const secret = 'a-long-enough-admin-secret';
  rig.env.ADMIN_TOKEN = secret;
  const get = (path, headers = {}) => rig.worker.fetch(new Request(`https://lobby.example${path}`, { headers }), rig.env);
  const auth = { Authorization: `Bearer ${secret}` };
  for (const path of ['/admin/api/summary', '/admin/api/history?days=365']) {
    assert.equal((await get(path)).status, 401, path);
    assert.equal((await get(path, auth)).status, 200, path);
  }
  assert.equal((await (await get('/admin/api/summary', auth)).json()).today.unique_players, 1);
  assert.equal((await (await get('/admin/api/history?days=7', auth)).json()).daily.at(-1).sessions, 1);
  assert.equal((await get('/admin/api/history?days=366', auth)).status, 400);
  for (const path of ['/metrics/summary', '/metrics/history?days=7', '/party.report', '/telemetry/party']) {
    assert.equal((await get(path, auth)).status, 404, path);
  }
  console.log = quiet;
});

test('the country is the edge\'s: a client cannot supply it, and only the directory keeps it', { skip }, async () => {
  stubCloudflare();
  const rig = await setup();
  const upgrade = async (ip, cf) => {
    const request = new Request('https://lobby.example/', { headers: { Upgrade: 'websocket', 'CF-Connecting-IP': ip, [COUNTRY_HEADER]: 'ZZ' } });
    if (cf) Object.defineProperty(request, 'cf', { value: cf });
    assert.equal((await rig.worker.fetch(request, rig.env)).status, 101);
    return rig.lobby().context.sockets.at(-1).deserializeAttachment().country;
  };
  assert.equal(await upgrade('203.0.113.8', { country: 'BR' }), 'BR');
  assert.equal(await upgrade('203.0.113.9', { country: 'XX' }), 'Unknown');
  assert.equal(await upgrade('203.0.113.10'), 'Unknown');
  assert.equal(withEdgeLimited(new Request('https://lobby.example/', { headers: { [COUNTRY_HEADER]: 'ZZ' } })).headers.get(COUNTRY_HEADER), 'Unknown');
});

test('the Worker answers the Connection Test beacon without a session, limits it per address, and the directory counts it', { skip }, async () => {
  stubCloudflare();
  const quiet = console.log;
  console.log = () => {};
  const rig = await setup();
  const query = new URLSearchParams({ diag: 'abcdef012345', ver: '0.4.2', plat: 'Windows', os: '10', arch: 'x64', proto: '4', worker: 'ok',
    ws: 'ok', ms: '90', stun: 'failed', direct: '0', relay: '1', usable: '1', fail: '', router: 'nat' }).toString();
  const get = (text, headers = {}, method = 'GET') => rig.worker.fetch(new Request(`https://lobby.example/telemetry/test?${text}`,
    { method, headers }), rig.env);

  const first = await get(query, { 'CF-Connecting-IP': '203.0.113.5' });
  assert.deepEqual([first.status, first.body], [204, null]);
  assert.equal((await get(`${query}&worker=bogus`)).status, 400);
  assert.equal((await get('')).status, 400);
  assert.equal((await get(query, {}, 'POST')).status, 405);
  assert.equal((await rig.worker.fetch(new Request('https://lobby.example/telemetry/other'), rig.env)).status, 404);

  const block = () => rig.lobby().object.hub.telemetrySummary(1).connection_beacon;
  assert.deepEqual([block().total, block().usable], [1, 1]);
  assert.deepEqual(block().stun, [{ value: 'failed', count: 1 }]);
  assert.equal(block().latency_ms.p50, 90);

  const codes = [];
  for (let i = 0; i < 8; i++) codes.push((await get(query, { 'CF-Connecting-IP': '203.0.113.5' })).status);
  assert.deepEqual(codes, [204, 204, 204, 204, 204, 429, 429, 429]);
  assert.equal((await get(query, { 'CF-Connecting-IP': '203.0.113.6' })).status, 204);
  assert.equal(block().total, 7);

  const broken = rig.lobby().object.hub;
  broken.telemetry = new Proxy({}, { get: () => () => { throw new Error('store is down'); } });
  assert.equal((await get(query, { 'CF-Connecting-IP': '203.0.113.7' })).status, 204);
  console.log = quiet;
});

test('the Worker refuses upgrades at the edge before any object exists, counts them, and leaves other routes alone', { skip }, async () => {
  stubCloudflare();
  const quiet = console.log;
  console.log = () => {};
  const rig = await setup();
  edgeCounts.limited = 0;
  let allow = false;
  const keys = [];
  rig.env.WS_UPGRADE_LIMITER = { limit: async ({ key }) => {
    keys.push(key);
    return { success: allow };
  } };
  rig.env.ADMIN_TOKEN = 'a-long-enough-admin-secret';
  const upgrade = (path, headers = {}) => rig.worker.fetch(new Request(`https://lobby.example${path}`,
    { headers: { Upgrade: 'websocket', 'CF-Connecting-IP': '203.0.113.7', ...headers } }), rig.env);

  for (const path of ['/', '/match/0123456789abcdef']) {
    const refused = await upgrade(path);
    assert.equal(refused.status, 429, path);
    assert.equal(refused.headers.get('Retry-After'), '60', path);
  }
  assert.deepEqual(keys, ['ws:203.0.113.7', 'ws:203.0.113.7']);
  assert.deepEqual([rig.objects.LOBBY.size, rig.objects.MATCH.size], [0, 0]);

  assert.equal((await rig.worker.fetch(new Request('https://lobby.example/health'), rig.env)).status, 200);
  assert.equal((await rig.worker.fetch(new Request('https://lobby.example/'), rig.env)).status, 200);
  assert.equal((await rig.worker.fetch(new Request('https://lobby.example/admin'), rig.env)).status, 200);
  assert.equal(rig.objects.LOBBY.size, 0);

  allow = true;
  assert.equal((await upgrade('/', { 'X-TMNT-Edge-Limited': '9999' })).status, 101);
  const counts = () => Object.fromEntries(rig.lobby().object.hub.telemetrySummary(3).abuse.map((a) => [a.value, a.count]));
  assert.deepEqual(counts(), { ws_upgrade_accepted: 1, ws_upgrade_limited: 2 });
  assert.equal((await upgrade('/')).status, 101);
  assert.equal(counts().ws_upgrade_limited, 2);

  const summary = await rig.worker.fetch(new Request('https://lobby.example/admin/telemetry/summary?days=1',
    { headers: { Authorization: 'Bearer a-long-enough-admin-secret' } }), rig.env);
  assert.deepEqual((await summary.json()).abuse.map((a) => a.value).sort(), ['ws_upgrade_accepted', 'ws_upgrade_limited']);

  rig.env.WS_UPGRADE_LIMITER = { limit: async () => { throw new Error('limiter down'); } };
  assert.equal((await upgrade('/')).status, 101);
  delete rig.env.WS_UPGRADE_LIMITER;
  assert.equal((await upgrade('/')).status, 101);
  console.log = quiet;
});

test('a woken object reuses its stored alarm, and a room reports its abuse counters to the directory', { skip }, async () => {
  stubCloudflare();
  const quiet = console.log;
  console.log = () => {};
  const rig = await setup();
  await rig.open('/');
  const owner = rig.lobby();
  const player = directory(owner, identity(1), 1);
  assert.equal(player.call('hello', { proto: PROTOCOL, game: GAME_ID, guest: 'base', build: 1, player: player.who.player,
    token: player.who.token, name: 'W1', version: '0.2.2', platform: 'Windows' }).code, 'ok');
  const created = player.call('party.create');
  assert.equal(created.code, 'ok');
  await rig.settle();
  assert.ok(owner.context.alarms.length > 0);

  const written = owner.context.alarms.length;
  new rig.classes.TMNTLobby(owner.context.ctx, rig.env);
  await rig.settle();
  assert.equal(owner.context.alarms.length, written);
  const room = rig.objects.MATCH.get(created.match);
  const roomWrites = room.context.alarms.length;
  assert.ok(roomWrites > 0);
  const wokenRoom = new rig.classes.TMNTMatch(room.context.ctx, rig.env);
  await rig.settle();
  wokenRoom.apply({ match: created.match, rev: 0, members: [] });
  assert.equal(room.context.alarms.length, roomWrites);

  owner.object.roomAbuse([['relay_abuse_closed', 2], ['made_up', 5], ['room_resume_limited', 'many']]);
  const counts = Object.fromEntries(owner.object.hub.telemetrySummary(3).abuse.map((a) => [a.value, a.count]));
  assert.equal(counts.relay_abuse_closed, 2);
  assert.equal(counts.made_up, undefined);
  assert.equal(counts.room_resume_limited, undefined);
  console.log = quiet;
});
