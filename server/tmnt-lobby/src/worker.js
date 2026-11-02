// Cloudflare Worker with two SQLite-backed Durable Object classes (README.md): TMNTLobby, the one global directory of
// players, friends, presence and parties, and TMNTMatch, one object per party holding its members' sockets, endpoints
// and relay. WebSockets use the Hibernation API: "ping" is answered by the runtime without waking the object, and every
// socket's state is its attachment. Text frames are signalling; binary frames are relay frames of a match room.
// The two classes talk through one-way Durable Object RPC calls (the hub's `room` host function and the room's `hub`).
import { DurableObject } from 'cloudflare:workers';

import { handleAdmin, isAdminPath } from './admin.js';
import { BEACON_PATH, BeaconGate, beaconQuery, handleBeacon, parseBeacon } from './beacon.js';
import { handleUpgrade, limitedCount } from './edge.js';
import { Hub } from './hub.js';
import { LIMITS, PROTOCOL, RELAY, SERVICE, WORKER_BUILD } from './protocol.js';
import { MatchRoom } from './room.js';

class WorkerConn {
  constructor(ws, ctx = null) {
    this.ws = ws;
    this.ctx = ctx;
  }

  attachment() {
    return this.ws.deserializeAttachment() ?? null;
  }

  setAttachment(value) {
    try {
      this.ws.serializeAttachment(value);
    } catch {
      // A socket that already closed keeps no state.
    }
  }

  send(text) {
    this.ws.send(text);
  }

  sendBinary(bytes) {
    this.ws.send(bytes);
  }

  get bufferedAmount() {
    return this.ws.bufferedAmount;
  }

  close(code, reason) {
    this.ws.close(code, reason);
  }

  // The last "ping" the runtime answered without waking the object.
  lastActive() {
    return this.ctx?.getWebSocketAutoResponseTimestamp(this.ws)?.getTime() ?? 0;
  }
}

const sqlOf = (sql) => ({
  all: (query, ...args) => sql.exec(query, ...args).toArray(),
  get: (query, ...args) => sql.exec(query, ...args).toArray()[0],
  run: (query, ...args) => {
    sql.exec(query, ...args);
  },
});

// A one-way call into another object; a failure is logged, never awaited by the caller.
function oneWay(ctx, call, what) {
  ctx.waitUntil(Promise.resolve().then(call).catch((error) => console.log(`${what} failed: ${error?.message ?? error}`)));
}

export class TMNTLobby extends DurableObject {
  constructor(ctx, env) {
    super(ctx, env);
    ctx.setWebSocketAutoResponse(new WebSocketRequestResponsePair('ping', 'pong'));
    // Every wake builds the hub again; knowing the stored alarm keeps it from being written again each time.
    ctx.blockConcurrencyWhile(async () => {
      this.hub = new Hub({
        sql: sqlOf(ctx.storage.sql),
        sockets: () => ctx.getWebSockets().map((ws) => new WorkerConn(ws)),
        now: () => Date.now(),
        setAlarm: (ms) => ctx.storage.setAlarm(ms),
        log: (text) => console.log(text),
        room: (id, method, ...args) =>
          oneWay(ctx, () => env.MATCH.get(env.MATCH.idFromName(id))[method](...args), `room ${method}`),
      }, { alarmAt: await ctx.storage.getAlarm() });
    });
  }

  async fetch(request) {
    if (request.headers.get('Upgrade')?.toLowerCase() !== 'websocket') return this.telemetryRoute(request);
    this.hub.edgeLimited(limitedCount(request.headers));
    const first = this.hub.admit(request.headers.get('CF-Connecting-IP') ?? '');
    if (!first) return new Response('Too many connections from this address', { status: 429 });
    const [client, server] = Object.values(new WebSocketPair());
    this.ctx.acceptWebSocket(server);
    server.serializeAttachment(first);
    return new Response(null, { status: 101, webSocket: client });
  }

  // The telemetry reads of the Worker's admin routes (admin.js), which have checked the token already, and the Connection
  // Test beacon (beacon.js), which the Worker has checked and limited already.
  async telemetryRoute(request) {
    const url = new URL(request.url);
    try {
      if (url.pathname === '/telemetry/summary') return Response.json(this.hub.telemetrySummary(Number(url.searchParams.get('days'))));
      if (url.pathname === BEACON_PATH) {
        const beacon = parseBeacon(url.searchParams);
        if (!beacon) return new Response(null, { status: 400 });
        this.hub.telemetryBeacon(beacon);
        return new Response(null, { status: 204 });
      }
      if (url.pathname === '/telemetry/session') {
        const found = this.hub.telemetrySession(url.searchParams.get('diag') ?? '');
        return found ? Response.json(found) : Response.json({ error: 'not_found' }, { status: 404 });
      }
    } catch (error) {
      console.log(`telemetry read failed: ${error?.message ?? error}`);
      return Response.json({ error: 'unavailable' }, { status: 502 });
    }
    return new Response('Not found', { status: 404 });
  }

  webSocketMessage(ws, message) {
    if (typeof message !== 'string' || message.length > LIMITS.telemetryFrameBytes) {
      ws.close(1009, 'text frames up to the size limit only');
      return;
    }
    this.hub.message(new WorkerConn(ws), message);
  }

