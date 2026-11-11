// Bug reports over the lobby WebSocket and in the admin store: what is accepted, what a receipt proves, the limits and
// the admin operations. No network, no real sockets.
import { test } from 'node:test';
import assert from 'node:assert/strict';
import { readFileSync } from 'node:fs';

import { handleAdmin } from '../src/admin.js';
import {
  INCIDENT, INCIDENT_STATUSES, incidentTicket, isTicket, logDownloadName, readClaims, readIncident, readReadClaim,
  readStatusChange,
} from '../src/incidents.js';
import { LIMITS, tokenHash } from '../src/protocol.js';
import { greeted, identity, makeHub } from './harness.js';

const HOUR = 3600 * 1000;

function packageBody(overrides = {}) {
  return {
    description: 'The game crashes when the second player joins a match.',
    launcher_version: '0.1.0',
    guest: 'TMNT: Turtles in Time Re-Shelled (XBLA) 0.0.1.3 (584109D2)',
    platform: 'Windows',
    operating_system: 'Windows 11 (build 26200)',
    runtime: 'ReXGlue SDK 0.10.0',
    log_file: 'tmnt_turtles_in_time_reshelled_003.log',
    log_clipped: 'no',
    log_text: 'Runtime configuration:\nRuntime OS: Windows 11\n',
    ...overrides,
  };
}

function file(env, client, overrides) {
  return client.call('incident.file', packageBody(overrides));
}

test('a report is stored and its receipt proves ownership', () => {
  const env = makeHub();
  const client = greeted(env, 1);
  const answer = file(env, client, { log_file: 'tmnt_turtles_in_time_reshelled_004.1.log' });
  assert.equal(answer.code, 'ok');
  assert.match(answer.ticket, /^TMNT-[A-HJ-NP-Z2-9]{6}$/);
  assert.match(answer.receipt, /^[0-9a-f]{64}$/);
  assert.ok(answer.submitted_at > 0);

  const wrong = env.hub.incidents.statuses([{ ticket: answer.ticket, receipt: '0'.repeat(64) }]);
  assert.deepEqual(wrong, []);
  const right = env.hub.incidents.statuses([{ ticket: answer.ticket, receipt: answer.receipt }]);
  assert.equal(right.length, 1);
  assert.equal(right[0].status, 'new');
  assert.equal(right[0].status_updated_at, answer.submitted_at);

  const report = env.hub.incidents.report({ ticket: answer.ticket, receipt: answer.receipt });
  assert.equal(report.description, packageBody().description);
  assert.equal(report.guest, 'TMNT: Turtles in Time Re-Shelled (XBLA) 0.0.1.3 (584109D2)');
  assert.equal(report.platform, 'Windows');
  assert.equal(report.runtime, 'ReXGlue SDK 0.10.0');
  assert.equal(report.log_file, 'tmnt_turtles_in_time_reshelled_004.1.log');
  assert.equal(report.log_text, packageBody().log_text);
});

test('a Linux report is stored and read like a Windows one', () => {
  const env = makeHub();
  const client = greeted(env, 11);
  const answer = file(env, client, {
    platform: 'Linux',
    operating_system: 'Linux: Ubuntu 24.04 LTS',
    log_text: 'Runtime configuration:\nRuntime OS: Linux: Ubuntu 24.04 LTS\nUser data: <home>/.local/share/TMNT\n',
  });
  assert.equal(answer.code, 'ok');
  const report = env.hub.incidents.report({ ticket: answer.ticket, receipt: answer.receipt });
  assert.equal(report.platform, 'Linux');
  assert.equal(report.operating_system, 'Linux: Ubuntu 24.04 LTS');
  assert.match(report.log_text, /Linux: Ubuntu 24\.04/);
  const listed = env.hub.incidents.list().incidents.find((row) => row.ticket === answer.ticket);
  assert.equal(listed.platform, 'Linux');
});

