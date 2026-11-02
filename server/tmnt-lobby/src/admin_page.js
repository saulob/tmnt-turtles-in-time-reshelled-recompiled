// The private telemetry dashboard (/admin): one self-contained page, its stylesheet and script inline and allowed by hash
// under a strict Content-Security-Policy, no external resources. The script keeps the token in memory (sessionStorage only
// when asked), sends it as a Bearer header and writes every value with textContent, never as markup.
import { sha256 } from './sha256.js';

const CSS = String.raw`:root {
  color-scheme: light;
  --surface-0: #f3f3f1;
  --surface-1: #fcfcfb;
  --border: #dcdbd6;
  --grid: #e7e6e2;
  --text-primary: #0b0b0b;
  --text-secondary: #52514e;
  --text-muted: #6d6c67;
  --accent: #2a78d6;
  --error: #b42318;
}
@media (prefers-color-scheme: dark) {
  :root:where(:not([data-theme="light"])) {
    color-scheme: dark;
    --surface-0: #111110;
    --surface-1: #1a1a19;
    --border: #34332f;
    --grid: #2a2a27;
    --text-primary: #f0efec;
    --text-secondary: #c3c2b7;
    --text-muted: #9a998f;
    --accent: #3987e5;
    --error: #f97066;
  }
}
:root[data-theme="dark"] {
  color-scheme: dark;
  --surface-0: #111110;
  --surface-1: #1a1a19;
  --border: #34332f;
  --grid: #2a2a27;
  --text-primary: #f0efec;
  --text-secondary: #c3c2b7;
  --text-muted: #9a998f;
  --accent: #3987e5;
  --error: #f97066;
}
* { box-sizing: border-box; }
html { -webkit-text-size-adjust: 100%; }
body { margin: 0; background: var(--surface-0); color: var(--text-primary); font: 15px/1.45 system-ui, -apple-system, "Segoe UI", Roboto, sans-serif; }
.top, main { max-width: 1120px; margin: 0 auto; padding: 0 16px; }
.top { padding-top: 24px; }
h1 { font-size: 22px; margin: 0 0 4px; }
h2 { font-size: 15px; margin: 0 0 8px; }
h3 { font-size: 12px; margin: 12px 0 6px; color: var(--text-secondary); text-transform: uppercase; letter-spacing: .04em; }
.muted { color: var(--text-muted); margin: 0 0 12px; font-size: 13px; }
.panel { background: var(--surface-1); border: 1px solid var(--border); border-radius: 10px; padding: 16px; margin: 0 0 16px; min-width: 0; }
.login { max-width: 420px; display: grid; gap: 10px; }
.check { display: flex; gap: 8px; align-items: center; color: var(--text-secondary); }
input[type="password"], select { font: inherit; padding: 8px 10px; border: 1px solid var(--border); border-radius: 6px; background: var(--surface-0); color: var(--text-primary); width: 100%; }
select { width: auto; }
button { font: inherit; padding: 8px 14px; border-radius: 6px; border: 1px solid var(--accent); background: var(--accent); color: #fff; cursor: pointer; }
button.quiet { background: transparent; color: var(--text-secondary); border-color: var(--border); }
button.link { background: transparent; border: 0; padding: 0; color: var(--accent); font-family: ui-monospace, Consolas, monospace; text-decoration: underline; }
.error { color: var(--error); margin: 0; min-height: 1.2em; }
.toolbar { display: flex; flex-wrap: wrap; gap: 10px; align-items: center; margin: 0 0 16px; }
.toolbar #status { flex: 1; margin: 0; }
.cards { display: grid; grid-template-columns: repeat(auto-fit, minmax(150px, 1fr)); gap: 12px; margin: 0 0 16px; }
.card { background: var(--surface-1); border: 1px solid var(--border); border-radius: 10px; padding: 14px 16px; display: flex; flex-direction: column; }
.card .label { color: var(--text-secondary); font-size: 13px; }
.card .value { font-size: 28px; font-weight: 600; font-variant-numeric: tabular-nums; }
.grid { display: grid; grid-template-columns: repeat(auto-fit, minmax(300px, 1fr)); gap: 16px; margin: 0 0 16px; }
.grid .panel { margin: 0; }
.scroll { overflow-x: auto; }
table { border-collapse: collapse; width: 100%; font-size: 13px; font-variant-numeric: tabular-nums; }
th, td { padding: 5px 8px; text-align: right; border-bottom: 1px solid var(--grid); white-space: nowrap; }
th:first-child, td:first-child { text-align: left; }
th { color: var(--text-secondary); font-weight: 600; }
.empty { color: var(--text-muted); font-size: 13px; margin: 4px 0; }
.mono { font-family: ui-monospace, Consolas, monospace; }
.notes ul { margin: 0; padding-left: 18px; color: var(--text-secondary); font-size: 13px; }
.notes li { margin: 4px 0; }
`;

