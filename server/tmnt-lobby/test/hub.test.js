import assert from 'node:assert/strict';
import { test } from 'node:test';

import { Hub } from '../src/hub.js';
import { BUCKETS, CAPABILITIES, LIMITS, MAX_PARTY, PROTOCOL, RELAY, normalizeFriendCode, playerIdFor } from '../src/protocol.js';
import { befriend, buildParty, greeted, identity, makeHub } from './harness.js';

const HEX16 = /^[0-9a-f]{16}$/;

// A host with a party and `n` online friends, none invited yet.
function hostWithFriends(env, n, { first = 1, build } = {}) {
  const host = greeted(env, first);
  const created = host.call('party.create');
  const friends = [];
  for (let i = 1; i <= n; i++) {
    const friend = greeted(env, first + i, build ? { build } : undefined);
    befriend(host, friend);
    friends.push(friend);
  }
  host.inbox = [];
  refill(env);
  return { host, created, friends };
}

// The social operations are limited per socket; a test that makes many moves the clock instead of waiting.
function refill(env) {
  env.clock.now += 60000;
}

const invite = (host, friend) => host.call('invite.send', { player: friend.who.player });

test('hello verifies the identity and returns the friend code', () => {
  const env = makeHub();
  const a = env.connect(identity(1));
  const reply = a.hello();
  assert.equal(reply.code, 'ok');
  assert.equal(reply.friend_code, identity(1).player);
  assert.match(reply.friend_code, /^[A-HJ-NP-Z2-9]{4}-[A-HJ-NP-Z2-9]{4}$/);
  assert.equal(reply.token, undefined);
});

test('hello answers with the service capabilities', () => {
  const env = makeHub();
  const reply = env.connect(identity(1)).hello({ relay: true });
  assert.deepEqual(reply.capabilities, CAPABILITIES);
  assert.equal(reply.capabilities.protocol_version, 4);
  assert.equal(reply.capabilities.max_party, MAX_PARTY);
  assert.equal(reply.capabilities.relay_frame_version, 2);
  assert.equal(reply.capabilities.public_endpoints, true);
  assert.equal(reply.capabilities.relay_supported, true);
  assert.equal(reply.capabilities.relay_max_payload, RELAY.maxPayload);
});

test('friend codes are stable and distinct per identity', () => {
  assert.equal(playerIdFor(identity(1).token), playerIdFor(identity(1).token));
  assert.notEqual(identity(1).player, identity(2).player);
  assert.equal(normalizeFriendCode('abcd 2345'), 'ABCD-2345');
});

test('a wrong player id, a different token for a known player and a bad protocol are refused', () => {
  const env = makeHub();
  const a = env.connect(identity(1));
  assert.equal(a.hello({ player: identity(2).player }).code, 'unauthorized');
  assert.equal(a.hello({ proto: PROTOCOL + 1 }).code, 'proto');
  assert.equal(a.hello({ proto: 3 }).code, 'proto');
  assert.equal(a.hello({ game: 'other' }).code, 'bad_game');
  assert.equal(a.hello({ guest: 'base099' }).code, 'bad_guest');
  assert.equal(a.hello({ token: 'zz' }).code, 'bad_request');
  assert.equal(a.hello().code, 'ok');
  assert.equal(a.hello().code, 'bad_request');
});

test('operations before hello are refused', () => {
  const env = makeHub();
  const a = env.connect(identity(1));
  assert.equal(a.call('friends.list').code, 'no_session');
  assert.equal(a.call('party.create').code, 'no_session');
});

test('malformed messages never crash the hub', () => {
  const env = makeHub();
  const a = greeted(env, 1);
  assert.equal(a.raw('not json').code, 'bad_request');
  assert.equal(a.raw('[]').code, 'bad_request');
  assert.equal(a.raw('{"op":5,"seq":1}').code, 'bad_request');
  assert.equal(a.raw('{"op":"friends.list"}').code, 'bad_request');
  assert.equal(a.call('no.such.op').code, 'bad_request');
  assert.equal(a.call('match.hello').code, 'bad_request');
  assert.equal(a.call('friends.add', { code: 42 }).code, 'bad_request');
  assert.equal(a.call('friends.add', { code: 'ABCD' }).code, 'bad_request');
  assert.equal(a.call('pm.join', { session: 42 }).code, 'not_found');
  assert.equal(a.call('invite.accept', { invite: 42 }).code, 'not_found');
  assert.equal(a.call('invite.send', { player: 'x' }).code, 'no_party');
  assert.equal(a.call('challenge.send', { player: 'x' }).code, 'bad_request');
  assert.equal(a.raw('x'.repeat(LIMITS.frameBytes + 1)).code, 'too_large');
  assert.equal(a.call('ping').code, 'ok');
  assert.deepEqual(env.logs, []);
});

test('unknown friend code is not found; own code is refused', () => {
  const env = makeHub();
  const a = greeted(env, 1);
  assert.equal(a.call('friends.add', { code: identity(9).player }).code, 'not_found');
  assert.equal(a.call('friends.add', { code: a.who.player }).code, 'bad_request');
});

test('friend request, accept, duplicate handling and remove', () => {
  const env = makeHub();
  const a = greeted(env, 1);
  const b = greeted(env, 2);
  const sent = a.call('friends.add', { code: b.who.player.toLowerCase() });
  assert.deepEqual([sent.code, sent.state, sent.player], ['ok', 'pending', b.who.player]);
  assert.equal(a.call('friends.add', { code: b.who.player }).state, 'pending');
  const request = b.events('friend.request');
  assert.equal(request.length, 1);
  assert.equal(request[0].player, a.who.player);
  assert.equal(b.call('friends.requests').incoming.length, 1);
  assert.equal(a.call('friends.requests').outgoing.length, 1);

  const accepted = b.call('friends.accept', { player: a.who.player });
  assert.equal(accepted.friend.player, a.who.player);
  assert.equal(a.events('friend.added').length, 1);
  assert.equal(a.call('friends.add', { code: b.who.player }).state, 'friends');
  assert.equal(b.call('friends.requests').incoming.length, 0);
  assert.equal(a.call('friends.list').friends.length, 1);
  assert.equal(b.call('friends.accept', { player: a.who.player }).code, 'not_found');

  assert.equal(a.call('friends.remove', { player: b.who.player }).code, 'ok');
  assert.equal(b.events('friend.removed').length, 1);
  assert.equal(a.call('friends.list').friends.length, 0);
  assert.equal(b.call('friends.list').friends.length, 0);
});

