// The private routes under /admin: a static dashboard shell and its JSON routes, all answering 404 unless the ADMIN_TOKEN
// secret is set, the JSON routes only with `Authorization: Bearer <ADMIN_TOKEN>`. No Cloudflare import, so Node runs it in
// the tests and in the development server.
import { ADMIN_CSP, ADMIN_HTML } from './admin_page.js';
import { isTicket, logDownloadName, readStatusChange } from './incidents.js';
import { HISTORY_MAX_DAYS } from './metrics.js';
import { TELEMETRY_LIMITS, isDiag } from './telemetry.js';
import { sha256Hex } from './sha256.js';

export const MIN_TOKEN_CHARS = 16;
const MAX_STATUS_BODY_CHARS = 8 * 1024;

const SECURITY_HEADERS = {
  'Cache-Control': 'no-store',
  'X-Content-Type-Options': 'nosniff',
  'Referrer-Policy': 'no-referrer',
  'X-Frame-Options': 'DENY',
  'Cross-Origin-Resource-Policy': 'same-origin',
};

export const isAdminPath = (pathname) => pathname === '/admin' || pathname.startsWith('/admin/');

export const adminEnabled = (env) => typeof env?.ADMIN_TOKEN === 'string' && env.ADMIN_TOKEN.length >= MIN_TOKEN_CHARS;

function reply(body, status, type, extra = {}) {
  return new Response(body, { status, headers: { ...SECURITY_HEADERS, 'Content-Type': type, ...extra } });
}

const json = (value, status = 200, extra = {}) => reply(JSON.stringify(value), status, 'application/json; charset=utf-8', extra);

// Compares digests so the time taken does not depend on where the texts differ.
export function tokenMatches(authorization, token) {
  if (typeof token !== 'string' || token.length < MIN_TOKEN_CHARS) return false;
  const given = typeof authorization === 'string' && authorization.startsWith('Bearer ') ? authorization.slice(7) : '';
  const a = sha256Hex(given);
  const b = sha256Hex(token);
  let diff = 0;
  for (let i = 0; i < a.length; i++) diff |= a.charCodeAt(i) ^ b.charCodeAt(i);
  return diff === 0;
}

// ?days=N for 1..max, else null; no parameter is the default range.
function dayRange(params, fallback, max) {
  const text = params.get('days');
  if (text === null) return fallback;
  if (!/^[1-9][0-9]{0,2}$/.test(text)) return null;
  const days = Number(text);
  return days <= max ? days : null;
}

export const summaryDays = (params) => dayRange(params, TELEMETRY_LIMITS.defaultDays, TELEMETRY_LIMITS.maxDays);
export const DEFAULT_HISTORY_DAYS = 30;
export const historyDays = (params) => dayRange(params, DEFAULT_HISTORY_DAYS, HISTORY_MAX_DAYS);

const ROUTES = {
  '/admin/telemetry/summary': { name: 'summary', methods: ['GET'] },
  '/admin/telemetry/session': { name: 'session', methods: ['GET'] },
  '/admin/api/summary': { name: 'metrics', methods: ['GET'] },
  '/admin/api/history': { name: 'history', methods: ['GET'] },
  '/admin/api/incidents': { name: 'incidents', methods: ['GET'] },
};

const INCIDENTS_PATH = '/admin/api/incidents';

// The dashboard API behind a path, or null. A report id is checked after the token, before anything reads it.
function adminRoute(pathname) {
  if (Object.hasOwn(ROUTES, pathname)) return ROUTES[pathname];
  if (!pathname.startsWith(`${INCIDENTS_PATH}/`)) return null;
  const [ticket, rest, ...more] = pathname.slice(INCIDENTS_PATH.length + 1).split('/');
  if (!isTicket(ticket) || more.length > 0) return null;
  if (rest === undefined) return { name: 'incident', ticket, methods: ['GET', 'DELETE'] };
  if (rest === 'log') return { name: 'incident_log', ticket, methods: ['GET'] };
  if (rest === 'status') return { name: 'incident_status', ticket, methods: ['POST'] };
  return null;
}

