import assert from 'node:assert/strict';
import { test } from 'node:test';

import { ADMIN_HTML, ADMIN_JS } from '../src/admin_page.js';
import { metricsDay } from '../src/metrics.js';
import { greeted, makeHub } from './harness.js';

const DAY_MS = 24 * 3600 * 1000;

class Node {
  constructor(tag) {
    this.tag = tag;
    this.children = [];
    this.attrs = {};
    this.listeners = {};
    this.style = {};
    this.className = '';
    this.hidden = false;
    this.value = '';
    this.offsetWidth = 50;
    this.offsetHeight = 20;
    this.ownText = '';
  }

  get firstChild() { return this.children[0] ?? null; }

  get textContent() { return this.ownText + this.children.map((c) => c.textContent).join(''); }

  set textContent(text) {
    this.children = [];
    this.ownText = String(text);
  }

  appendChild(child) {
    this.children.push(child);
    return child;
  }

  removeChild(child) {
    this.children = this.children.filter((c) => c !== child);
    return child;
  }

  setAttribute(name, value) { this.attrs[name] = String(value); }

  getAttribute(name) { return this.attrs[name] ?? null; }

  addEventListener(type, fn) { (this.listeners[type] ??= []).push(fn); }

  fire(type, event = { clientX: 10, clientY: 10 }) { for (const fn of this.listeners[type] ?? []) fn(event); }

  getBoundingClientRect() { return { left: 0, top: 0, right: 900, bottom: 200, width: 900, height: 200 }; }

  focus() {}

  scrollIntoView() {}

  find(predicate, found = []) {
    for (const child of this.children) {
      if (predicate(child)) found.push(child);
      child.find(predicate, found);
    }
    return found;
  }

  hasClass(name) { return (this.attrs.class ?? this.className).split(' ').includes(name); }
}

function cardsOf(html) {
  return [...html.matchAll(/<div class="card"[^>]*data-help="([^"]*)"/g)].map((m) => {
    const card = new Node('div');
    card.setAttribute('data-help', m[1]);
    return card;
  });
}

async function dashboard(env, days) {
  const ids = new Map();
  const get = (id) => {
    if (!ids.has(id)) ids.set(id, new Node('div'));
    return ids.get(id);
  };
  const cards = cardsOf(ADMIN_HTML);
  const document = {
    hidden: false,
    getElementById: get,
    createElement: (tag) => new Node(tag),
    createElementNS: (ns, tag) => new Node(tag),
    createTextNode: (text) => Object.assign(new Node('#text'), { ownText: text }),
    querySelectorAll: () => cards,
  };
  get('range').value = String(days);
  get('tip').hidden = true;
  const requests = [];
  const fetch = async (path) => {
    requests.push(path);
    const query = new URL(path, 'https://x').searchParams;
    return { ok: true, status: 200, json: async () => JSON.parse(JSON.stringify(path.startsWith('/admin/api/summary')
      ? env.hub.metricsSummary() : env.hub.metricsHistory(Number(query.get('days'))))) };
  };
  const storage = { getItem: () => 'token', setItem() {}, removeItem() {} };
  new Function('document', 'window', 'fetch', 'sessionStorage', 'setInterval', 'clearInterval', ADMIN_JS)(
    document, { innerWidth: 1200, innerHeight: 800 }, fetch, storage, () => 0, () => {});
  for (let i = 0; i < 10; i++) await new Promise((resolve) => setImmediate(resolve));
  return { ids, get, cards, tip: get('tip'), requests };
}

const dayAgo = (env, n) => metricsDay(env.clock.now - n * DAY_MS);

function putDay(env, n, columns) {
  const names = Object.keys(columns);
  env.host.sql.run(`INSERT INTO metrics_daily (day, ${names.join(', ')}) VALUES (?, ${names.map(() => '?').join(', ')})`,
    dayAgo(env, n), ...Object.values(columns));
}

function putDimensions(env, n, dimension, values) {
  for (const [value, count] of Object.entries(values)) {
    env.host.sql.run('INSERT INTO metrics_daily_dimensions VALUES (?, ?, ?, ?)', dayAgo(env, n), dimension, value, count);
  }
}