test('requests in both directions become a friendship; decline and cancel clear them', () => {
  const env = makeHub();
  const a = greeted(env, 1);
  const b = greeted(env, 2);
  const c = greeted(env, 3);
  a.call('friends.add', { code: b.who.player });
  assert.equal(b.call('friends.add', { code: a.who.player }).state, 'friends');
  assert.equal(a.call('friends.list').friends.length, 1);
  c.call('friends.add', { code: a.who.player });
  assert.equal(a.call('friends.decline', { player: c.who.player }).code, 'ok');
  assert.equal(a.call('friends.requests').incoming.length, 0);
  c.call('friends.add', { code: b.who.player });
  assert.equal(c.call('friends.decline', { player: b.who.player }).code, 'ok');
  assert.equal(b.call('friends.requests').incoming.length, 0);
  assert.equal(b.call('friends.decline', { player: c.who.player }).code, 'not_found');
});

test('friends persist across a hub restart and an offline friend has no presence', () => {
  const env = makeHub();
  const a = greeted(env, 1);
  const b = greeted(env, 2);
  befriend(a, b);
  b.drop();
  const list = a.call('friends.list').friends;
  assert.equal(list[0].online, false);
  assert.equal(list[0].state, 'offline');
  assert.equal(a.events('friend.offline').length, 1);
  const b2 = greeted(env, 2);
  assert.equal(b2.call('friends.list').friends.length, 1);
  assert.equal(a.call('friends.list').friends[0].online, true);
});

test('presence carries platform and application version, and goes online and offline', () => {
  const env = makeHub();
  const a = greeted(env, 1);
  const b = env.connect(identity(2), { platform: 'Linux', version: '0.3.0' });
  befriend(a, greeted(env, 3));
  b.hello();
  befriend(a, b);
  const view = a.call('friends.list').friends.find((f) => f.player === b.who.player);
  assert.deepEqual([view.online, view.state, view.platform, view.version], [true, 'online', 'Linux', '0.3.0']);
  b.drop();
  assert.equal(a.call('friends.list').friends.find((f) => f.player === b.who.player).online, false);
  const probe = env.connect(identity(2), { presence: false });
  probe.hello();
  assert.equal(a.call('friends.list').friends.find((f) => f.player === b.who.player).online, false);
});

test('a status event reaches friends when someone comes online or renames', () => {
  const env = makeHub();
  const a = greeted(env, 1);
  const b = greeted(env, 2);
  befriend(a, b);
  b.drop();
  a.inbox = [];
  const b2 = env.connect(identity(2), { name: 'NewName' });
  b2.hello();
  const status = a.events('friend.status');
  assert.equal(status.at(-1).friend.name, 'NewName');
  assert.equal(status.at(-1).friend.online, true);
});

test('an unknown game image is refused at sign-in', () => {
  const env = makeHub();
  const other = env.connect(identity(30), { guest: 'other' });
  assert.equal(other.hello().code, 'bad_guest');
});

// ---- parties ----

test('party.create makes a party of one, answers the credentials to its owner and shows friends a party state', () => {
  const env = makeHub();
  const a = greeted(env, 1);
  const b = greeted(env, 2);
  befriend(a, b);
  const created = a.call('party.create');
  assert.equal(created.code, 'ok');
  assert.match(created.match, HEX16);
  assert.match(created.diag, /^[0-9a-f]{12}$/);
  assert.match(created.join_token, /^[0-9a-f]{32}$/);
  assert.deepEqual([created.slot, created.generation], [1, 0]);
  assert.deepEqual(created.party.members.map((m) => [m.slot, m.player, m.state]), [[1, a.who.player, 'joined']]);
  assert.deepEqual([created.party.match, created.party.diag, created.party.host, created.party.session, created.party.invited],
    [created.match, created.diag, a.who.player, '', []]);
  assert.equal(a.call('party.create').code, 'busy');
  assert.equal(b.call('friends.list').friends[0].state, 'party');
  assert.equal(b.events('friend.status').at(-1).friend.state, 'party');
  assert.equal(env.hub.stats().parties, 1);
  const probe = env.connect(identity(3), { presence: false });
  probe.hello();
  assert.equal(probe.call('party.create').code, 'bad_request');
});

test('the diag id is random per party, shared by its members and not derived from anyone', () => {
  const env = makeHub();
  const one = buildParty(env, 3, { connect: false });
  const two = buildParty(env, 2, { connect: false, first: 20 });
  assert.match(one.diag, /^[0-9a-f]{12}$/);
  assert.deepEqual(new Set(one.data.map((d) => d.diag)), new Set([one.diag]));
  assert.notEqual(one.diag, two.diag);
  assert.ok(!env.logs.join('\n').includes(one.clients[0].who.player));
  assert.ok(env.logs.some((l) => l.includes(`diag=${one.diag}`)));
});

test('an invite carries the sender and a lifetime; accepting joins the sender party at the lowest free slot', () => {
  const env = makeHub();
  const { host, created, friends: [b] } = hostWithFriends(env, 1);
  const sent = invite(host, b);
  assert.equal(sent.code, 'ok');
  assert.match(sent.invite, HEX16);
  const [received] = b.events('invite');
  assert.deepEqual(received, { ev: 'invite', invite: sent.invite, from: { player: host.who.player, name: host.fields.name },
    expires_ms: LIMITS.inviteMs, session: '', slots: MAX_PARTY });
  assert.deepEqual(host.events('party.update').at(-1).party.invited, [{ player: b.who.player, name: b.fields.name }]);

  const accepted = b.call('invite.accept', { invite: sent.invite });
  assert.deepEqual([accepted.code, accepted.match, accepted.diag, accepted.slot, accepted.generation],
    ['ok', created.match, created.diag, 2, 0]);
  assert.match(accepted.join_token, /^[0-9a-f]{32}$/);
  assert.notEqual(accepted.join_token, created.join_token);
  assert.deepEqual(accepted.party.members.map((m) => m.player), [host.who.player, b.who.player]);
  assert.deepEqual(accepted.party.invited, []);
  assert.deepEqual(host.events('invite.ended'), [{ ev: 'invite.ended', invite: sent.invite, reason: 'accepted' }]);
  const update = host.events('party.update').at(-1).party;
  assert.deepEqual([update.members.length, update.invited.length, update.generation > accepted.party.generation - 1], [2, 0, true]);
  assert.equal(b.events('party.update').at(-1).party.match, created.match);
  assert.equal(b.call('friends.list').friends[0].state, 'party');
  assert.equal(b.call('invite.accept', { invite: sent.invite }).code, 'not_found');
});

