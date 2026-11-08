import { sha256Hex } from './sha256.js';

export const SERVICE = 'tmnt-lobby';
export const PROTOCOL = 4;
export const MAX_PARTY = 4;
export const GAME_ID = 'tmnt-reshelled-pc';
export const GUESTS = ['base'];

export const LIMITS = {
  frameBytes: 4096,
  telemetryFrameBytes: 8192 + 128,
  nameChars: 16,
  versionChars: 16,
  platformChars: 16,
  lanCandidates: 4,
  friends: 200,
  pendingOut: 50,
  pendingIn: 50,
  requestDays: 14,
  registrationsPerIpPerDay: 20,
  socketsPerIp: 16,
  graceMs: 30000,
  playerMatchMs: 180000,
  inviteMs: 120000,
  pendingInvites: 8,
  partyIdleMs: 30 * 60 * 1000,
  roomIdleMs: 60000,
  helloMs: 10000,
  silenceMs: 45000,
  tombstoneMs: 3600 * 1000,
  roomSockets: 2 * MAX_PARTY + 2,
  ruleFields: 8,
  searchResults: 20,
  matchMaxMs: 4 * 3600 * 1000,
};

export const BUCKETS = {
  any: { burst: 60, perSecond: 20 },
  social: { burst: 10, perSecond: 1 },
  signal: { burst: 40, perSecond: 5 },
  search: { burst: 6, perSecond: 1 },
  telemetry: { burst: 6, perSecond: 0.2 },
  telemetryAll: { burst: 120, perSecond: 10 },
  beacon: { burst: 6, perSecond: 0.2 },
  beaconAll: { burst: 60, perSecond: 5 },
};

export const WORKER_BUILD = '2026-10-31-party1';

// Binary relay frames: magic "SR", version, type, src slot, dst slot, token (u32), sequence (u32), payload length (u16),
// payload.
export const RELAY = {
  version: 2,
  data: 1,
  headerBytes: 16,
  maxPayload: 1472,
  connection: { packets: { burst: 1600, perSecond: 800 }, bytes: { burst: 512 * 1024, perSecond: 256 * 1024 } },
  match: { packets: { burst: 4800, perSecond: 2400 }, bytes: { burst: 1536 * 1024, perSecond: 768 * 1024 } },
  overrunsToClose: 256,
  invalid: { burst: 200, perSecond: 5 },
  bufferedBytes: 256 * 1024,
  loggedForwards: 3,
  loggedDrops: 3,
};

export const CAPABILITIES = {
  protocol_version: PROTOCOL,
  worker_build: WORKER_BUILD,
  public_endpoints: true,
  relay_supported: true,
  relay_max_payload: RELAY.maxPayload,
  relay_frame_version: RELAY.version,
  max_party: MAX_PARTY,
  snes_features: true,
  snes_modes: true,
};

// SNES gameplay mechanics only: the soundtrack is each player's own and is never negotiated.
export const SNES_KEYS = ['directional_combat', 'throw_control', 'dash', 'double_jump'];
// How a player's SNES Dash starts. It is one choice per player: `dash` says it is on and `dash_mode` which kind.
export const DASH_MODES = ['off', 'auto', 'manual'];
const SNES_ID = /^[0-9a-f]{1,8}$/;

const snesFlags = () => Object.fromEntries(SNES_KEYS.map((key) => [key, false]));
const snesIds = () => Object.fromEntries(SNES_KEYS.map((key) => [key, '']));

// A Dash announced without a mode is the Auto dash of clients that predate modes; a mode nobody writes is not guessed.
const dashModeOf = (enabled) => {
  if (!isObject(enabled) || enabled.dash !== true) return 'off';
  if (enabled.dash_mode === undefined) return 'auto';
  return DASH_MODES.includes(enabled.dash_mode) ? enabled.dash_mode : 'off';
};

