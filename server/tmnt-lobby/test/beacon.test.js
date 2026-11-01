import assert from 'node:assert/strict';
import { after, before, test } from 'node:test';

import { BEACON_PATH, BeaconGate, beaconQuery, handleBeacon, parseBeacon } from '../src/beacon.js';
import { startLobbyServer } from '../local/server.js';
import { makeHub } from './harness.js';

const DIAG = 'abcdef012345';
const ADDRESS = '203.0.113.99';

const valid = (over = {}) => ({ diag: DIAG, ver: '0.4.2', plat: 'Windows', os: '10.0.26200', arch: 'x64', proto: '4', sproto: '4',
  worker: 'ok', ws: 'ok', ms: '120', stun: 'ok', direct: '1', relay: '1', usable: '1', fail: '', router: 'nat', ...over });

const qs = (params) => new URLSearchParams(Object.entries(params).filter(([, v]) => v !== undefined));

const requestOf = (params, method = 'GET') => {
  const query = new URLSearchParams(Object.entries(params).filter(([, v]) => v !== undefined)).toString();
  return new Request(`https://lobby.example${BEACON_PATH}?${query}`, { method });
};

function rig({ specs, maxAddresses, store } = {}) {
  const env = makeHub();
  const logs = [];
  const gate = new BeaconGate({ now: () => env.clock.now, specs, maxAddresses });
  const record = store ?? (async (beacon) => env.hub.telemetryBeacon(beacon));
  const call = (params, { method = 'GET', address = ADDRESS } = {}) => handleBeacon(requestOf(params, method),
    { gate, record, address, log: (text) => logs.push(text) });
  const rows = () => ['tel_counters', 'tel_latency', 'tel_events', 'tel_sessions'].flatMap((t) => env.host.sql.all(`SELECT * FROM ${t}`));
  const counters = () => env.host.sql.all("SELECT k, n FROM tel_counters WHERE k LIKE 'ct.%'");
  return { env, gate, call, logs, rows, counters };
}

const values = (list) => Object.fromEntries(list.map((i) => [i.value, i.count]));

test('a valid beacon is answered 204 and counted in the daily counters and the summary', async () => {
  const { env, call, counters } = rig();
  const response = await call(valid());
  assert.equal(response.status, 204);
  assert.equal(response.body, null);
  assert.equal(response.headers.get('Cache-Control'), 'no-store');
  const failed = await call(valid({ usable: '0', worker: 'unreachable', ws: 'skipped', stun: 'off', direct: '0', relay: '0',
    fail: 'worker_unreachable', router: 'none', plat: 'SteamDeck', ver: '0.4.3', ms: undefined, sproto: undefined }));
  assert.equal(failed.status, 204);
  await call(valid({ ms: '300', plat: 'Linux' }));
  await call(valid({ ms: '200', fail: 'not_usable', usable: '0' }));

  const keys = Object.fromEntries(counters().map((r) => [r.k, r.n]));
  assert.equal(keys['ct.total'], 4);
  assert.equal(keys['ct.usable'], 2);
  assert.equal(keys['ct.unusable'], 2);
  assert.equal(keys['ct.fail.worker_unreachable'], 1);
  assert.equal(keys['ct.fail.not_usable'], 1);
  assert.equal(keys['ct.worker.ok'], 3);
  assert.equal(keys['ct.worker.unreachable'], 1);
  assert.equal(keys['ct.platform.Windows'], 2);

  const block = env.hub.telemetrySummary(1).connection_beacon;
  assert.deepEqual([block.total, block.usable, block.unusable, block.direct_available, block.relay_available], [4, 2, 2, 3, 3]);
  assert.deepEqual(values(block.failures), { worker_unreachable: 1, not_usable: 1 });
  assert.deepEqual(values(block.worker), { ok: 3, unreachable: 1 });
  assert.deepEqual(values(block.ws), { ok: 3, skipped: 1 });
  assert.deepEqual(values(block.stun), { ok: 3, off: 1 });
  assert.deepEqual(values(block.router), { nat: 3, none: 1 });
  assert.deepEqual(values(block.platform), { Windows: 2, Linux: 1, SteamDeck: 1 });
  assert.deepEqual(values(block.version), { '0.4.2': 3, '0.4.3': 1 });
  assert.deepEqual([block.latency_ms.n, block.latency_ms.p50, block.latency_ms.max], [3, 200, 300]);
  assert.equal(env.hub.telemetrySummary(1).totals.sessions, 0);
});