test('a host and three guests: three invites share one match, slots 2 to 4, and everyone sees the same party', () => {
  const env = makeHub();
  const { host, created, friends } = hostWithFriends(env, 3);
  const invites = friends.map((f) => invite(host, f).invite);
  assert.deepEqual(env.host.sql.all('SELECT DISTINCT party FROM party_invites').map((r) => r.party), [created.match]);
  assert.equal(env.hub.stats().invites, 3);
  const answers = friends.map((f, i) => f.call('invite.accept', { invite: invites[i] }));
  assert.deepEqual(answers.map((a) => a.match), [created.match, created.match, created.match]);
  assert.deepEqual(answers.map((a) => a.slot), [2, 3, 4]);
  assert.equal(new Set([created.join_token, ...answers.map((a) => a.join_token)]).size, 4);
  const snapshot = host.events('party.update').at(-1).party;
  assert.deepEqual(snapshot.members.map((m) => m.slot), [1, 2, 3, 4]);
  for (const f of friends) assert.deepEqual(f.events('party.update').at(-1).party.members, snapshot.members);
  assert.equal(host.events('invite.ended').filter((e) => e.reason === 'accepted').length, 3);
  assert.deepEqual(env.hub.stats(), { ...env.hub.stats(), parties: 1, members: 4, invites: 0 });
});

test('a fifth member is turned away: the invites left when the party fills end party_full', () => {
  const env = makeHub();
  const { host, friends } = hostWithFriends(env, 5);
  const invites = friends.map((f) => invite(host, f).invite);
  for (let i = 0; i < 3; i++) assert.equal(friends[i].call('invite.accept', { invite: invites[i] }).slot, i + 2);
  for (const late of [3, 4]) {
    assert.deepEqual(friends[late].events('invite.ended'), [{ ev: 'invite.ended', invite: invites[late], reason: 'party_full' }]);
    assert.equal(friends[late].call('invite.accept', { invite: invites[late] }).code, 'party_full');
  }
  const extra = greeted(env, 40);
  befriend(host, extra);
  assert.equal(invite(host, extra).code, 'party_full');
  assert.equal(env.hub.stats().members, MAX_PARTY);
  assert.equal(env.hub.stats().invites, 0);
});

test('invite errors: no party, not host, not friends, offline, busy, already invited, version, full', () => {
  const env = makeHub();
  const a = greeted(env, 1);
  const b = greeted(env, 2);
  const c = greeted(env, 3, { build: 2 });
  befriend(a, b);
  befriend(a, c);
  assert.equal(invite(a, b).code, 'no_party');
  const created = a.call('party.create');
  assert.equal(a.call('invite.send', { player: identity(9).player }).code, 'not_friends');
  assert.equal(invite(a, c).code, 'version_mismatch');
  assert.equal(a.call('invite.send', { player: 'nope' }).code, 'bad_request');
  assert.equal(a.call('invite.send', { player: a.who.player }).code, 'bad_request');
  refill(env);
  const first = invite(a, b);
  assert.equal(invite(a, b).code, 'already_invited');
  assert.equal(b.events('invite').length, 1);
  b.call('invite.accept', { invite: first.invite });
  const d = greeted(env, 4);
  befriend(b, d);
  assert.equal(invite(b, d).code, 'not_host');
  const other = d.call('party.create');
  assert.equal(other.code, 'ok');
  const e = greeted(env, 5);
  befriend(a, e);
  refill(env);
  assert.equal(invite(a, d).code, 'not_friends');
  befriend(a, d);
  assert.equal(invite(a, d).code, 'busy');
  assert.equal(invite(a, e).code, 'ok');
  e.drop();
  assert.equal(invite(a, e).code, 'offline');
  assert.ok(created.match);
});

test('declining ends an invite for the host; an unknown decline is harmless', () => {
  const env = makeHub();
  const { host, friends: [b] } = hostWithFriends(env, 1);
  const sent = invite(host, b).invite;
  assert.equal(b.call('invite.decline', { invite: sent }).code, 'ok');
  assert.deepEqual(host.events('invite.ended'), [{ ev: 'invite.ended', invite: sent, reason: 'declined' }]);
  assert.equal(host.events('party.update').at(-1).party.invited.length, 0);
  assert.equal(b.call('invite.decline', { invite: sent }).code, 'ok');
  assert.equal(b.call('invite.accept', { invite: sent }).code, 'not_found');
  assert.equal(invite(host, b).code, 'ok');
});

test('an invite ends when it expires, with an exact answer to a late accept', () => {
  const env = makeHub();
  const { host, friends: [b, c] } = hostWithFriends(env, 2);
  const swept = invite(host, b).invite;
  env.advance(LIMITS.inviteMs + 1000);
  assert.equal(b.events('invite.ended').at(-1).reason, 'expired');
  assert.equal(host.events('invite.ended').at(-1).reason, 'expired');
  assert.equal(b.call('invite.accept', { invite: swept }).code, 'not_found');

  const late = invite(host, c).invite;
  env.clock.now += LIMITS.inviteMs + 1000;
  assert.equal(c.call('invite.accept', { invite: late }).code, 'expired');
  assert.equal(host.events('invite.ended').at(-1).reason, 'expired');
  assert.equal(env.hub.stats().members, 1);
});

test('the host leaving, or going offline, ends its invites for the invitees', () => {
  const env = makeHub();
  const { host, friends: [b, c] } = hostWithFriends(env, 2);
  const left = invite(host, b).invite;
  assert.equal(host.call('party.leave').code, 'ok');
  assert.deepEqual(b.events('invite.ended'), [{ ev: 'invite.ended', invite: left, reason: 'host_left' }]);
  assert.equal(b.call('invite.accept', { invite: left }).code, 'host_left');

  const again = greeted(env, 50);
  befriend(again, c);
  again.call('party.create');
  const gone = invite(again, c).invite;
  again.drop();
  assert.equal(c.events('invite.ended').at(-1).reason, 'host_left');
  assert.equal(c.call('invite.accept', { invite: gone }).code, 'host_left');
});