async function saveIncidentStatus(request, ticket, source) {
  const text = await request.text();
  if (text.length > MAX_STATUS_BODY_CHARS) return json({ error: 'too_large' }, 413);
  let body = null;
  try {
    body = JSON.parse(text);
  } catch {
    return json({ error: 'bad_request', detail: 'invalid JSON' }, 400);
  }
  const { change, detail } = readStatusChange(body);
  if (!change) return json({ error: 'bad_request', detail }, 400);
  const saved = await source.setIncidentStatus(ticket, change);
  return saved ? json(saved) : json({ error: 'not_found' }, 404);
}

// source: { summary(days), session(diag), metricsSummary(), metricsHistory(days), incidents(), incident(ticket),
// setIncidentStatus(ticket, change), deleteIncident(ticket) }, each a Promise; read only after the token matched.
export async function handleAdmin(request, env, source, log = () => {}) {
  if (!adminEnabled(env)) return json({ error: 'not_found' }, 404);
  const url = new URL(request.url);
  if (request.method === 'GET' && (url.pathname === '/admin' || url.pathname === '/admin/')) {
    return reply(ADMIN_HTML, 200, 'text/html; charset=utf-8', { 'Content-Security-Policy': ADMIN_CSP });
  }
  const route = adminRoute(url.pathname);
  if (!route) {
    if (request.method !== 'GET') return json({ error: 'method_not_allowed' }, 405, { Allow: 'GET' });
    return json({ error: 'not_found' }, 404);
  }
  if (!route.methods.includes(request.method)) {
    return json({ error: 'method_not_allowed' }, 405, { Allow: route.methods.join(', ') });
  }
  if (!tokenMatches(request.headers.get('Authorization'), env.ADMIN_TOKEN)) {
    return json({ error: 'unauthorized' }, 401, { 'WWW-Authenticate': 'Bearer' });
  }
  let days = 0;
  let diag = '';
  if (route.name === 'summary') {
    days = summaryDays(url.searchParams);
    if (days === null) return json({ error: 'bad_request', detail: `days must be 1 to ${TELEMETRY_LIMITS.maxDays}` }, 400);
  } else if (route.name === 'history') {
    days = historyDays(url.searchParams);
    if (days === null) return json({ error: 'bad_request', detail: `days must be 1 to ${HISTORY_MAX_DAYS}` }, 400);
  } else if (route.name === 'session') {
    diag = url.searchParams.get('diag') ?? '';
    if (!isDiag(diag)) return json({ error: 'bad_request', detail: 'diag must be 12 lower-case hex characters' }, 400);
  }
  try {
    if (route.name === 'summary') return json(await source.summary(days));
    if (route.name === 'metrics') return json(await source.metricsSummary());
    if (route.name === 'history') return json(await source.metricsHistory(days));
    if (route.name === 'session') {
      const found = await source.session(diag);
      return found ? json(found) : json({ error: 'not_found' }, 404);
    }
    if (route.name === 'incidents') return json(await source.incidents());
    if (route.name === 'incident_status') return await saveIncidentStatus(request, route.ticket, source);
    if (route.name === 'incident' && request.method === 'DELETE') {
      return await source.deleteIncident(route.ticket) ? json({ deleted: route.ticket }) : json({ error: 'not_found' }, 404);
    }
    if (route.name === 'incident') {
      const incident = await source.incident(route.ticket);
      return incident ? json(incident) : json({ error: 'not_found' }, 404);
    }
    const incident = await source.incident(route.ticket);
    if (!incident) return json({ error: 'not_found' }, 404);
    return reply(incident.log_text, 200, 'text/plain; charset=utf-8',
      { 'Content-Disposition': `attachment; filename="${logDownloadName(incident)}"` });
  } catch (error) {
    log(`admin: telemetry unavailable: ${error?.message ?? error}`);
    return json({ error: 'unavailable' }, 502);
  }
}
