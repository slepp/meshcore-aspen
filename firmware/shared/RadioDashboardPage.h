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
  --bg: #131b19; --panel: #19231f; --panel-alt: #25332c; --border: #35463b;
  --text: #edf1e8; --muted: #a8b6aa; --tx: #91c6a1; --rx: #b1bfd1;
  --warn: #ffd18a; --bad: #ff9b9b; --focus: #88c6ff;
  --field-header: #ffd18a; --field-route: #c5a7ff; --field-src: #88c6ff;
  --field-dst: #ffb38a; --field-mac: #f5a6d6;
}
* { box-sizing: border-box; }
body { margin: 0; background: var(--bg); color: var(--text); font: 15px/1.5 system-ui, sans-serif; }
main { max-width: 1320px; margin: auto; padding: 30px 24px 48px; }
header, .section-head, .actions, .legend, .key-value { display: flex; align-items: center; gap: 12px; }
header { justify-content: space-between; flex-wrap: wrap; margin-bottom: 24px; }
.brand { display: flex; align-items: center; gap: 14px; min-width: 0; max-width: 100%; }
.mark { color: var(--tx); font: 38px/1 Georgia, serif; }
h1 { font-size: clamp(22px, 3vw, 30px); line-height: 1.15; margin: 0 0 5px; overflow-wrap: anywhere; }
h1 { font-family: Georgia, serif; }
h2 { font-size: 16px; margin: 0; }
p { margin: 0; }
.muted, .eyebrow, small { color: var(--muted); }
.eyebrow { font-size: 12px; letter-spacing: .09em; text-transform: uppercase; }
.pill { display: inline-block; padding: 3px 8px; border-left: 2px solid currentColor; font-size: 12px; white-space: nowrap; }
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
.card { background: var(--panel); border-top: 1px solid var(--border); padding: 18px; min-width: 0; }
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
#events td { vertical-align: top; padding: 6px 8px; font-variant-numeric: tabular-nums; }
#events td:first-child { padding-left: 0; }
.packet-detail { min-width: 24em; max-width: 44em; font-size: 12px; line-height: 1.35; }
.packet-heading { display: block; font-size: 13px; color: var(--text); }
.packet-route { display: block; color: var(--muted); }
.packet-route:last-of-type { display: inline; }
.packet-detail details { display: inline-block; margin: 0 0 0 10px; vertical-align: top; }
.packet-detail details[open] { display: block; margin: 8px 0; }
.packet-detail summary { font-size: 11px; }
.disclosure { padding: 0; border: 0; background: none; font: inherit; color: var(--tx); text-align: left; }
.signal-child { color: var(--muted); }
.signal-child td:first-child { padding-left: 22px; }
.spark { width: 100px; height: 26px; display: block; color: var(--tx); }
.spark polyline { fill: none; stroke: currentColor; stroke-width: 1.5; }
.signal-note { font-size: 12px; max-width: 85ch; margin: 8px 0; }
#topology { width: 100%; height: auto; max-height: 390px; color: var(--muted); }
.topology-edge { stroke: var(--muted); opacity: .45; }
.topology-edge.strong { stroke: var(--tx); opacity: .85; }
.topology-edge.weak { stroke: var(--warn); opacity: .8; }
.topology-node { fill: var(--panel-alt); stroke: var(--tx); stroke-width: 1.5; }
.topology-label { fill: var(--text); font: 16px ui-monospace, monospace; paint-order: stroke; stroke: var(--panel); stroke-width: 4px; }
.packet-bytes { display: block; white-space: normal; min-width: 20em; max-width: 30em; line-height: 1.5; }
.packet-bytes span { display: inline-block; margin-right: .4em; }
.packet-bytes span:last-child { margin-right: 0; }
.field-header { color: var(--field-header); }
.field-route, .rf-rx { color: var(--field-route); }
.field-src { color: var(--field-src); }
.field-dst { color: var(--field-dst); }
.field-mac { color: var(--field-mac); }
.field-payload { color: var(--muted); }
.rf-result { white-space: nowrap; font-weight: 600; font-size: 16px; line-height: 1.1; }
.rf-icon { width: 13px; height: 13px; margin-left: 3px; vertical-align: -1px; fill: none; stroke: currentColor; stroke-width: 1.5; }
.rf-state { font-size: 12px; margin-left: 3px; }
.packet-source { display: block; font-size: 11px; color: var(--muted); }
.packet-legend { display: flex; gap: 12px; flex-wrap: wrap; }
.role-key { white-space: normal; overflow-wrap: anywhere; max-width: 38em; display: block; }
.empty { color: var(--muted); padding: 24px 0; text-align: center; }
.wide { margin-bottom: 16px; }
footer { margin-top: 24px; display: flex; justify-content: space-between; gap: 12px; flex-wrap: wrap; font-size: 12px; color: var(--muted); }
@media (max-width: 850px) { .summary { grid-template-columns: repeat(2, minmax(0, 1fr)); } .two { grid-template-columns: 1fr; } }
@media (max-width: 480px) { main { padding: 20px 12px; } .card { padding: 15px; } .number { font-size: 23px; } .actions { flex-wrap: wrap; } }
@media (prefers-color-scheme: light) {
  :root { --bg: #f3f1e9; --panel: #faf9f4; --panel-alt: #e7ece3; --border: #cbd3c7; --text: #26362e;
    --muted: #59685e; --tx: #276746; --rx: #4d6382; --warn: #926000; --bad: #b33232; --focus: #176eb5; }
  :root { --field-header: #926000; --field-route: #6841a5; --field-src: #176eb5; --field-dst: #a34215; --field-mac: #9a2973; }
}
</style>
</head>
<body>
<main>
  <header>
    <div class="brand"><div class="mark" aria-hidden="true">A</div><div>
      <h1 id="device-name">MeshCore Radio</h1><p class="eyebrow">Aspen / Radio observatory</p>
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
  <nav class="legend wide" aria-label="Dashboard sections"><a href="#packet-report-panel">Repeater signal</a><a href="#topology-panel">Mesh paths</a><a href="#activity-panel">Packet decoder</a><a href="#connections">Connections</a></nav>

  <section class="grid summary" aria-label="Radio overview">
    <article class="card"><p class="eyebrow">Radio profile</p><p id="frequency" class="number">-</p><p id="modulation" class="muted">Awaiting profile</p></article>
    <article class="card"><p class="eyebrow">Device</p><p id="uptime" class="number">-</p><p id="wifi" class="muted">WiFi status unavailable</p>
      <p id="heap" class="muted">Memory status unavailable</p>
      <details><summary>DMA memory</summary><small>Free / largest block / lowest free since boot</small><p id="dma-heap">-</p></details></article>
    <article class="card"><p class="eyebrow">KISS clients</p><p id="client-count" class="number">-</p><p id="owner" class="muted">-</p>
      <div id="companion-overview" hidden><p class="eyebrow">Companion clients</p><p id="companion-count" class="number">-</p><p id="companion-status" role="status">-</p></div></article>
    <article class="card"><p class="eyebrow">Transmit queue</p><p id="queue" class="number">-</p><p id="radio-state" class="muted">Waiting for status</p></article>
  </section>

  <section id="packet-report-panel" class="card wide" hidden>
    <div class="section-head"><h2>Repeater signal</h2><span class="muted">Heard over RF</span></div>
    <div id="packet-reports"></div>
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
      <p class="muted chart-note">Sent counts transmissions completed by this radio.</p>
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
  <section id="topology-panel" class="card wide">
    <div class="section-head"><h2>Mesh paths</h2><span id="topology-status" class="muted"></span></div>
    <div class="scroll" tabindex="0" aria-label="Mesh path diagram"><svg id="topology" style="min-width:700px" viewBox="0 0 900 340" role="img" aria-label="Observed repeater paths"></svg></div>
    <p class="signal-note muted">Arrows follow received flood paths toward this radio. Larger nodes and thicker links appear more often in the recent packet window. Green / amber links show last-hop mean SNR above / below 0 dB; grey links have no measured signal. Prefixes share their first byte.</p>
    <details><summary>Observed links</summary><div class="scroll"><table><thead><tr><th>From</th><th>To</th><th>Observations</th><th>Last-hop SNR</th></tr></thead><tbody id="topology-links"></tbody></table></div></details>
  </section>
  <section id="companion-panel" class="card wide" hidden>
    <div class="section-head"><h2>Connected companions</h2><span id="companion-endpoint" class="muted"></span></div>
    <p id="companion-detail" class="muted"></p>
    <div class="scroll" tabindex="0" aria-label="Connected companion clients">
      <table><thead><tr><th>Client</th><th>Address</th><th>App name</th><th>Session</th><th>Protocol</th><th>Status</th></tr></thead><tbody id="companion-clients"></tbody></table>
    </div>
    <p id="companion-empty" class="empty">No connected companions.</p>
  </section>
  <section id="connections" class="card wide">
    <div class="section-head"><h2>Connected KISS clients</h2></div>
    <div class="scroll" tabindex="0" aria-label="Connected KISS clients">
      <table><thead><tr><th>Client</th><th>Session</th><th>Protocol</th><th>Airtime factor</th><th>TX allowance</th><th>TX airtime</th></tr></thead><tbody id="clients"></tbody></table>
    </div>
    <p id="clients-empty" class="empty">No connected KISS clients.</p>
  </section>
  <section id="activity-panel" class="card">
    <div class="section-head"><div><h2>Recent activity</h2><p id="history-detail" class="muted">-</p></div>
      <label class="muted">Show <select id="filter"><option value="all">All events</option><option value="rx">RX only</option><option value="tx">TX only</option></select></label>
    </div>
    <div class="scroll" tabindex="0" aria-label="Recent packet events">
      <table><thead><tr><th>Age</th><th>RF</th><th>Bytes / source</th><th>Queue</th><th>Air</th><th title="RSSI (dBm) / SNR (dB)">RSSI / SNR</th><th>Decoded packet</th></tr></thead><tbody id="events"></tbody></table>
    </div>
    <p id="events-empty" class="empty">No matching events yet.</p>
    <details class="chart-note"><summary>Decode key</summary>
      <p id="contact-detail" class="muted">Companion contacts unavailable.</p>
      <p class="packet-legend"><span class="field-header">Header / transport</span><span class="field-route">Path / route</span><span class="field-src">Source / key</span><span class="field-dst">Destination / channel</span><span class="field-mac">MAC</span><span class="field-payload">Payload</span></p>
      <p class="muted">↑ TX · ↓ RX · × failed/rejected · ? unconfirmed · … queued. The radio-wave icon marks activity on the radio.</p>
      <p class="muted">Air: ~ estimated RX time. RSSI / SNR: dBm / dB. Hover an outcome, source or byte group for details.</p>
      <p class="muted">Names are unverified contact matches based on public-key prefixes. ? marks a candidate from an incomplete contact list; [N matches] marks a collision. Path = traversed hops; Route = remaining hops. Hex previews show up to 16 bytes, followed by ... when more bytes remain.</p>
    </details>
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
      identities.get(key).add(contact.name);
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
  if (matches.length > 1) return prefix + ' [' + matches.length + ' matches]';
  if (matches.length === 1 && matches[0][1].size === 1)
    return prefix + ' (' + [...matches[0][1]][0] +
      (identities.incomplete && prefix.length < 64 ? '?' : '') + ')';
  return prefix;
}
function decodePacket(event, identities, fields = []) {
  const hex = event.preview_hex;
  const length = event.length;
  if (typeof hex !== 'string' || hex.length > 32 || !/^(?:[0-9a-f]{2})*$/i.test(hex) ||
      !Number.isInteger(length) || length < 0 || length > 255 || hex.length / 2 > length)
    return 'Malformed packet preview';
  if (!hex.length) return length ? 'Packet preview unavailable' : 'No packet bytes';
  const raw = (hex.match(/../g) || []).map(byte => parseInt(byte, 16));
  const mark = (start, count, kind, label) => {
    const end = Math.min(start + count, raw.length);
    if (end > start) fields.push({start, end, kind, label});
  };
  const asHex = (start, count) => hex.slice(start * 2, (start + count) * 2).toLowerCase();
  const route = raw[0] & 3, type = raw[0] >> 2 & 15, version = raw[0] >> 6;
  const parts = [[packetTypes[type] || (type === 15 ? 'Custom' : 'Unknown type ' + type), packetRoutes[route], 'v' + version].join(' / ')];
  const finish = message => [...parts, message].filter(Boolean).join('\n');
  mark(0, 1, 'header', parts[0]);
  if (version !== 0) return finish('Unsupported payload version');
  const available = end => end <= raw.length;
  const missing = field => finish((raw.length < length ? 'Preview missing ' : 'Malformed packet: missing ') + field);
  let offset = route === 0 || route === 3 ? 5 : 1;
  if (length < offset + 1) return finish('Malformed packet: missing route header');
  if (!available(offset + 1)) return missing('route header');
  if (offset === 5) {
    const code = at => (raw[at] | raw[at + 1] << 8).toString(16).padStart(4, '0');
    parts[0] += ' / transport 0x' + code(1) + ', 0x' + code(3);
    mark(1, 2, 'header', 'Transport code 0x' + code(1));
    mark(3, 2, 'header', 'Transport code 0x' + code(3));
  }
  const packed = raw[offset++], width = (packed >> 6) + 1, count = packed & 63;
  const pathBytes = width * count, payloadAt = offset + pathBytes;
  if (width === 4 || pathBytes > 64) return finish('Malformed packet: invalid path width/count');
  if (payloadAt >= length || length - payloadAt > 184) return finish('Malformed packet: invalid path/payload length');
  mark(offset - 1, 1, 'route', 'Path header: ' + count + ' × ' + width + ' B');
  const trace = type === 9 && (route === 2 || route === 3);
  const hops = [];
  if (trace) mark(offset, pathBytes, 'route', 'Trace signal bytes');
  else for (let i = 0; i < count; i++)
    mark(offset + i * width, width, 'route', 'Hop ' + (i + 1) + ' prefix');
  for (let i = 0; i < count && available(offset + (i + 1) * width); i++)
    hops.push(trace ? asHex(offset + i * width, width) : prefixLabel(asHex(offset + i * width, width), identities));
  parts.push((trace ? 'Signal ' + pathBytes + ' B' :
    (route === 0 || route === 1 ? 'Path ' : 'Route ') + count + ' × ' + width + ' B') +
    (hops.length ? ': ' + hops.join(' → ') : ''));
  if (!available(payloadAt)) return missing('path bytes');
  const payloadLength = length - payloadAt;
  const prefix = at => prefixLabel(asHex(at, 1), identities);
  if ([0, 1, 2, 8].includes(type)) {
    if (payloadLength < 4) return finish('Malformed packet: short private envelope');
    mark(payloadAt, 1, 'dst', 'Destination ' + prefix(payloadAt));
    mark(payloadAt + 1, 1, 'src', 'Source ' + prefix(payloadAt + 1));
    if (!available(payloadAt + 2)) return missing('source/destination prefixes');
    mark(payloadAt + 2, 2, 'mac', 'Message authentication code');
    mark(payloadAt + 4, payloadLength - 4, 'payload', 'Encrypted payload');
    parts[0] += ' / encrypted';
    parts.push('src ' + prefix(payloadAt + 1) + ' → dst ' + prefix(payloadAt));
  } else if (type === 7) {
    if (payloadLength < 35) return finish('Malformed packet: short anonymous envelope');
    if (!available(payloadAt + 1)) return missing('destination prefix');
    mark(payloadAt, 1, 'dst', 'Destination ' + prefix(payloadAt));
    mark(payloadAt + 1, 32, 'src', 'Sender public key');
    mark(payloadAt + 33, 2, 'mac', 'Message authentication code');
    mark(payloadAt + 35, payloadLength - 35, 'payload', 'Encrypted payload');
    parts[0] += ' / encrypted';
    parts.push('src anonymous → dst ' + prefix(payloadAt));
  } else if (type === 4) {
    if (payloadLength < 100) return finish('Malformed packet: short advert');
    if (!available(payloadAt + 1)) return missing('advert source prefix');
    const visible = Math.min(32, raw.length - payloadAt);
    mark(payloadAt, visible, 'src', 'Advert public key');
    parts.push('key ' + prefixLabel(asHex(payloadAt, visible), identities));
  } else if (type === 5 || type === 6) {
    if (payloadLength < 3) return finish('Malformed packet: short group envelope');
    if (!available(payloadAt + 1)) return missing('channel hash');
    mark(payloadAt, 1, 'dst', 'Channel hash');
    mark(payloadAt + 1, 2, 'mac', 'Message authentication code');
    mark(payloadAt + 3, payloadLength - 3, 'payload', 'Encrypted payload');
    parts[0] += ' / encrypted';
    parts.push('channel ' + asHex(payloadAt, 1));
  } else if (type === 3) {
    if (payloadLength !== 4) return finish('Malformed packet: ACK needs 4 bytes');
    mark(payloadAt, 4, 'payload', 'ACK reference');
    if (!available(payloadAt + 4)) return missing('ACK reference');
    parts.push('ref ' + asHex(payloadAt, 4));
  } else if (type === 9 && payloadLength < 9) {
    return finish('Malformed packet: short trace');
  } else {
    mark(payloadAt, payloadLength, 'payload', 'Payload');
  }
  return finish();
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
function rfResult(row, event, label) {
  const rx = event.direction === 'rx', tx = event.direction === 'tx';
  const td = cell(row, rx ? '↓' : tx ? '↑' : event.direction === 'local' ? '↔' : '?',
    'rf-result ' + (rx ? 'rf-rx' : tx && event.state === 2 ? 'good' :
      tx && (event.state === 0 || event.state === 3) ? 'bad' : 'warning'));
  td.title = label;
  td.setAttribute('aria-label', label);
  if (rx || (tx && (event.state === 2 || event.rf_ms > 0))) {
    const icon = document.createElementNS('http://www.w3.org/2000/svg', 'svg');
    icon.setAttribute('viewBox', '0 0 16 16');
    icon.setAttribute('class', 'rf-icon');
    icon.setAttribute('aria-hidden', 'true');
    const path = document.createElementNS('http://www.w3.org/2000/svg', 'path');
    path.setAttribute('d', 'M2 6a8.5 8.5 0 0 1 12 0M4.5 8.5a5 5 0 0 1 7 0M7 11a1.5 1.5 0 0 1 2 0M8 13v1');
    icon.appendChild(path);
    td.appendChild(icon);
  }
  if (tx && event.state !== 2) {
    const state = document.createElement('span');
    state.className = 'rf-state';
    state.textContent = event.state === 0 || event.state === 3 ? '×' : event.state === 1 ? '…' : '?';
    td.appendChild(state);
  }
}
function packetBytes(event, fields) {
  const code = document.createElement('code');
  code.className = 'packet-bytes';
  const hex = typeof event.preview_hex === 'string' ? event.preview_hex.slice(0, 32) : '';
  const bytes = hex.match(/.{1,2}/g) || [];
  for (let at = 0; at < bytes.length;) {
    const field = fields.find(f => f.start <= at && at < f.end);
    const end = field ? field.end : at + 1;
    const span = document.createElement('span');
    span.className = field ? 'field-' + field.kind : '';
    span.title = field ? field.label : 'Undecoded byte';
    span.textContent = bytes.slice(at, end).join(' ') + (end < bytes.length ? ' ' : '');
    code.appendChild(span);
    at = end;
  }
  if (event.preview_truncated || !bytes.length) {
    const tail = document.createElement('span');
    tail.textContent = bytes.length ? ' ...' : '(no packet bytes)';
    code.appendChild(tail);
  }
  return code;
}
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
const openPacketBytes = new Set();
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
    (identities.incomplete ? ' (incomplete).' : '.') :
    'Companion contacts unavailable; using on-device names.');
  const rows = [];
  const retainedBytes = new Set();
  eventAges = [];
  for (const e of data.history.events) {
    if (filter !== 'all' && e.direction !== filter) continue;
    const row = document.createElement('tr');
    eventAges.push([cell(row, activityAge(data.uptime_ms - e.at_ms)), e.at_ms]);
    const label = e.direction === 'rx' ? 'Received over RF' : e.direction === 'local' ? 'Local reflection' :
      e.direction !== 'tx' ? 'Unknown direction' : (states[e.state] || 'Other') +
      (e.reason ? ' / ' + (reasons[e.reason] || 'reason ' + e.reason) : '');
    rfResult(row, e, label);
    const size = cell(row, e.length + ' B');
    if (e.direction === 'tx') {
      const source = document.createElement('span');
      source.className = 'packet-source';
      source.textContent = sourceLabel(data, e.source_slot, e.source_generation);
      source.title = 'Source ' + e.source_slot + ' / session ' + e.source_generation + ' / job ' + e.job_id;
      size.appendChild(source);
    }
    cell(row, e.direction === 'rx' ? '-' : duration(e.queue_ms));
    const airtime = cell(row, e.direction === 'rx' ? '~' + duration(e.estimated_ms) : duration(e.rf_ms));
    airtime.title = e.direction === 'rx' ? 'Estimated RX airtime' : 'Observed TX airtime';
    const signal = e.direction === 'rx' && Number.isFinite(e.rssi_dbm) && Number.isFinite(e.snr_db) &&
      !(e.rssi_dbm === 127 && e.snr_db === -32);
    const rssi = cell(row, signal ? number(e.rssi_dbm) + ' / ' + number(e.snr_db) : '-');
    rssi.title = 'RSSI (dBm) / SNR (dB)';
    const fields = [];
    const decoded = decodePacket(e, identities, fields).split('\n');
    const td = cell(row, '', 'packet-detail');
    decoded.forEach((line, i) => {
      const part = document.createElement(i ? 'span' : 'strong');
      part.className = i ? 'packet-route' : 'packet-heading';
      part.textContent = line; td.appendChild(part);
    });
    const bytes = document.createElement('details'), summary = document.createElement('summary');
    const packetKey = e.sequence + ':' + e.at_ms;
    retainedBytes.add(packetKey); bytes.open = openPacketBytes.has(packetKey);
    bytes.ontoggle = () => { if (bytes.open) openPacketBytes.add(packetKey); else openPacketBytes.delete(packetKey); };
    summary.textContent = 'Bytes · ' + Math.min(e.length, 16) + ' of ' + e.length;
    bytes.appendChild(summary); bytes.appendChild(packetBytes(e, fields)); td.appendChild(bytes);
    rows.push(row);
  }
  byId('events').replaceChildren(...rows);
  byId('events-empty').hidden = rows.length !== 0;
  eventSignature = signature;
  for (const key of openPacketBytes) if (!retainedBytes.has(key)) openPacketBytes.delete(key);
}

