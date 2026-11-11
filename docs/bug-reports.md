# Bug reports

The launcher's **Bug Report** page sends a problem description and one sanitized game log to the TMNT online service. A
report is identified by a report ID (`TMNT-XXXXXX`) and a receipt. The receipt is the only proof that the report belongs
to this PC; a report ID alone cannot read it.

## The page

1. **Report the problem.** A description of 10 to 500 characters. Spaces around it do not count.
2. **Choose one game log.** The newest five logs are listed, newest first. The default is the newest session that reached
   the game, not the launcher's own session. The log roles are Current launcher session, Current session · Launcher +
   gameplay, Last gameplay, Gameplay session and Launcher only.
3. **Preview what will be sent.** The preview is the exact sanitized report, field by field. Nothing is sent until **Send
   report** is pressed.
4. **Send.** The result shows the report ID. The ID can be copied.

The log folder is the same one the **About** page opens (see [launcher.md](launcher.md)). TMNT writes
`tmnt_turtles_in_time_reshelled_NNN.log`; a rotating sink may leave `tmnt_turtles_in_time_reshelled_NNN.1.log` beside it.
Both are accepted. The file tail is read (up to 256 KiB), scrubbed, then limited to the last 64 KiB on a whole line.

## What is sent

| Field | Meaning |
| --- | --- |
| `description` | The player's text, trimmed and scrubbed. |
| `launcher_version` | The port version (`0.1.0`). |
| `guest` | The game image/build: `TMNT: Turtles in Time Re-Shelled (XBLA) 0.0.1.3 (584109D2)`, or `Not installed`. |
| `platform` | `Windows` or `Linux`. |
| `operating_system` | Windows version and build, or the Linux distribution. |
| `runtime` | The ReXGlue SDK version, for example `ReXGlue SDK 0.10.0`. |
| `log_file` | The selected log's file name only, never its path. |
| `log_clipped` | `yes` when only the end of the log was sent. |
| `log_text` | The sanitized log text. |

No account, Friend Code, address, receipt, token or local path is part of the package.

## Privacy filtering

The log scrubber runs before the preview and before the request, and the preview is built from the package the request
uses. It removes:

- the user name, computer name and domain, profile folder and home folder, plus `XDG_DATA_HOME`, `XDG_CONFIG_HOME` and `XDG_CACHE_HOME` on Linux;
- absolute paths outside the system's folders, mount points and network shares;
- the port install folder, the user-data folder and the AppImage path;
- labelled secrets (`password`, `token`, `secret`, `api_key`, `cookie`, `credential`, `friend_code`, bearer tokens and
  JWT-looking values);
- the online identity token and Friend Code when they appear in the log;
- account-like numbers after `peer ` / `account `, room-like codes and other players' details.

It keeps the technical information a report needs: OS, CPU, GPU, driver/API versions, build, renderer, game identity,
runtime configuration, errors and stack traces. Only LF line endings, tabs and valid UTF-8 remain. Automatic filtering
cannot promise to catch everything, which is why the preview is always available.

## My reports

The page keeps the reports sent from this PC in `<user data>/bug_reports.json` (normally `user_data/bug_reports.json`;
see [user-data.md](user-data.md)). The file holds the ticket, receipt, description, log name, game facts, last known status
and developer message. It is written to `bug_reports.json.new` and moved into place, so an interrupted save leaves the
previous history. A damaged file is renamed `bug_reports.json.damaged` and the list starts empty. At most 50 reports are
kept.

Status checks run off the UI thread. A check sends only tickets and receipts. A report the service no longer returns is
marked **No longer available**, keeps its last known status and message, and stays out of automatic checks; **Refresh**
queries it again on request. Opening the page does not mark everything read: a report becomes seen when it is opened.

A status change marks the report **Updated**. A developer message that is new or changed marks it **New reply**, which
takes priority over a plain update. The sidebar shows an attention dot while either is unseen. The report detail shows
the developer message in its own accent-bordered panel.

**Remove from this PC** deletes only the local history entry. The developer keeps the report until an admin deletes it
there.