// What a client announced: a feature it does not support is neither enabled nor identified, and anything missing or
// malformed reads as unsupported, so a client without SNES features announces nothing at all.
export function normalizeSnes(value) {
  const out = { supports: snesFlags(), enabled: { ...snesFlags(), dash_mode: 'off' }, compat: snesIds() };
  if (!isObject(value)) return out;
  for (const key of SNES_KEYS) {
    const supported = isObject(value.supports) && value.supports[key] === true;
    out.supports[key] = supported;
    out.enabled[key] = supported && isObject(value.enabled) && value.enabled[key] === true;
    const id = isObject(value.compat) ? value.compat[key] : '';
    out.compat[key] = supported && typeof id === 'string' && SNES_ID.test(id) ? id : '';
  }
  const mode = out.enabled.dash ? dashModeOf(value.enabled) : 'off';
  out.enabled.dash = mode !== 'off';
  out.enabled.dash_mode = mode;
  return out;
}

// The other players as one: supported only when every one of them is, with the data id they share, or `mixed` when they
// do not share one. Their choices are not combined: each applies to its own player's turtle.
export function combineSnes(list) {
  const out = { supports: snesFlags(), compat: snesIds() };
  for (const key of SNES_KEYS) {
    out.supports[key] = list.every((s) => s.supports[key]);
    const ids = new Set(list.map((s) => s.compat[key]));
    out.compat[key] = out.supports[key] ? (ids.size === 1 ? [...ids][0] : 'mixed') : '';
  }
  return out;
}

// A mechanic is available to a match only when every player supports it with the same data.
export function supportedSnes(list) {
  const out = snesFlags();
  if (list.length < 2) return out;
  for (const key of SNES_KEYS) {
    out[key] = list.every((s) => s.supports[key]) && new Set(list.map((s) => s.compat[key])).size === 1;
  }
  return out;
}

export const offSnesTable = () => ({ supported: snesFlags(), players: {} });

const seatRow = (supported, announced) => {
  const row = Object.fromEntries(SNES_KEYS.map((key) => [key, supported[key] && announced.enabled[key]]));
  return { ...row, dash_mode: row.dash ? announced.enabled.dash_mode : 'off' };
};

// `entries`: [slot, announcement] for every member. Each slot's row is its own player's choice among what the match
// supports, so every member is told the same table.
export function snesTable(entries) {
  const supported = supportedSnes(entries.map(([, s]) => s));
  const players = Object.fromEntries(entries.map(([slot, s]) => [String(slot), seatRow(supported, s)]));
  return { supported, players };
}

// The header of one relay frame, or the reason it is not one. The payload is never looked at.
export function parseRelayFrame(bytes) {
  if (!(bytes instanceof Uint8Array)) return { error: 'not_binary' };
  if (bytes.length < 4) return { error: 'truncated' };
  if (bytes[0] !== 0x53 || bytes[1] !== 0x52) return { error: 'bad_magic' };
  if (bytes[2] !== RELAY.version) return { error: 'bad_version' };
  if (bytes[3] !== RELAY.data) return { error: 'bad_type' };
  if (bytes.length < RELAY.headerBytes) return { error: 'truncated' };
  const view = new DataView(bytes.buffer, bytes.byteOffset, bytes.byteLength);
  const length = view.getUint16(14);
  if (length > RELAY.maxPayload || bytes.length > RELAY.headerBytes + RELAY.maxPayload) return { error: 'oversized' };
  if (length === 0 || bytes.length !== RELAY.headerBytes + length) return { error: 'bad_length' };
  return { type: bytes[3], src: bytes[4], dst: bytes[5], token: view.getUint32(6), seq: view.getUint32(10), length };
}

export function encodeRelayFrame(src, dst, token, seq, payload) {
  const bytes = new Uint8Array(RELAY.headerBytes + payload.length);
  const view = new DataView(bytes.buffer);
  bytes.set([0x53, 0x52, RELAY.version, RELAY.data, src, dst]);
  view.setUint32(6, token >>> 0);
  view.setUint32(10, seq >>> 0);
  view.setUint16(14, payload.length);
  bytes.set(payload, RELAY.headerBytes);
  return bytes;
}