test('the parameters are checked against fixed enumerations; the parsed beacon is exactly what was sent', () => {
  const parsed = parseBeacon(qs(valid({ usable: '0', fail: 'signin_refused', sproto: '5' })));
  assert.deepEqual(parsed, { diag: DIAG, ver: '0.4.2', plat: 'Windows', os: '10.0.26200', arch: 'x64', proto: 4, sproto: 5, worker: 'ok',
    ws: 'ok', ms: 120, stun: 'ok', direct: true, relay: true, usable: false, fail: 'signin_refused', router: 'nat' });
  assert.deepEqual(parseBeacon(new URLSearchParams(beaconQuery(parsed))), parsed);
  const minimal = parseBeacon(qs(valid({ os: undefined, arch: undefined, sproto: undefined, ms: undefined,
    fail: undefined, router: undefined })));
  assert.deepEqual([minimal.os, minimal.arch, minimal.sproto, minimal.ms, minimal.fail, minimal.router], ['', '', null, null, '', 'unknown']);
  assert.deepEqual(parseBeacon(qs({ ...valid(), unknown_param: 'x'.repeat(5000) })), parseBeacon(qs(valid())));
  for (const worker of ['ok', 'unreachable', 'bad_status', 'unrecognized', 'protocol_mismatch']) {
    assert.equal(parseBeacon(qs(valid({ worker }))).worker, worker);
  }
  for (const ws of ['ok', 'failed', 'refused', 'skipped']) assert.equal(parseBeacon(qs(valid({ ws }))).ws, ws);
  for (const stun of ['ok', 'failed', 'off']) assert.equal(parseBeacon(qs(valid({ stun }))).stun, stun);
  for (const router of ['none', 'mapped', 'nat', 'unknown']) assert.equal(parseBeacon(qs(valid({ router }))).router, router);
  for (const plat of ['Windows', 'Linux', 'SteamDeck', 'Other']) assert.equal(parseBeacon(qs(valid({ plat }))).plat, plat);
  for (const fail of ['worker_unreachable', 'worker_bad_status', 'worker_unrecognized', 'websocket_failed', 'signin_refused',
    'identity_unavailable', 'direct_unavailable', 'not_usable']) {
    assert.equal(parseBeacon(qs(valid({ fail }))).fail, fail);
  }
});

test('anything malformed, overlong or outside the enumerations is answered 400 and records nothing', async () => {
  const { call, rows, logs } = rig();
  const bad = [
    { diag: undefined }, { diag: '' }, { diag: 'ABCDEF012345' }, { diag: 'abcdef01234' }, { diag: 'abcdef0123456' }, { diag: 'xyzxyzxyzxyz' },
    { ver: undefined }, { ver: '' }, { ver: 'x'.repeat(17) }, { ver: '1 2' }, { ver: '1/2' }, { ver: 'a@b' },
    { plat: undefined }, { plat: 'Mac' }, { plat: 'windows' }, { plat: 'Windows'.repeat(10) },
    { os: 'x'.repeat(25) }, { os: 'Windows 10' }, { os: '<b>' }, { arch: 'x'.repeat(25) }, { arch: 'x 64' },
    { proto: undefined }, { proto: '' }, { proto: 'four' }, { proto: '1001' }, { proto: '-1' }, { proto: '4.5' }, { proto: '123456' },
    { sproto: 'x' }, { sproto: '1001' }, { sproto: '' },
    { worker: undefined }, { worker: 'fine' }, { worker: 'OK' }, { ws: undefined }, { ws: 'open' }, { stun: undefined }, { stun: 'on' },
    { ms: '60001' }, { ms: '-1' }, { ms: '1.5' }, { ms: 'abc' }, { ms: '1e3' }, { ms: '' },
    { direct: undefined }, { direct: '2' }, { direct: 'true' }, { relay: undefined }, { relay: '' }, { usable: undefined }, { usable: 'yes' },
    { fail: 'direct_timeout' }, { fail: 'host_left' }, { fail: 'made_up' }, { fail: 'other' }, { fail: 'NOT_USABLE' },
    { router: 'lan' }, { router: '' },
  ];
  for (const change of bad) {
    const response = await call(valid(change));
    assert.equal(response.status, 400, JSON.stringify(change));
    assert.equal(response.body, null);
  }
  const twice = new Request(`https://lobby.example${BEACON_PATH}?${qs(valid())}&usable=0`);
  assert.equal((await handleBeacon(twice, { gate: new BeaconGate(), record: async () => assert.fail('recorded'), address: ADDRESS })).status, 400);
  assert.equal((await call({})).status, 400);
  assert.deepEqual(rows(), []);
  assert.deepEqual(logs, []);
});