test('accepting one invite ends the others of the invitee', () => {
  const env = makeHub();
  const one = hostWithFriends(env, 1, { first: 1 });
  const two = greeted(env, 20);
  two.call('party.create');
  const target = one.friends[0];
  befriend(two, target);
  const first = invite(one.host, target).invite;
  const second = invite(two, target).invite;
  assert.equal(target.call('invite.accept', { invite: first }).slot, 2);
  assert.deepEqual(two.events('invite.ended'), [{ ev: 'invite.ended', invite: second, reason: 'accepted_elsewhere' }]);
  assert.equal(target.events('invite.ended').filter((e) => e.invite === second)[0].reason, 'accepted_elsewhere');
  assert.equal(target.call('invite.accept', { invite: second }).code, 'not_found');
  assert.equal(two.events('party.update').at(-1).party.invited.length, 0);
});

test('an invitee already in a party cannot accept another', () => {
  const env = makeHub();
  const { host, friends: [b] } = hostWithFriends(env, 1);
  const sent = invite(host, b).invite;
  const own = b.call('party.create');
  assert.equal(own.code, 'ok');
  assert.equal(b.call('invite.accept', { invite: sent }).code, 'busy');
});

test('a freed slot goes to the next joiner and the others keep theirs', () => {
  const env = makeHub();
  const { host, friends } = hostWithFriends(env, 4);
  const slots = friends.slice(0, 3).map((f) => f.call('invite.accept', { invite: invite(host, f).invite }).slot);
  assert.deepEqual(slots, [2, 3, 4]);
  assert.equal(friends[0].call('party.leave').code, 'ok');
  const update = host.events('party.update').at(-1).party;
  assert.deepEqual(update.members.map((m) => m.slot), [1, 3, 4]);
  assert.equal(friends[0].call('friends.list').friends.find((f) => f.player === host.who.player).state, 'party');
  assert.equal(friends[3].call('invite.accept', { invite: invite(host, friends[3]).invite }).slot, 2);
});

test('a member leaving tells the rest; the host leaving ends the party for everyone', () => {
  const env = makeHub();
  const { host, created, friends: [b, c] } = hostWithFriends(env, 2);
  b.call('invite.accept', { invite: invite(host, b).invite });
  c.call('invite.accept', { invite: invite(host, c).invite });
  b.inbox = [];
  c.inbox = [];
  assert.equal(b.call('party.leave').code, 'ok');
  assert.deepEqual(c.events('party.update').at(-1).party.members.map((m) => m.slot), [1, 3]);
  assert.equal(b.events('party.ended').length, 0);
  assert.equal(b.call('party.leave').code, 'ok');
  assert.equal(b.call('friends.list').friends.find((f) => f.player === host.who.player).state, 'party');

  assert.equal(host.call('party.leave').code, 'ok');
  assert.deepEqual(c.events('party.ended'), [{ ev: 'party.ended', match: created.match, reason: 'host_left' }]);
  assert.equal(host.events('party.ended').length, 0);
  assert.deepEqual([env.hub.stats().parties, env.hub.stats().members], [0, 0]);
  assert.equal(c.call('friends.list').friends.find((f) => f.player === host.who.player).state, 'online');
  assert.equal(host.call('party.create').code, 'ok');
});

test('only the host can kick, and a kicked member hears it', () => {
  const env = makeHub();
  const { host, created, friends: [b, c] } = hostWithFriends(env, 2);
  b.call('invite.accept', { invite: invite(host, b).invite });
  c.call('invite.accept', { invite: invite(host, c).invite });
  b.inbox = [];
  c.inbox = [];
  assert.equal(b.call('party.kick', { player: c.who.player }).code, 'not_host');
  assert.equal(host.call('party.kick', { player: host.who.player }).code, 'bad_request');
  assert.equal(host.call('party.kick', { player: identity(90).player }).code, 'not_found');
  assert.equal(host.call('party.kick', { player: 'x' }).code, 'bad_request');
  assert.equal(greeted(env, 91).call('party.kick', { player: b.who.player }).code, 'no_party');
  assert.equal(host.call('party.kick', { player: b.who.player }).code, 'ok');
  assert.deepEqual(b.events('party.ended'), [{ ev: 'party.ended', match: created.match, reason: 'kicked' }]);
  assert.deepEqual(c.events('party.update').at(-1).party.members.map((m) => m.slot), [1, 3]);
  assert.equal(b.call('party.leave').code, 'ok');
  assert.equal(b.call('party.create').code, 'ok');
});

// ---- the party game session ----

test('pm.host creates the party when there is none and advertises its game session', () => {
  const env = makeHub();
  const a = greeted(env, 1);
  const hosted = a.call('pm.host', { slots: 3, rules: { rounds: 2, timer: 1, rule: 0 } });
  assert.equal(hosted.code, 'ok');
  assert.match(hosted.session, HEX16);
  assert.match(hosted.match, HEX16);
  assert.match(hosted.join_token, /^[0-9a-f]{32}$/);
  assert.deepEqual([hosted.slot, hosted.generation, hosted.party.session], [1, 0, hosted.session]);
  assert.equal(env.hub.stats().parties, 1);
  const again = a.call('pm.host', { slots: 4 });
  assert.deepEqual([again.session, again.match, again.join_token], [hosted.session, undefined, undefined]);
  assert.equal(env.hub.stats().parties, 1);
  assert.equal(env.hub.stats().player_matches, 1);
});

test('pm.host keeps an existing party and is for the host only', () => {
  const env = makeHub();
  const { host, created, friends: [b] } = hostWithFriends(env, 1);
  b.call('invite.accept', { invite: invite(host, b).invite });
  const hosted = host.call('pm.host', {});
  assert.equal(hosted.session.length, 16);
  assert.equal(hosted.match, undefined);
  assert.equal(host.events('party.update').at(-1).party.session, hosted.session);
  assert.equal(b.events('party.update').at(-1).party.session, hosted.session);
  assert.equal(b.call('pm.host', {}).code, 'not_host');
  assert.equal(created.match, host.call('pm.host', {}).match ?? created.match);
});

