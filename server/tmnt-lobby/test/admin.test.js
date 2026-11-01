import assert from 'node:assert/strict';
import { createHash } from 'node:crypto';
import { after, before, test } from 'node:test';
import { runInNewContext } from 'node:vm';

import { adminEnabled, handleAdmin, isAdminPath, summaryDays, tokenMatches } from '../src/admin.js';
import { ADMIN_CSP, ADMIN_HTML, ADMIN_JS } from '../src/admin_page.js';
import { GAME_ID, PROTOCOL } from '../src/protocol.js';
import { startLobbyServer } from '../local/server.js';
import { greeted, identity, makeHub } from './harness.js';

const TOKEN = 'correct-horse-battery-staple';
const ENV = { ADMIN_TOKEN: TOKEN };
const DIAG = 'abcdef012345';
const PAYLOAD = '<img src=x onerror=alert(1)>"</td><script>alert(2)</script>';

const get = (path, headers = {}, method = 'GET') => new Request(`https://lobby.example${path}`, { method, headers });
const bearer = (token) => ({ Authorization: `Bearer ${token}` });
const quiet = () => {};

function populated() {
  const env = makeHub();
  const a = greeted(env, 1);
  a.call('telemetry.batch', {
    diag: DIAG,
    ctx: { game: GAME_ID, proto: 4, version: '0.4.2', platform: 'Windows', os: '10', arch: 'x64', fp: '9f2c41aa7be0d311', slot: 1,
      host: true, party: 2 },
    session: { dur_ms: 5, ct_usable: true, worker: true, stun: true, direct: true, relay: true, compat: 'ok', reason: 'host_left' },
    peers: [{ ls: 1, rs: 2, state: 'connected', transport: 'relay', direct: 'failed', relay: 'ok', fallback: 'direct_timeout', conn_ms: 1,
      est_ms: 9, ping: { sent: 5, recv: 5, last: 40, min: 30, max: 50, avg: 40, jit: 3, to: 0, ooo: 0, dup: 0, loss: 0 }, tx: 1, rx: 1,
      reconn: 0, reason: '' }],
    events: [{ e: 'relay_fallback', t: 1, f: { reason: 'direct_timeout' } }],
  });
  return env;
}

const source = (env) => ({
  summary: async (days) => env.hub.telemetrySummary(days),
  session: async (diag) => env.hub.telemetrySession(diag),
});

test('the admin routes answer 404 until an admin token of enough length is set', async () => {
  const env = populated();
  for (const config of [{}, { ADMIN_TOKEN: '' }, { ADMIN_TOKEN: 'short' }, { ADMIN_TOKEN: undefined }, undefined]) {
    for (const path of ['/admin', '/admin/', '/admin/telemetry/summary', `/admin/telemetry/session?diag=${DIAG}`, '/admin/other']) {
      const response = await handleAdmin(get(path, bearer(TOKEN)), config, source(env), quiet);
      assert.equal(response.status, 404, path);
    }
  }
  assert.equal(adminEnabled({ ADMIN_TOKEN: TOKEN }), true);
  assert.equal(adminEnabled({ ADMIN_TOKEN: 'x'.repeat(15) }), false);
});

test('the JSON routes need the exact bearer token', async () => {
  const env = populated();
  const paths = ['/admin/telemetry/summary', `/admin/telemetry/session?diag=${DIAG}`];
  const rejected = [{}, { Authorization: TOKEN }, { Authorization: `Basic ${TOKEN}` }, bearer(''), bearer('wrong'), bearer(`${TOKEN}x`),
    bearer(TOKEN.slice(0, -1)), bearer(TOKEN.toUpperCase()), { Authorization: `bearer ${TOKEN}` }];
  for (const path of paths) {
    for (const headers of rejected) {
      const response = await handleAdmin(get(path, headers), ENV, source(env), quiet);
      assert.equal(response.status, 401, `${path} ${JSON.stringify(headers)}`);
      assert.equal(response.headers.get('WWW-Authenticate'), 'Bearer');
      assert.deepEqual(await response.json(), { error: 'unauthorized' });
    }
    const response = await handleAdmin(get(path, bearer(TOKEN)), ENV, source(env), quiet);
    assert.equal(response.status, 200);
    assert.match(response.headers.get('Content-Type'), /^application\/json/);
    assert.equal(response.headers.get('Cache-Control'), 'no-store');
  }
  assert.equal(tokenMatches(`Bearer ${TOKEN}`, TOKEN), true);
  assert.equal(tokenMatches(`Bearer ${TOKEN}`, 'short'), false);
});