test('the player operations answer without a hello and hide an unknown report', () => {
  const env = makeHub();
  const client = env.connect(identity(2));
  const answer = client.call('incident.status', { reports: [{ ticket: 'TMNT-AAAAAA', receipt: 'a'.repeat(64) }] });
  assert.equal(answer.code, 'ok');
  assert.deepEqual(answer.reports, []);
  const read = client.call('incident.read', { ticket: 'TMNT-AAAAAA', receipt: 'a'.repeat(64) });
  assert.equal(read.code, 'not_found');
  assert.equal(client.call('incident.read', { ticket: 'TMNT-AAAAAA', receipt: 'wrong' }).code, 'bad_request');
  assert.equal(client.call('incident.read', { ticket: 'TMNT-AAAAAA', receipt: 'a'.repeat(64), extra: 1 }).code, 'bad_request');
});

test('status changes and developer messages', () => {
  const env = makeHub();
  const client = greeted(env, 3);
  const answer = file(env, client);
  const saved = env.hub.setIncidentStatus(answer.ticket, { status: 'investigating', fixed_version: '', player_message: 'We are looking.' });
  assert.equal(saved.status, 'investigating');
  assert.equal(saved.player_message, 'We are looking.');
  const statuses = env.hub.incidents.statuses([{ ticket: answer.ticket, receipt: answer.receipt }]);
  assert.equal(statuses[0].status, 'investigating');
  assert.equal(statuses[0].player_message, 'We are looking.');

  const fixed = env.hub.setIncidentStatus(answer.ticket, { status: 'fixed', fixed_version: '0.1.1', player_message: 'Fixed.' });
  assert.equal(fixed.status, 'fixed');
  assert.equal(fixed.fixed_version, '0.1.1');
  const reopened = env.hub.setIncidentStatus(answer.ticket, { status: 'investigating', fixed_version: '', player_message: 'Still looking.' });
  assert.equal(reopened.status, 'investigating');
  assert.equal(reopened.fixed_version, '');
  assert.equal(reopened.player_message, 'Still looking.');
});

test('an account gets two accepted reports an hour', () => {
  const env = makeHub();
  const client = greeted(env, 4);
  assert.equal(file(env, client).code, 'ok');
  env.clock.now += 1000;
  assert.equal(file(env, client).code, 'ok');
  env.clock.now += 1000;
  assert.equal(file(env, client).code, 'incident_hourly_limit');
  env.clock.now += HOUR + 1;
  assert.equal(file(env, client).code, 'ok');
});

test('the daily limit and the service-wide ceiling are separate', () => {
  const env = makeHub();
  const client = greeted(env, 5);
  for (let i = 0; i < INCIDENT.perAccountPerDay; i++) {
    assert.equal(file(env, client).code, 'ok', `report ${i + 1}`);
    env.clock.now += HOUR + 1;
  }
  assert.equal(file(env, client).code, 'incident_daily_limit');
});

test('a malformed or oversized body is refused without storing anything', () => {
  const env = makeHub();
  const client = greeted(env, 6);
  assert.equal(file(env, client, { unknown: 'field' }).code, 'bad_request');
  assert.equal(file(env, client, { guest: 'Not a game' }).code, 'bad_request');
  assert.equal(file(env, client, { platform: 'Win<dows>' }).code, 'bad_request');
  assert.equal(file(env, client, { runtime: 'x'.repeat(INCIDENT.metadataChars + 1) }).code, 'too_large');
  assert.equal(file(env, client, { launcher_version: ' 0.1.0' }).code, 'bad_request');
  assert.equal(file(env, client, { log_file: 'other.log' }).code, 'bad_request');
  assert.equal(file(env, client, { log_clipped: 'maybe' }).code, 'bad_request');
  assert.equal(file(env, client, { description: 'x'.repeat(INCIDENT.descriptionChars + 1) }).code, 'too_large');
  assert.equal(file(env, client, { log_text: 'x'.repeat(INCIDENT.logBytes + 1) }).code, 'too_large');
  assert.equal(env.hub.incidents.summary().total, 0);
});