function putPlayers(env, n, keys) {
  for (const key of keys) env.host.sql.run('INSERT INTO metrics_players VALUES (?, ?)', dayAgo(env, n), key);
}

const everyText = (page) => [...page.ids.values()].map((n) => n.textContent).join('\n');
const clean = (text) => !/NaN|Infinity|undefined|\[object/.test(text);
const tipLines = (page) => page.tip.children.map((c) => c.textContent);
const valueLabels = (page, id) => page.get(id).find((n) => n.hasClass('value-label')).map((n) => n.textContent);
const hits = (page, id) => page.get(id).find((n) => n.hasClass('hit'));
const rowsOf = (page, id) => page.get(id).find((n) => n.hasClass('row')).map((r) => ({
  name: r.find((n) => n.hasClass('name'))[0].textContent,
  count: r.find((n) => n.hasClass('count'))[0].textContent,
  width: r.find((n) => n.hasClass('fill'))[0].style.width,
}));
const cardText = (page, id) => page.get(id).find((n) => n.hasClass('card')).map((c) => c.textContent);

test('zero distributions show No data yet: no NaN, Infinity, undefined, percentages or bars', async () => {
  const page = await dashboard(makeHub(), 30);
  for (const id of ['d-occupancy', 'd-lifetime']) {
    const box = page.get(id);
    assert.ok(box.textContent.includes('No data yet'), id);
    assert.ok(!box.textContent.includes('%'), id);
    assert.equal(box.find((n) => n.hasClass('fill')).length, 0, id);
  }
  assert.ok(page.get('activity-note').textContent.startsWith('Room lifecycle statistics begin as rooms end.'));
  assert.ok(clean(everyText(page)));
});

test('a distribution with data shows counts, percentages and no bar for zero values', async () => {
  const env = makeHub();
  putDay(env, 0, { rooms_closed: 20, rooms_with_guest: 15, party_total: 35 });
  putDimensions(env, 0, 'occupancy', { 1: 5, 2: 15 });
  const page = await dashboard(env, 30);
  const rows = rowsOf(page, 'd-occupancy');
  assert.deepEqual(rows.map((r) => r.count), ['5 · 25%', '15 · 75%', '0 · 0%', '0 · 0%']);
  assert.deepEqual(rows.map((r) => Math.round(parseFloat(r.width))), [33, 100, 0, 0]);
  assert.ok(page.get('d-lifetime').textContent.includes('No data yet'));
  assert.ok(page.get('activity-note').textContent.includes('rooms that ended (20)'));
  assert.deepEqual(cardText(page, 'activity'), ['Rooms created0', 'Successful joins0', 'Rooms with a guest15 · 75%',
    'Rooms without a guest5 · 25%', 'Average largest party1.75']);
  assert.ok(clean(everyText(page)));
});

test('the period comparison names both exact ranges and handles an empty previous period', async () => {
  const env = makeHub();
  putDay(env, 0, { sessions: 5, rooms_created: 0, room_joins: 15, rooms_relayed: 2 });
  putDay(env, 40, { sessions: 0, rooms_created: 0, room_joins: 10 });
  putPlayers(env, 1, ['a', 'b', 'c']);
  putPlayers(env, 35, ['a', 'd']);
  const page = await dashboard(env, 30);
  assert.equal(page.get('compare-note').textContent,
    `${dayAgo(env, 29)} to ${dayAgo(env, 0)} compared with ${dayAgo(env, 59)} to ${dayAgo(env, 30)}.`);
  const table = page.get('compare');
  assert.deepEqual(table.find((n) => n.tag === 'th').map((n) => n.textContent), ['Metric', 'This period', 'Previous period', 'Change']);
  const change = (name) => table.find((n) => n.tag === 'tr' && n.children[0]?.textContent === name)[0].children[3].textContent;
  assert.equal(change('Unique players'), '+1 (+50%)');
  assert.equal(change('Sessions'), 'new');
  assert.equal(change('Rooms created'), '-');
  assert.equal(change('Successful joins'), '+5 (+50%)');
  assert.deepEqual(table.find((n) => n.tag === 'tr').slice(1).map((r) => r.children[0].textContent), ['Unique players', 'Sessions',
    'Rooms created', 'Successful joins', 'Rooms with a guest', 'Peak online', 'Peak in rooms']);
  assert.ok(clean(table.textContent));
});

test('overview cards carry their explanations and show them on hover and keyboard focus', async () => {
  const page = await dashboard(makeHub(), 7);
  const helps = page.cards.map((c) => c.getAttribute('data-help'));
  assert.equal(helps.length, 9);
  assert.equal([...ADMIN_HTML.matchAll(/class="card" tabindex="0" data-help=/g)].length, 9);
  assert.ok(!/\son[a-z]+\s*=/i.test(ADMIN_HTML));
  for (const [i, card] of page.cards.entries()) {
    assert.ok(card.listeners.mousemove && card.listeners.mouseleave && card.listeners.focus && card.listeners.blur);
    card.fire('mousemove');
    assert.equal(page.tip.hidden, false);
    assert.deepEqual(tipLines(page), [helps[i]]);
    card.fire('mouseleave');
    assert.equal(page.tip.hidden, true);
    card.fire('focus', {});
    assert.deepEqual(tipLines(page), [helps[i]]);
    card.fire('blur', {});
    assert.equal(page.tip.hidden, true);
  }
});

test('the tooltips keep online players, unique players, sessions, joins and rooms apart', () => {
  const helps = cardsOf(ADMIN_HTML).map((c) => c.getAttribute('data-help'));
  const help = (word) => helps.find((h) => h.startsWith(word));
  assert.match(help('Players connected'), /once per connected account/);
  assert.match(help('Online sessions'), /10-minute deduplication window/);
  assert.match(help('Successful room joins'), /not unique players/);
  assert.match(help('A unique player is counted once for today'), /anonymous server-side key/);
  assert.match(help('A unique player is counted once for the whole'), /several days/);
  assert.match(help('Multiplayer rooms currently'), /open/);
});

test('unique player cards show today and the selected period, and the period card follows the range', async () => {
  const env = makeHub();
  putPlayers(env, 0, ['a']);
  putPlayers(env, 3, ['a', 'b']);
  putPlayers(env, 20, ['c']);
  putPlayers(env, 80, ['d']);
  putPlayers(env, 200, ['e']);
  const expected = { 7: '2', 30: '3', 90: '4', 365: '5' };
  for (const [days, count] of Object.entries(expected)) {
    const page = await dashboard(env, Number(days));
    assert.equal(page.get('c-unique-today').textContent, '1');
    assert.equal(page.get('c-unique-range').textContent, count, `${days} days`);
    assert.equal(page.get('c-unique-range-label').textContent, `Unique players · ${days} days`);
  }
});

test('the live cards show their numbers', async () => {
  const env = makeHub();
  greeted(env, 1).call('party.create');
  const page = await dashboard(env, 7);
  assert.equal(page.get('c-online').textContent, '1');
  assert.equal(page.get('c-in-rooms').textContent, '1');
  assert.equal(page.get('c-rooms').textContent, '1');
  assert.equal(page.get('c-peak').textContent, '1');
  assert.equal(page.get('c-sessions').textContent, '1');
  assert.equal(page.get('c-rooms-today').textContent, '1');
  assert.equal(page.get('c-joins-today').textContent, '0');
});

const CHARTS = [['h-unique', 'unique_players', 'Unique players'], ['h-sessions', 'sessions', 'Sessions'],
  ['h-rooms', 'rooms_created', 'Rooms created'], ['h-joins', 'room_joins', 'Successful joins'],
  ['h-peak-online', 'peak_online', 'Peak online'], ['h-peak-rooms', 'peak_in_rooms', 'Peak in rooms']];

function seedDaily(env) {
  putDay(env, 0, { sessions: 1175, rooms_created: 805, room_joins: 415, peak_online: 18, peak_in_rooms: 10 });
  putDay(env, 2, { sessions: 3, rooms_created: 2, room_joins: 1, peak_online: 4, peak_in_rooms: 2 });
  putPlayers(env, 0, ['a', 'b']);
  putPlayers(env, 2, ['a']);
}

test('daily bars carry value labels for 7 and 30 days, none for zero bars', async () => {
  for (const days of [7, 30]) {
    const env = makeHub();
    seedDaily(env);
    const page = await dashboard(env, days);
    assert.deepEqual(valueLabels(page, 'h-sessions'), ['3', '1,175'], `${days} days`);
    assert.deepEqual(valueLabels(page, 'h-unique'), ['1', '2'], `${days} days`);
    for (const [id] of CHARTS) assert.equal(hits(page, id).length, days, id);
  }
});

test('daily bars carry no value labels for 90 and 365 days, and keep their tooltips', async () => {
  for (const days of [90, 365]) {
    const env = makeHub();
    seedDaily(env);
    const page = await dashboard(env, days);
    for (const [id] of CHARTS) {
      assert.deepEqual(valueLabels(page, id), [], `${id} ${days}`);
      assert.equal(hits(page, id).length, days, id);
    }
  }
});

test('daily chart tooltips show the metric name and value, never a DOM object', async () => {
  const env = makeHub();
  seedDaily(env);
  const page = await dashboard(env, 30);
  const expected = { 'h-unique': 'Unique players: 2', 'h-sessions': 'Sessions: 1,175', 'h-rooms': 'Rooms created: 805',
    'h-joins': 'Successful joins: 415', 'h-peak-online': 'Peak online: 18', 'h-peak-rooms': 'Peak in rooms: 10' };
  for (const [id] of CHARTS) {
    const slots = hits(page, id);
    slots.at(-1).fire('mousemove');
    const [date, metric] = tipLines(page);
    assert.ok(/^\d{1,2} [A-Z][a-z]{2}$/.test(date), date);
    assert.equal(metric, expected[id]);
    slots.at(0).fire('mousemove');
    assert.ok(/^[A-Za-z ]+: 0$/.test(tipLines(page)[1]));
    slots.at(0).fire('mouseleave');
    assert.equal(page.tip.hidden, true);
  }
  assert.ok(clean(everyText(page)) && !ADMIN_JS.includes('[object'));
});

test('the daily table carries unique players next to sessions, as separate columns', async () => {
  const env = makeHub();
  seedDaily(env);
  const page = await dashboard(env, 7);
  const table = page.get('daily');
  const head = table.find((n) => n.tag === 'th').map((n) => n.textContent);
  assert.deepEqual(head.slice(0, 3), ['Day', 'Sessions', 'Unique players']);
  const row = table.find((n) => n.tag === 'tr' && n.children[0]?.textContent === dayAgo(env, 0))[0];
  assert.deepEqual(row.children.slice(1, 3).map((c) => c.textContent), ['1,175', '2']);
});

function seedParties(env) {
  putDimensions(env, 0, 'character', { Leonardo: 30, Raphael: 20, Donatello: 10, Michelangelo: 40 });
  putDimensions(env, 1, 'character', { Leonardo: 10 });
  putDimensions(env, 0, 'host_character', { Leonardo: 3, Raphael: 1 });
  putDimensions(env, 0, 'party_size', { 1: 5, 2: 10, 4: 5 });
  putDimensions(env, 0, 'party_composition', { 'Leonardo+Raphael': 6, 'Leonardo+Leonardo+Raphael': 3, 'Michelangelo+Donatello': 11 });
  putDimensions(env, 0, 'party_duplicate', { yes: 5, no: 15 });
}

test('with no party reports the section says No data yet: no percentages, bars or cards', async () => {
  const page = await dashboard(makeHub(), 30);
  assert.ok(ADMIN_HTML.includes('Characters &amp; parties'));
  assert.ok(page.get('party-note').textContent.startsWith('No data yet.'));
  assert.equal(page.get('party-cards').children.length, 0);
  for (const id of ['d-character', 'd-host', 'd-party-size', 'd-composition']) {
    const box = page.get(id);
    assert.ok(box.textContent.includes('No data yet'), id);
    assert.ok(!box.textContent.includes('%'), id);
    assert.equal(box.find((n) => n.hasClass('fill')).length, 0, id);
  }
  assert.ok(clean(everyText(page)));
});

test('the section explains that these are anonymous aggregate snapshots from compatible clients', () => {
  assert.match(ADMIN_HTML, /Anonymous aggregate party snapshots from compatible clients: the turtles in an online party/);
  assert.match(ADMIN_HTML, /not that nobody played/);
});

test('turtle popularity shows every turtle in canonical order with its count and share of appearances', async () => {
  const env = makeHub();
  seedParties(env);
  const page = await dashboard(env, 30);
  const rows = rowsOf(page, 'd-character');
  assert.deepEqual(rows.map((r) => r.name), ['Leonardo', 'Michelangelo', 'Raphael', 'Donatello']);
  assert.deepEqual(rows.map((r) => r.count), ['40 · 36%', '40 · 36%', '20 · 18%', '10 · 9%']);
  assert.deepEqual(rows.map((r) => parseFloat(r.width)), [100, 100, 50, 25]);
  assert.ok(page.get('d-character').textContent.includes('Turtles in parties'));
  assert.ok(clean(everyText(page)));
});

test('a turtle nobody played still has its row, at zero and without a bar', async () => {
  const env = makeHub();
  putDimensions(env, 0, 'character', { Leonardo: 2, Raphael: 2 });
  putDimensions(env, 0, 'party_size', { 2: 2 });
  const rows = rowsOf(await dashboard(env, 30), 'd-character');
  assert.deepEqual(rows.map((r) => [r.name, r.count]), [['Leonardo', '2 · 50%'], ['Michelangelo', '0 · 0%'], ['Raphael', '2 · 50%'],
    ['Donatello', '0 · 0%']]);
  assert.equal(rows[1].width, '0%');
});

test('host turtles, party sizes and the snapshot cards render from their own counts', async () => {
  const env = makeHub();
  seedParties(env);
  const page = await dashboard(env, 30);
  assert.deepEqual(rowsOf(page, 'd-host').map((r) => r.count), ['3 · 75%', '0 · 0%', '1 · 25%', '0 · 0%']);
  assert.deepEqual(rowsOf(page, 'd-party-size').map((r) => [r.name, r.count]),
    [['1 player', '5 · 25%'], ['2 players', '10 · 50%'], ['3 players', '0 · 0%'], ['4 players', '5 · 25%']]);
  assert.deepEqual(cardText(page, 'party-cards'), ['Parties reported20', 'Average party size2.25']);
  assert.ok(page.get('party-note').textContent.startsWith('Last 30 days.'));
  assert.ok(page.get('party-note').textContent.includes(`Reporting began on ${dayAgo(env, 0)}; earlier rooms are not part of these counts.`));
});

test('compositions are listed most common first, with the share of reported parties', async () => {
  const env = makeHub();
  seedParties(env);
  const page = await dashboard(env, 30);
  assert.deepEqual(rowsOf(page, 'd-composition').map((r) => [r.name, r.count]), [['Michelangelo + Donatello', '11 · 55%'],
    ['Leonardo + Raphael', '6 · 30%'], ['Leonardo + Leonardo + Raphael', '3 · 15%']]);
});

test('only the top twelve compositions are listed and the rest are counted, never drawn', async () => {
  const env = makeHub();
  putDimensions(env, 0, 'party_size', { 2: 21 });
  const names = ['Leonardo', 'Michelangelo', 'Raphael', 'Donatello'];
  const values = {};
  let n = 0;
  for (let i = 0; i < 4; i++) for (let j = i; j < 4; j++) for (let k = j; k < 4 && n < 15; k++, n++) values[`${names[i]}+${names[j]}+${names[k]}`] = 15 - n;
  putDimensions(env, 0, 'party_composition', values);
  const page = await dashboard(env, 30);
  assert.equal(rowsOf(page, 'd-composition').length, 12);
  assert.ok(page.get('d-composition').textContent.includes('3 more not shown'));
});

test('a range without reports names the first day anything was reported instead of showing zero use', async () => {
  const env = makeHub();
  putDimensions(env, 40, 'party_size', { 2: 4 });
  putDimensions(env, 40, 'character', { Leonardo: 4, Raphael: 4 });
  const page = await dashboard(env, 7);
  const note = page.get('party-note').textContent;
  assert.ok(note.includes('No data yet in the last 7 days'));
  assert.ok(note.includes(dayAgo(env, 40)));
  assert.equal(page.get('party-cards').children.length, 0);
  assert.ok(page.get('d-character').textContent.includes('No data yet'));
  assert.ok(clean(everyText(page)));
});

test('missing host figures leave that block empty without breaking the others', async () => {
  const env = makeHub();
  putDimensions(env, 0, 'party_size', { 3: 2 });
  putDimensions(env, 0, 'character', { Donatello: 6 });
  putDimensions(env, 0, 'party_composition', { 'Donatello+Donatello+Donatello': 2 });
  putDimensions(env, 0, 'party_duplicate', { yes: 2 });
  const page = await dashboard(env, 30);
  assert.ok(page.get('d-host').textContent.includes('No data yet'));
  assert.equal(rowsOf(page, 'd-character').find((r) => r.name === 'Donatello').count, '6 · 100%');
});

test('the page never shows the Worker secret warning, keyed or not', async () => {
  for (const env of [makeHub(), makeHub({ metricsSecret: 'k'.repeat(40) })]) {
    const page = await dashboard(env, 7);
    assert.ok(!everyText(page).includes('METRICS_SECRET'));
  }
  assert.ok(!ADMIN_HTML.includes('METRICS_SECRET') && !ADMIN_JS.includes('METRICS_SECRET'));
  assert.ok(!ADMIN_HTML.includes('range-note'));
});

test('connection health, abuse protection and the breakdowns render their counters', async () => {
  const env = makeHub();
  putDay(env, 0, { rooms_closed: 4, rooms_relayed: 1, incompatible_rejects: 2 });
  putDimensions(env, 0, 'abuse', { hello_reconnect_deduped: 7, party_join_limited: 2, party_report_limited: 3 });
  putDimensions(env, 0, 'reject', { online_protocol: 2 });
  putDimensions(env, 0, 'platform', { Windows: 3 });
  putDimensions(env, 0, 'version', { '0.4.2': 3 });
  putDimensions(env, 0, 'country', { BR: 2, Unknown: 1 });
  const page = await dashboard(env, 30);
  assert.deepEqual(cardText(page, 'health'), ['Reconnects not counted as sessions7', 'Incompatibility rejects2',
    'Rooms that used the relay1 · 25%', 'Room operations refused2']);
  assert.ok(page.get('abuse').textContent.includes('Party snapshots refused: too many'));
  assert.ok(page.get('rejects').textContent.includes('Party join refused'));
  assert.ok(page.get('d-version').textContent.includes('v0.4.2'));
  assert.ok(page.get('d-country').textContent.includes('Unknown'));
  assert.ok(clean(everyText(page)));
});

test('the dashboard asks only for the two aggregate routes, whatever the range', async () => {
  const page = await dashboard(makeHub(), 365);
  assert.deepEqual([...page.requests].sort(), ['/admin/api/history?days=365', '/admin/api/summary']);
});

const SECTIONS = ['Period comparison', 'Multiplayer activity', 'Characters &amp; parties', 'Historical activity', 'Daily totals',
  'Connection health', 'Platforms and countries', 'Abuse protection', 'How to read this'];

test('the sections follow the Dragon dashboard order, with the top cards first', () => {
  const at = SECTIONS.map((s) => ADMIN_HTML.indexOf(`>${s}</h2>`));
  assert.ok(at.every((i) => i > 0), SECTIONS.filter((s, i) => at[i] < 0).join());
  assert.deepEqual([...at].sort((a, b) => a - b), at);
  assert.ok(ADMIN_HTML.indexOf('id="c-online"') < at[0]);
  assert.deepEqual([...ADMIN_HTML.matchAll(/<h2( class="section")?>([^<]*)</g)].filter((m) => m[1]).map((m) => m[2]),
    ['Period comparison', 'Multiplayer activity', 'Characters &amp; parties', 'Historical activity', 'Connection health',
      'Platforms and countries', 'Abuse protection']);
});

test('the top cards are the Dragon ones plus the two unique player cards', () => {
  const labels = [...ADMIN_HTML.matchAll(/<span (?:id="[^"]*" )?class="label">([^<]*)</g)].map((m) => m[1]);
  assert.deepEqual(labels, ['Online now', 'In rooms', 'Open rooms', 'Peak today', 'Unique players today', 'Unique players in period',
    'Sessions today', 'Rooms today', 'Joins today']);
});

test('technical telemetry is gone from the page: no heading, panel, control or route', async () => {
  for (const text of ['Operational telemetry', 'Transport mix', 'Latency percentiles', 'Connection Tests', 'Connection tests', 'Worker totals',
    'Client events', 'Recent sessions', 'Parties started', 'Batches', 'Direct links', 'Relayed links', 'Sessions by protocol',
    'Sessions by OS', 'Chosen', 'Direct results', 'Relay results', 'Fallback', 'Disconnect reasons', 'Invite outcomes',
    'Service errors', 'Other endings', 'Rejected telemetry', 'Diag', 'diag', 'Worker:', 'telemetry counters', 'op-note',
    'Compatibility']) {
    assert.ok(!ADMIN_HTML.includes(text), text);
  }
  assert.ok(!/telemetry\/(summary|session)|openSession|renderDetail|latencyTable/.test(ADMIN_JS));
  const page = await dashboard(makeHub(), 30);
  for (const id of ['out', 'detail', 'cards', 'op-note']) assert.equal(page.ids.has(id), false, id);
});

test('there is one Abuse protection section and one user-facing Sessions concept', () => {
  assert.equal([...ADMIN_HTML.matchAll(/Abuse protection/g)].length, 1);
  assert.equal([...ADMIN_HTML.matchAll(/Protection counters/g)].length, 1);
  const labels = [...ADMIN_HTML.matchAll(/class="label">([^<]*)</g)].map((m) => m[1]).filter((l) => /^Sessions/.test(l));
  assert.deepEqual(labels, ['Sessions today']);
  assert.equal([...ADMIN_HTML.matchAll(/<h2>Sessions[^<]*</g)].length, 1);
});

test('the party section has no repeated turtles, players represented or most played card', async () => {
  const env = makeHub();
  seedParties(env);
  const page = await dashboard(env, 30);
  const text = everyText(page) + ADMIN_HTML;
  for (const word of ['repeated', 'Players represented', 'Most played', 'Rooms closed']) assert.ok(!text.includes(word), word);
});

test('the page keeps its controls and the Dragon wording', () => {
  for (const text of ['<option value="7">', '<option value="30" selected>', '<option value="90">', '<option value="365">', 'id="refresh"',
    'id="signout"', 'Dashboard token', 'Keep for this tab', 'Open dashboard', 'prefers-color-scheme: dark',
    'Turtles in Time Re-Shelled Recompiled · Online Metrics',
    'Anonymous aggregate statistics of the online service. Days are America/Sao_Paulo.</p>',
    'Operational counts for the selected range. They describe the service, not players.', 'minmax(150px, 1fr)']) {
    assert.ok(ADMIN_HTML.includes(text), text);
  }
  assert.ok(!/Dragon|Fighter|Sorceress|operational telemetry/i.test(ADMIN_HTML));
});

test('the dashboard reads only its own routes, with no dangerous construct in production code', () => {
  assert.deepEqual([...ADMIN_JS.matchAll(/api\('([^']+)/g)].map((m) => m[1]).sort(), ['/admin/api/history?days=', '/admin/api/summary']);
  assert.equal([...ADMIN_JS.matchAll(/fetch\(/g)].length, 1);
  assert.ok(!/innerHTML|outerHTML|insertAdjacentHTML|document\.write|\beval\b|new Function/.test(ADMIN_JS));
  assert.ok(!/localStorage/.test(ADMIN_JS));
});