test('the QoS data, rules and slots of the advertisement are validated', () => {
  const env = makeHub();
  const a = greeted(env, 1);
  const b = greeted(env, 2);
  const blob = '0a'.repeat(112);
  a.call('party.create');
  befriend(a, b);
  b.call('invite.accept', { invite: invite(a, b).invite });
  refill(env);
  assert.equal(a.call('pm.host', { qos: blob }).code, 'ok');
  assert.equal(b.call('pm.search').sessions[0].qos, blob);
  assert.equal(a.call('pm.host', { qos: 'xyz' }).code, 'bad_request');
  assert.equal(a.call('pm.host', { qos: '0'.repeat(513) }).code, 'bad_request');
  assert.equal(a.call('pm.host', {}).code, 'ok');
  assert.equal(b.call('pm.search').sessions[0].qos, '');
  refill(env);
  for (const slots of [1, 5, 6, 2.5, '3']) assert.equal(a.call('pm.host', { slots }).code, 'bad_request');
  refill(env);
  for (const slots of [2, 3, 4]) assert.equal(a.call('pm.host', { slots }).code, 'ok');
  assert.equal(a.call('pm.host', { rules: { rounds: 999 } }).code, 'bad_request');
  assert.equal(a.call('pm.host', { rules: { 'Bad Key': 1 } }).code, 'bad_request');
  assert.equal(a.call('pm.host', { rules: [] }).code, 'bad_request');
  assert.equal(env.hub.stats().player_matches, 1);
  const fresh = greeted(env, 7);
  assert.equal(fresh.call('pm.host', { slots: 6 }).code, 'bad_request');
  assert.equal(env.hub.stats().parties, 1);
});

test('pm.search lists the own party advertisement to its guests and the open sessions of friends to players in no party', () => {
  const env = makeHub();
  const { host, friends: [b, c, d] } = hostWithFriends(env, 3);
  const stranger = greeted(env, 30);
  const other = greeted(env, 31);
  befriend(other, stranger);
  other.call('pm.host', {});
  const hosted = host.call('pm.host', { slots: 4, rules: { rounds: 2 } });
  assert.deepEqual(b.call('pm.search').sessions.map((s) => s.session), [hosted.session]);
  b.call('invite.accept', { invite: invite(host, b).invite });
  c.call('invite.accept', { invite: invite(host, c).invite });
  const found = b.call('pm.search').sessions;
  assert.equal(found.length, 1);
  assert.deepEqual([found[0].session, found[0].player, found[0].slots, found[0].rules],
    [hosted.session, host.who.player, 4, { rounds: 2 }]);
  assert.equal(found[0].ip, undefined);
  assert.equal(c.call('pm.search').sessions.length, 1);
  assert.deepEqual(host.call('pm.search').sessions, []);
  assert.deepEqual(d.call('pm.search').sessions.map((s) => s.session), [hosted.session]);
  assert.deepEqual(stranger.call('pm.search').sessions.map((s) => s.player), [other.who.player]);
  assert.deepEqual(other.call('pm.search').sessions, []);
});

test('pm.search answers with at most the service\'s limit of sessions', () => {
  const env = makeHub();
  const seeker = greeted(env, 1);
  for (let i = 0; i < LIMITS.searchResults + 3; i++) {
    const host = greeted(env, 100 + i, { ip: `198.18.2.${i + 1}` });
    env.host.sql.run('INSERT INTO friends (player, friend, since) VALUES (?, ?, 1)', seeker.who.player, host.who.player);
    assert.equal(host.call('pm.host', {}).code, 'ok');
  }
  assert.equal(seeker.call('pm.search').sessions.length, LIMITS.searchResults);
});

test('a new advertisement on an existing party counts as creating one', () => {
  const env = makeHub();
  const host = greeted(env, 1);
  assert.equal(host.call('pm.host', {}).code, 'ok');
  let refused = null;
  for (let i = 0; i < 20 && !refused; i++) {
    assert.equal(host.call('pm.cancel').code, 'ok');
    const again = host.call('pm.host', {});
    if (again.code !== 'ok') refused = again.code;
  }
  assert.equal(refused, 'limit');
  env.clock.now += 60000;
  assert.equal(host.call('pm.host', {}).code, 'ok');
  // The refresh of a standing advertisement costs no creation.
  env.clock.now += 200000;
  for (let i = 0; i < 8; i++) {
    env.clock.now += 30000;
    assert.equal(host.call('pm.host', {}).code, 'ok');
  }
});

test('a friend in no party joins the hosted game session directly and becomes the next member', () => {
  const env = makeHub();
  const { host, friends: [b, c] } = hostWithFriends(env, 2);
  const stranger = greeted(env, 30);
  const hosted = host.call('pm.host', { slots: 2 });
  assert.equal(stranger.call('pm.join', { session: hosted.session }).code, 'not_found');
  const joined = b.call('pm.join', { session: hosted.session });
  assert.equal(joined.code, 'ok');
  assert.equal(joined.slot, 2);
  assert.ok(joined.join_token && joined.match && joined.party.members.length === 2);
  assert.deepEqual(c.call('pm.search').sessions, []);
  assert.equal(c.call('pm.join', { session: hosted.session }).code, 'party_full');
});

test('friend matchmaking fills four slots, hides a full session, and drops the ad when the host leaves', () => {
  const env = makeHub();
  const { host, friends: [b, c, d, e] } = hostWithFriends(env, 4);
  const session = host.call('pm.host', { slots: 4 }).session;
  assert.deepEqual([b, c, d].map((g) => g.call('pm.join', { session }).slot), [2, 3, 4]);
  assert.deepEqual(e.call('pm.search').sessions, []);
  assert.equal(e.call('pm.join', { session }).code, 'party_full');
  host.drop();
  assert.equal(env.hub.stats().player_matches, 0);
  assert.deepEqual(e.call('pm.search').sessions, []);
  assert.equal(e.call('pm.join', { session }).code, 'not_found');
});

test('pm.search and pm.join hide an advertisement whose build differs', () => {
  const env = makeHub();
  const host = greeted(env, 1);
  const other = greeted(env, 3, { build: 2 });
  befriend(host, other);
  const session = host.call('pm.host', { slots: 4 }).session;
  assert.deepEqual(other.call('pm.search').sessions, []);
  assert.equal(other.call('pm.join', { session }).code, 'version_mismatch');
});