test('the readers reject impossible fields and accept only bounded claims', () => {
  assert.equal(readIncident({ ...packageBody(), op: 'incident.file', seq: 1 }).incident.description, packageBody().description);
  assert.equal(readIncident({ ...packageBody(), extra: 1 }).code, 'bad_request');
  assert.equal(readIncident({ ...packageBody(), description: ' too short ' }).code, 'bad_request');
  const claims = Array.from({ length: INCIDENT.statusBatch + 1 }, () => ({ ticket: 'TMNT-AAAAAA', receipt: 'a'.repeat(64) }));
  assert.equal(readClaims({ reports: claims }), null);
  assert.deepEqual(readClaims({ reports: claims.slice(0, 1) }), [{ ticket: 'TMNT-AAAAAA', receipt: 'a'.repeat(64) }]);
  assert.deepEqual(readReadClaim({ ticket: 'TMNT-AAAAAA', receipt: 'a'.repeat(64), op: 'incident.read', seq: 2 }),
    { ticket: 'TMNT-AAAAAA', receipt: 'a'.repeat(64) });
  assert.equal(readReadClaim({ ticket: 'TMNT-AAAAAA', receipt: 'a'.repeat(64), extra: 1 }), null);
  assert.equal(readReadClaim({ ticket: 'bad', receipt: 'a'.repeat(64) }), null);
  assert.equal(readStatusChange({ status: 'made_up' }).detail, 'status');
  assert.equal(readStatusChange({ status: 'fixed', fixed_version: 'bad version!' }).detail, 'fixed_version');
  assert.equal(readStatusChange({ status: 'new', player_message: 'x'.repeat(INCIDENT.messageChars + 1) }).detail, 'player_message');
  assert.deepEqual(readStatusChange({ status: 'fixed', fixed_version: '0.2.0', player_message: ' Fixed. ' }).change,
    { status: 'fixed', fixed_version: '0.2.0', player_message: 'Fixed.' });
});

test('tickets and download names are formed only from validated text', () => {
  const ticket = incidentTicket(Uint8Array.from([0, 1, 2, 3, 4, 5]));
  assert.ok(isTicket(ticket));
  assert.ok(INCIDENT_STATUSES.includes('fixed'));
  assert.equal(logDownloadName({ ticket, log_file: 'tmnt_turtles_in_time_reshelled_003.log' }),
    `${ticket}-tmnt_turtles_in_time_reshelled_003.log`);
  assert.equal(logDownloadName({ ticket, log_file: '../../secret.txt' }), `${ticket}.log`);
  assert.equal(logDownloadName({ ticket: 'bad', log_file: 'tmnt_turtles_in_time_reshelled_003.log' }), 'tmnt-bug-report.log');
});