test('only GET is served; other methods answer 405 and record nothing', async () => {
  const { call, rows } = rig();
  for (const method of ['POST', 'PUT', 'DELETE', 'PATCH', 'HEAD']) {
    const response = await call(valid(), { method });
    assert.equal(response.status, 405, method);
    assert.equal(response.headers.get('Allow'), 'GET');
  }
  assert.deepEqual(rows(), []);
});

test('the service as a whole and each address are limited; the limits recover and the address map is bounded', async () => {
  const specs = { beacon: { burst: 2, perSecond: 0.5 }, beaconAll: { burst: 5, perSecond: 1 } };
  const { env, call, counters, gate } = rig({ specs, maxAddresses: 4 });
  assert.deepEqual([(await call(valid(), { address: '198.51.100.1' })).status, (await call(valid(), { address: '198.51.100.1' })).status,
    (await call(valid(), { address: '198.51.100.1' })).status], [204, 204, 429]);
  assert.equal((await call(valid(), { address: '198.51.100.1' })).headers.get('Retry-After'), '10');
  assert.equal((await call(valid(), { address: '198.51.100.2' })).status, 204);
  assert.equal(Object.fromEntries(counters().map((r) => [r.k, r.n]))['ct.total'], 3);

  assert.equal((await call(valid(), { address: '198.51.100.2' })).status, 204);
  assert.equal((await call(valid(), { address: '198.51.100.3' })).status, 204);
  assert.equal((await call(valid(), { address: '198.51.100.4' })).status, 429, 'the whole service is out of tokens');
  assert.equal(Object.fromEntries(counters().map((r) => [r.k, r.n]))['ct.total'], 5, 'a limited beacon records nothing');

  env.advance(4000);
  assert.equal((await call(valid(), { address: '198.51.100.1' })).status, 204);

  for (let i = 10; i < 40; i++) {
    env.advance(2000);
    assert.equal((await call(valid(), { address: `198.51.100.${i}` })).status, 204);
  }
  assert.ok(gate.addresses.size <= 4);
  assert.equal(JSON.stringify([...gate.addresses.keys()]).includes('198.51.100'), false, 'only hashes are kept');

  const bursty = rig({ specs: { beacon: { burst: 1, perSecond: 0 }, beaconAll: { burst: 100, perSecond: 100 } } });
  assert.equal((await bursty.call(valid(), { address: '2001:db8:1:1:aaaa::1' })).status, 204);
  assert.equal((await bursty.call(valid(), { address: '2001:db8:1:1:bbbb::2' })).status, 429, 'one IPv6 network is one address');
  assert.equal((await bursty.call(valid(), { address: '2001:db8:2:1::1' })).status, 204);
});