test('an expired advertisement that was not swept answers offline and is removed', () => {
  const env = makeHub();
  const { host, friends: [b] } = hostWithFriends(env, 1);
  const session = host.call('pm.host', {}).session;
  env.clock.now += LIMITS.playerMatchMs + 1000;
  assert.deepEqual(b.call('pm.search').sessions, []);
  assert.equal(b.call('pm.join', { session }).code, 'offline');
  assert.equal(env.hub.stats().player_matches, 0);
});

test('an invite carries the hosted session id, and accepting it reaches the hidden match', () => {
  const env = makeHub();
  const { host, created, friends: [b, c] } = hostWithFriends(env, 2);
  const quiet = invite(host, c).invite;
  assert.equal(c.events('invite').at(-1).session, '');
  host.call('invite.send', { player: c.who.player });
  const session = host.call('pm.host', {}).session;
  const sent = invite(host, b);
  assert.equal(b.events('invite').at(-1).session, session);
  const accepted = b.call('invite.accept', { invite: sent.invite });
  assert.equal(accepted.code, 'ok');
  assert.equal(accepted.match, created.match);
  assert.equal(b.call('pm.join', { session }).session, session);
  assert.ok(quiet);
});

test('an invitee that disconnects loses the invite and a second socket keeps presence', () => {
  const env = makeHub();
  const { host, friends: [b] } = hostWithFriends(env, 1);
  const sent = invite(host, b).invite;
  const second = env.connect(b.who);
  second.hello();
  b.drop();
  assert.equal(host.call('friends.list').friends[0].online, true);
  second.drop();
  assert.equal(host.call('friends.list').friends[0].online, false);
  assert.equal(env.hub.stats().invites, 0);
  const back = env.connect(b.who);
  back.hello();
  assert.equal(back.call('invite.accept', { invite: sent }).code, 'not_found');
});

test('a socket without presence cannot host, invite, accept or join', () => {
  const env = makeHub();
  const { host, friends: [b] } = hostWithFriends(env, 1);
  const session = host.call('pm.host', {}).session;
  const probe = env.connect(identity(2), { presence: false });
  probe.hello();
  for (const [op, body] of [['party.create', {}], ['pm.host', {}], ['invite.send', { player: host.who.player }],
    ['invite.accept', { invite: 'x' }], ['pm.join', { session }]]) {
    assert.equal(probe.call(op, body).code, 'bad_request', op);
  }
  assert.ok(b);
});

test('joining the game session needs the own party advertisement; it marks the member and answers its connection data', () => {
  const env = makeHub();
  const { host, created, friends: [b, c] } = hostWithFriends(env, 2);
  const stranger = greeted(env, 30);
  const hosted = host.call('pm.host', { slots: 3 });
  const slotB = b.call('invite.accept', { invite: invite(host, b).invite }).slot;
  c.call('invite.accept', { invite: invite(host, c).invite });
  assert.equal(stranger.call('pm.join', { session: hosted.session }).code, 'not_found');
  assert.equal(b.call('pm.join', { session: 'nope' }).code, 'not_found');
  assert.equal(host.call('pm.join', { session: hosted.session }).code, 'bad_request');
  assert.equal(host.call('friends.list').friends.find((f) => f.player === b.who.player).state, 'party');
  const joined = b.call('pm.join', { session: hosted.session });
  assert.deepEqual([joined.code, joined.session, joined.match, joined.slot, joined.generation],
    ['ok', hosted.session, created.match, slotB, 0]);
  assert.equal(host.call('friends.list').friends.find((f) => f.player === b.who.player).state, 'connecting');
  assert.equal(host.call('friends.list').friends.find((f) => f.player === c.who.player).state, 'party');
});

test('cancel, the host going offline and expiry remove the advertisement; the party stays', () => {
  const env = makeHub();
  const { host, friends: [b] } = hostWithFriends(env, 1);
  b.call('invite.accept', { invite: invite(host, b).invite });
  host.call('pm.host', {});
  assert.equal(host.call('pm.cancel').code, 'ok');
  assert.equal(b.call('pm.search').sessions.length, 0);
  assert.equal(host.events('party.update').at(-1).party.session, '');
  assert.equal(b.call('friends.list').friends[0].state, 'party');
  assert.equal(host.call('pm.cancel').code, 'ok');

  host.call('pm.host', {});
  host.drop();
  assert.equal(env.hub.stats().player_matches, 0);
  assert.equal(env.hub.stats().parties, 1);

  const host2 = greeted(env, 1);
  const stale = host2.call('pm.host', {}).session;
  env.advance(LIMITS.playerMatchMs + 1000);
  assert.equal(env.hub.stats().player_matches, 0);
  assert.equal(b.call('pm.join', { session: stale }).code, 'not_found');
  assert.equal(env.hub.stats().parties, 1);
});

test('a refreshed advertisement outlives the expiry window', () => {
  const env = makeHub();
  const { host, friends: [b] } = hostWithFriends(env, 1);
  b.call('invite.accept', { invite: invite(host, b).invite });
  const session = host.call('pm.host', {}).session;
  env.advance(LIMITS.playerMatchMs - 1000);
  host.call('pm.host', {});
  env.advance(LIMITS.playerMatchMs - 1000);
  assert.equal(b.call('pm.search').sessions[0].session, session);
});

test('busy players cannot create or host a second party', () => {
  const env = makeHub();
  const { host, friends: [b] } = hostWithFriends(env, 1);
  b.call('invite.accept', { invite: invite(host, b).invite });
  assert.equal(b.call('party.create').code, 'busy');
  assert.equal(host.call('party.create').code, 'busy');
  assert.equal(b.call('pm.host', {}).code, 'not_host');
});

test('a player hosting its own session cannot accept an invitation', () => {
  const env = makeHub();
  const { host, friends: [b] } = hostWithFriends(env, 1);
  const sent = invite(host, b).invite;
  b.call('pm.host', {});
  assert.equal(env.hub.stats().player_matches, 1);
  assert.equal(b.call('invite.accept', { invite: sent }).code, 'busy');
  assert.equal(env.hub.stats().player_matches, 1);
});

// ---- presence, restarts, expiry ----