export const isObject = (v) => v !== null && typeof v === 'object' && !Array.isArray(v);
export const isInt = (v, lo, hi) => Number.isInteger(v) && v >= lo && v <= hi;

export function takeToken(bucket, spec, now, cost = 1) {
  const [tokens, last] = bucket ?? [spec.burst, now];
  const refilled = Math.min(spec.burst, tokens + ((now - last) / 1000) * spec.perSecond);
  if (refilled < cost) return null;
  return [refilled - cost, now];
}

// Packets and bytes both have to fit; nothing is taken from either when one does not.
export function takeRelayBudget(budget, spec, now, bytes) {
  const packets = takeToken(budget.packets, spec.packets, now);
  const volume = packets && takeToken(budget.bytes, spec.bytes, now, bytes);
  if (!volume) return false;
  budget.packets = packets;
  budget.bytes = volume;
  return true;
}

const PLAYER_ALPHABET = 'ABCDEFGHJKLMNPQRSTUVWXYZ23456789';

export class ProtocolError extends Error {
  constructor(code, detail = '') {
    super(detail || code);
    this.code = code;
  }
}

export const fail = (code, detail) => {
  throw new ProtocolError(code, detail);
};

export const tokenHash = (token) => sha256Hex(`tmnt-token:${token}`);
export const joinTokenHash = (token) => sha256Hex(`tmnt-join:${token}`);

// The public id is also the friend code. Must match DerivePlayerId in src/online/identity.cpp.
export function playerIdFor(token) {
  const digest = sha256Hex(`tmnt-player-id:${token}`);
  let id = '';
  for (let i = 0; i < 8; i++) {
    id += PLAYER_ALPHABET[parseInt(digest.slice(i * 2, i * 2 + 2), 16) % PLAYER_ALPHABET.length];
    if (i === 3) id += '-';
  }
  return id;
}

export function normalizeFriendCode(value) {
  if (typeof value !== 'string') fail('bad_request', 'code expected');
  const compact = value.toUpperCase().replace(/[\s-]/g, '');
  if (compact.length !== 8 || [...compact].some((c) => !PLAYER_ALPHABET.includes(c))) fail('bad_request', 'malformed code');
  return `${compact.slice(0, 4)}-${compact.slice(4)}`;
}

export function cleanName(value) {
  if (typeof value !== 'string') fail('bad_request', 'name expected');
  const name = value.replace(/[^A-Za-z0-9_-]/g, '').slice(0, LIMITS.nameChars);
  return name || 'Player';
}

export function cleanLabel(value, maxChars) {
  if (value === undefined) return '';
  if (typeof value !== 'string') fail('bad_request', 'text expected');
  return value.replace(/[^A-Za-z0-9 ._+-]/g, '').slice(0, maxChars);
}

export function parseHello(message) {
  if (message.proto !== PROTOCOL) fail('proto', 'protocol version not supported');
  if (message.game !== GAME_ID) fail('bad_game', 'unknown game');
  if (!GUESTS.includes(message.guest)) fail('bad_guest', 'unknown guest version');
  if (!Number.isInteger(message.build) || message.build < 0 || message.build > 0xffff) {
    fail('bad_request', 'build expected');
  }
  if (typeof message.token !== 'string' || !/^[0-9a-f]{64}$/.test(message.token)) {
    fail('bad_request', 'token expected');
  }
  if (message.player !== playerIdFor(message.token)) fail('unauthorized', 'player id does not match');
  return {
    player: message.player,
    hash: tokenHash(message.token),
    name: cleanName(message.name ?? 'Player'),
    guest: message.guest,
    build: message.build,
    version: cleanLabel(message.version, LIMITS.versionChars),
    platform: cleanLabel(message.platform, LIMITS.platformChars),
    presence: message.presence !== false,
    relay: message.relay === true,
  };
}

