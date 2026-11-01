// The Connection Test beacon: GET /telemetry/test?... records one outcome of the game's Connection Test before any session
// exists. Unauthenticated, best effort and never an error for the caller: 204 when well-formed, 400 when not (no detail),
// 429 past the rate limits, 405 for other methods. Only daily counters are kept; no address, query or identifier is stored.
// No Cloudflare import, so Node runs it in the tests and in the development server.
import { BUCKETS, ipGroup, isLocalAddress, takeToken } from './protocol.js';
import { isReason, reasonCategory } from './reasons.js';
import { sha256Hex } from './sha256.js';
import { isDiag } from './telemetry.js';

export const BEACON_PATH = '/telemetry/test';
export const BEACON_ADDRESSES = 512;

const PLATFORMS = ['Windows', 'Linux', 'SteamDeck', 'Other'];
const WORKER = ['ok', 'unreachable', 'bad_status', 'unrecognized', 'protocol_mismatch'];
const WS = ['ok', 'failed', 'refused', 'skipped'];
const STUN = ['ok', 'failed', 'off'];
const ROUTER = ['none', 'mapped', 'nat', 'unknown'];
const VERSION = /^[0-9A-Za-z._-]{1,16}$/;
const LABEL = /^[A-Za-z0-9._-]{0,24}$/;
const NUMBER = /^[0-9]{1,5}$/;
const NAMES = ['diag', 'ver', 'plat', 'os', 'arch', 'proto', 'sproto', 'worker', 'ws', 'ms', 'stun', 'direct', 'relay', 'usable', 'fail',
  'router'];

// The checked beacon, or null for anything malformed (a missing required parameter, an unknown or overlong value, a
// repeated parameter). Parameters this does not know are ignored and never stored.
export function parseBeacon(params) {
  const raw = {};
  for (const name of NAMES) {
    const all = params.getAll(name);
    if (all.length > 1) return null;
    raw[name] = all.length ? all[0] : undefined;
  }
  const required = (value, ok) => (value !== undefined && ok(value) ? value : null);
  const oneOf = (list) => (value) => list.includes(value);
  const integer = (max) => (value) => NUMBER.test(value) && Number(value) <= max;
  const flag = oneOf(['0', '1']);

  if (!isDiag(raw.diag)) return null;
  const ver = required(raw.ver, (v) => VERSION.test(v));
  const plat = required(raw.plat, oneOf(PLATFORMS));
  const proto = required(raw.proto, integer(1000));
  const worker = required(raw.worker, oneOf(WORKER));
  const ws = required(raw.ws, oneOf(WS));
  const stun = required(raw.stun, oneOf(STUN));
  const direct = required(raw.direct, flag);
  const relay = required(raw.relay, flag);
  const usable = required(raw.usable, flag);
  if ([ver, plat, proto, worker, ws, stun, direct, relay, usable].includes(null)) return null;

  const optional = (value, ok, fallback) => (value === undefined ? fallback : ok(value) ? value : null);
  const os = optional(raw.os, (v) => LABEL.test(v), '');
  const arch = optional(raw.arch, (v) => LABEL.test(v), '');
  const sproto = optional(raw.sproto, integer(1000), '');
  const ms = optional(raw.ms, integer(60000), '');
  const fail = optional(raw.fail, (v) => v === '' || (isReason(v) && reasonCategory(v) === 'connection_test'), '');
  const router = optional(raw.router, oneOf(ROUTER), 'unknown');
  if ([os, arch, sproto, ms, fail, router].includes(null)) return null;

  return {
    diag: raw.diag, ver, plat, os, arch, proto: Number(proto), sproto: sproto === '' ? null : Number(sproto), worker, ws,
    ms: ms === '' ? null : Number(ms), stun, direct: direct === '1', relay: relay === '1', usable: usable === '1', fail, router,
  };
}

// The checked beacon as a query string, for the hop from the Worker to the directory object, which checks it again.
export function beaconQuery(beacon) {
  const out = new URLSearchParams();
  for (const [name, value] of Object.entries({ ...beacon, direct: +beacon.direct, relay: +beacon.relay, usable: +beacon.usable })) {
    if (value !== null && value !== '') out.set(name, String(value));
  }
  return out.toString();
}

// Bounded in-memory token buckets: one for the whole service, one per address keyed by a salted hash of it (the address
// itself is never kept and the salt lives in memory only). The newest BEACON_ADDRESSES addresses are remembered.
export class BeaconGate {
  constructor({ now = () => Date.now(), specs = BUCKETS, maxAddresses = BEACON_ADDRESSES, exemptLoopback = false } = {}) {
    this.now = now;
    this.specs = specs;
    this.maxAddresses = maxAddresses;
    this.exemptLoopback = exemptLoopback;
    this.salt = null;
    this.all = null;
    this.addresses = new Map();
  }

  allow(address) {
    const now = this.now();
    const all = takeToken(this.all, this.specs.beaconAll, now);
    if (!all) return false;
    if (this.exemptLoopback && isLocalAddress(address)) {
      this.all = all;
      return true;
    }
    this.salt ??= Array.from(crypto.getRandomValues(new Uint8Array(16)), (b) => b.toString(16).padStart(2, '0')).join('');
    const key = sha256Hex(`${this.salt}|${ipGroup(address ?? '')}`).slice(0, 16);
    const own = takeToken(this.addresses.get(key), this.specs.beacon, now);
    if (!own) return false;
    this.all = all;
    this.addresses.delete(key);
    this.addresses.set(key, own);
    while (this.addresses.size > this.maxAddresses) this.addresses.delete(this.addresses.keys().next().value);
    return true;
  }
}

function answer(status, extra = {}) {
  return new Response(null, { status, headers: { 'Cache-Control': 'no-store', 'X-Content-Type-Options': 'nosniff', ...extra } });
}

// record(beacon) stores one checked beacon; whatever it does, a well-formed beacon is answered 204.
export async function handleBeacon(request, { gate, record, address = '', log = () => {} }) {
  if (request.method !== 'GET') return answer(405, { Allow: 'GET' });
  let beacon = null;
  try {
    beacon = parseBeacon(new URL(request.url).searchParams);
  } catch {
    beacon = null;
  }
  if (!beacon) return answer(400);
  let allowed = false;
  try {
    allowed = gate.allow(address);
  } catch (error) {
    log(`beacon: limiter failed: ${error?.message ?? error}`);
  }
  if (!allowed) return answer(429, { 'Retry-After': '10' });
  try {
    await record(beacon);
  } catch (error) {
    log(`beacon: not recorded: ${error?.message ?? error}`);
  }
  return answer(204);
}