test('a party outlives its directory socket: the member returns within the grace and is told the party', () => {
  const env = makeHub();
  const { clients: [host, b], data } = buildParty(env, 2, { connect: false });
  b.drop();
  assert.equal(host.call('friends.list').friends[0].online, false);
  env.advance(5000);
  const b2 = env.connect(b.who);
  b2.hello();
  const update = b2.events('party.update');
  assert.equal(update.length, 1);
  assert.equal(update[0].party.match, data[0].match);
  env.advance(LIMITS.graceMs * 2);
  assert.equal(env.hub.stats().members, 2);
});

test('a member gone from the directory without a room seat is removed after the grace; the host takes the party with it', () => {
  const env = makeHub();
  const { clients: [host, b, c], data } = buildParty(env, 3, { connect: false });
  host.inbox = [];
  c.inbox = [];
  b.drop();
  env.advance(LIMITS.graceMs + 1000);
  assert.deepEqual(host.events('party.update').at(-1).party.members.map((m) => m.slot), [1, 3]);
  assert.equal(env.hub.stats().members, 2);

  host.drop();
  env.advance(LIMITS.graceMs + 1000);
  assert.deepEqual(c.events('party.ended'), [{ ev: 'party.ended', match: data[0].match, reason: 'host_left' }]);
  assert.deepEqual([env.hub.stats().parties, env.hub.stats().members], [0, 0]);
});

test('a member whose room seat is held stays in the party while its directory socket is away', () => {
  const env = makeHub();
  const { clients: [host, b] } = buildParty(env, 2);
  b.drop();
  env.advance(LIMITS.graceMs + 1000);
  assert.equal(env.hub.stats().members, 2);
  assert.equal(host.events('party.ended').length, 0);
  assert.ok((env.host.alarmAt ?? Infinity) > env.clock.now, 'a held seat schedules no alarm in the past');
});

test('a party of the host alone expires after a long silence; activity keeps it', () => {
  const env = makeHub();
  const a = greeted(env, 1);
  const kept = a.call('party.create');
  env.advance(LIMITS.partyIdleMs - 60000);
  a.call('pm.host', {});
  a.inbox = [];
  env.advance(LIMITS.partyIdleMs - 60000);
  assert.equal(env.hub.stats().parties, 1);
  env.advance(120000);
  assert.deepEqual(a.events('party.ended'), [{ ev: 'party.ended', match: kept.match, reason: 'expired' }]);
  assert.equal(env.hub.stats().parties, 0);
});

test('a hub restart drops what only sockets kept alive and gives parties the grace period', () => {
  const env = makeHub();
  const { clients: [host, b], data } = buildParty(env, 2, { connect: false });
  host.call('pm.host', {});
  const pending = hostWithFriends(env, 1, { first: 30 });
  invite(pending.host, pending.friends[0]);
  env.host.conns.clear();
  env.hub.reconcile();
  assert.equal(env.hub.stats().player_matches, 0);
  assert.equal(env.hub.stats().invites, 0);
  assert.equal(env.hub.stats().parties, 2);
  const host2 = env.connect(host.who);
  host2.hello();
  const b2 = env.connect(b.who);
  b2.hello();
  assert.equal(host2.events('party.update')[0].party.match, data[0].match);
  env.advance(LIMITS.graceMs + 1000);
  assert.equal(env.hub.stats().parties, 1);
});

test('a hibernated object wakes with its parties, invites and sockets intact', () => {
  const env = makeHub();
  const { host, friends: [b] } = hostWithFriends(env, 1);
  const sent = invite(host, b).invite;
  const woke = new Hub(env.host);
  env.rooms.hub = woke;
  host.hub = woke;
  b.hub = woke;
  assert.equal(b.call('invite.accept', { invite: sent }).code, 'ok');
  assert.equal(host.events('party.update').at(-1).party.members.length, 2);
  assert.equal(env.hub.stats().members, 2);
});

test('two independent parties run side by side without crossing', () => {
  const env = makeHub();
  const one = buildParty(env, 3, { first: 1 });
  const two = buildParty(env, 2, { first: 20 });
  assert.notEqual(one.match, two.match);
  assert.equal(one.clients[0].call('party.leave').code, 'ok');
  assert.equal(two.clients[0].events('party.ended').length, 0);
  assert.equal(two.rooms[0].call('ping').code, 'ok');
  assert.equal(env.hub.stats().parties, 1);
  assert.equal(two.clients[1].call('friends.list').friends[0].state, 'party');
});

test('rate limits, per-address sockets and new-player limits', () => {
  const env = makeHub();
  const a = greeted(env, 1);
  let limited = 0;
  for (let i = 0; i < 100; i++) if (a.call('ping').code === 'limit') limited++;
  assert.ok(limited > 0);
  const crowd = makeHub();
  let admitted = 0;
  for (let i = 0; i < LIMITS.socketsPerIp + 4; i++) if (crowd.connect(identity(100 + i)).admitted) admitted++;
  assert.equal(admitted, LIMITS.socketsPerIp);
  const registrations = makeHub();
  const results = [];
  for (let i = 0; i < LIMITS.registrationsPerIpPerDay + 2; i++) {
    const c = registrations.connect(identity(200 + i), { ip: '198.51.100.4' });
    results.push(c.admitted ? c.hello().code : 'refused');
    if (c.admitted) c.drop();
  }
  assert.ok(results.includes('limit') || results.includes('refused'));
});

test('the social operations share a slower limit', () => {
  const env = makeHub();
  const a = greeted(env, 1);
  const results = [];
  for (let i = 0; i < 15; i++) results.push(a.call('friends.add', { code: identity(9).player }).code);
  assert.equal(results[0], 'not_found');
  assert.ok(results.includes('limit'));
  const pings = [];
  for (let i = 0; i < 15; i++) pings.push(a.call('ping').code);
  assert.ok(!pings.includes('limit'));
});

test('no more than the pending-invite cap is outstanding for one party', () => {
  const env = makeHub();
  const host = greeted(env, 1);
  host.call('party.create');
  const friends = [];
  for (let i = 0; i < LIMITS.pendingInvites + 1; i++) {
    const f = greeted(env, 100 + i);
    befriend(host, f);
    friends.push(f);
  }
  const codes = [];
  for (const f of friends) {
    env.advance(1500);
    host.call('ping');
    codes.push(invite(host, f).code);
  }
  assert.equal(codes.filter((c) => c === 'ok').length, LIMITS.pendingInvites);
  assert.equal(codes.at(-1), 'limit');
});