export function ipv4Kind(ip) {
  const m = typeof ip === 'string' ? /^(\d{1,3})\.(\d{1,3})\.(\d{1,3})\.(\d{1,3})$/.exec(ip) : null;
  if (!m) return null;
  const [a, b, c, d] = m.slice(1).map(Number);
  if ([a, b, c, d].some((n) => n > 255)) return null;
  if (a === 127) return 'loopback';
  if (a === 0 || a >= 224) return 'reserved';
  if (a === 100 && b >= 64 && b <= 127) return 'carrier';
  if (a === 10 || (a === 172 && b >= 16 && b <= 31) || (a === 192 && b === 168) || (a === 169 && b === 254)) {
    return 'private';
  }
  return 'public';
}

export function parseMatchHello(message) {
  if (message.proto !== PROTOCOL) fail('proto', 'protocol version not supported');
  if (typeof message.match !== 'string' || !/^[0-9a-f]{16}$/.test(message.match)) fail('bad_request', 'match expected');
  if (!isInt(message.slot, 1, 255)) fail('bad_request', 'slot expected');
  if (typeof message.token !== 'string' || !/^[0-9a-f]{32}$/.test(message.token)) fail('bad_request', 'token expected');
  const seen = message.generation_seen ?? 0;
  if (!isInt(seen, 0, 0x7fffffff)) fail('bad_request', 'generation_seen expected');
  return { match: message.match, slot: message.slot, token: message.token, relay: message.relay === true, generationSeen: seen };
}

export function parseEndpoint(message) {
  if (!Number.isInteger(message.port) || message.port < 1024 || message.port > 65535) {
    fail('bad_request', 'port expected');
  }
  const lan = message.lan ?? [];
  if (!Array.isArray(lan) || lan.length > LIMITS.lanCandidates) fail('bad_request', 'lan expected');
  for (const ip of lan) {
    const kind = ipv4Kind(ip);
    if (kind !== 'loopback' && kind !== 'private') fail('bad_request', 'lan address expected');
  }
  // The sender's gameplay socket as STUN saw it: the endpoint a peer elsewhere can punch. Only the pair means anything,
  // and this service never sees UDP, so it passes it on as the sender's own claim.
  let publicIp = '';
  let publicPort = 0;
  if (message.public_ip !== undefined || message.public_port !== undefined) {
    if (typeof message.public_ip !== 'string' || ipv4Kind(message.public_ip) !== 'public') {
      fail('bad_request', 'public_ip expected');
    }
    if (!Number.isInteger(message.public_port) || message.public_port < 1 || message.public_port > 65535) {
      fail('bad_request', 'public_port expected');
    }
    publicIp = message.public_ip;
    publicPort = message.public_port;
  }
  return { port: message.port, lan: [...lan], publicIp, publicPort };
}

// Per-address limits group an IPv4-mapped IPv6 address with its IPv4 address and other IPv6 clients by their /64.
export function ipGroup(ip) {
  if (typeof ip !== 'string' || !ip.includes(':')) return String(ip);
  const mapped = /^::ffff:(\d{1,3}(?:\.\d{1,3}){3})$/i.exec(ip);
  if (mapped) return mapped[1];
  const [head, tail] = ip.split('::');
  const before = head ? head.split(':') : [];
  const after = tail ? tail.split(':') : [];
  const gap = tail === undefined ? [] : Array(Math.max(0, 8 - before.length - after.length)).fill('0');
  return [...before, ...gap, ...after].slice(0, 4).map((g) => g.toLowerCase().replace(/^0+(?=.)/, '')).join(':');
}
export const isLocalAddress = (ip) => ip === '::1' || ipv4Kind(ip) === 'loopback';

export function compatibleGuests(a, b) {
  return Boolean(a && b) && a.guest === b.guest && a.build === b.build;
}