test('the admin API lists, reads, downloads and updates reports behind the token', async () => {
  const env = makeHub();
  const client = greeted(env, 7);
  const answer = file(env, client);
  const envWithToken = { ADMIN_TOKEN: 'x'.repeat(24) };
  const source = {
    summary: async () => ({}),
    session: async () => null,
    metricsSummary: async () => ({}),
    metricsHistory: async () => ({}),
    incidents: async () => env.hub.incidentList(),
    incident: async (ticket) => env.hub.incident(ticket),
    setIncidentStatus: async (ticket, change) => env.hub.setIncidentStatus(ticket, change),
  };
  const headers = { Authorization: `Bearer ${envWithToken.ADMIN_TOKEN}` };
  const list = await handleAdmin(new Request('https://x/admin/api/incidents', { headers }), envWithToken, source);
  assert.equal(list.status, 200);
  assert.equal((await list.json()).incidents.length, 1);

  const one = await handleAdmin(new Request(`https://x/admin/api/incidents/${answer.ticket}`, { headers }), envWithToken, source);
  assert.equal(one.status, 200);
  const detail = await one.json();
  assert.equal(detail.log_text, packageBody().log_text);
  assert.equal(detail.status, 'new');

  const log = await handleAdmin(new Request(`https://x/admin/api/incidents/${answer.ticket}/log`, { headers }), envWithToken, source);
  assert.equal(log.status, 200);
  assert.match(log.headers.get('Content-Disposition'), new RegExp(answer.ticket));

  const saved = await handleAdmin(new Request(`https://x/admin/api/incidents/${answer.ticket}/status`, {
    method: 'POST', headers: { ...headers, 'Content-Type': 'application/json' },
    body: JSON.stringify({ status: 'closed', player_message: 'Done.' }),
  }), envWithToken, source);
  assert.equal(saved.status, 200);
  assert.equal((await saved.json()).status, 'closed');

  assert.equal((await handleAdmin(new Request('https://x/admin/api/incidents/TMNT-AAAAAA', { headers }), envWithToken, source)).status, 404);
  assert.equal((await handleAdmin(new Request('https://x/admin/api/incidents/not-a-ticket', { headers }), envWithToken, source)).status, 400);
  assert.equal((await handleAdmin(new Request(`https://x/admin/api/incidents/${answer.ticket}`, { method: 'DELETE', headers }),
    envWithToken, source)).status, 405);
});

test('Investigating with a stale fixed version saves and clears it', async () => {
  const env = makeHub();
  const client = greeted(env, 17);
  const answer = file(env, client);
  env.hub.setIncidentStatus(answer.ticket, { status: 'fixed', fixed_version: '0.1.0', player_message: '' });
  const envWithToken = { ADMIN_TOKEN: 'x'.repeat(24) };
  const source = {
    summary: async () => ({}), session: async () => null, metricsSummary: async () => ({}), metricsHistory: async () => ({}),
    incidents: async () => env.hub.incidentList(), incident: async (ticket) => env.hub.incident(ticket),
    setIncidentStatus: async (ticket, change) => env.hub.setIncidentStatus(ticket, change),
  };
  const response = await handleAdmin(new Request(`https://x/admin/api/incidents/${answer.ticket}/status`, {
    method: 'POST',
    headers: { Authorization: `Bearer ${envWithToken.ADMIN_TOKEN}`, 'Content-Type': 'application/json' },
    body: JSON.stringify({ status: 'investigating', fixed_version: '0.1.0', player_message: 'testando' }),
  }), envWithToken, source);
  assert.equal(response.status, 200);
  const saved = await response.json();
  assert.equal(saved.status, 'investigating');
  assert.equal(saved.fixed_version, '');
  assert.equal(saved.player_message, 'testando');
  const detail = env.hub.incident(answer.ticket);
  assert.equal(detail.status, 'investigating');
  assert.equal(detail.fixed_version, '');
  assert.equal(detail.player_message, 'testando');
});

test('SF2 status transitions keep fixed_version only on Fixed', () => {
  const transitions = [
    ['new', 'investigating', '', ''],
    ['investigating', 'fixed', '0.1.1', '0.1.1'],
    ['fixed', 'investigating', '0.1.1', ''],
    ['fixed', 'closed', '0.1.1', ''],
    ['new', 'duplicate', '', ''],
    ['new', 'cannot_reproduce', '', ''],
    ['new', 'not_a_bug', '', ''],
  ];
  for (const [from, to, version, expected] of transitions) {
    const { change } = readStatusChange({ status: to, fixed_version: version, player_message: 'message' });
    assert.equal(change.status, to, `${from} -> ${to}`);
    assert.equal(change.fixed_version, expected, `${from} -> ${to}`);
    assert.equal(change.player_message, 'message', `${from} -> ${to}`);
  }
});

