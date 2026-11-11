#!/usr/bin/env node
// Development service: the Worker's directory hub and match rooms over node:http and node:sqlite, without Cloudflare.
//   node server/tmnt-lobby/local/server.js [--host 127.0.0.1] [--port 8791] [--db <file>|:memory:] [--verbose]
//                                           [--admin-token <16+ characters>]   (or TMNT_ADMIN_TOKEN)
// Instances reach it with TMNT_LOBBY=ws://127.0.0.1:8791/ in their environment. Loopback clients are exempt from the
// per-address limits. With a --db file, players and friendships survive restarts; parties, rooms and presence never do.
// With an admin token, /admin serves the private dashboard, and /admin/api/* and /admin/telemetry/* its JSON (admin.js).
import { createServer } from 'node:http';
import { pathToFileURL } from 'node:url';
import { parseArgs } from 'node:util';

import { adminEnabled, handleAdmin, isAdminPath } from '../src/admin.js';
import { BEACON_PATH, BeaconGate, handleBeacon } from '../src/beacon.js';
import { Hub } from '../src/hub.js';
import { LIMITS, PROTOCOL, RELAY, SERVICE, WORKER_BUILD } from '../src/protocol.js';
import { LocalConn, LocalHost, sqliteStore } from './host.js';
import { LocalRooms } from './rooms.js';
import { upgrade } from './ws.js';

const describe = (text) => text.replace(/"(token|join_token)":"[0-9a-f]+"/g, '"$1":"<hidden>"').slice(0, 300);
const ROOM_PATH = /^\/match\/([0-9a-f]{16})$/;