test('no address, query string or identifier is stored anywhere', async () => {
  const { env, call, rows } = rig();
  await call(valid({ ver: '0.4.2-rc.1' }), { address: ADDRESS });
  await call({ ...valid(), tracking: 'secret-value', friend: 'Q7K2-9ZXA' }, { address: ADDRESS });
  const dump = JSON.stringify(rows());
  for (const text of [ADDRESS, DIAG, 'secret-value', 'Q7K2-9ZXA', 'diag=', 'tracking', '10.0.26200', 'x64', 'tmnt']) {
    assert.equal(dump.includes(text), false, text);
  }
  assert.equal(env.host.sql.get('SELECT COUNT(*) AS n FROM tel_sessions').n, 0);
  assert.equal(env.host.sql.get('SELECT COUNT(*) AS n FROM tel_events').n, 0);
  assert.ok(env.host.sql.get('SELECT COUNT(*) AS n FROM tel_latency').n <= 2);
  assert.ok(JSON.stringify(env.hub.telemetrySummary(1)).includes(DIAG) === false);
});

test('the latency samples are bounded and expire', async () => {
  const { env, call } = rig({ specs: { beacon: { burst: 1e9, perSecond: 1e9 }, beaconAll: { burst: 1e9, perSecond: 1e9 } } });
  for (let i = 0; i < 1100; i++) await call(valid({ ms: String(i % 1000) }));
  assert.equal(env.host.sql.get('SELECT COUNT(*) AS n FROM tel_latency').n, 1000);
  env.clock.now += 8 * 24 * 3600 * 1000;
  await call(valid());
  assert.equal(env.host.sql.get('SELECT COUNT(*) AS n FROM tel_latency').n, 1);
});

test('a store or limiter that throws never turns into an error for the caller', async () => {
  const down = rig({ store: async () => { throw new Error('store is down'); } });
  const response = await down.call(valid());
  assert.equal(response.status, 204);
  assert.deepEqual(down.logs, ['beacon: not recorded: store is down']);
  assert.equal((await down.call(valid({ diag: 'nope' }))).status, 400);

  const broken = rig();
  broken.env.hub.telemetry = new Proxy({}, { get: () => () => { throw new Error('no space'); } });
  assert.equal((await broken.call(valid())).status, 204);
  broken.env.hub.telemetry = null;
  assert.equal((await broken.call(valid())).status, 204);

  const sql = rig();
  const real = sql.env.host.sql.run;
  sql.env.host.sql.run = (query, ...args) => {
    if (query.includes('tel_counters') || query.includes('tel_latency')) throw new Error('disk full');
    return real(query, ...args);
  };
  assert.equal((await sql.call(valid())).status, 204);

  const limiter = { allow() { throw new Error('limiter broke'); } };
  const response2 = await handleBeacon(requestOf(valid()), { gate: limiter, record: async () => assert.fail('recorded'), address: ADDRESS });
  assert.equal(response2.status, 429);
  const odd = await handleBeacon({ method: 'GET', get url() { throw new Error('no url'); } }, { gate: limiter, record: async () => {} });
  assert.equal(odd.status, 400);
});

let server;
let base;

before(async () => {
  server = await startLobbyServer({ port: 0 });
  base = `http://127.0.0.1:${server.port}`;
});

after(async () => {
  await server.close();
});

test('the development server handles the beacon through the same code', async () => {
  const query = (over) => new URLSearchParams(Object.entries(valid(over)).filter(([, v]) => v !== undefined)).toString();
  const ok = await fetch(`${base}${BEACON_PATH}?${query({})}`);
  assert.equal(ok.status, 204);
  assert.equal(await ok.text(), '');
  assert.equal((await fetch(`${base}${BEACON_PATH}?${query({ worker: 'bogus' })}`)).status, 400);
  assert.equal((await fetch(`${base}${BEACON_PATH}`)).status, 400);
  assert.equal((await fetch(`${base}${BEACON_PATH}?${query({})}`, { method: 'POST' })).status, 405);
  assert.equal((await fetch(`${base}${BEACON_PATH}x`)).status, 404);
  for (let i = 0; i < 8; i++) assert.equal((await fetch(`${base}${BEACON_PATH}?${query({ usable: '0', fail: 'not_usable' })}`)).status, 204);
  const block = server.hub.telemetrySummary(1).connection_beacon;
  assert.deepEqual([block.total, block.usable, block.unusable], [9, 1, 8]);
  assert.deepEqual(values(block.failures), { not_usable: 8 });
});