  webSocketClose(ws, code) {
    this.hub.closed(new WorkerConn(ws));
    try {
      ws.close(code === 1005 ? 1000 : code, 'closed');
    } catch {
      // Already closed.
    }
  }

  webSocketError(ws) {
    this.hub.closed(new WorkerConn(ws));
  }

  alarm() {
    this.hub.alarm();
  }

  // Reports from a match room (RPC).
  roomMember(match, slot, state, generation, seq) {
    this.hub.roomMember(match, slot, state, generation, seq);
  }

  roomLeft(match, slot, reason, seq) {
    this.hub.roomLeft(match, slot, reason, seq);
  }

  roomEnded(match, reason, seq) {
    this.hub.roomEnded(match, reason, seq);
  }

  roomEvent(diag, event, fields) {
    this.hub.roomEvent(diag, event, fields);
  }

  roomAbuse(entries) {
    this.hub.roomAbuse(entries);
  }
}

export class TMNTMatch extends DurableObject {
  constructor(ctx, env) {
    super(ctx, env);
    ctx.setWebSocketAutoResponse(new WebSocketRequestResponsePair('ping', 'pong'));
    ctx.blockConcurrencyWhile(async () => {
      this.room = new MatchRoom({
        sql: sqlOf(ctx.storage.sql),
        sockets: () => ctx.getWebSockets().map((ws) => new WorkerConn(ws, ctx)),
        now: () => Date.now(),
        setAlarm: (ms) => ctx.storage.setAlarm(ms),
        log: (text) => console.log(text),
        hub: (method, ...args) =>
          oneWay(ctx, () => env.LOBBY.get(env.LOBBY.idFromName('global'))[method](...args), `hub ${method}`),
        destroy: () => ctx.storage.deleteAll(),
      }, { alarmAt: await ctx.storage.getAlarm() });
    });
  }

  async fetch(request) {
    const first = this.room.admit(request.headers.get('CF-Connecting-IP') ?? '');
    if (!first) return new Response('Too many connections', { status: 429 });
    const [client, server] = Object.values(new WebSocketPair());
    this.ctx.acceptWebSocket(server);
    server.serializeAttachment(first);
    this.room.accepted();
    return new Response(null, { status: 101, webSocket: client });
  }

  webSocketMessage(ws, message) {
    const conn = new WorkerConn(ws, this.ctx);
    if (message instanceof ArrayBuffer) {
      this.room.relay(conn, new Uint8Array(message));
      return;
    }
    if (typeof message !== 'string' || message.length > LIMITS.frameBytes) {
      ws.close(1009, 'text frames up to the size limit only');
      return;
    }
    this.room.message(conn, message);
  }

  webSocketClose(ws, code) {
    this.room.closed(new WorkerConn(ws, this.ctx));
    try {
      ws.close(code === 1005 ? 1000 : code, 'closed');
    } catch {
      // Already closed.
    }
  }

  webSocketError(ws) {
    this.room.closed(new WorkerConn(ws, this.ctx));
  }

  alarm() {
    this.room.alarm();
  }

  // Commands from the directory (RPC).
  apply(spec) {
    this.room.apply(spec);
  }

  end(match, reason) {
    this.room.end(match, reason);
  }
}

const json = (body, status = 200) =>
  Response.json(body, { status, headers: { 'X-Content-Type-Options': 'nosniff', 'Cache-Control': 'no-store' } });

const ROOM_PATH = /^\/match\/([0-9a-f]{16})$/;
const beaconGate = new BeaconGate();

export default {
  async fetch(request, env) {
    const url = new URL(request.url);
    if (request.headers.get('Upgrade')?.toLowerCase() === 'websocket') {
      const room = ROOM_PATH.exec(url.pathname)?.[1];
      if (room) return handleUpgrade(request, env, env.MATCH.get(env.MATCH.idFromName(room)));
      return handleUpgrade(request, env, env.LOBBY.get(env.LOBBY.idFromName('global')), { reportsLimited: true });
    }
    const lobby = () => env.LOBBY.get(env.LOBBY.idFromName('global'));
    if (url.pathname === BEACON_PATH) {
      return handleBeacon(request, {
        gate: beaconGate,
        address: request.headers.get('CF-Connecting-IP') ?? '',
        record: async (beacon) => {
          const response = await lobby().fetch(new Request(`https://lobby.internal${BEACON_PATH}?${beaconQuery(beacon)}`));
          if (!response.ok) throw new Error(`lobby answered ${response.status}`);
        },
        log: (text) => console.log(text),
      });
    }
    if (isAdminPath(url.pathname)) {
      const read = async (path) => {
        const response = await lobby().fetch(new Request(`https://lobby.internal${path}`));
        if (response.status === 404) return null;
        if (!response.ok) throw new Error(`lobby answered ${response.status}`);
        return response.json();
      };
      return handleAdmin(request, env, {
        summary: (days) => read(`/telemetry/summary?days=${days}`),
        session: (diag) => read(`/telemetry/session?diag=${diag}`),
      }, (text) => console.log(text));
    }
    if (request.method === 'GET' && (url.pathname === '/' || url.pathname === '/health')) {
      return json({ service: SERVICE, protocol: PROTOCOL, status: 'ok', worker_build: WORKER_BUILD,
        relay_supported: true, relay_max_payload: RELAY.maxPayload });
    }
    return json({ error: 'not_found' }, 404);
  },
};
