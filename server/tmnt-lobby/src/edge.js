// The Worker's gate for WebSocket upgrades, before any Durable Object exists in the request's path. No Cloudflare import,
// so Node runs it in the tests.
import { countryCode } from './metrics.js';
import { ipGroup } from './protocol.js';

export const LIMITED_HEADER = 'X-TMNT-Edge-Limited';
export const COUNTRY_HEADER = 'X-TMNT-Edge-Country';
const MAX_PENDING_LIMITED = 1_000_000;
const RETRY_AFTER_SECONDS = '60';

// Without the binding (development) or when it fails, upgrades pass: the objects' own limits still apply.
export async function upgradeAllowed(limiter, ip) {
  if (!limiter || !ip) return true;
  try {
    return (await limiter.limit({ key: `ws:${ipGroup(ip)}` })).success !== false;
  } catch {
    return true;
  }
}

// Upgrades refused here never reach the directory, so their count rides on the next one this isolate forwards to it.
export const edgeCounts = { limited: 0 };

// Any edge header from the client is replaced by the Worker's.
export function withEdgeLimited(request, limited = 0) {
  const headers = new Headers(request.headers);
  headers.set(COUNTRY_HEADER, countryCode(request.cf?.country));
  if (limited > 0) headers.set(LIMITED_HEADER, String(limited));
  else headers.delete(LIMITED_HEADER);
  return new Request(request, { headers });
}

export function limitedCount(headers) {
  const count = Number(headers.get(LIMITED_HEADER));
  return Number.isInteger(count) && count > 0 ? Math.min(count, MAX_PENDING_LIMITED) : 0;
}

// target: the object that takes the upgrade. Only the directory is told how many upgrades were refused before it.
export async function handleUpgrade(request, env, target, { reportsLimited = false } = {}) {
  if (!(await upgradeAllowed(env.WS_UPGRADE_LIMITER, request.headers.get('CF-Connecting-IP')))) {
    edgeCounts.limited = Math.min(edgeCounts.limited + 1, MAX_PENDING_LIMITED);
    return new Response('Too many connection attempts', { status: 429, headers: { 'Retry-After': RETRY_AFTER_SECONDS } });
  }
  let limited = 0;
  if (reportsLimited) {
    limited = edgeCounts.limited;
    edgeCounts.limited = 0;
  }
  return target.fetch(withEdgeLimited(request, limited));
}
