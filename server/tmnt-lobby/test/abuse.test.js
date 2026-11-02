import assert from 'node:assert/strict';
import { test } from 'node:test';

import { AbuseCounts, DIAGNOSTICS, PROTECTION, RateLimiter, checkedCounts } from '../src/abuse.js';
import { LIMITED_HEADER, edgeCounts, handleUpgrade, limitedCount, upgradeAllowed, withEdgeLimited } from '../src/edge.js';
import { ipGroup } from '../src/protocol.js';

const upgrade = (headers = {}) => new Request('https://lobby.example/', { headers: { Upgrade: 'websocket', ...headers } });

function stubLimiter(allowed = () => true) {
  const keys = [];
  return { keys, limit: async ({ key }) => {
    keys.push(key);
    return { success: allowed(key) };
  } };
}

function stubTarget() {
  const seen = [];
  return { seen, fetch: async (request) => {
    seen.push(request);
    return new Response(null, { status: 200 });
  } };
}

test('addresses are grouped: IPv4 as is, IPv4-mapped IPv6 as IPv4, IPv6 by /64 whatever its spelling', () => {
  assert.equal(ipGroup('203.0.113.7'), '203.0.113.7');
  assert.equal(ipGroup('::ffff:203.0.113.7'), '203.0.113.7');
  assert.equal(ipGroup('::FFFF:203.0.113.7'), '203.0.113.7');
  const compressed = ipGroup('2001:db8:1:2::abcd');
  assert.equal(compressed, '2001:db8:1:2');
  assert.equal(ipGroup('2001:db8:1:2::1'), compressed);
  assert.equal(ipGroup('2001:0DB8:0001:0002:0000:0000:0000:0001'), compressed);
  assert.equal(ipGroup('2001:db8:1:2:ffff:ffff:ffff:ffff'), compressed);
  assert.equal(ipGroup('2001:db8:1:3::1'), '2001:db8:1:3');
  assert.equal(ipGroup('2001:db8::1'), '2001:db8:0:0');
  assert.equal(ipGroup('2001:db8:0:0:5::1'), '2001:db8:0:0');
  assert.equal(ipGroup('::1'), '0:0:0:0');
  assert.equal(ipGroup('::'), '0:0:0:0');
  assert.equal(ipGroup(undefined), 'undefined');
});

test('a rate limiter allows its burst, refills over time and never takes from a refused request', () => {
  const limiter = new RateLimiter({ burst: 3, perSecond: 1 });
  assert.deepEqual([1, 2, 3, 4].map(() => limiter.take('a', 1000)), [true, true, true, false]);
  assert.equal(limiter.allows('a', 1000), false);
  assert.equal(limiter.take('b', 1000), true);
  assert.equal(limiter.take('a', 2000), true);
  assert.equal(limiter.take('a', 2000), false);
  assert.equal(limiter.take('a', 1_000_000), true);
  assert.equal(limiter.tokens('a', 1_000_000_000), 3);
  const early = new RateLimiter({ burst: 1, perSecond: 1 });
  early.take('a', 5000);
  assert.equal(early.allows('a', 1000), false);
});

test('a rate limiter forgets idle keys first and stays within its key bound', () => {
  const limiter = new RateLimiter({ burst: 2, perSecond: 1 }, 8);
  for (let i = 0; i < 100; i++) limiter.take(`key${i}`, 1000 + i);
  assert.ok(limiter.buckets.size <= 8);
  const busy = new RateLimiter({ burst: 2, perSecond: 0 }, 4);
  for (let i = 0; i < 20; i++) busy.take(`key${i}`, 1000);
  assert.ok(busy.buckets.size <= 4);
});

test('abuse counts accept only known names and report the first pending one', () => {
  const counts = new AbuseCounts();
  assert.equal(counts.add('hello_limited'), true);
  assert.equal(counts.add('hello_limited', 2), false);
  assert.throws(() => counts.add('made_up'), /unknown diagnostic/);
  assert.deepEqual(counts.drain(), [['hello_limited', 3]]);
  assert.deepEqual(counts.drain(), []);
});

test('a report from a room is reduced to known names and positive whole counts', () => {
  assert.deepEqual(checkedCounts([['relay_abuse_closed', 2], ['made_up', 1], ['hello_limited', -1], ['hello_limited', 1.5],
    ['room_resume_limited', 3, 'extra'], 'text', null]), [['relay_abuse_closed', 2], ['room_resume_limited', 3]]);
  assert.deepEqual(checkedCounts('text'), []);
  assert.deepEqual(checkedCounts(Array.from({ length: DIAGNOSTICS.size + 1 }, () => ['hello_limited', 1])), []);
});