const expandedSignals = new Set(), signalHistory = new Map();
let signalUptime = 0, signalSignature = '', signalAges = [];
function signalAge(v, data) {
  return v.last_seen_utc > 0 ? activityAge(Date.now() - v.last_seen_utc * 1000) :
    Number.isFinite(v.last_seen_ms) ? activityAge(((data.uptime_ms % 4294967296) - v.last_seen_ms + 4294967296) % 4294967296) : '—';
}
function signalGroups(report) {
  const groups = new Map();
  for (const item of report.rows) {
    const label = /^(?:[0-9a-f]{2}){1,3}$/i.test(item.label) ? item.label.slice(0, 2).toUpperCase() : item.label;
    if (!groups.has(label)) groups.set(label, []);
    groups.get(label).push(item);
  }
  return [...groups].map(([label, rows]) => {
    const values = rows.map(row => Object.fromEntries(report.fields.map((name, i) => [name, row.values[i]])));
    const sum = key => values.reduce((n, v) => n + (Number.isFinite(v[key]) ? v[key] : 0), 0);
    const mean = key => {
      const valid = values.filter(v => v.samples > 0 && Number.isFinite(v[key]));
      const weight = valid.reduce((n, v) => n + v.samples, 0);
      return weight ? valid.reduce((n, v) => n + v[key] * v.samples, 0) / weight : NaN;
    };
    const p95 = values.filter(v => v.samples > 0).map(v => v.p95_db).filter(Number.isFinite);
    return {label, rows, samples: sum('samples'), window: sum('window'), heard_total: sum('heard_total'),
      mean_db: mean('mean_db'), rssi_mean_dbm: mean('rssi_mean_dbm'),
      p95: p95.length ? [Math.min(...p95), Math.max(...p95)] : [],
      last_seen_utc: Math.max(0, ...values.map(v => v.last_seen_utc || 0))};
  }).sort((a, b) => b.heard_total - a.heard_total);
}
function sparkline(key, value, at, revision) {
  let series = signalHistory.get(key) || [];
  if (Number.isFinite(value) && Number.isFinite(at) && at > 0 &&
      (!series.length || series[series.length - 1].revision !== revision)) {
    if (series.length && at < series[series.length - 1].at) series = [];
    series.push({value, at, revision});
    series = series.slice(-60); signalHistory.set(key, series);
  }
  const svg = document.createElementNS('http://www.w3.org/2000/svg', 'svg');
  svg.setAttribute('viewBox', '0 0 100 26'); svg.setAttribute('class', 'spark');
  svg.setAttribute('role', 'img');
  svg.setAttribute('aria-label', series.length < 2 ? 'Waiting for another signal observation' :
    'Mean SNR while this page is open: ' + number(series[0].value) + ' to ' + number(series[series.length - 1].value) + ' dB');
  if (series.length > 1) {
    const min = Math.min(...series.map(p => p.value)) - .5, max = Math.max(...series.map(p => p.value)) + .5;
    const span = Math.max(1, series[series.length - 1].at - series[0].at);
    const path = document.createElementNS('http://www.w3.org/2000/svg', 'polyline');
    path.setAttribute('points', series.map(p =>
      (2 + 96 * (p.at - series[0].at) / span).toFixed(1) + ',' +
      (24 - 22 * (p.value - min) / (max - min)).toFixed(1)).join(' '));
    svg.appendChild(path);
  }
  return svg;
}
function renderPacketReports(data) {
  if (data.uptime_ms < signalUptime) { signalHistory.clear(); signalSignature = ''; }
  signalUptime = data.uptime_ms;
  const signature = JSON.stringify([data.packet_reports, [...expandedSignals]]);
  if (signature === signalSignature) {
    for (const [td, v] of signalAges) td.textContent = signalAge(v, data);
    return;
  }
  signalSignature = signature; signalAges = [];
  const focused = document.activeElement?.id;
  const retained = new Set();
  const sections = (data.packet_reports || []).map(report => {
    const section = document.createElement('section');
    const snr = report.name === 'repeater_snr';
    const title = document.createElement('h3');
    title.textContent = snr ? 'Nearby repeaters' : report.name;
    section.appendChild(title);
    const detail = document.createElement('p'); detail.className = 'muted';
    detail.textContent = snr ?
      'Last repeater hops, grouped by first byte. Expand a prefix to see its individual signal windows.' : 'Packet slot ' + report.slot;
    section.appendChild(detail);
    const scroll = document.createElement('div'); scroll.className = 'scroll'; scroll.setAttribute('tabindex', '0');
    const table = document.createElement('table');
    const head = document.createElement('thead'), heading = document.createElement('tr');
    const columns = snr ? ['Hop', 'Samples', 'Heard', 'Mean SNR', 'P95 by prefix', 'Mean RSSI', 'SNR trend', 'Last heard'] : ['Label', ...report.fields];
    for (const name of columns) { const th = document.createElement('th'); th.textContent = name; heading.appendChild(th); }
    head.appendChild(heading); table.appendChild(head);
    const body = document.createElement('tbody');
    const signalRow = (item, v, key, child = false) => {
      const row = document.createElement('tr'); if (child) row.className = 'signal-child';
      const label = cell(row, item.label);
      if (item.rows && item.rows.length > 1) {
        const toggle = document.createElement('button'); toggle.className = 'disclosure'; toggle.type = 'button';
        toggle.id = 'signal-' + key;
        toggle.textContent = (expandedSignals.has(key) ? '▾ ' : '▸ ') + item.label + ' · ' + item.rows.length;
        toggle.setAttribute('aria-expanded', String(expandedSignals.has(key)));
        toggle.onclick = () => { if (expandedSignals.has(key)) expandedSignals.delete(key); else expandedSignals.add(key); renderPacketReports(data); };
        label.textContent = ''; label.appendChild(toggle);
      }
      const metric = (value, unit) => Number.isFinite(value) ? number(Math.round(value * 100) / 100) + unit : '—';
      cell(row, number(v.samples) + ' / ' + number(v.window)); cell(row, number(v.heard_total));
      cell(row, metric(v.mean_db, ' dB'));
      cell(row, item.p95 ? item.p95.map(p => metric(p, '')).filter((p, i, a) => i === 0 || p !== a[0]).join('…') + (item.p95.length ? ' dB' : '—') : metric(v.p95_db, ' dB'));
      cell(row, metric(v.rssi_mean_dbm, ' dBm'));
      retained.add(key);
      cell(row, '').appendChild(sparkline(key, v.mean_db, v.last_seen_utc,
        JSON.stringify(item.rows ? item.rows.map(r => r.values) : item.values)));
      signalAges.push([cell(row, signalAge(v, data)), v]);
      body.appendChild(row);
    };
    if (snr) for (const group of signalGroups(report)) {
      const key = report.slot + ':' + group.label;
      // Record every child on updates, so expanding does not start its trend from scratch.
      for (const item of group.rows) {
        const v = Object.fromEntries(report.fields.map((name, i) => [name, item.values[i]]));
        const childKey = key + ':' + item.label; retained.add(childKey);
        sparkline(childKey, v.mean_db, v.last_seen_utc, JSON.stringify(item.values));
      }
      signalRow(group, group, key);
      if (expandedSignals.has(key) && group.rows.length > 1) for (const item of group.rows)
        signalRow(item, Object.fromEntries(report.fields.map((name, i) => [name, item.values[i]])), key + ':' + item.label, true);
    } else for (const item of report.rows) {
      const row = document.createElement('tr'); cell(row, item.label);
      for (const value of item.values) cell(row, number(value)); body.appendChild(row);
    }
    table.appendChild(body); scroll.appendChild(table); section.appendChild(scroll);
    if (snr) {
      const note = document.createElement('p'); note.className = 'muted chart-note';
      note.textContent = 'Expand a prefix to see its individual signal histories. Group means use sample counts as weights; grouped P95 shows the range across those histories. Sparklines show the last 60 mean SNR observations collected while this page is open. This table tracks repeater hops; a prefix may match several nodes.';
      section.appendChild(note);
    }
    return section;
  });
  byId('packet-reports').replaceChildren(...sections);
  byId('packet-report-panel').hidden = sections.length === 0;
  if (focused?.startsWith('signal-')) byId(focused)?.focus();
  for (const key of signalHistory.keys()) if (!retained.has(key)) signalHistory.delete(key);
  for (const key of expandedSignals) if (!retained.has(key)) expandedSignals.delete(key);
}