const HEAD = String.raw`<!doctype html>
<html lang="en">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1">
<meta name="referrer" content="no-referrer">
<meta name="robots" content="noindex, nofollow">
<title>TMNT Online Telemetry</title>
`;

const BODY = String.raw`</head>
<body>
<header class="top">
  <h1>TMNT Turtles in Time Re-Shelled · Online Telemetry</h1>
  <p class="muted">Private. Anonymous operational data of the online service and its clients. Days are UTC.</p>
</header>
<main>
  <form id="login" class="panel login" autocomplete="off">
    <label for="token">Admin token</label>
    <input id="token" type="password" autocomplete="off" spellcheck="false" required>
    <label class="check"><input id="remember" type="checkbox"> Keep for this tab</label>
    <button type="submit">Open dashboard</button>
    <p id="login-error" class="error" role="alert"></p>
  </form>
  <section id="dash" hidden>
    <div class="toolbar">
      <label for="range">Range</label>
      <select id="range">
        <option value="1">1 day</option>
        <option value="7" selected>7 days</option>
        <option value="30">30 days</option>
      </select>
      <button id="refresh" type="button">Refresh</button>
      <span id="status" class="muted" role="status"></span>
      <button id="signout" type="button" class="quiet">Sign out</button>
    </div>
    <div id="cards" class="cards"></div>
    <div id="out"></div>
    <div id="detail" class="panel" hidden></div>
    <div class="panel notes">
      <h2>How to read this</h2>
      <ul>
        <li>Counters (sessions, events, reasons, server events) cover the range chosen. Transport mix and the latency
          percentiles come from the per-session reports still kept (7 days, at most 2000), one value per peer link.</li>
        <li>A session is one player's report of one party, identified by the party's random diag id; it is not a player count.
          Lobby sessions (events before any party) are counted apart and never enter the transport mix or the percentiles.</li>
        <li>Each event has one source: parties, invites and room outcomes come from the Worker; the clients add what only they see.
          Retried events are counted once (duplicates are shown). Host and guest disconnects are derived from the Worker's party and member endings.</li>
        <li>Connection Tests are anonymous outcomes sent before any session exists; they are counters, one per test.</li>
        <li>Nothing here stores friend codes, tokens, addresses, names or gameplay.</li>
      </ul>
    </div>
  </section>
</main>
`;

