#pragma once

// Offline, read-only UI. Device and packet data are rendered with textContent,
// never interpolated into this document or interpreted as HTML.
static const char RADIO_DASHBOARD_PAGE[] = R"HTML(<!doctype html>
<html lang="en">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1">
<meta name="color-scheme" content="dark light">
<title>MeshCore radio stats</title>
<style>
:root {
  --bg: #0b1220; --panel: #131e30; --panel-alt: #19263b; --border: #2a3b53;
  --text: #edf4ff; --muted: #a6b7ce; --tx: #5ee0bd; --rx: #aeb3ff;
  --warn: #ffd18a; --bad: #ff9b9b; --focus: #88c6ff;
}
* { box-sizing: border-box; }
body { margin: 0; background: var(--bg); color: var(--text); font: 15px/1.5 system-ui, sans-serif; }
main { max-width: 1320px; margin: auto; padding: 30px 24px 48px; }
header, .section-head, .actions, .legend, .key-value { display: flex; align-items: center; gap: 12px; }
header { justify-content: space-between; flex-wrap: wrap; margin-bottom: 24px; }
.brand { display: flex; align-items: center; gap: 14px; min-width: 0; max-width: 100%; }
.mark { border: 1px solid var(--tx); border-radius: 12px; padding: 8px 13px; color: var(--tx); font: bold 22px monospace; }
h1 { font-size: clamp(22px, 3vw, 30px); line-height: 1.15; margin: 0 0 5px; overflow-wrap: anywhere; }
h2 { font-size: 16px; margin: 0; }
p { margin: 0; }
.muted, .eyebrow, small { color: var(--muted); }
.eyebrow { font-size: 12px; letter-spacing: .09em; text-transform: uppercase; }
.pill { display: inline-block; padding: 3px 10px; border: 1px solid var(--border); border-radius: 20px; font-size: 12px; white-space: nowrap; }
.good { color: var(--tx); } .warning { color: var(--warn); } .bad { color: var(--bad); }
button, select { font: inherit; color: var(--text); background: var(--panel-alt); border: 1px solid var(--border); border-radius: 8px; padding: 7px 12px; }
button { cursor: pointer; } button:disabled { opacity: .55; cursor: wait; }
button:hover { border-color: var(--muted); }
a { color: var(--tx); }
details { margin-top: 10px; } summary { cursor: pointer; color: var(--muted); }
:focus-visible { outline: 2px solid var(--focus); outline-offset: 3px; }
.notice { padding: 12px 16px; border: 1px solid var(--border); border-radius: 10px; margin-bottom: 20px; background: var(--panel); }
.grid { display: grid; gap: 16px; }
.summary { grid-template-columns: repeat(4, minmax(0, 1fr)); margin-bottom: 16px; }
.two { grid-template-columns: repeat(2, minmax(0, 1fr)); margin-bottom: 16px; }
.card { background: var(--panel); border: 1px solid var(--border); border-radius: 14px; padding: 20px; min-width: 0; }
.number { font-size: 29px; line-height: 1.3; font-variant-numeric: tabular-nums; margin: 6px 0; }
.section-head { justify-content: space-between; flex-wrap: wrap; margin-bottom: 16px; }
.key-value { justify-content: space-between; padding: 8px 0; border-bottom: 1px solid var(--border); }
.key-value:last-child { border: 0; }
.key-value > :last-child { text-align: right; font-variant-numeric: tabular-nums; }
.meter { height: 7px; border-radius: 8px; background: var(--border); overflow: hidden; margin: 12px 0; }
.meter > span { display: block; height: 100%; background: var(--tx); width: 0; }
.outcomes { display: grid; grid-template-columns: repeat(3, 1fr); gap: 12px; margin: 16px 0; }
.outcomes .number { font-size: 23px; }
.legend { gap: 18px; flex-wrap: wrap; font-size: 12px; }
.swatch { display: inline-block; width: 20px; height: 3px; background: var(--tx); margin: 0 6px 3px 0; }
.swatch.rx { background: var(--rx); }
.swatch.estimate { background: none; height: 0; border-top: 3px dashed var(--rx); }
canvas { display: block; width: 100%; height: 150px; margin-top: 12px; }
.chart-note { margin-top: 12px; font-size: 12px; }
.scroll { overflow-x: auto; }
table { border-collapse: collapse; width: 100%; font-size: 13px; white-space: nowrap; }
th { text-align: left; color: var(--muted); font-size: 11px; letter-spacing: .04em; text-transform: uppercase; }
th, td { padding: 11px 10px; border-bottom: 1px solid var(--border); }
th:first-child, td:first-child { padding-left: 0; }
tr:last-child td { border-bottom: 0; }
code { color: var(--muted); font: 11px/1.4 ui-monospace, monospace; }
.packet-detail { white-space: normal; min-width: 22em; max-width: 42em; }
.role-key { white-space: normal; overflow-wrap: anywhere; max-width: 38em; display: block; }
.empty { color: var(--muted); padding: 24px 0; text-align: center; }
.wide { margin-bottom: 16px; }
footer { margin-top: 24px; display: flex; justify-content: space-between; gap: 12px; flex-wrap: wrap; font-size: 12px; color: var(--muted); }
@media (max-width: 850px) { .summary { grid-template-columns: repeat(2, minmax(0, 1fr)); } .two { grid-template-columns: 1fr; } }
@media (max-width: 480px) { main { padding: 20px 12px; } .card { padding: 15px; } .number { font-size: 23px; } .actions { flex-wrap: wrap; } }
@media (prefers-color-scheme: light) {
  :root { --bg: #f2f5fa; --panel: #fff; --panel-alt: #edf2f9; --border: #d3ddeb; --text: #18283e;
    --muted: #52657e; --tx: #007b62; --rx: #6350b5; --warn: #926000; --bad: #b33232; --focus: #176eb5; }
}
</style>
</head>
<body>
<main>
  <header>
    <div class="brand"><div class="mark" aria-hidden="true">M</div><div>
      <h1 id="device-name">MeshCore Radio</h1><p class="muted">Radio stats</p>
    </div></div>
    <div class="actions">
      <a id="admin-link" href="/admin" hidden>Administration</a>
      <span id="connection" class="pill">Connecting</span>
      <button id="pause" type="button" aria-pressed="false">Pause updates</button>
      <button id="refresh" type="button">Refresh</button>
    </div>
  </header>
  <div id="notice" class="notice" role="status" aria-live="polite" hidden></div>
  <noscript><p class="notice">Enable JavaScript for live updates, or view <a href="/api/status">status JSON</a>.</p></noscript>

  <section class="grid summary" aria-label="Radio overview">
    <article class="card"><p class="eyebrow">Radio profile</p><p id="frequency" class="number">-</p><p id="modulation" class="muted">Awaiting profile</p></article>
    <article class="card"><p class="eyebrow">Device</p><p id="uptime" class="number">-</p><p id="wifi" class="muted">WiFi status unavailable</p>
      <p id="heap" class="muted">Memory status unavailable</p>
      <details><summary>DMA memory</summary><small>Free / largest block / lowest free since boot</small><p id="dma-heap">-</p></details></article>
    <article class="card"><p class="eyebrow">KISS clients</p><p id="client-count" class="number">-</p><p id="owner" class="muted">-</p></article>
    <article class="card"><p class="eyebrow">Transmit queue</p><p id="queue" class="number">-</p><p id="radio-state" class="muted">Waiting for status</p></article>
  </section>

  <div class="grid two">
    <section class="card">
      <div class="section-head"><h2>TX airtime allowance</h2><span id="profile-state" class="pill">-</span></div>
      <p id="credit" class="number">-</p><p id="budget-detail" class="muted">Available TX allowance</p>
      <div class="meter" role="meter" id="budget-meter" aria-label="Remaining shared TX allowance" aria-valuemin="0" aria-valuemax="100" aria-valuenow="0"><span id="budget-fill"></span></div>
      <div class="key-value"><span class="muted">TX airtime since boot</span><strong id="tx-airtime">-</strong></div>
      <div class="key-value"><span class="muted">Estimated RX airtime since boot</span><strong id="rx-airtime">-</strong></div>
      <div class="key-value"><span class="muted">Channel access</span><span id="carrier">-</span></div>
    </section>
    <section class="card">
      <div class="section-head"><h2>Packet counts</h2><span class="pill">Since boot</span></div>
      <div class="outcomes">
        <div><p class="muted">Received over RF</p><p id="rx-count" class="number">-</p></div>
        <div><p class="muted">Sent over RF</p><p id="tx-count" class="number good">-</p></div>
        <div><p class="muted">Accepted for TX</p><p id="tx-accepted" class="number">-</p></div>
        <div><p class="muted">Rejected before TX</p><p id="tx-rejected" class="number warning">-</p></div>
        <div><p class="muted">Failed TX</p><p id="tx-failed" class="number bad">-</p></div>
        <div><p class="muted">Unconfirmed TX</p><p id="tx-unknown" class="number warning">-</p></div>
      </div>
      <div class="key-value"><span class="muted">Radio receive errors</span><span id="rx-errors">-</span></div>
      <p class="muted chart-note">Sent means the radio completed transmission, not that a recipient acknowledged it.</p>
    </section>
  </div>

  <div class="grid two">
    <section class="card">
      <div class="section-head"><h2>Packet traffic</h2><span class="muted">Latest 60 seconds</span></div>
      <div class="legend"><span><i class="swatch rx"></i>RX packets</span><span><i class="swatch"></i>TX RF attempts</span></div>
      <canvas id="packets-chart" role="img" aria-label="Packet counts per second"></canvas>
    </section>
    <section class="card">
      <div class="section-head"><h2>Airtime</h2><span id="occupancy" class="muted">-</span></div>
      <div class="legend"><span><i class="swatch"></i>TX</span><span><i class="swatch estimate"></i>RX (estimated)</span></div>
      <canvas id="airtime-chart" role="img" aria-label="Observed TX and estimated RX milliseconds per second"></canvas>
    </section>
  </div>

  <section id="role-panel" class="card wide" hidden>
    <div class="section-head"><h2>On-device roles</h2></div>
    <div class="scroll" tabindex="0" aria-label="On-device service identities and status">
      <table><thead><tr><th>Role</th><th>Name</th><th>Status</th><th>Identity</th><th>Radio session</th></tr></thead><tbody id="roles"></tbody></table>
    </div>
  </section>
  <section class="card wide">
    <div class="section-head"><h2>Connected clients</h2></div>
    <div class="scroll" tabindex="0" aria-label="Connected KISS clients">
      <table><thead><tr><th>Client</th><th>Session</th><th>Protocol</th><th>Airtime factor</th><th>TX allowance</th><th>TX airtime</th></tr></thead><tbody id="clients"></tbody></table>
    </div>
    <p id="clients-empty" class="empty">No connected clients.</p>
  </section>
  <section class="card">
    <div class="section-head"><div><h2>Recent activity</h2><p id="history-detail" class="muted">-</p></div>
      <label class="muted">Show <select id="filter"><option value="all">All events</option><option value="rx">RX only</option><option value="tx">TX only</option></select></label>
    </div>
    <div class="scroll" tabindex="0" aria-label="Recent packet events">
      <table><thead><tr><th>Age</th><th>Result</th><th>Size / source</th><th>Queue wait</th><th>Airtime</th><th>RSSI / SNR</th><th>Packet / route</th><th>Packet bytes (hex)</th></tr></thead><tbody id="events"></tbody></table>
    </div>
    <p id="events-empty" class="empty">No matching events yet.</p>
    <p id="contact-detail" class="muted chart-note">Contact names unavailable; prefix matches use on-device identities only.</p>
    <p class="muted chart-note">RX is received over RF. Local reflections are not recorded here. Prefix matches do not authenticate a sender or exclude collisions elsewhere on the mesh. Only the first 16 packet bytes are available. Encrypted content stays encrypted.</p>
  </section>
  <footer><span id="updated">Connecting...</span><span id="firmware-version"></span></footer>
</main>
<script>
'use strict';
const byId = id => document.getElementById(id);
const text = (id, value) => { byId(id).textContent = value; };
const numbers = new Intl.NumberFormat(undefined, {maximumFractionDigits: 4});
const number = value => numbers.format(value);
const factor = value => value >= 10000 ? value.toExponential(3) : number(value);
function duration(ms) {
  if (ms < 1000) return number(ms) + ' ms';
  if (ms < 60000) return number(ms / 1000) + ' s';
  const seconds = Math.floor(ms / 1000);
  const days = Math.floor(seconds / 86400);
  const hours = Math.floor(seconds / 3600) % 24;
  const minutes = Math.floor(seconds / 60) % 60;
  return (days ? days + 'd ' : '') + hours + 'h ' + minutes + 'm';
}
function activityAge(ms) {
  if (!Number.isFinite(ms)) return '-';
  const seconds = Math.floor(Math.max(0, ms) / 1000);
  if (seconds < 60) return seconds + 's';
  if (seconds < 3600) return Math.floor(seconds / 60) + 'm';
  if (seconds < 86400) return Math.floor(seconds / 3600) + 'h';
  return Math.floor(seconds / 86400) + 'd';
}
const packetTypes = ['Request', 'Response', 'Private text', 'ACK', 'Advert',
  'Group text', 'Group data', 'Anonymous request', 'Returned path', 'Trace',
  'Multipart', 'Control'];
const packetRoutes = ['Transport flood', 'Flood', 'Direct', 'Transport direct'];
const contactTypes = {1: 'Chat', 2: 'Repeater', 3: 'Room', 4: 'Sensor'};
function knownIdentities(data) {
  const identities = new Map();
  const contacts = data.contacts;
  const items = contacts && Array.isArray(contacts.items) ? contacts.items : [];
  identities.incomplete = !!contacts && (contacts.truncated !== false || contacts.total !== items.length);
  for (const contact of items) {
    if (!contact || typeof contact.public_key !== 'string' || !/^[0-9a-f]{64}$/i.test(contact.public_key)) {
      identities.incomplete = true;
      continue;
    }
    const key = contact.public_key.toLowerCase();
    if (!identities.has(key)) identities.set(key, new Set());
    if (typeof contact.name === 'string' && contact.name)
      identities.get(key).add(contact.name + (contactTypes[contact.type] ? ' [' + contactTypes[contact.type] + ']' : ''));
  }
  for (const role of data.roles || []) {
    if (typeof role.public_key !== 'string' || !/^[0-9a-f]{64}$/i.test(role.public_key)) continue;
    const key = role.public_key.toLowerCase();
    const name = typeof role.name === 'string' && role.name ? role.name : roleNames[role.role] || 'On-device role';
    if (items.some(c => c && typeof c.public_key === 'string' && c.public_key.toLowerCase() === key)) continue;
    if (!identities.has(key)) identities.set(key, new Set());
    identities.get(key).add(name);
  }
  return identities;
}
function prefixLabel(prefix, identities) {
  if (!/^(?:[0-9a-f]{2})+$/i.test(prefix)) return prefix;
  prefix = prefix.toLowerCase();
  const matches = [...identities].filter(([key]) => key.startsWith(prefix));
  if (matches.length > 1) return prefix + ' (ambiguous: ' + matches.length + ' known identities' +
    (identities.incomplete ? '; contact list incomplete' : '') + ')';
  if (identities.incomplete && prefix.length < 64) {
    const candidate = matches.length === 1 && matches[0][1].size === 1 ? [...matches[0][1]][0] : '';
    return prefix + ' (' + (candidate ? 'candidate ' + candidate + '; prefix match; ' : '') + 'contact list incomplete)';
  }
  if (matches.length === 1 && matches[0][1].size === 1)
    return prefix + ' (matches ' + [...matches[0][1]][0] + ')';
  return prefix;
}
function decodePacket(event, identities) {
  const hex = event.preview_hex;
  const length = event.length;
  if (typeof hex !== 'string' || hex.length > 32 || !/^(?:[0-9a-f]{2})*$/i.test(hex) ||
      !Number.isInteger(length) || length < 0 || length > 255 || hex.length / 2 > length)
    return 'Malformed packet preview';
  if (!hex.length) return length ? 'Packet preview unavailable' : 'No packet bytes';
  const raw = (hex.match(/../g) || []).map(byte => parseInt(byte, 16));
  const asHex = (start, count) => hex.slice(start * 2, (start + count) * 2).toLowerCase();
  const route = raw[0] & 3, type = raw[0] >> 2 & 15, version = raw[0] >> 6;
  const parts = [packetTypes[type] || (type === 15 ? 'Custom' : 'Unknown type ' + type), packetRoutes[route], 'v' + version];
  const finish = message => [...parts, message].filter(Boolean).join(' / ');
  if (version !== 0) return finish('Unsupported payload version');
  const available = end => end <= raw.length;
  const missing = field => finish((raw.length < length ? 'Preview ends before ' : 'Malformed packet: missing ') + field);
  let offset = route === 0 || route === 3 ? 5 : 1;
  if (length < offset + 1) return finish('Malformed packet: missing route header');
  if (!available(offset + 1)) return missing('route header');
  if (offset === 5) {
    const code = at => (raw[at] | raw[at + 1] << 8).toString(16).padStart(4, '0');
    parts.push('transport codes 0x' + code(1) + ', 0x' + code(3));
  }
  const packed = raw[offset++], width = (packed >> 6) + 1, count = packed & 63;
  const pathBytes = width * count, payloadAt = offset + pathBytes;
  if (width === 4 || pathBytes > 64) return finish('Malformed packet: invalid path width/count');
  if (payloadAt >= length || length - payloadAt > 184) return finish('Malformed packet: invalid path/payload length');
  const trace = type === 9 && (route === 2 || route === 3);
  parts.push(trace ? 'trace signal bytes ' + pathBytes :
    (route === 0 || route === 1 ? 'traversed path' : 'remaining route') +
    ' ' + count + ' hop' + (count === 1 ? '' : 's') + ' × ' + width + ' B');
  const hops = [];
  for (let i = 0; i < count && available(offset + (i + 1) * width); i++)
    hops.push(trace ? asHex(offset + i * width, width) : prefixLabel(asHex(offset + i * width, width), identities));
  if (hops.length) parts.push(hops.join(' → '));
  if (!available(payloadAt)) return missing('end of path');
  const payloadLength = length - payloadAt;
  const prefix = at => prefixLabel(asHex(at, 1), identities);
  if ([0, 1, 2, 8].includes(type)) {
    if (payloadLength < 4) return finish('Malformed packet: short private envelope');
    if (!available(payloadAt + 2)) return missing('source/destination prefixes');
    parts.push('src ' + prefix(payloadAt + 1) + ' → dst ' + prefix(payloadAt), 'encrypted');
  } else if (type === 7) {
    if (payloadLength < 35) return finish('Malformed packet: short anonymous envelope');
    if (!available(payloadAt + 1)) return missing('destination prefix');
    parts.push('dst ' + prefix(payloadAt), 'anonymous source / encrypted');
  } else if (type === 4) {
    if (payloadLength < 100) return finish('Malformed packet: short advert');
    if (!available(payloadAt + 1)) return missing('advert source prefix');
    const visible = Math.min(32, raw.length - payloadAt);
    parts.push('advert key prefix ' + prefixLabel(asHex(payloadAt, visible), identities));
  } else if (type === 5 || type === 6) {
    if (payloadLength < 3) return finish('Malformed packet: short group envelope');
    if (!available(payloadAt + 1)) return missing('channel hash');
    parts.push('channel hash ' + asHex(payloadAt, 1), 'encrypted');
  } else if (type === 3) {
    if (payloadLength !== 4) return finish('Malformed packet: ACK needs 4 bytes');
    if (!available(payloadAt + 4)) return missing('ACK reference');
    parts.push('ACK reference ' + asHex(payloadAt, 4));
  } else if (type === 9 && payloadLength < 9) {
    return finish('Malformed packet: short trace');
  }
  return finish(raw.length < length ? 'partial preview' : '');
}
function cell(row, value, className = '') {
  const td = document.createElement('td');
  td.textContent = value;
  td.className = className;
  row.appendChild(td);
  return td;
}
const reasons = ['', 'Invalid request', 'Queue full', 'Stale generation', 'Not owner', 'Busy',
  'Expired', 'RF start failed', 'RF timeout', 'Disconnected', 'Not configured'];
const states = ['Rejected', 'Queued', 'Sent over RF', 'Failed', 'Unconfirmed'];
const roleNames = {repeater: 'Repeater', room: 'Room', companion: 'Companion', observer: 'Observer', bot: 'KISS bot', management: 'Management', 'command-bot': 'Command bot'};
function sourceLabel(data, slot, generation) {
  const role = (data.roles || []).find(r => r.source_slot === slot &&
    (generation === undefined || r.source_generation === generation));
  if (role) return roleNames[role.role] || role.role;
  return slot < data.kiss.capacity ? 'KISS ' + (slot + 1) : 'Radio source ' + slot;
}
let paused = false, socket = null, retryTimer = null, watchdog = null;
let latest = null, lastSuccess = 0, retryDelay = 1000;

function chart(id, bins, fields, airtime) {
  const canvas = byId(id);
  const width = canvas.clientWidth;
  if (!width) return;
  const height = 150, scale = Math.min(window.devicePixelRatio || 1, 2);
  canvas.width = Math.round(width * scale); canvas.height = Math.round(height * scale);
  const ctx = canvas.getContext('2d');
  ctx.scale(scale, scale);
  const css = getComputedStyle(document.documentElement);
  const colours = [css.getPropertyValue('--tx').trim(), css.getPropertyValue('--rx').trim()];
  const values = bins.flatMap(b => fields.map(f => b[f]));
  const maximum = airtime ? Math.ceil(Math.max(1000, ...values) / 500) * 500 : Math.max(1, ...values);
  const left = 42, right = width - 8, top = 12, bottom = 132;
  ctx.font = '11px system-ui'; ctx.lineWidth = 1;
  for (let n = 0; n <= 2; n++) {
    const y = bottom - n / 2 * (bottom - top);
    ctx.strokeStyle = css.getPropertyValue('--border').trim();
    ctx.beginPath(); ctx.moveTo(left, y); ctx.lineTo(right, y); ctx.stroke();
    ctx.fillStyle = css.getPropertyValue('--muted').trim(); ctx.textAlign = 'right';
    ctx.fillText(number(maximum * n / 2), left - 6, y + 4);
  }
  const count = Math.max(60, bins.length), step = (right - left) / count;
  const offset = count - bins.length;
  fields.forEach((field, series) => {
    ctx.strokeStyle = colours[series]; ctx.fillStyle = colours[series]; ctx.lineWidth = 2;
    ctx.setLineDash(airtime && series === 1 ? [5, 4] : []);
    if (airtime) {
      ctx.beginPath();
      bins.forEach((b, i) => {
        const x = left + (offset + i + .5) * step, y = bottom - b[field] / maximum * (bottom - top);
        if (i) ctx.lineTo(x, y); else ctx.moveTo(x, y);
      });
      ctx.stroke();
    } else {
      bins.forEach((b, i) => {
        const h = b[field] / maximum * (bottom - top);
        ctx.fillRect(left + (offset + i + series / 2) * step, bottom - h, Math.max(1, step * .4), h);
      });
    }
  });
  ctx.setLineDash([]);
  canvas.setAttribute('aria-label', airtime
    ? 'TX observed and RX estimated milliseconds per second. Maximum scale ' + maximum + ' milliseconds.'
    : 'Last window: ' + bins.reduce((n, b) => n + b.rx_packets, 0) + ' received packets, ' +
      bins.reduce((n, b) => n + b.tx_packets, 0) + ' completed RF transmit attempts.');
}

let eventSignature = null, eventAges = [];
function renderEvents(data) {
  const filter = byId('filter').value;
  const signature = JSON.stringify([filter, data.history.events, (data.roles || []).map(r =>
    [r.role, r.name, r.public_key, r.source_slot, r.source_generation]), data.kiss.capacity, data.contacts || null]);
  if (signature === eventSignature) {
    for (const [td, at] of eventAges) {
      const age = activityAge(data.uptime_ms - at);
      if (td.textContent !== age) td.textContent = age;
    }
    return;
  }
  const identities = knownIdentities(data);
  const contacts = data.contacts;
  text('contact-detail', contacts ? 'Companion contacts: ' + (Array.isArray(contacts.items) ? contacts.items.length : 0) + ' of ' + contacts.total +
    (identities.incomplete ? '. Candidate prefix names are uncertain; omitted contacts may collide.' : '. Prefix names use companion contacts and on-device identities.') :
    'Contact names unavailable; prefix matches use on-device identities only.');
  const rows = [];
  eventAges = [];
  for (const e of data.history.events) {
    if (filter !== 'all' && e.direction !== filter) continue;
    const row = document.createElement('tr');
    eventAges.push([cell(row, activityAge(data.uptime_ms - e.at_ms)), e.at_ms]);
    const label = e.direction === 'rx' ? 'Received over RF' : e.direction === 'local' ? 'Local reflection' :
      e.direction !== 'tx' ? 'Unknown direction' : (states[e.state] || 'Other') +
      (e.reason ? ' / ' + (reasons[e.reason] || 'reason ' + e.reason) : '');
    cell(row, label, e.direction === 'rx' ? '' : e.state === 2 ? 'good' : 'warning');
    cell(row, e.length + ' B' + (e.direction === 'tx' ? ' / ' + sourceLabel(data, e.source_slot, e.source_generation) + ' #' + e.source_generation : ''));
    cell(row, e.direction === 'rx' ? '-' : duration(e.queue_ms));
    cell(row, e.direction === 'rx' ? 'est. ' + duration(e.estimated_ms) : duration(e.rf_ms));
    const signal = e.direction === 'rx' && Number.isFinite(e.rssi_dbm) && Number.isFinite(e.snr_db) &&
      !(e.rssi_dbm === 127 && e.snr_db === -32);
    cell(row, signal ? number(e.rssi_dbm) + ' dBm / ' + number(e.snr_db) + ' dB' : '-');
    cell(row, decodePacket(e, identities), 'packet-detail');
    const preview = document.createElement('code');
    const hex = typeof e.preview_hex === 'string' ? e.preview_hex.slice(0, 32) : '';
    preview.textContent = (hex.match(/.{1,2}/g) || []).join(' ') +
      (e.preview_truncated ? ' ...' : '') || '(no packet bytes)';
    const td = document.createElement('td'); td.appendChild(preview); row.appendChild(td);
    rows.push(row);
  }
  byId('events').replaceChildren(...rows);
  byId('events-empty').hidden = rows.length !== 0;
  eventSignature = signature;
}

function render(data) {
  const p = data.profile, s = data.scheduler, t = data.totals;
  text('device-name', data.device_name); document.title = data.device_name + ' | Radio stats';
  text('firmware-version', data.firmware_version || '');
  byId('admin-link').hidden = !(data.roles || []).some(r => r.role === 'management');
  text('frequency', (p.frequency_hz / 1000000).toFixed(3) + ' MHz');
  text('modulation', number(p.bandwidth_hz / 1000) + ' kHz / SF' + p.sf + ' / CR 4/' + p.cr + ' / ' + p.tx_power_dbm + ' dBm');
  text('uptime', duration(data.uptime_ms));
  text('wifi', data.wifi.connected ? 'WiFi ' + data.wifi.rssi_dbm + ' dBm / up ' + duration(data.wifi.uptime_ms) : 'WiFi disconnected');
  text('client-count', data.kiss.connected + ' / ' + data.kiss.capacity);
  text('owner', s.owner_slot < 0 ? 'No radio settings owner' : 'Radio settings owner: ' + sourceLabel(data, s.owner_slot));
  text('queue', s.queued + ' / ' + s.capacity);
  text('radio-state', p.fault ? 'Profile fault: TX disabled' : s.transmitting ? 'RF transmission in progress' : s.carrier_wait ? 'Waiting for carrier access' : 'Ready');
  text('profile-state', p.fault ? 'TX disabled' : p.committed ? 'Saved settings' : 'Default settings');
  byId('profile-state').className = 'pill ' + (p.fault ? 'bad' : p.committed ? 'good' : 'warning');
  text('credit', duration(s.credit_ms));
  text('budget-detail', 'of ' + duration(s.maximum_credit_ms) + ' / 1-hour window / airtime factor ' + factor(p.airtime_factor));
  const percent = s.maximum_credit_ms ? Math.min(100, s.credit_ms / s.maximum_credit_ms * 100) : 0;
  byId('budget-fill').style.width = percent + '%';
  byId('budget-meter').setAttribute('aria-valuenow', Math.round(percent));
  text('tx-airtime', duration(t.tx_rf_ms)); text('rx-airtime', 'est. ' + duration(t.rx_estimated_ms));
  text('carrier', (p.cad ? 'CAD on' : 'CAD off') + ' / interference ' + (p.interference_threshold ? p.interference_threshold + ' dB above floor' : 'off'));
  text('heap', 'Memory: ' + number(Math.round(data.memory.free_bytes / 1024)) + ' KiB free / ' + number(Math.round(data.memory.minimum_bytes / 1024)) + ' KiB lowest');
  const dma = [data.memory.dma_free_bytes, data.memory.dma_largest_bytes, data.memory.dma_minimum_bytes];
  text('dma-heap', dma.every(Number.isFinite) ? dma.map(number).join(' / ') + ' B' : 'Unavailable');
  for (const [id, key] of [['rx-count', 'rx_packets'], ['tx-count', 'tx_succeeded'], ['tx-accepted', 'tx_accepted'],
    ['tx-rejected', 'tx_rejected'], ['tx-failed', 'tx_failed'], ['tx-unknown', 'tx_unknown'], ['rx-errors', 'rx_errors']]) text(id, number(t[key]));
  const rows = data.kiss.clients.map(c => {
    const row = document.createElement('tr');
    cell(row, 'KISS ' + (c.slot + 1) + (c.slot === s.owner_slot ? ' / owner' : ''));
    cell(row, c.generation); cell(row, c.negotiated ? 'Queued v1' : 'Legacy');
    cell(row, factor(c.airtime_factor)); cell(row, duration(c.credit_ms)); cell(row, duration(c.rf_ms));
    return row;
  });
  byId('clients').replaceChildren(...rows); byId('clients-empty').hidden = rows.length !== 0;
  const roleRows = (data.roles || []).map(r => {
    const row = document.createElement('tr');
    cell(row, roleNames[r.role] || r.role);
    cell(row, r.name);
    cell(row, (r.ready ? 'Ready' : r.state.replaceAll('-', ' ')) +
      (r.fault ? ' / ' + r.fault : ''),
      r.fault ? 'bad' : r.ready ? 'good' : 'warning');
    const key = document.createElement('code');
    key.className = 'role-key'; key.textContent = r.public_key || 'Unavailable';
    const td = document.createElement('td');
    if (r.public_key) {
      const details = document.createElement('details'), summary = document.createElement('summary');
      summary.textContent = r.public_key.slice(0, 12) + '...';
      summary.setAttribute('aria-label', 'Show full public key for ' + r.name);
      details.appendChild(summary); details.appendChild(key); td.appendChild(details);
    } else td.appendChild(key);
    row.appendChild(td);
    cell(row, r.source_slot === null ? '-' : r.source_slot + ' / #' + r.source_generation);
    return row;
  });
  byId('roles').replaceChildren(...roleRows);
  byId('role-panel').hidden = roleRows.length === 0;
  text('history-detail', 'Latest ' + data.history.capacity + ' events');
  const span = Math.max(1, data.uptime_ms - data.traffic[0].second * 1000);
  text('occupancy', (data.traffic.reduce((n, b) => n + b.tx_rf_ms, 0) / span * 100).toFixed(1) + '% TX');
  chart('packets-chart', data.traffic, ['tx_packets', 'rx_packets'], false);
  chart('airtime-chart', data.traffic, ['tx_rf_ms', 'rx_estimated_ms'], true);
  renderEvents(data);
}

function showDisconnected(message) {
  if (paused || document.hidden) return;
  text('connection', latest ? 'Stale' : 'Connecting');
  byId('connection').className = 'pill warning';
  byId('notice').hidden = false;
  text('notice', message + (latest ? ' Last updated ' + new Date(lastSuccess).toLocaleTimeString() + '.' : ''));
}
function armWatchdog(stream) {
  clearTimeout(watchdog);
  watchdog = setTimeout(() => {
    if (socket !== stream) return;
    showDisconnected('Updates interrupted. Reconnecting...');
    stream.close();
  }, 6000);
}
function connectStream() {
  if (socket || paused || document.hidden) return;
  clearTimeout(retryTimer);
  const scheme = location.protocol === 'https:' ? 'wss:' : 'ws:';
  const stream = new WebSocket(scheme + '//' + location.host + '/api/live');
  socket = stream;
  armWatchdog(stream);
  stream.onmessage = event => {
    if (socket !== stream || paused || document.hidden) return;
    try {
      const data = JSON.parse(event.data);
      if (data.api_version !== 1) throw new Error('Unsupported dashboard API version.');
      render(data); latest = data; lastSuccess = Date.now(); retryDelay = 1000;
      armWatchdog(stream);
      text('connection', 'Live'); byId('connection').className = 'pill good';
      byId('notice').hidden = !data.profile.fault;
      text('notice', data.profile.fault ? 'Radio configuration fault. TX stopped. Check the device log.' : '');
      text('updated', 'Updated ' + new Date(lastSuccess).toLocaleTimeString());
    } catch (error) {
      showDisconnected(error.message);
      stream.close();
    }
  };
  stream.onerror = () => {
    if (socket === stream) {
      showDisconnected('Connection interrupted. Reconnecting...');
      stream.close();
    }
  };
  stream.onclose = () => {
    if (socket !== stream) return;
    socket = null;
    clearTimeout(watchdog);
    if (!paused && !document.hidden) {
      showDisconnected('Connection interrupted. Reconnecting...');
      retryTimer = setTimeout(connectStream, retryDelay + Math.floor(Math.random() * 500));
      retryDelay = Math.min(retryDelay * 2, 10000);
    }
  };
}
function closeStream() {
  clearTimeout(retryTimer);
  clearTimeout(watchdog);
  if (socket) socket.close();
}
byId('pause').addEventListener('click', () => {
  paused = !paused;
  byId('pause').setAttribute('aria-pressed', String(paused));
  text('pause', paused ? 'Resume updates' : 'Pause updates');
  if (paused) {
    closeStream();
    text('connection', 'Paused'); byId('connection').className = 'pill warning';
    byId('notice').hidden = true;
  } else connectStream();
});
text('refresh', 'Reconnect');
byId('refresh').addEventListener('click', () => {
  if (paused || document.hidden) return;
  retryDelay = 1000;
  closeStream();
  connectStream();
});
byId('filter').addEventListener('change', () => { if (latest) renderEvents(latest); });
document.addEventListener('visibilitychange', () => {
  if (document.hidden) {
    closeStream();
    text('connection', 'Paused'); byId('connection').className = 'pill warning';
    byId('notice').hidden = true;
  } else if (!paused) connectStream();
});
window.addEventListener('resize', () => { if (latest) render(latest); });
connectStream();
</script>
</body>
</html>)HTML";
