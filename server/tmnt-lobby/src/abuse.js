// In-memory abuse limits and counters. Nothing here is written to SQL on its own: a limit that cost a write per hit
// would defeat its purpose. No Cloudflare import, so Node runs it in the tests.
export const PROTECTION = {
  sessionWindowMs: 10 * 60 * 1000,
  seenRefreshMs: 15 * 60 * 1000,
  rejectWindowMs: 10 * 60 * 1000,
  advertiseRefreshMs: 45 * 1000,
  hello: { burst: 12, perSecond: 1 / 15 },
  createAccount: { burst: 6, perSecond: 1 / 20 },
  createAddress: { burst: 20, perSecond: 1 / 6 },
  joinAccount: { burst: 20, perSecond: 1 / 5 },
  resumeAccount: { burst: 12, perSecond: 1 / 10 },
  inviteAccount: { burst: 20, perSecond: 1 / 3 },
  inviteAddress: { burst: 60, perSecond: 1 / 2 },
  friendAccount: { burst: 30, perSecond: 1 / 2 },
  telemetryAccount: { burst: 12, perSecond: 0.2 },
  partyReportAll: { burst: 30, perSecond: 1 / 30 },
  partyReportAccount: { burst: 3, perSecond: 1 / 120 },
  partyReportAddress: { burst: 12, perSecond: 1 / 30 },
  trackedKeys: 4096,
  flushMs: 5 * 60 * 1000,
};

export const DIAGNOSTICS = new Set([
  'ws_upgrade_accepted', 'ws_upgrade_limited', 'ws_upgrade_ip_cap', 'hello_accepted', 'hello_limited',
  'party_create_accepted', 'party_create_limited', 'party_join_limited', 'invite_limited', 'friend_request_limited',
  'telemetry_limited', 'room_resume_limited', 'relay_abuse_closed', 'hello_reconnect_deduped', 'party_report_limited',
]);

const MAX_COUNT = 1_000_000;

export class RateLimiter {
  constructor(spec, maxKeys = PROTECTION.trackedKeys) {
    this.spec = spec;
    this.maxKeys = maxKeys;
    this.buckets = new Map();
  }

  tokens(key, now) {
    const [tokens, last] = this.buckets.get(key) ?? [this.spec.burst, now];
    return Math.min(this.spec.burst, tokens + (Math.max(0, now - last) / 1000) * this.spec.perSecond);
  }

  allows(key, now) {
    return this.tokens(key, now) >= 1;
  }

  take(key, now) {
    const tokens = this.tokens(key, now);
    if (tokens < 1) return false;
    this.buckets.delete(key);
    if (this.buckets.size >= this.maxKeys) this.prune(now);
    this.buckets.set(key, [tokens - 1, now]);
    return true;
  }

  // Idle keys are as good as new ones; if every key is busy the oldest go.
  prune(now) {
    for (const key of [...this.buckets.keys()]) {
      if (this.tokens(key, now) >= this.spec.burst) this.buckets.delete(key);
    }
    for (const key of this.buckets.keys()) {
      if (this.buckets.size < this.maxKeys) break;
      this.buckets.delete(key);
    }
  }
}

// Counter names and numbers only, held until the owner writes them in one batch.
export class AbuseCounts {
  constructor() {
    this.pending = new Map();
  }

  // True when this is the first pending count.
  add(name, amount = 1) {
    if (!DIAGNOSTICS.has(name)) throw new Error(`unknown diagnostic ${name}`);
    const first = this.pending.size === 0;
    this.pending.set(name, Math.min(MAX_COUNT, (this.pending.get(name) ?? 0) + amount));
    return first;
  }

  drain() {
    const entries = [...this.pending];
    this.pending.clear();
    return entries;
  }
}

// What a hub accepts from a room's report: known names with sane counts, nothing else.
export function checkedCounts(entries) {
  if (!Array.isArray(entries) || entries.length > DIAGNOSTICS.size) return [];
  return entries.filter((e) => Array.isArray(e) && DIAGNOSTICS.has(e[0]) && Number.isInteger(e[1]) && e[1] > 0 && e[1] <= MAX_COUNT)
    .map(([name, count]) => [name, count]);
}