const JS = String.raw`(function () {
  'use strict';
  var KEY = 'tmnt-admin-token';
  var token = '';
  var timer = null;
  var $ = function (id) { return document.getElementById(id); };

  function storedToken() {
    try { return sessionStorage.getItem(KEY) || ''; } catch (e) { return ''; }
  }
  function storeToken(value) {
    try { if (value) sessionStorage.setItem(KEY, value); else sessionStorage.removeItem(KEY); } catch (e) { /* storage blocked */ }
  }
  function el(tag, cls, text) {
    var node = document.createElement(tag);
    if (cls) node.className = cls;
    if (text !== undefined) node.textContent = String(text);
    return node;
  }
  function clear(node) { while (node.firstChild) node.removeChild(node.firstChild); }
  function fmt(n) { return n === null || n === undefined ? '-' : Number(n).toLocaleString('en-US'); }
  function when(ms) { return ms ? new Date(ms).toISOString().replace('T', ' ').slice(0, 19) + 'Z' : '-'; }
  function pct(part, whole) { return whole ? Math.round(100 * part / whole) + '%' : '-'; }

  function AuthError() {}
  function api(path) {
    return fetch(path, { headers: { Authorization: 'Bearer ' + token }, cache: 'no-store', credentials: 'omit' })
      .then(function (r) {
        if (r.status === 401) throw new AuthError();
        if (!r.ok) throw new Error('The service answered ' + r.status + '.');
        return r.json();
      });
  }

  function table(headers, rows, onCell) {
    var wrap = el('div', 'scroll');
    var t = el('table');
    var head = el('tr');
    headers.forEach(function (h) { head.appendChild(el('th', '', h)); });
    var thead = el('thead');
    thead.appendChild(head);
    t.appendChild(thead);
    var body = el('tbody');
    rows.forEach(function (row) {
      var tr = el('tr');
      row.forEach(function (cell, i) {
        var td = el('td');
        if (onCell && onCell(td, cell, i, row)) { tr.appendChild(td); return; }
        td.textContent = typeof cell === 'number' ? fmt(cell) : String(cell === null || cell === undefined ? '-' : cell);
        tr.appendChild(td);
      });
      body.appendChild(tr);
    });
    t.appendChild(body);
    wrap.appendChild(t);
    return wrap;
  }
  function counts(items) {
    if (!items || !items.length) return el('p', 'empty', 'None');
    var total = items.reduce(function (a, b) { return a + b.count; }, 0);
    return table(['Value', 'Count', 'Share'], items.slice(0, 15).map(function (i) { return [i.value, i.count, pct(i.count, total)]; }));
  }
  function panel(title, content) {
    var box = el('div', 'panel');
    box.appendChild(el('h2', '', title));
    (Array.isArray(content) ? content : [content]).forEach(function (c) { if (c) box.appendChild(c); });
    return box;
  }
  function sub(title, node) {
    var box = el('div');
    box.appendChild(el('h3', '', title));
    box.appendChild(node);
    return box;
  }
  function entries(object) {
    return Object.keys(object || {}).map(function (k) { return { value: k, count: object[k] }; });
  }
  function pairs(object) {
    return Object.keys(object).map(function (k) { return [k.replace(/_/g, ' '), object[k]]; });
  }

  var ABUSE = {
    ws_upgrade_accepted: 'WebSocket upgrades accepted by the directory',
    ws_upgrade_limited: 'WebSocket upgrades refused at the edge',
    ws_upgrade_ip_cap: 'WebSocket upgrades refused: too many sockets from one address',
    hello_accepted: 'Sign-ins accepted',
    hello_limited: 'Sign-ins refused: too many',
    party_create_accepted: 'Parties created',
    party_create_limited: 'Party creations refused',
    party_join_limited: 'Party joins refused',
    invite_limited: 'Invites refused',
    friend_request_limited: 'Friend requests refused',
    telemetry_limited: 'Telemetry batches refused for the account',
    room_resume_limited: 'Match room reconnects refused',
    relay_abuse_closed: 'Relay senders or relays closed for abuse'
  };

  function card(label, value) {
    var c = el('div', 'card');
    c.appendChild(el('span', 'label', label));
    c.appendChild(el('span', 'value', fmt(value)));
    return c;
  }

  function latencyTable(latency) {
    var rows = [];
    [['all', 'All links'], ['direct', 'Direct'], ['relay', 'Relay']].forEach(function (set) {
      [['rtt_ms', 'RTT ms'], ['jitter_ms', 'Jitter ms'], ['loss', 'Loss']].forEach(function (metric) {
        var d = latency[set[0]][metric[0]];
        rows.push([set[1], metric[1], d.n, d.min, d.p50, d.p90, d.p95, d.p99, d.max]);
      });
    });
    return table(['Set', 'Metric', 'N', 'Min', 'P50', 'P90', 'P95', 'P99', 'Max'], rows);
  }

  function renderDetail(data) {
    var box = $('detail');
    clear(box);
    box.hidden = false;
    box.appendChild(el('h2', 'mono', 'Session ' + data.diag));
    data.reporters.forEach(function (r) {
      var c = r.ctx || {};
      box.appendChild(el('h3', '', 'Slot ' + r.slot + (c.host ? ' (host)' : '') + ' · ' + (c.platform || '?') + ' ' + (c.version || '?') +
        ' · ' + (c.os || '?') + ' ' + (c.arch || '') + ' · build ' + (c.fp || '?') + ' · party of ' + (c.party || '?')));
      box.appendChild(el('p', 'muted', 'First report ' + when(r.first_ms) + ', last ' + when(r.last_ms) + ', ' + fmt(r.batches) +
        ' batches, ' + fmt(r.events) + ' events, ' + fmt(r.dropped) + ' dropped on the client.'));
      if (r.session) box.appendChild(table(['Duration ms', 'Test usable', 'Worker', 'STUN', 'Direct', 'Relay', 'Compat', 'Reason'],
        [[r.session.dur_ms, r.session.ct_usable, r.session.worker, r.session.stun, r.session.direct, r.session.relay,
          r.session.compat, r.session.reason]]));
      if (r.peers && r.peers.length) {
        box.appendChild(table(['To', 'State', 'Transport', 'Direct', 'Relay', 'Fallback', 'Est ms', 'RTT avg', 'Jitter', 'Loss', 'Sent', 'Recv', 'Reason'],
          r.peers.map(function (p) {
            return [p.rs, p.state, p.transport, p.direct, p.relay, p.fallback, p.est_ms, p.ping.avg, p.ping.jit, p.ping.loss, p.tx, p.rx, p.reason];
          })));
      }
    });
    box.appendChild(el('h3', '', 'Events (' + data.events.length + ')'));
    box.appendChild(table(['Time', 'From', 'Slot', 'Event', 'Fields'], data.events.map(function (e) {
      var f = Object.keys(e.fields).map(function (k) { return k + '=' + e.fields[k]; }).join(' ');
      return [when(e.ms), e.src, e.slot || '', e.name + (e.src === 'client' ? ' (t+' + e.t + 's)' : ''), f];
    })));
    box.scrollIntoView();
  }

  function openSession(diag) {
    api('/admin/telemetry/session?diag=' + encodeURIComponent(diag)).then(renderDetail).catch(function (error) {
      if (error instanceof AuthError) { signOut('The token was not accepted.'); return; }
      $('status').textContent = error && error.message ? error.message : 'Could not load the session.';
    });
  }

  function render(s) {
    var cards = $('cards');
    clear(cards);
    var pairsTotal = s.transport.direct + s.transport.relay + s.transport.failed;
    [['Parties started', s.totals.parties_started], ['Sessions', s.totals.sessions], ['Batches', s.totals.batches],
      ['Client events', s.totals.client_events], ['Direct links', s.transport.direct], ['Relayed links', s.transport.relay],
      ['Online now', s.live ? s.live.online : null], ['Parties now', s.live ? s.live.parties : null]]
      .forEach(function (c) { cards.appendChild(card(c[0], c[1])); });

    var out = $('out');
    clear(out);
    var g1 = el('div', 'grid');
    g1.appendChild(panel('Sessions by platform', counts(s.sessions_by.platform)));
    g1.appendChild(panel('Sessions by port version', counts(s.sessions_by.version)));
    g1.appendChild(panel('Sessions by OS', counts(s.sessions_by.os)));
    g1.appendChild(panel('Sessions by protocol', counts(s.sessions_by.proto)));
    out.appendChild(g1);

    out.appendChild(panel('Transport mix', [
      table(['Pairs', 'Direct', 'Relay', 'Failed', 'Pending', 'Direct share', 'Relay share'],
        [[s.transport.pairs, s.transport.direct, s.transport.relay, s.transport.failed, s.transport.pending,
          pct(s.transport.direct, pairsTotal), pct(s.transport.relay, pairsTotal)]]),
      sub('Chosen (client events)', counts(s.selected))
    ]));
    out.appendChild(panel('Latency percentiles (per peer link)', latencyTable(s.latency)));

    var g2 = el('div', 'grid');
    g2.appendChild(panel('Direct results', counts(s.direct_results)));
    g2.appendChild(panel('Relay results', counts(s.relay_results)));
    out.appendChild(g2);

    var rs = s.reasons;
    var g2b = el('div', 'grid');
    g2b.appendChild(panel('Fallback / path reasons', counts(entries(rs.fallback))));
    g2b.appendChild(panel('Disconnect reasons', counts(entries(rs.disconnect))));
    g2b.appendChild(panel('Invite outcomes', counts(entries(rs.invite))));
    g2b.appendChild(panel('Service errors', counts(entries(rs.service))));
    g2b.appendChild(panel('Connection Test failures', counts(entries(rs.connection_test))));
    g2b.appendChild(panel('Other (not in the shared list)', table(['Item', 'Count'], [['other', rs.other], ['unknown values seen', s.totals.unknown_reasons]])));
    out.appendChild(g2b);

    out.appendChild(panel('Invites', [
      table(['Sent', 'Accepted', 'Declined', 'Expired', 'Fifth member rejected'],
        [[s.invites.sent, s.invites.accepted, s.invites.declined, s.invites.expired, s.invites.party_full]]),
      sub('Other endings', counts(s.invites.ended_other)),
      sub('Failures seen by clients', counts(s.invites.client_failed))
    ]));
    var cb = s.connection_beacon;
    var lat = cb.latency_ms;
    out.appendChild(panel('Connection Tests', [
      table(['Tests', 'Usable', 'Usable share', 'Unusable', 'Direct possible', 'Relay possible', 'Median ms', 'P90 ms'],
        [[cb.total, cb.usable, pct(cb.usable, cb.total), cb.unusable, cb.direct_available, cb.relay_available, lat.p50, lat.p90]]),
      sub('Failures', counts(cb.failures)),
      sub('Worker', counts(cb.worker)),
      sub('WebSocket', counts(cb.ws)),
      sub('STUN', counts(cb.stun)),
      sub('Router', counts(cb.router)),
      sub('Platform', counts(cb.platform)),
      sub('Version', counts(cb.version))
    ]));
    var ct = s.connection_tests;
    out.appendChild(panel('Connection tests (session reports)', [
      table(['Reports', 'Usable', 'Worker reachable', 'STUN ok', 'Direct possible', 'Relay possible'],
        [[ct.reports, ct.usable, ct.worker_reachable, ct.stun_ok, ct.direct_available, ct.relay_available]]),
      sub('Compatibility', counts(ct.compatibility))
    ]));

    var sv = s.server;
    var g3 = el('div', 'grid');
    g3.appendChild(panel('Worker: party ended', counts(sv.party_ended)));
    g3.appendChild(panel('Worker: member left', counts(sv.member_left)));
    g3.appendChild(panel('Worker: room left', counts(sv.room_left)));
    g3.appendChild(panel('Worker: room ended', counts(sv.room_ended)));
    g3.appendChild(panel('Worker: room transport', counts(sv.room_transport)));
    g3.appendChild(panel('Worker: relay closed', counts(sv.relay_closed)));
    g3.appendChild(panel('Worker: rate limited', counts(sv.rate_limited)));
    g3.appendChild(panel('Rejected telemetry batches', counts(s.rejected_batches)));
    out.appendChild(g3);
    var abuse = s.abuse || [];
    out.appendChild(panel('Abuse protection (aggregate counts)', abuse.length ? table(['Counter', 'Count'],
      abuse.map(function (a) { return [ABUSE[a.value] || a.value, a.count]; })) : el('p', 'empty', 'None')));
    out.appendChild(panel('Worker totals', table(['Item', 'Count'], pairs({ parties_started: sv.parties_started,
      parties_reaching_4_of_4: sv.parties_reached_4 + ' (' + pct(sv.parties_reached_4, sv.parties_started) + ')',
      host_disconnects: sv.host_disconnects, guest_disconnects: sv.guest_disconnects, member_joined: sv.member_joined,
      room_joined: sv.room_joined, room_peer_lost: sv.room_peer_lost, fifth_member_rejected: sv.fifth_member_rejected,
      ignored_client_events: s.totals.ignored_events, duplicate_events: s.totals.duplicate_events,
      lobby_sessions: s.totals.lobby_sessions, redacted_values: s.totals.redacted_values,
      dropped_on_clients: s.totals.client_dropped }))));
    out.appendChild(panel('Client events', counts(s.client_events)));

    var recent = s.recent_sessions;
    out.appendChild(panel('Recent sessions', recent.length ? table(['Diag', 'Last seen', 'Reporters', 'Events', 'Platforms', 'Ended'],
      recent.map(function (r) { return [r.diag, when(r.last_ms), r.reporters, r.events, r.platforms.join(', '), r.ended]; }),
      function (td, cell, i) {
        if (i !== 0) return false;
        var b = el('button', 'link', cell);
        b.type = 'button';
        b.addEventListener('click', function () { openSession(cell); });
        td.appendChild(b);
        return true;
      }) : el('p', 'empty', 'None yet')));

    $('status').textContent = 'Updated ' + when(s.generated) + '; ' + fmt(s.stored.events) + ' events and ' + fmt(s.stored.sessions) + ' reports stored.';
  }

  function load() {
    $('status').textContent = 'Loading...';
    return api('/admin/telemetry/summary?days=' + encodeURIComponent($('range').value)).then(render).catch(function (error) {
      if (error instanceof AuthError) { signOut('The token was not accepted.'); return; }
      $('status').textContent = error && error.message ? error.message : 'Could not load the telemetry.';
    });
  }
  function showDashboard() {
    $('login').hidden = true;
    $('dash').hidden = false;
    load();
    if (timer) clearInterval(timer);
    timer = setInterval(function () { if (!document.hidden) load(); }, 60000);
  }
  function signOut(message) {
    token = '';
    storeToken('');
    if (timer) clearInterval(timer);
    timer = null;
    $('dash').hidden = true;
    $('login').hidden = false;
    $('token').value = '';
    $('login-error').textContent = message || '';
    $('token').focus();
  }

  $('login').addEventListener('submit', function (event) {
    event.preventDefault();
    token = $('token').value.trim();
    $('token').value = '';
    if (!token) return;
    storeToken($('remember').checked ? token : '');
    $('login-error').textContent = '';
    showDashboard();
  });
  $('refresh').addEventListener('click', load);
  $('range').addEventListener('change', load);
  $('signout').addEventListener('click', function () { signOut(''); });
  token = storedToken();
  if (token) { $('remember').checked = true; showDashboard(); }
}());
`;

const hash = (text) => {
  let binary = '';
  for (const byte of sha256(new TextEncoder().encode(text))) binary += String.fromCharCode(byte);
  return `'sha256-${btoa(binary)}'`;
};

export const ADMIN_CSS = CSS;
export const ADMIN_JS = JS;
export const ADMIN_HTML = `${HEAD}<style>${CSS}</style>\n${BODY}<script>${JS}</script>\n</body>\n</html>\n`;
export const ADMIN_CSP = `default-src 'none'; script-src ${hash(JS)}; style-src ${hash(CSS)}; connect-src 'self'; base-uri 'none'; ` +
  "form-action 'none'; frame-ancestors 'none'";