test('the policy matches the agreed numbers', () => {
  assert.deepEqual(PROTECTION.hello, { burst: 12, perSecond: 1 / 15 });
  assert.deepEqual(PROTECTION.createAccount, { burst: 6, perSecond: 1 / 20 });
  assert.deepEqual(PROTECTION.createAddress, { burst: 20, perSecond: 1 / 6 });
  assert.deepEqual(PROTECTION.joinAccount, { burst: 20, perSecond: 1 / 5 });
  assert.deepEqual(PROTECTION.resumeAccount, { burst: 12, perSecond: 1 / 10 });
  assert.equal(PROTECTION.inviteAccount.burst, 20);
  assert.ok(PROTECTION.inviteAddress.burst > PROTECTION.inviteAccount.burst);
});

test('the edge limiter is keyed by address group and fails open', async () => {
  const limiter = stubLimiter();
  for (const ip of ['203.0.113.7', '::ffff:203.0.113.7', '2001:db8::1', '2001:0db8:0000:0000:ffff:ffff:ffff:ffff']) {
    assert.equal(await upgradeAllowed(limiter, ip), true);
  }
  assert.deepEqual(limiter.keys, ['ws:203.0.113.7', 'ws:203.0.113.7', 'ws:2001:db8:0:0', 'ws:2001:db8:0:0']);
  assert.equal(await upgradeAllowed(stubLimiter(() => false), '203.0.113.7'), false);
  assert.equal(await upgradeAllowed(undefined, '203.0.113.7'), true);
  assert.equal(await upgradeAllowed(stubLimiter(() => false), ''), true);
  assert.equal(await upgradeAllowed(stubLimiter(() => false), null), true);
  assert.equal(await upgradeAllowed({ limit: async () => { throw new Error('limiter down'); } }, '203.0.113.7'), true);
  assert.equal(await upgradeAllowed({ limit: async () => ({}) }, '203.0.113.7'), true);
});

test('a refused upgrade answers 429 with Retry-After and never reaches the object', async () => {
  edgeCounts.limited = 0;
  const target = stubTarget();
  const env = { WS_UPGRADE_LIMITER: stubLimiter(() => false) };
  const response = await handleUpgrade(upgrade({ 'CF-Connecting-IP': '203.0.113.7' }), env, target, { reportsLimited: true });
  assert.equal(response.status, 429);
  assert.equal(response.headers.get('Retry-After'), '60');
  assert.equal(target.seen.length, 0);
  assert.equal(edgeCounts.limited, 1);
});

test('an allowed upgrade is forwarded, and the refusals since the last forward ride on it once', async () => {
  edgeCounts.limited = 0;
  const target = stubTarget();
  const denied = { WS_UPGRADE_LIMITER: stubLimiter(() => false) };
  const open = { WS_UPGRADE_LIMITER: stubLimiter() };
  const request = (extra) => upgrade({ 'CF-Connecting-IP': '203.0.113.7', ...extra });
  for (let i = 0; i < 3; i++) await handleUpgrade(request(), denied, target, { reportsLimited: true });
  assert.equal((await handleUpgrade(request(), open, target, { reportsLimited: true })).status, 200);
  assert.equal(limitedCount(target.seen[0].headers), 3);
  assert.equal(target.seen[0].headers.get('Upgrade'), 'websocket');
  assert.equal(edgeCounts.limited, 0);
  await handleUpgrade(request(), open, target, { reportsLimited: true });
  assert.equal(limitedCount(target.seen[1].headers), 0);
});

test('a client cannot set the refused-upgrade count, and a match room is never told it', async () => {
  edgeCounts.limited = 0;
  const target = stubTarget();
  const open = { WS_UPGRADE_LIMITER: stubLimiter() };
  await handleUpgrade(upgrade({ [LIMITED_HEADER]: '99999' }), open, target, { reportsLimited: true });
  assert.equal(target.seen[0].headers.get(LIMITED_HEADER), null);
  edgeCounts.limited = 5;
  await handleUpgrade(upgrade({ [LIMITED_HEADER]: '99999' }), open, target);
  assert.equal(target.seen[1].headers.get(LIMITED_HEADER), null);
  assert.equal(edgeCounts.limited, 5);
  assert.equal(limitedCount(new Headers({ [LIMITED_HEADER]: '7' })), 7);
  for (const bad of ['0', '-1', '1.5', 'x', '']) assert.equal(limitedCount(new Headers({ [LIMITED_HEADER]: bad })), 0);
  assert.equal(limitedCount(new Headers({ [LIMITED_HEADER]: '99999999999' })), 1_000_000);
  assert.equal(withEdgeLimited(upgrade(), 4).headers.get(LIMITED_HEADER), '4');
});

test('without a usable address or binding every upgrade is forwarded', async () => {
  edgeCounts.limited = 0;
  const target = stubTarget();
  assert.equal((await handleUpgrade(upgrade(), { WS_UPGRADE_LIMITER: stubLimiter(() => false) }, target)).status, 200);
  assert.equal((await handleUpgrade(upgrade({ 'CF-Connecting-IP': '203.0.113.7' }), {}, target)).status, 200);
  assert.equal(target.seen.length, 2);
});