test('the summary and the session route validate their parameters and report failures', async () => {
  const env = populated();
  const ok = async (path) => handleAdmin(get(path, bearer(TOKEN)), ENV, source(env), quiet);
  const summary = await (await ok('/admin/telemetry/summary')).json();
  assert.equal(summary.days, 7);
  assert.equal(summary.totals.sessions, 1);
  assert.equal((await (await ok('/admin/telemetry/summary?days=30')).json()).days, 30);
  for (const days of ['0', '31', '-1', 'x', '1.5', '007', '']) {
    assert.equal((await ok(`/admin/telemetry/summary?days=${days}`)).status, 400, days);
  }
  assert.equal(summaryDays(new URLSearchParams('days=3')), 3);
  const session = await (await ok(`/admin/telemetry/session?diag=${DIAG}`)).json();
  assert.equal(session.diag, DIAG);
  assert.equal(session.reporters[0].peers[0].transport, 'relay');
  for (const diag of ['', 'xyz', 'ABCDEF012345', `${DIAG}0`, '%27%20OR%201=1']) {
    assert.equal((await ok(`/admin/telemetry/session?diag=${diag}`)).status, 400, diag);
  }
  assert.equal((await ok('/admin/telemetry/session?diag=000000000000')).status, 404);
  for (const path of ['/admin/telemetry', '/admin/telemetry/sql', '/admin/api/summary', '/admin/telemetry/summary/x']) {
    assert.equal((await ok(path)).status, 404, path);
  }
  assert.equal((await handleAdmin(get('/admin/telemetry/summary', bearer(TOKEN), 'POST'), ENV, source(env), quiet)).status, 405);

  const logs = [];
  const broken = { summary: async () => { throw new Error('store is down'); }, session: async () => { throw new Error('store is down'); } };
  const failed = await handleAdmin(get('/admin/telemetry/summary', bearer(TOKEN)), ENV, broken, (t) => logs.push(t));
  assert.equal(failed.status, 502);
  assert.deepEqual(await failed.json(), { error: 'unavailable' });
  assert.equal(logs.length, 1);
  assert.equal((await handleAdmin(get(`/admin/telemetry/session?diag=${DIAG}`, bearer(TOKEN)), ENV, broken, quiet)).status, 502);
});

test('the summary and the session never carry a friend code, a token, an address or a match id', () => {
  const env = populated();
  const text = JSON.stringify([env.hub.telemetrySummary(7), env.hub.telemetrySession(DIAG)]);
  assert.doesNotMatch(text, /[A-Z0-9]{4}-[A-Z0-9]{4}/);
  assert.doesNotMatch(text, /[0-9a-f]{32,}/);
  assert.doesNotMatch(text, /\d+\.\d+\.\d+\.\d+/);
  assert.ok(!text.includes(identity(1).player));
});