test('the admin stores every SF2 status transition with the fixed version rule', async () => {
  const env = makeHub();
  const client = greeted(env, 18);
  const answer = file(env, client);
  const envWithToken = { ADMIN_TOKEN: 'x'.repeat(24) };
  const source = {
    summary: async () => ({}), session: async () => null, metricsSummary: async () => ({}), metricsHistory: async () => ({}),
    incidents: async () => env.hub.incidentList(), incident: async (ticket) => env.hub.incident(ticket),
    setIncidentStatus: async (ticket, change) => env.hub.setIncidentStatus(ticket, change),
  };
  const post = (body) => handleAdmin(new Request(`https://x/admin/api/incidents/${answer.ticket}/status`, {
    method: 'POST',
    headers: { Authorization: `Bearer ${envWithToken.ADMIN_TOKEN}`, 'Content-Type': 'application/json' },
    body: JSON.stringify(body),
  }), envWithToken, source);
  const expected = [
    ['investigating', '0.1.0', ''],
    ['fixed', '0.1.1', '0.1.1'],
    ['investigating', '0.1.1', ''],
    ['closed', '0.1.1', ''],
    ['duplicate', '0.1.1', ''],
    ['cannot_reproduce', '0.1.1', ''],
    ['not_a_bug', '0.1.1', ''],
  ];
  for (const [status, version, fixed] of expected) {
    const response = await post({ status, fixed_version: version, player_message: `message-${status}` });
    assert.equal(response.status, 200, status);
    const saved = await response.json();
    assert.equal(saved.status, status);
    assert.equal(saved.fixed_version, fixed, status);
    assert.equal(saved.player_message, `message-${status}`, status);
  }
});

test('reports, status checks, reads and replies reach the metrics', () => {
  const env = makeHub();
  const client = greeted(env, 8);
  const answer = file(env, client);
  env.hub.setIncidentStatus(answer.ticket, { status: 'fixed', fixed_version: '0.2.0', player_message: 'Fixed.' });
  client.call('incident.status', { reports: [{ ticket: answer.ticket, receipt: answer.receipt }] });
  client.call('incident.read', { ticket: answer.ticket, receipt: answer.receipt });
  const { today } = env.hub.metricsSummary();
  assert.equal(today.incidents_filed, 1);
  assert.equal(today.incident_status_checks, 1);
  assert.equal(today.incident_reads, 1);
  assert.equal(today.incident_messages, 1);
});

test('a realistic TMNT report moves through status, read and the admin store', () => {
  const env = makeHub();
  const client = greeted(env, 9);
  const answer = file(env, client);
  assert.equal(answer.code, 'ok');
  assert.match(answer.ticket, /^TMNT-[A-HJ-NP-Z2-9]{6}$/);
  assert.match(answer.receipt, /^[0-9a-f]{64}$/);
  assert.equal(answer.submitted_at, env.clock.now);

  const listed = client.call('incident.status', { reports: [{ ticket: answer.ticket, receipt: answer.receipt }] });
  assert.equal(listed.code, 'ok');
  assert.equal(listed.reports[0].status, 'new');

  const read = client.call('incident.read', { ticket: answer.ticket, receipt: answer.receipt });
  assert.equal(read.code, 'ok');
  assert.equal(read.report.description, packageBody().description);
  assert.equal(read.report.guest, packageBody().guest);
  assert.equal(read.report.platform, 'Windows');
  assert.equal(read.report.runtime, 'ReXGlue SDK 0.10.0');
  assert.equal(read.report.submitted_at, answer.submitted_at);

  const detail = env.hub.incident(answer.ticket);
  assert.equal(detail.status, 'new');
  assert.equal(detail.guest, packageBody().guest);
  assert.ok(!('receipt' in detail) && !('receipt_hash' in detail));
  assert.equal(env.hub.incidentList().incidents[0].ticket, answer.ticket);

  env.hub.setIncidentStatus(answer.ticket, { status: 'investigating', fixed_version: '', player_message: 'We are looking.' });
  const updated = client.call('incident.status', { reports: [{ ticket: answer.ticket, receipt: answer.receipt }] });
  assert.equal(updated.reports[0].status, 'investigating');
  assert.equal(updated.reports[0].player_message, 'We are looking.');

  env.hub.setIncidentStatus(answer.ticket, { status: 'fixed', fixed_version: '0.1.1', player_message: 'Fixed.' });
  const fixed = client.call('incident.status', { reports: [{ ticket: answer.ticket, receipt: answer.receipt }] });
  assert.equal(fixed.reports[0].status, 'fixed');
  assert.equal(fixed.reports[0].fixed_version, '0.1.1');
  assert.equal(fixed.reports[0].player_message, 'Fixed.');
});

