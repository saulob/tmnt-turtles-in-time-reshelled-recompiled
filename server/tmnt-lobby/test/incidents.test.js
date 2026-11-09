// Bug reports over the lobby WebSocket and in the admin store: what is accepted, what a receipt proves, the limits and
// the admin operations. No network, no real sockets.
import { test } from 'node:test';
import assert from 'node:assert/strict';

import { handleAdmin } from '../src/admin.js';
import {
  INCIDENT, INCIDENT_STATUSES, incidentTicket, isTicket, logDownloadName, readClaims, readIncident, readStatusChange,
} from '../src/incidents.js';
import { greeted, identity, makeHub } from './harness.js';

const HOUR = 3600 * 1000;

function packageBody(overrides = {}) {
  return {
    description: 'The game crashes when the second player joins a match.',
    launcher_version: '0.1.0',
    game_build: 'TMNT: Turtles in Time Re-Shelled (XBLA)',
    game_version: '0.0.1.3',
    game_id: '584109D2',
    operating_system: 'Windows 11 (build 26200)',
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
  assert.equal(report.game_id, '584109D2');
  assert.equal(report.log_file, 'tmnt_turtles_in_time_reshelled_004.1.log');
  assert.equal(report.log_text, packageBody().log_text);
});

test('the player operations answer without a hello and hide an unknown report', () => {
  const env = makeHub();
  const client = env.connect(identity(2));
  const answer = client.call('incident.status', { reports: [{ ticket: 'TMNT-AAAAAA', receipt: 'a'.repeat(64) }] });
  assert.equal(answer.code, 'ok');
  assert.deepEqual(answer.reports, []);
  const read = client.call('incident.read', { ticket: 'TMNT-AAAAAA', receipt: 'a'.repeat(64) });
  assert.equal(read.code, 'not_found');
});

test('status changes, developer messages and deletion', () => {
  const env = makeHub();
  const client = greeted(env, 3);
  const answer = file(env, client);
  const saved = env.hub.setIncidentStatus(answer.ticket, { status: 'investigating', fixed_version: '', player_message: 'We are looking.' });
  assert.equal(saved.status, 'investigating');
  assert.equal(saved.player_message, 'We are looking.');
  const statuses = env.hub.incidents.statuses([{ ticket: answer.ticket, receipt: answer.receipt }]);
  assert.equal(statuses[0].status, 'investigating');
  assert.equal(statuses[0].player_message, 'We are looking.');

  assert.equal(env.hub.deleteIncident(answer.ticket), true);
  assert.deepEqual(env.hub.incidents.statuses([{ ticket: answer.ticket, receipt: answer.receipt }]), []);
  assert.equal(env.hub.incidents.report({ ticket: answer.ticket, receipt: answer.receipt }), null);
  assert.equal(env.hub.deleteIncident(answer.ticket), false);
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
  assert.equal(file(env, client, { game_id: 'not-an-id' }).code, 'bad_request');
  assert.equal(file(env, client, { log_file: 'other.log' }).code, 'bad_request');
  assert.equal(file(env, client, { log_clipped: 'maybe' }).code, 'bad_request');
  assert.equal(file(env, client, { description: 'x'.repeat(INCIDENT.descriptionChars + 1) }).code, 'too_large');
  assert.equal(file(env, client, { log_text: 'x'.repeat(INCIDENT.logBytes + 1) }).code, 'too_large');
  assert.equal(env.hub.incidents.summary().total, 0);
});

test('the readers reject impossible fields and accept only bounded claims', () => {
  assert.equal(readIncident({ ...packageBody(), op: 'incident.file', seq: 1 }).incident.description, packageBody().description);
  assert.equal(readIncident({ ...packageBody(), extra: 1 }).code, 'bad_request');
  const claims = Array.from({ length: INCIDENT.statusBatch + 1 }, () => ({ ticket: 'TMNT-AAAAAA', receipt: 'a'.repeat(64) }));
  assert.equal(readClaims({ reports: claims }), null);
  assert.deepEqual(readClaims({ reports: claims.slice(0, 1) }), [{ ticket: 'TMNT-AAAAAA', receipt: 'a'.repeat(64) }]);
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

test('the admin API lists, reads, updates and deletes reports behind the token', async () => {
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
    deleteIncident: async (ticket) => env.hub.deleteIncident(ticket),
  };
  const headers = { Authorization: `Bearer ${envWithToken.ADMIN_TOKEN}` };
  const list = await handleAdmin(new Request('https://x/admin/api/incidents', { headers }), envWithToken, source);
  assert.equal(list.status, 200);
  assert.equal((await list.json()).incidents.length, 1);

  const one = await handleAdmin(new Request(`https://x/admin/api/incidents/${answer.ticket}`, { headers }), envWithToken, source);
  assert.equal(one.status, 200);
  assert.equal((await one.json()).log_text, packageBody().log_text);

  const log = await handleAdmin(new Request(`https://x/admin/api/incidents/${answer.ticket}/log`, { headers }), envWithToken, source);
  assert.equal(log.status, 200);
  assert.match(log.headers.get('Content-Disposition'), new RegExp(answer.ticket));

  const saved = await handleAdmin(new Request(`https://x/admin/api/incidents/${answer.ticket}/status`, {
    method: 'POST', headers: { ...headers, 'Content-Type': 'application/json' },
    body: JSON.stringify({ status: 'closed', player_message: 'Done.' }),
  }), envWithToken, source);
  assert.equal(saved.status, 200);
  assert.equal((await saved.json()).status, 'closed');

  const removed = await handleAdmin(new Request(`https://x/admin/api/incidents/${answer.ticket}`, { method: 'DELETE', headers }),
    envWithToken, source);
  assert.equal(removed.status, 200);
  assert.equal((await removed.json()).deleted, answer.ticket);
  const gone = await handleAdmin(new Request(`https://x/admin/api/incidents/${answer.ticket}`, { headers }), envWithToken, source);
  assert.equal(gone.status, 404);
});