test('the dashboard is one self-contained page under a strict policy', async () => {
  const env = populated();
  const response = await handleAdmin(get('/admin'), ENV, source(env), quiet);
  assert.equal(response.status, 200);
  assert.match(response.headers.get('Content-Type'), /^text\/html/);
  assert.equal(response.headers.get('Content-Security-Policy'), ADMIN_CSP);
  assert.equal(await response.text(), ADMIN_HTML);
  assert.equal((await handleAdmin(get('/admin/'), ENV, source(env), quiet)).status, 200);

  const hashes = [...ADMIN_HTML.matchAll(/<(script|style)>([\s\S]*?)<\/\1>/g)]
    .map((m) => `'sha256-${createHash('sha256').update(m[2]).digest('base64')}'`);
  assert.equal(hashes.length, 2);
  for (const hash of hashes) assert.ok(ADMIN_CSP.includes(hash), hash);
  assert.match(ADMIN_CSP, /default-src 'none'/);
  assert.doesNotMatch(ADMIN_CSP, /unsafe-/);
  assert.doesNotMatch(ADMIN_HTML, /<script[^>]*\ssrc=|<link\b|<img\b|<iframe|<form[^>]*action/i);
  assert.doesNotMatch(ADMIN_HTML, /https?:\/\/|\/\/cdn|@import|url\(/i);
  assert.doesNotMatch(ADMIN_JS, /innerHTML|outerHTML|insertAdjacentHTML|document\.write|\beval\b|new Function|setAttribute\(\s*['"]style/);
  assert.doesNotMatch(ADMIN_JS, /localStorage|console\.log/);
  assert.equal(isAdminPath('/admin'), true);
  assert.equal(isAdminPath('/administrator'), false);
});

class FakeNode {
  constructor(tag, id = '') {
    Object.assign(this, { tag, id, children: [], listeners: {}, text: '', className: '', hidden: false, value: '', checked: false,
      title: '', type: '' });
  }

  get firstChild() { return this.children[0] ?? null; }

  get textContent() { return this.text; }

  set textContent(value) {
    this.text = String(value);
    this.children = [];
  }

  appendChild(child) { this.children.push(child); return child; }

  removeChild(child) { this.children = this.children.filter((c) => c !== child); return child; }

  addEventListener(type, fn) { (this.listeners[type] ??= []).push(fn); }

  set innerHTML(value) { throw new Error(`innerHTML set to ${value}`); }

  set outerHTML(value) { throw new Error(`outerHTML set to ${value}`); }

  insertAdjacentHTML() { throw new Error('insertAdjacentHTML used'); }

  setAttribute() {}

  focus() {}

  scrollIntoView() {}

  all() { return [this, ...this.children.flatMap((c) => c.all())]; }
}

async function runDashboard(summary, detail) {
  const nodes = Object.fromEntries(['login', 'token', 'remember', 'dash', 'range', 'refresh', 'status', 'signout', 'login-error', 'cards',
    'out', 'detail'].map((id) => [id, new FakeNode('div', id)]));
  nodes.range.value = '7';
  nodes.dash.hidden = true;
  const requests = [];
  const sandbox = {
    document: { getElementById: (id) => nodes[id], createElement: (tag) => new FakeNode(tag), hidden: false },
    sessionStorage: { getItem: () => '', setItem() {}, removeItem() {} },
    fetch: async (path, options) => {
      requests.push({ path, options });
      const body = path.includes('/session') ? detail : summary;
      return { status: 200, ok: true, json: async () => body };
    },
    setInterval: () => 1,
    clearInterval() {},
    encodeURIComponent,
    Date,
    Number,
    String,
    Object,
    Array,
    Promise,
    Error,
    Math,
  };
  runInNewContext(ADMIN_JS, sandbox);
  nodes.token.value = TOKEN;
  nodes.login.listeners.submit[0]({ preventDefault() {} });
  const settle = async () => {
    for (let i = 0; i < 5; i++) await new Promise((resolve) => setImmediate(resolve));
  };
  await settle();
  return { nodes, requests, settle };
}

test('the dashboard script writes every value as text, never as markup', async () => {
  const env = populated();
  const summary = JSON.parse(JSON.stringify(env.hub.telemetrySummary(7)));
  summary.sessions_by.platform = [{ value: PAYLOAD, count: 3 }];
  summary.fallback_reasons = [{ value: PAYLOAD, count: 2 }];
  summary.server.party_ended = [{ value: PAYLOAD, count: 1 }];
  summary.reasons.fallback = { [PAYLOAD]: 4 };
  summary.connection_beacon.failures = [{ value: PAYLOAD, count: 2 }];
  summary.recent_sessions = [{ diag: PAYLOAD, last_ms: 1, reporters: 1, events: 1, first_ms: 1, platforms: [PAYLOAD], ended: PAYLOAD }];
  const detail = JSON.parse(JSON.stringify(env.hub.telemetrySession(DIAG)));
  detail.diag = PAYLOAD;
  detail.reporters[0].ctx.platform = PAYLOAD;
  detail.reporters[0].peers[0].reason = PAYLOAD;
  detail.events[0].name = PAYLOAD;
  detail.events[0].fields = { [PAYLOAD]: PAYLOAD };

  const { nodes, requests, settle } = await runDashboard(summary, detail);
  assert.equal(nodes.login.hidden, true);
  assert.equal(nodes.dash.hidden, false);
  assert.equal(requests[0].path, '/admin/telemetry/summary?days=7');
  assert.equal(JSON.stringify(requests[0].options.headers), JSON.stringify({ Authorization: `Bearer ${TOKEN}` }));
  assert.equal(requests[0].options.credentials, 'omit');
  const texts = nodes.out.all().map((n) => n.text);
  assert.ok(texts.includes(PAYLOAD), 'the hostile value is shown as plain text');
  assert.ok(texts.some((t) => t.includes('Sessions by platform')));
  assert.ok(nodes.cards.all().some((n) => n.text === 'Parties started'));
  assert.ok(texts.includes('Latency percentiles (per peer link)'));
  for (const title of ['Fallback / path reasons', 'Disconnect reasons', 'Invite outcomes', 'Service errors', 'Connection Test failures',
    'Connection Tests']) {
    assert.ok(texts.includes(title), title);
  }
  assert.ok(texts.includes('parties reaching 4 of 4'));

  const link = nodes.out.all().find((n) => n.tag === 'button' && n.text === PAYLOAD);
  assert.ok(link);
  link.listeners.click[0]();
  await settle();
  assert.equal(requests.at(-1).path, `/admin/telemetry/session?diag=${encodeURIComponent(PAYLOAD)}`);
  assert.equal(nodes.detail.hidden, false);
  assert.ok(nodes.detail.all().some((n) => n.text === `Session ${PAYLOAD}`));
  assert.ok(nodes.detail.all().some((n) => n.text.includes(PAYLOAD)));
});

let server;
let base;

before(async () => {
  server = await startLobbyServer({ port: 0, adminToken: TOKEN });
  base = `127.0.0.1:${server.port}`;
});

after(async () => {
  await server.close();
});

test('the development server serves the dashboard and the routes, and takes telemetry over a real WebSocket', async () => {
  const page = await fetch(`http://${base}/admin`);
  assert.equal(page.status, 200);
  assert.match(await page.text(), /Online Telemetry/);
  assert.equal((await fetch(`http://${base}/admin/telemetry/summary`)).status, 401);
  assert.equal((await fetch(`http://${base}/admin/telemetry/summary`, { headers: bearer('nope') })).status, 401);
  assert.equal((await fetch(`http://${base}/health`)).status, 200);

  const who = identity(7);
  const ws = new WebSocket(`ws://${base}/`);
  const inbox = [];
  ws.addEventListener('message', (e) => {
    if (e.data !== 'pong') inbox.push(JSON.parse(e.data));
  });
  await new Promise((resolve, reject) => {
    ws.addEventListener('open', resolve, { once: true });
    ws.addEventListener('error', () => reject(new Error('handshake failed')), { once: true });
  });
  let seq = 0;
  const call = async (op, body) => {
    const mine = ++seq;
    ws.send(JSON.stringify({ op, seq: mine, ...body }));
    for (let i = 0; i < 200; i++) {
      const found = inbox.findIndex((m) => m.re === mine);
      if (found >= 0) return inbox.splice(found, 1)[0];
      await new Promise((resolve) => setTimeout(resolve, 10));
    }
    throw new Error('timed out');
  };
  assert.equal((await call('hello', { proto: PROTOCOL, game: GAME_ID, guest: 'base', build: 1, player: who.player, token: who.token,
    name: 'Net', version: '0.2.2', platform: 'Windows' })).code, 'ok');
  const wide = 'z'.repeat(48);
  const events = Array.from({ length: 12 }, (_, i) => ({ e: 'direct_result', t: i,
    f: { k0: wide, k1: wide, k2: wide, k3: wide, k4: wide, k5: wide, k6: wide, k7: String(i) } }));
  const body = { diag: DIAG, ctx: { game: GAME_ID, proto: 4, version: '0.4.2', platform: 'Windows', os: '10', arch: 'x64', fp: 'ab',
    slot: 1, host: true, party: 1 }, events };
  assert.ok(JSON.stringify(body).length > 4096);
  const reply = await call('telemetry.batch', body);
  assert.deepEqual([reply.code, reply.accepted], ['ok', 12]);
  assert.equal(ws.readyState, WebSocket.OPEN);
  ws.close();

  const summary = await (await fetch(`http://${base}/admin/telemetry/summary?days=1`, { headers: bearer(TOKEN) })).json();
  assert.equal(summary.totals.client_events, 12);
  const session = await (await fetch(`http://${base}/admin/telemetry/session?diag=${DIAG}`, { headers: bearer(TOKEN) })).json();
  assert.equal(session.events.length, 12);
});

test('without an admin token the development server has no admin routes', async () => {
  const bare = await startLobbyServer({ port: 0 });
  try {
    for (const path of ['/admin', '/admin/telemetry/summary']) {
      const response = await fetch(`http://127.0.0.1:${bare.port}${path}`, { headers: bearer(TOKEN) });
      assert.equal(response.status, 404, path);
    }
  } finally {
    await bare.close();
  }
});