## Statuses

`New`, `Investigating`, `Fixed`, `Cannot reproduce`, `Duplicate`, `Not a bug`, `Closed`. A Fixed report may name the
version it was fixed in, shown as "Fixed in version X".

## Service

The report service is the existing TMNT Worker in `server/tmnt-lobby`; it is not a second service. Reports use the same
WebSocket connection and envelope as the rest of the online service:

- `incident.file` submits a report. It signs in with presence off, so Online Play need not be on. The account may file 2
  accepted reports per rolling hour and 10 per rolling 24 hours; the service-wide and per-address ceilings are
  infrastructure protection. Refusals answer `incident_hourly_limit`, `incident_daily_limit`, `limit`, `bad_request` or
  `too_large`.
- `incident.status` sends up to 50 `{ticket, receipt}` claims and returns only the claims that match. A wrong receipt and
  an unknown ticket look the same.
- `incident.read` returns a stored report only for the matching receipt.

The service validates every field and measures sizes itself: 500 description characters, 64 KiB of log text in UTF-8,
64 characters of metadata. A report is stored as received or refused, never rewritten. The newest 500 reports are kept;
report statuses and their receipt hashes are kept separately (5000) so a status can outlive its log. When a report is
removed by retention, the owner's next status check simply leaves it out.

## Admin

`/admin` on the deployed Worker is the private dashboard. It has **Dashboard** and **Bugs** tabs. Bug reports need
the same `ADMIN_TOKEN` as the dashboard; no new binding, secret or Durable Object migration is required. The tables are
created when the TMNT lobby object starts.

The Bugs tab filters by port version, game image and status. Its table lists report ID, submission date, port, game
image, platform, operating system, description preview and status. Opening a report shows the full description, facts,
sanitized log, status meaning, developer message and **Close report**. **Fixed in version** appears only while the
selected status is **Fixed**; the Worker stores it as empty for every other status, even when a stale value is sent.
**Save status** posts to `/admin/api/incidents/<ticket>/status`; nothing is saved until then. A save that changes nothing
keeps its timestamp. A new or changed message is what the launcher shows as **New reply**.

Admin routes, all with `Authorization: Bearer <ADMIN_TOKEN>`:

| Method and path | Purpose |
| --- | --- |
| `GET /admin/api/incidents` | List reports (no logs). |
| `GET /admin/api/incidents/<ticket>` | One report with `log_text`. |
| `GET /admin/api/incidents/<ticket>/log` | The sanitized log as a text download. |
| `POST /admin/api/incidents/<ticket>/status` | `{status, fixed_version, player_message}`. |
| `DELETE /admin/api/incidents/<ticket>` | Removes the report, log, status and receipt hash. |

Service metrics on the Dashboard include reports filed, rejected, status checks, report reads, developer replies and
deleted reports, with the existing current/previous period comparison. Reports by status are live counts.

## Worker setup and deployment

The Worker configuration is `server/tmnt-lobby/wrangler.toml` and `server/tmnt-lobby/package.json`. It needs the existing
`LOBBY` and `MATCH` Durable Object bindings and the `WS_UPGRADE_LIMITER` rate limit. The dashboard needs the secret
`ADMIN_TOKEN` (at least 16 characters):

```
cd server/tmnt-lobby
npx wrangler secret put ADMIN_TOKEN
```

`tools/worker.ps1` deploys from the repository root:

```
.\tools\worker.ps1
.\tools\worker.ps1 -DryRun
```

It finds the repository root, checks `server/tmnt-lobby/wrangler.toml` and `npx`, enters the Worker folder, runs
`npx wrangler deploy` (adding `--dry-run` for `-DryRun`), restores the previous folder and propagates Wrangler's exit
code. A dry run bundles and validates without uploading anything. Deploying is a manual developer action; the build and
tests never deploy.

The launcher uses `wss://tmnt-lobby.saulobenigno.workers.dev/` by default. `TMNT_LOBBY` overrides it for a local service
(`node server/tmnt-lobby/local/server.js`).