function observedTopology(events) {
  const nodes = new Map(), edges = new Map();
  let paths = 0;
  for (const event of events) {
    if (event.direction !== 'rx' || typeof event.preview_hex !== 'string' ||
        !/^(?:[0-9a-f]{2}){2,16}$/i.test(event.preview_hex)) continue;
    const raw = event.preview_hex.match(/../g).map(byte => parseInt(byte, 16));
    const route = raw[0] & 3, offset = route === 0 ? 5 : 1;
    if ((route !== 0 && route !== 1) || raw[0] >> 6 || raw.length <= offset) continue;
    const width = (raw[offset] >> 6) + 1, count = raw[offset] & 63, end = offset + 1 + width * count;
    if (width > 3 || !count || width * count > 64 || end > raw.length || end >= event.length) continue;
    const path = [];
    for (let i = 0; i < count; i++) {
      const prefix = event.preview_hex.slice((offset + 1 + i * width) * 2, (offset + 1 + (i + 1) * width) * 2).toUpperCase();
      const label = prefix.slice(0, 2);
      if (!nodes.has(label)) nodes.set(label, {label, prefixes: new Set(), count: 0, distance: 0});
      const node = nodes.get(label); node.prefixes.add(prefix); node.count++; node.distance += count - i;
      path.push(label);
    }
    path.push('radio'); paths++;
    for (let i = 1; i < path.length; i++) {
      if (path[i - 1] === path[i]) continue;
      const key = path[i - 1] + ':' + path[i];
      if (!edges.has(key)) edges.set(key, {from: path[i - 1], to: path[i], count: 0, signal: 0, measured: 0});
      const edge = edges.get(key); edge.count++;
      if (i === path.length - 1 && Number.isFinite(event.snr_db) && Number.isFinite(event.rssi_dbm) &&
          !(event.snr_db === -32 && event.rssi_dbm === 127)) {
        edge.signal += event.snr_db; edge.measured++;
      }
    }
  }
  return {nodes: [...nodes.values()], edges: [...edges.values()], paths};
}
let topologySignature = '';
function renderTopology(data) {
  const signature = JSON.stringify(data.history.events);
  if (signature === topologySignature) return;
  topologySignature = signature;
  const graph = observedTopology(data.history.events), svg = byId('topology');
  svg.replaceChildren();
  text('topology-status', graph.paths + ' flood paths / latest ' + data.history.events.length + ' events');
  svg.setAttribute('aria-label', graph.paths ? 'Observed mesh: ' + graph.nodes.length + ' prefixes, ' + graph.edges.length + ' directed links' : 'Waiting for complete received flood paths');
  svg.style.display = graph.paths ? '' : 'none';
  const draw = (tag, attributes, content) => {
    const element = document.createElementNS('http://www.w3.org/2000/svg', tag);
    for (const [key, value] of Object.entries(attributes)) element.setAttribute(key, value);
    if (content !== undefined) element.textContent = content;
    return element;
  };
  const defs = draw('defs', {}), marker = draw('marker', {id: 'path-arrow', viewBox: '0 0 10 10', refX: 10, refY: 5, markerWidth: 5, markerHeight: 5, orient: 'auto-start-reverse'});
  marker.appendChild(draw('path', {d: 'M0 0L10 5L0 10z', fill: 'currentColor'})); defs.appendChild(marker); svg.appendChild(defs);
  const positions = new Map([['radio', {x: 450, y: 170, radius: 13}]]);
  const shown = graph.nodes.sort((a, b) => b.count - a.count).slice(0, 24).sort((a, b) => a.label.localeCompare(b.label));
  svg.setAttribute('aria-label', graph.paths ? 'Observed mesh: showing ' + shown.length + ' of ' + graph.nodes.length + ' prefixes; all links listed below' : 'Waiting for complete received flood paths');
  if (shown.length < graph.nodes.length) text('topology-status', graph.paths + ' flood paths / showing ' + shown.length + ' of ' + graph.nodes.length + ' prefixes');
  shown.forEach((node, i) => {
    const angle = -Math.PI / 2 + i * 2 * Math.PI / shown.length;
    const distance = Math.min(310, 110 + 65 * (node.distance / node.count - 1));
    positions.set(node.label, {x: 450 + Math.cos(angle) * distance, y: 170 + Math.sin(angle) * distance * .43,
      radius: 5 + Math.min(11, Math.sqrt(node.count) * 2)});
  });
  const rows = [];
  for (const edge of graph.edges.sort((a, b) => b.count - a.count)) {
    const from = positions.get(edge.from), to = positions.get(edge.to);
    const snr = edge.measured ? edge.signal / edge.measured : null;
    const row = document.createElement('tr'); cell(row, edge.from); cell(row, edge.to === 'radio' ? 'This radio' : edge.to);
    cell(row, edge.count); cell(row, snr === null ? '—' : number(Math.round(snr * 100) / 100) + ' dB'); rows.push(row);
    if (!from || !to) continue;
    const dx = to.x - from.x, dy = to.y - from.y, distance = Math.max(1, Math.hypot(dx, dy));
    const line = draw('line', {x1: from.x + dx / distance * from.radius, y1: from.y + dy / distance * from.radius,
      x2: to.x - dx / distance * (to.radius + 4), y2: to.y - dy / distance * (to.radius + 4),
      'stroke-width': 1 + Math.min(4, Math.sqrt(edge.count)), 'marker-end': 'url(#path-arrow)',
      class: 'topology-edge' + (snr === null ? '' : snr >= 0 ? ' strong' : ' weak')});
    line.appendChild(draw('title', {}, edge.from + ' → ' + (edge.to === 'radio' ? data.device_name : edge.to) + ': ' + edge.count + ' observations' + (snr === null ? '' : ', mean SNR ' + number(snr) + ' dB')));
    svg.appendChild(line);
  }
  for (const node of [...shown, {label: 'radio', prefixes: new Set()}]) {
    const p = positions.get(node.label), group = draw('g', {});
    group.appendChild(draw('circle', {cx: p.x, cy: p.y, r: p.radius, class: 'topology-node'}));
    group.appendChild(draw('text', {x: p.x, y: p.y + p.radius + 16, 'text-anchor': 'middle', class: 'topology-label'}, node.label === 'radio' ? 'This radio' : node.label));
    group.appendChild(draw('title', {}, node.label === 'radio' ? data.device_name : [...node.prefixes].join(', ') + ' · ' + node.count + ' appearances'));
    svg.appendChild(group);
  }
  byId('topology-links').replaceChildren(...rows);
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
  renderPacketReports(data);
  renderTopology(data);
  const companion = data.companion;
  byId('companion-overview').hidden = byId('companion-panel').hidden = !companion;
  const companionRows = [];
  if (companion) {
    const full = companion.capacity > 0 && companion.connected >= companion.capacity;
    text('companion-count', companion.connected + ' / ' + companion.capacity);
    byId('companion-count').className = 'number ' + (full ? 'warning' : '');
    text('companion-status', !companion.listening ? 'Companion connections unavailable.' : full ?
      'Full: new connections are rejected.' :
      (companion.capacity - companion.connected) + ' connection ' +
        (companion.capacity - companion.connected === 1 ? 'slot' : 'slots') + ' available.');
    byId('companion-status').className = !companion.listening || full ? 'warning' : 'muted';
    text('companion-endpoint', companion.port ? 'TCP port ' + companion.port : 'Listener stopped');
    text('companion-detail', (full ? 'Disconnect another companion client to connect. ' : '') +
      number(companion.rejected) + ' connections rejected since boot. App names are supplied by clients.');
    for (const c of companion.clients) {
      const row = document.createElement('tr');
      cell(row, 'Companion ' + (c.slot + 1));
      cell(row, c.address + ':' + c.port);
      cell(row, c.app_name || 'Not supplied');
      cell(row, c.generation);
      cell(row, c.protocol_version ? 'v' + c.protocol_version : 'Not supplied');
      cell(row, c.app_started ? 'Connected' : 'Waiting for app');
      companionRows.push(row);
    }
  }
  byId('companion-clients').replaceChildren(...companionRows);
  byId('companion-empty').hidden = companionRows.length !== 0;
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