test('a join token is shown only to its owner and the hub keeps only its hash', () => {
  const env = makeHub();
  const { clients: [a, b], data } = buildParty(env, 2, { connect: false });
  assert.ok(!JSON.stringify(a.inbox).includes(data[1].join_token));
  assert.ok(!JSON.stringify(b.inbox).includes(data[0].join_token));
  const stored = env.host.sql.all('SELECT join_hash FROM party_members').map((r) => r.join_hash);
  assert.equal(stored.length, 2);
  for (const d of data) assert.ok(!stored.includes(d.join_token));
  const everything = JSON.stringify([...a.inbox, ...b.inbox, env.logs]);
  assert.ok(!everything.includes(a.who.token));
  assert.ok(!everything.includes(b.who.token));
  for (const d of data) assert.ok(!env.logs.join('\n').includes(d.join_token));
});

test('an invite and a party report the slots of the game session, and a smaller session fills sooner', () => {
  for (const slots of [2, 3, 4]) {
    const env = makeHub();
    const { host, friends } = hostWithFriends(env, 4);
    const { session } = host.call('pm.host', { slots });
    assert.equal(host.events('party.update').at(-1).party.slots, slots);
    const sent = host.call('invite.send', { player: friends[0].who.player });
    const [received] = friends[0].events('invite');
    assert.deepEqual([received.session, received.slots], [session, slots]);
    assert.equal(friends[0].call('invite.accept', { invite: sent.invite }).code, 'ok');
    for (let joined = 2; joined < slots; joined++) {
      const next = friends[joined - 1];
      assert.equal(next.call('invite.accept', { invite: host.call('invite.send', { player: next.who.player }).invite }).code, 'ok');
    }
    assert.equal(host.call('invite.send', { player: friends[3].who.player }).code, 'party_full');
  }
});

test('an invite out for the last place ends for the others once that place is taken, by invite or by a join', () => {
  const env = makeHub();
  const { host, friends: [b, c] } = hostWithFriends(env, 2);
  host.call('pm.host', { slots: 2 });
  const first = invite(host, b);
  const second = invite(host, c);
  assert.equal(b.call('invite.accept', { invite: first.invite }).code, 'ok');
  assert.equal(c.events('invite.ended').at(-1).reason, 'party_full');
  assert.equal(c.call('invite.accept', { invite: second.invite }).code, 'party_full');

  const quick = makeHub();
  const { host: other, friends: [d, e] } = hostWithFriends(quick, 2);
  const { session } = other.call('pm.host', { slots: 2 });
  invite(other, e);
  assert.equal(d.call('pm.join', { session }).code, 'ok');
  assert.equal(e.events('invite.ended').at(-1).reason, 'party_full');
});

test('cancelling the advertisement ends the invites to it and takes everyone out of the game', () => {
  const env = makeHub();
  const { host, friends: [b, c] } = hostWithFriends(env, 2);
  const { session } = host.call('pm.host', { slots: 3 });
  assert.equal(b.call('pm.join', { session }).code, 'ok');
  const sent = invite(host, c);
  assert.equal(host.call('friends.list').friends.find((f) => f.player === b.who.player).state, 'connecting');
  assert.equal(host.call('pm.cancel').code, 'ok');
  assert.equal(c.events('invite.ended').at(-1).reason, 'cancelled');
  assert.equal(c.call('invite.accept', { invite: sent.invite }).code, 'not_found');
  assert.equal(host.call('friends.list').friends.find((f) => f.player === b.who.player).state, 'party');
  assert.equal(b.call('friends.list').friends.find((f) => f.player === host.who.player).state, 'party');
  assert.equal(env.hub.stats().invites, 0);
});

test('presence follows a player through a match and back, for the host and for a guest', () => {
  const env = makeHub();
  const { host, created, friends: [b, watcher] } = hostWithFriends(env, 2);
  befriend(b, watcher);
  const stateOf = (who) => watcher.call('friends.list').friends.find((f) => f.player === who.who.player).state;
  assert.deepEqual([stateOf(host), stateOf(b)], ['party', 'online']);
  const { session } = host.call('pm.host', { slots: 2 });
  const joined = b.call('pm.join', { session });
  assert.deepEqual([stateOf(host), stateOf(b)], ['connecting', 'connecting']);
  const hostRoom = env.room(created.match, host.who);
  const guestRoom = env.room(created.match, b.who);
  hostRoom.hello(created);
  guestRoom.hello(joined);
  hostRoom.call('match.link', { peer: 2, state: 'connected' });
  guestRoom.call('match.link', { peer: 1, state: 'connected' });
  assert.deepEqual([stateOf(host), stateOf(b)], ['in_game', 'in_game']);

  // The guest's game ends: the guest is free at once and the host is no longer in a game.
  assert.equal(b.call('party.leave').code, 'ok');
  assert.equal(stateOf(b), 'online');
  assert.equal(watcher.events('friend.status').filter((e) => e.friend.player === b.who.player).at(-1).friend.state, 'online');
  assert.equal(stateOf(host), 'party');

  // The host's game ends: the party is gone with it, and the next match starts from nothing.
  assert.equal(host.call('pm.cancel').code, 'ok');
  assert.equal(host.call('party.leave').code, 'ok');
  assert.equal(stateOf(host), 'online');
  assert.equal(env.hub.stats().parties, 0);
  const second = host.call('pm.host', { slots: 2 });
  assert.notEqual(second.match, created.match);
  assert.notEqual(second.session, session);
});

test('a former host can join a friend as soon as its own party is over', () => {
  const env = makeHub();
  const { host, friends: [b] } = hostWithFriends(env, 1);
  host.call('pm.host', { slots: 2 });
  host.call('pm.cancel');
  host.call('party.leave');
  const { session } = b.call('pm.host', { slots: 2 });
  assert.equal(host.call('pm.search').sessions.length, 1);
  assert.equal(host.call('pm.join', { session }).code, 'ok');
});

test('a search has a budget of its own, which a client searching every few seconds never reaches', () => {
  const env = makeHub();
  const { host } = hostWithFriends(env, 1);
  for (let i = 0; i < 40; i++) {
    assert.equal(host.call('pm.search').code, 'ok');
    env.clock.now += 3000;
  }
  const replies = Array.from({ length: BUCKETS.search.burst + 2 }, () => host.call('pm.search').code);
  assert.ok(replies.includes('limit'));
});