export function startLobbyServer({ host: address = '127.0.0.1', port = 8791, db = ':memory:', verbose = false,
  log = () => {}, adminToken = '', telemetry } = {}) {
  const host = new LocalHost(sqliteStore(db), { log });
  const rooms = new LocalRooms({ log, timers: true });
  const hub = new Hub(host, { local: true, telemetry });
  rooms.install(host, hub);
  host.onAlarm = () => hub.alarm();
  let stopped = false;

  const beaconGate = new BeaconGate({ exemptLoopback: true });

  const adminSource = {
    summary: async (days) => hub.telemetrySummary(days),
    session: async (diag) => hub.telemetrySession(diag),
    metricsSummary: async () => hub.metricsSummary(),
    metricsHistory: async (days) => hub.metricsHistory(days),
    incidents: async () => hub.incidentList(),
    incident: async (ticket) => hub.incident(ticket),
    setIncidentStatus: async (ticket, change) => hub.setIncidentStatus(ticket, change),
  };

  async function send(res, response) {
    res.statusCode = response.status;
    for (const [name, value] of response.headers) res.setHeader(name, value);
    res.end(response.status === 204 ? undefined : await response.text());
  }

  const requestOf = (req) => new Request(new URL(req.url, 'http://local'), { method: req.method, headers: req.headers });

  async function admin(req, res) {
    await send(res, await handleAdmin(requestOf(req), { ADMIN_TOKEN: adminToken }, adminSource, log));
  }

  async function beacon(req, res) {
    await send(res, await handleBeacon(requestOf(req), {
      gate: beaconGate,
      address: req.socket.remoteAddress?.replace(/^::ffff:/, '') ?? '',
      record: async (checked) => hub.telemetryBeacon(checked),
      log,
    }));
  }

  const server = createServer((req, res) => {
    const path = new URL(req.url, 'http://local').pathname;
    if (path === BEACON_PATH) {
      beacon(req, res).catch((error) => {
        log(`beacon: ${error?.message ?? error}`);
        res.statusCode = 204;
        res.end();
      });
      return;
    }
    if (isAdminPath(path)) {
      admin(req, res).catch((error) => {
        log(`admin: ${error?.message ?? error}`);
        res.statusCode = 500;
        res.end();
      });
      return;
    }
    const found = path === '/' || path === '/health';
    res.statusCode = found ? 200 : 404;
    res.setHeader('Content-Type', 'application/json');
    res.end(JSON.stringify(found ? { service: SERVICE, protocol: PROTOCOL, status: 'ok', worker_build: WORKER_BUILD,
      relay_supported: true, relay_max_payload: RELAY.maxPayload } : { error: 'not_found' }));
  });

  // The sockets of one match room, as the Worker routes them: /match/<16 hex>.
  function upgradeRoom(req, socket, id) {
    const ip = req.socket.remoteAddress?.replace(/^::ffff:/, '') ?? '';
    const { room, host: roomHost } = rooms.ensure(id);
    const first = room.admit(ip);
    if (!first) return socket.end('HTTP/1.1 429 Too Many Requests\r\nConnection: close\r\n\r\n');
    let conn = null;
    const transport = upgrade(req, socket, {
      maxMessage: LIMITS.telemetryFrameBytes * 2,
      onText(text) {
        if (stopped) return;
        if (text === 'ping') {
          room.ping(conn);
          return transport.send('pong');
        }
        if (verbose) log(`<- room ${first.cid} ${describe(text)}`);
        room.message(conn, text);
      },
      onBinary(bytes) {
        if (!stopped) room.relay(conn, bytes);
      },
      onClose() {
        if (!conn || stopped) return;
        room.closed(conn);
        roomHost.conns.delete(conn);
      },
    });
    if (!transport) return socket.end('HTTP/1.1 400 Bad Request\r\nConnection: close\r\n\r\n');
    conn = new LocalConn(transport);
    conn.setAttachment(first);
    roomHost.conns.add(conn);
    room.accepted();
  }

  server.on('upgrade', (req, socket) => {
    const roomId = ROOM_PATH.exec(new URL(req.url, 'http://local').pathname)?.[1];
    if (roomId) return upgradeRoom(req, socket, roomId);
    const ip = req.socket.remoteAddress?.replace(/^::ffff:/, '') ?? '';
    const first = hub.admit(ip);
    if (!first) return socket.end('HTTP/1.1 429 Too Many Requests\r\nConnection: close\r\n\r\n');
    let conn = null;
    const transport = upgrade(req, socket, {
      maxMessage: LIMITS.telemetryFrameBytes * 2,
      onText(text) {
        if (stopped) return;
        if (text === 'ping') return transport.send('pong');
        if (verbose) log(`<- ${first.cid} ${describe(text)}`);
        hub.message(conn, text);
      },
      onClose() {
        if (!conn || stopped) return;
        hub.closed(conn);
        host.conns.delete(conn);
      },
    });
    if (!transport) return socket.end('HTTP/1.1 400 Bad Request\r\nConnection: close\r\n\r\n');
    const send = transport.send;
    transport.send = (text) => {
      if (verbose) log(`-> ${first.cid} ${describe(text)}`);
      send(text);
    };
    conn = new LocalConn(transport);
    conn.setAttachment(first);
    host.conns.add(conn);
  });

  return new Promise((resolve) => {
    server.listen(port, address, () => {
      resolve({
        port: server.address().port,
        hub,
        host,
        rooms,
        close: () => new Promise((done) => {
          stopped = true;
          if (host.timer) clearTimeout(host.timer);
          for (const conn of host.conns) conn.close(1001, 'server stopping');
          rooms.close();
          server.close(() => {
            host.sql.close();
            done();
          });
          server.closeAllConnections?.();
        }),
      });
    });
  });
}

if (process.argv[1] && import.meta.url === pathToFileURL(process.argv[1]).href) {
  const { values } = parseArgs({
    options: {
      host: { type: 'string', default: '127.0.0.1' },
      port: { type: 'string', default: '8791' },
      db: { type: 'string', default: ':memory:' },
      verbose: { type: 'boolean', default: false },
      'admin-token': { type: 'string' },
    },
  });
  const log = (text) => console.log(`${new Date().toISOString().slice(11, 23)} ${text}`);
  const started = await startLobbyServer({ host: values.host, port: Number(values.port), db: values.db,
    verbose: values.verbose, log, adminToken: values['admin-token'] ?? process.env.TMNT_ADMIN_TOKEN ?? '' });
  log(`tmnt lobby on ws://${values.host}:${started.port}/ (database ${values.db}); Ctrl+C stops it`);
  if (adminEnabled({ ADMIN_TOKEN: values['admin-token'] ?? process.env.TMNT_ADMIN_TOKEN })) {
    log(`telemetry dashboard on http://${values.host}:${started.port}/admin`);
  }
}