test('a report keeps no address, token, token hash or friend code', () => {
  const env = makeHub();
  const who = identity(10);
  const client = greeted(env, 10, { ip: '198.51.100.23' });
  const answer = file(env, client);
  const tables = JSON.stringify(['incidents', 'incident_quota', 'incident_tracking'].map((table) =>
    env.host.sql.all(`SELECT * FROM ${table}`)));
  const detail = JSON.stringify(env.hub.incident(answer.ticket));
  for (const secret of ['198.51.100.23', who.token, tokenHash(who.token), who.player, answer.receipt]) {
    assert.ok(!tables.includes(secret), `incident tables hold ${secret.slice(0, 8)}`);
    assert.ok(!detail.includes(secret), `admin detail holds ${secret.slice(0, 8)}`);
  }
});

test('a report at the exact limits is accepted', () => {
  const env = makeHub();
  const client = greeted(env, 11);
  const answer = file(env, client, {
    description: 'A'.repeat(INCIDENT.descriptionChars),
    log_text: `${'a'.repeat(INCIDENT.logBytes - 1)}\n`,
  });
  assert.equal(answer.code, 'ok');
  assert.equal(env.hub.incident(answer.ticket).description.length, INCIDENT.descriptionChars);
});

test('the current and rotated TMNT log names are accepted', () => {
  const env = makeHub();
  const client = greeted(env, 12);
  assert.equal(file(env, client, { log_file: 'tmnt_turtles_in_time_reshelled_093.log' }).code, 'ok');
  assert.equal(file(env, client, { log_file: 'tmnt_turtles_in_time_reshelled_093.1.log' }).code, 'ok');
});

test('filing needs a signed-in player', () => {
  const env = makeHub();
  const client = env.connect(identity(13));
  assert.equal(file(env, client).code, 'no_session');
});

test('the service announces bug_reports at sign-in', () => {
  const env = makeHub();
  const reply = env.connect(identity(15)).hello({ presence: false });
  assert.equal(reply.code, 'ok');
  assert.equal(reply.capabilities.bug_reports, true);
});

test('the raw production C++ messages are accepted by the real Worker', () => {
  const env = makeHub();
  const client = greeted(env, 16);
  const lines = readFileSync(new URL('../../../tests/incident_wire_vectors.txt', import.meta.url), 'utf8')
    .trim().split(/\r?\n/).filter(Boolean);
  assert.equal(lines.length, 3);
  const filed = client.raw(lines[0]);
  assert.equal(filed.code, 'ok');
  assert.match(filed.ticket, /^TMNT-[A-HJ-NP-Z2-9]{6}$/);
  const status = client.raw(lines[1]);
  assert.equal(status.code, 'ok');
  assert.deepEqual(status.reports, []);
  const read = client.raw(lines[2]);
  assert.equal(read.code, 'not_found');
});

test('only a report may be larger than an ordinary frame', () => {
  const env = makeHub();
  const client = greeted(env, 14);
  const padding = 'x'.repeat(LIMITS.frameBytes + 10);
  assert.equal(client.call('ping', { padding }).code, 'too_large');
  assert.equal(client.call('lobby.list', { padding }).code, 'too_large');
  assert.equal(file(env, client, { log_text: `${padding}\n` }).code, 'ok');
  assert.equal(file(env, client, { log_text: 'y'.repeat(LIMITS.incidentFrameBytes) }).code, 'too_large');
});
