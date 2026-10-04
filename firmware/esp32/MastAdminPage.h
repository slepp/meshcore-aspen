// SPDX-License-Identifier: Apache-2.0
#pragma once

// Served only at /admin; all device/source text is rendered as text, not HTML.
static const char MastAdminPage[] = R"html(<!doctype html>
<html lang="en"><head>
<meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1">
<meta name="color-scheme" content="dark light"><title>MeshCore mast administration</title>
<style>
:root{--bg:#0b1220;--panel:#131e30;--border:#2a3b53;--text:#edf4ff;--muted:#a6b7ce;--ok:#5ee0bd;--warn:#ffd18a;--bad:#ff9b9b}
*{box-sizing:border-box}body{margin:0;background:var(--bg);color:var(--text);font:15px/1.5 system-ui,sans-serif}
main{max-width:1120px;margin:auto;padding:24px 18px 48px}h1{margin:0;font-size:28px}h2{font-size:20px;margin:0 0 12px}h3{font-size:16px}
a{color:var(--ok)}p{margin:10px 0}.muted,small{color:var(--muted)}.warning{color:var(--warn)}.error{color:var(--bad)}.success{color:var(--ok)}
header,.actions{display:flex;gap:10px;align-items:center;flex-wrap:wrap}header{justify-content:space-between}
section,.notice{background:var(--panel);border:1px solid var(--border);border-radius:12px;padding:18px;margin:18px 0}
nav{display:flex;gap:16px;flex-wrap:wrap;margin:20px 0}fieldset{border:0;padding:0;margin:0;min-width:0}
button,input,select,textarea{font:inherit;color:var(--text);background:var(--bg);border:1px solid var(--border);border-radius:6px;padding:7px 10px}
button{cursor:pointer}button:disabled{opacity:.5;cursor:not-allowed}label{display:block;margin:10px 0}
label input:not([type=checkbox]),label select{display:block;width:100%;margin-top:4px}input[type=checkbox]{margin-right:8px}
textarea{width:100%;min-height:300px;font:14px/1.5 ui-monospace,monospace;tab-size:2}
.grid{display:grid;grid-template-columns:repeat(2,minmax(0,1fr));gap:18px}.fields{display:grid;grid-template-columns:repeat(3,minmax(0,1fr));gap:12px}
pre{white-space:pre-wrap;overflow-wrap:anywhere;margin:12px 0;font:13px/1.5 ui-monospace,monospace}.scroll{overflow-x:auto}
table{width:100%;border-collapse:collapse;text-align:left}td,th{padding:8px;border-bottom:1px solid var(--border)}th{color:var(--muted)}
code{overflow-wrap:anywhere}details{margin:14px 0}summary{cursor:pointer}#session{font-size:13px}#result{min-height:2em;max-height:240px;overflow:auto}
nav{position:sticky;top:0;background:var(--bg);padding:12px 0;z-index:1}section{scroll-margin-top:72px}
:focus-visible{outline:2px solid #88c6ff;outline-offset:3px}[hidden]{display:none!important}
@media(max-width:680px){.grid,.fields{grid-template-columns:1fr}main{padding:18px 12px}section{padding:14px}}
@media(prefers-color-scheme:light){:root{--bg:#f2f5fa;--panel:#fff;--border:#d3ddeb;--text:#18283e;--muted:#52657e;--ok:#007b62;--warn:#926000;--bad:#b33232}}
</style></head><body><main>
<header><div><h1>Mast administration</h1><p class="muted">Configure roles, radio settings and bot programs</p></div><a href="/">Radio stats</a></header>
<p class="notice warning">This connection is unencrypted. Use a trusted LAN or an encrypted tunnel.</p>
<section aria-label="Admin login"><form id="login-form" class="actions">
<label>Admin password <input id="password" type="password" maxlength="15" autocomplete="current-password" required></label>
<button id="login" type="submit">Log in</button><button id="logout" type="button" disabled>Log out</button>
<span id="session" role="status">Logged out</span></form></section>
<nav aria-label="Administration sections"><a href="#roles">Roles</a><a href="#radio">Radio</a><a href="#program">Bot programs</a><a href="#services">Services</a><a href="#storage">Backups</a><a href="#firmware">Firmware</a><a href="#advanced">Console</a></nav>
<div class="notice" role="status" aria-live="polite"><strong id="operation">Log in to manage this node</strong><pre id="result"></pre>
<button id="inspect" disabled>Check pending changes</button></div>
<noscript><p class="notice">Enable JavaScript to use these controls.</p></noscript>
<div id="controls">
<section id="roles"><h2>Roles</h2>
<div class="actions"><button id="refresh">Refresh status</button>
<button data-write="apply" data-warning="Restart this node to apply saved settings? All roles and connections will stop during the restart.">Restart to apply settings</button></div>
<p id="role-generation" class="muted">Role state has not been read.</p>
<div class="scroll"><table><thead><tr><th>Role</th><th>Running now</th><th>After restart</th><th>Change</th></tr></thead><tbody id="role-rows"></tbody></table></div>
<p class="muted">Role enable/disable changes take effect after a restart. These switches do not change Management or KISS.</p>
<h3>Role name and advert</h3><div class="fields">
<label>Role <select id="role"><option value="repeater">Repeater</option><option value="room">Room</option><option value="companion">Companion</option><option value="bot">Bot</option><option value="management">Management</option><option value="kiss">KISS service</option></select></label>
<label>Name <input id="role-name" maxlength="31" placeholder="Up to 31 ASCII characters; no colon"></label></div>
<div class="actions"><button id="role-read">Read role settings</button><button id="role-save">Save name</button><button id="role-advert">Send zero-hop advert</button></div>
<p class="muted">Renaming keeps the role's identity. An advert announces it to radios in range.</p>
<details><summary>Role settings and pending changes</summary><button data-read="job" data-target="role-detail">Check pending changes</button><pre id="role-detail"></pre></details></section>
<section id="radio"><h2>Radio settings</h2><pre id="radio-status">Radio settings not loaded</pre>
<p class="warning">All roles share this channel. Retuning can disconnect RF administration and cancel queued packets.
Choose a legal frequency and power, and keep WiFi or USB access available.</p>
<div class="fields"><label>Frequency (Hz)<input id="freq" type="number" min="150000000" max="960000000" step="1" placeholder="912525000"></label>
<label>Bandwidth (Hz)<select id="bw"><option>7810</option><option>10420</option><option>15630</option><option>20830</option><option>31250</option><option>41700</option><option>62500</option><option>125000</option><option selected>250000</option><option>500000</option></select></label>
<label>Spreading factor<select id="sf"><option>5</option><option>6</option><option selected>7</option><option>8</option><option>9</option><option>10</option><option>11</option><option>12</option></select></label>
<label>Coding rate<select id="cr"><option selected>5</option><option>6</option><option>7</option><option>8</option></select></label>
<label>TX power (dBm)<input id="power" type="number" min="0" max="22" value="2" step="1"></label>
<label>Temporary change duration (seconds)<input id="seconds" type="number" min="1" max="3600" value="60" step="1"></label></div>
<div class="actions"><button id="radio-copy">Load current settings</button><button id="radio-save">Save and retune</button><button id="radio-temp">Retune temporarily</button></div>
<p class="muted">Temporary changes return to the saved channel when the duration expires.</p></section>
<section id="program"><h2>Bot programs</h2>
<button id="inventory">Load program status</button>
<p id="runtime-note" class="muted">Load program status to see available runtimes.</p>
<details><summary>Program status and capabilities</summary><button data-read="bot log" data-target="program-status">Read bot log</button><pre id="program-status">Program status not loaded</pre><pre id="capabilities"></pre></details>
<label>Runtime<select id="program-runtime"><option value="lua">Lua</option><option id="wasm-option" value="wasm" disabled>Wasm</option></select></label>
<p class="muted">Installing replaces the selected runtime's custom commands. Lua and Wasm are managed separately.
Message the bot with <code>!plugins</code> or <code>!help</code> to list its commands.</p>
<label id="lua-editor">Lua source (4096 UTF-8 bytes maximum)<textarea id="source" spellcheck="false" placeholder="function hello(name) reply('Hello '..name) end"></textarea></label>
<div id="wasm-editor" hidden><label>Wasm module or package (4096 bytes maximum)<input id="wasm-file" type="file" accept=".wasm,.mcbot,application/wasm,application/octet-stream"></label>
<p id="wasm-info" class="muted">Choose a Wasm file to upload.</p></div>
<div class="actions"><button id="hello">Use hello example</button><button id="source-read">Load installed source</button><button id="source-draft">Download draft</button>
<button id="source-hash">Show draft hash</button></div>
<p id="draft-info" class="muted">Download unsaved edits before closing this tab.</p>
<div class="actions"><button id="source-stage" disabled>Upload draft</button><button id="source-install" disabled>Install uploaded draft</button>
<button data-read="source status" data-target="program-status">Check activation</button><button data-read="source hash" data-target="program-status">Read installed hash</button></div>
<p id="upload-info" class="muted">Upload a draft, then install it to activate its commands.</p>
<details><summary>Recovery and help text</summary><div class="actions">
<button id="source-rollback">Restore previous program</button><button id="source-remove">Restore bundled commands</button>
<button id="source-retry">Retry selected program</button>
<button data-write="source cancel" data-warning="Discard this unfinished upload? The running program will stay unchanged.">Cancel upload</button></div>
<p class="muted">Restoring a program cancels its running commands but keeps bot data.</p>
<label>Program help text (up to 96 ASCII characters)<input id="helptext" maxlength="96"></label>
<div class="actions"><button id="help-read">Load help text</button><button id="help-save">Save help text</button></div></details></section>
<section id="services"><h2>Services</h2><div class="grid">
<div><h3>Device metrics</h3><div class="actions">
<button id="telemetry-read">Load publishing status</button><button data-read="telemetry counts" data-target="telemetry-status">Message counts</button>
<button data-read="telemetry tls" data-target="telemetry-status">TLS status</button><button data-read="telemetry tls-heap" data-target="telemetry-status">TLS memory</button>
<button data-read="telemetry endpoint status" data-target="telemetry-status">Endpoint readiness</button></div><pre id="telemetry-status"></pre>
<label>Publishing<select id="telemetry-on"><option value="off">Off</option><option value="on">On</option></select></label>
<label>Interval (30..86400 seconds)<input id="interval" type="number" min="30" max="86400" step="1" value="60"></label>
<div class="actions"><button id="telemetry-save" disabled>Save publishing state</button><button id="interval-save" disabled>Save interval</button></div>
<p class="muted">Publishing requires a configured TLS endpoint and trusted time.</p></div>
<div><h3>Connections and bot permissions</h3><div class="actions">
<button id="service-read">Load service status</button><button data-read="companion stats" data-target="service-status">Companion TCP</button>
<button data-read="companion errors" data-target="service-status">Companion errors</button><button data-read="wifi status" data-target="service-status">WiFi status</button>
<button data-read="bot limits" data-target="service-status">Bot limits</button></div><pre id="service-status"></pre>
<label>Bot permission<select id="grant"><option value="shared">Shared data and timers</option><option value="home">Configured HTTPS service</option>
<option value="reminders">Private reminders</option><option value="discovery">Base telemetry and path discovery</option><option value="channel-wait">Channel follow-up messages</option></select></label>
<label>Permission<select id="grant-on"><option value="off">Off</option><option value="on">On</option></select></label>
<div class="actions"><button id="grant-read">Load permission</button><button id="grant-save">Save permission</button>
<button data-write="bot cancel" data-warning="Stop running bot commands and message collectors? Actions already sent cannot be undone. Scheduled reminders will stay unchanged.">Stop bot work</button></div>
<p class="muted">Discovery shares Base telemetry with signed contacts. HTTPS access is limited to the configured service.</p></div></div></section>
<section id="storage"><h2>Bot data backups</h2>
<p class="warning">Backups contain private bot data. Keep downloaded files private. They do not include programs, identities, passwords or permissions.</p>
<button id="storage-read">Load backup options</button>
<details><summary>Storage limits and transfer status</summary><button data-read="data status" data-target="storage-status">Check transfer</button>
<button data-write="data clear" data-warning="Discard the current backup transfer? Saved bot data will stay unchanged.">Clear transfer</button>
<pre id="storage-status"></pre></details>
<div class="fields"><label>Data<select id="data-kind"><option value="kv">Key-value data</option><option value="timers">Named timers</option><option value="reminders">Personal reminders</option></select></label>
<label>Scope<select id="data-scope"><option value="caller">User</option><option value="conversation">Conversation</option><option value="bot">Bot</option><option value="channel">Channel</option></select></label>
<label>User public key or channel digest<input id="principal" maxlength="64" placeholder="64 lowercase hex characters"></label></div>
<p class="muted">For bot scope, enter 64 zeros. Reminders use a user's public key.</p>
<button id="data-export" disabled>Download backup</button>
<h3>Restore a backup</h3><label>Backup file (.bkd, .btd or .brd)<input id="backup" type="file" accept=".bkd,.btd,.brd"></label>
<pre id="backup-info">No backup selected</pre>
<label><input id="no-rearm" type="checkbox">I understand restored timers and reminders will not run; matching pending work will be cancelled.</label>
<div class="actions"><button id="data-stage" disabled>Validate backup</button><button id="data-restore" disabled>Restore backup</button></div>
<p class="warning">Restoring key-value data replaces every key in the selected scope. Timer and reminder restores merge records without scheduling them again.</p></section>
<section id="firmware"><h2>Firmware update</h2>
<button id="firmware-status">Check firmware status</button>
<pre id="firmware-detail">Check status before choosing an update.</pre>
<label>Application image (.bin)<input id="firmware-image" type="file" accept=".bin,application/octet-stream"></label>
<label>Signed update manifest (.json)<input id="firmware-manifest" type="file" accept=".json,application/json"></label>
<div class="actions"><button id="firmware-upload" disabled>Upload and verify</button><button id="firmware-reboot" disabled>Restart with update</button></div>
<p class="warning">Choose an application-only image and its signed manifest, never a signing key, bootloader or full-flash image.
Restarting interrupts all roles. Keep USB recovery accessible.</p>
<p class="muted">After restarting, log in and check the running image hash and boot health.</p></section>
<section id="advanced"><h2>Command console</h2><p>Use CLI commands for settings without a form. Changes require confirmation.</p>
<form id="command-form" class="actions"><label>Command<input id="command" maxlength="162" value="status" size="60"></label><button type="submit">Run native command</button></form>
<div class="actions"><button data-read="help" data-target="result">Command help</button><button data-read="role help" data-target="result">Role help</button><button data-read="bot help" data-target="result">Bot help</button>
<button data-read="source help" data-target="result">Source help</button><button data-read="data help" data-target="result">Data help</button></div>
<details><summary>Passwords and private settings</summary><p>Change passwords, WiFi credentials, channel keys and private identities through encrypted Management RF using the owner CLI. These settings are not transferred over this unencrypted page.</p></details></section>
</div>
</main><script>
'use strict';
const $ = id => document.getElementById(id);
const hex = bytes => Array.from(bytes, v => v.toString(16).padStart(2, '0')).join('');
const unhex = text => {
  if (!/^(?:[0-9a-f]{2})+$/.test(text)) throw Error('Invalid hexadecimal readback');
  return Uint8Array.from(text.match(/../g), x => parseInt(x, 16));
};
function sha256(bytes) {
  const k=[], initial=[];let candidate=2;
  while(k.length<64){let prime=true;for(let d=2;d*d<=candidate;d++)if(candidate%d===0){prime=false;break}
    if(prime){if(initial.length<8)initial.push((Math.sqrt(candidate)%1*4294967296)>>>0);k.push((Math.cbrt(candidate)%1*4294967296)>>>0)}candidate++}
  const data=new Uint8Array((bytes.length+9+63)&~63);data.set(bytes);data[bytes.length]=128;
  const view=new DataView(data.buffer);view.setUint32(data.length-4,bytes.length*8);
  const r=(v,n)=>(v>>>n)|(v<<(32-n)), w=new Uint32Array(64), h=initial.slice();
  for(let at=0;at<data.length;at+=64){
    for(let i=0;i<16;i++)w[i]=view.getUint32(at+4*i);
    for(let i=16;i<64;i++){const a=w[i-15],b=w[i-2];w[i]=(w[i-16]+(r(a,7)^r(a,18)^(a>>>3))+w[i-7]+(r(b,17)^r(b,19)^(b>>>10)))>>>0}
    let [a,b,c,d,e,f,g,z]=h;
    for(let i=0;i<64;i++){const t=(z+(r(e,6)^r(e,11)^r(e,25))+((e&f)^(~e&g))+k[i]+w[i])>>>0;
      const u=((r(a,2)^r(a,13)^r(a,22))+((a&b)^(a&c)^(b&c)))>>>0;
      z=g;g=f;f=e;e=(d+t)>>>0;d=c;c=b;b=a;a=(t+u)>>>0}
    [a,b,c,d,e,f,g,z].forEach((v,i)=>h[i]=(h[i]+v)>>>0);
  }
  return h.map(v=>v.toString(16).padStart(8,'0')).join('');
}
let token='', expires=0, busy=false, uncertain=false, effective=null, uploaded=null, backup=null, staged=null, wasmBytes=null;
const unresolved=[];
let inFlight=null, lastMutation=null;
let uploadAttempt=null;
let firmwareStatus=null, expectedFirmware=null;
const supported={source:false,wasm:false,data:false,telemetry:false}, seen={};
function programRuntime() { return $('program-runtime').value==='wasm'?'wasm':'lua'; }
function sourceCommand(text) { return text.replace(/^source /,programRuntime()==='wasm'?'source wasm ':'source '); }
function sourceCmd(text,literal=false) { return cmd(sourceCommand(text),literal); }
function show(text, kind='muted') { $('result').textContent=text; $('result').className=kind; }
function updateControls() {
  const blocked=!token || busy || ['verified','receiving'].includes(firmwareStatus?.state), local=['source','helptext','hello','source-hash','source-draft'];
  document.querySelectorAll('#controls button, #controls input, #controls select, #controls textarea').forEach(control=>{
    control.disabled=local.includes(control.id)?(control.tagName==='BUTTON' && busy):blocked;
  });
  $('login').disabled=busy || !!token; $('password').disabled=busy || !!token;
  $('logout').disabled=busy || !token; $('inspect').disabled=busy || !token;
  $('source-stage').disabled=blocked || !supported.source; $('source-install').disabled=blocked || !supported.source || !uploaded;
  $('wasm-option').disabled=!supported.wasm;
  const wasm=programRuntime()==='wasm';
  $('lua-editor').hidden=wasm; $('wasm-editor').hidden=!wasm;
  $('hello').hidden=wasm; $('source-draft').hidden=wasm; $('draft-info').hidden=wasm;
  $('source-remove').textContent=programRuntime()==='wasm'?'Remove Wasm commands':'Restore bundled Lua commands';
  $('data-export').disabled=blocked || !supported.data; $('data-stage').disabled=blocked || !supported.data || !backup;
  $('data-restore').disabled=blocked || !supported.data || !staged;
  $('telemetry-save').disabled=blocked || !supported.telemetry; $('interval-save').disabled=blocked || !supported.telemetry;
  $('role-advert').disabled=blocked || $('role').value==='kiss';
  $('firmware-status').disabled=busy || !token;
  $('firmware-upload').disabled=busy || !token || uncertain || !firmwareStatus || !['idle','failed'].includes(firmwareStatus.state);
  $('firmware-reboot').disabled=busy || !token || uncertain || firmwareStatus?.reboot_ready!==true;
  $('firmware-image').disabled=busy || !token; $('firmware-manifest').disabled=busy || !token;
}
function forgetSession() {
  token=''; expires=0; uploaded=null; staged=null; backup=null; effective=null;
  Object.keys(supported).forEach(k=>supported[k]=false); Object.keys(seen).forEach(k=>delete seen[k]);
  $('session').textContent='Logged out. Unsaved drafts are still in this tab.';
  $('role-rows').replaceChildren(); $('role-generation').textContent='Role state has not been read.';
  for(const id of ['role-detail','radio-status','program-status','capabilities','storage-status','telemetry-status','service-status','backup-info']) $(id).textContent='';
  $('backup').value=''; $('no-rearm').checked=false; $('upload-info').textContent='Log in and upload a draft before installing.';
  updateControls();
}
function unknown(message) {
  const affected=inFlight?.mutation?inFlight:lastMutation;
  if(affected && !unresolved.some(x=>x.command===affected.command && x.runtime===affected.runtime)) unresolved.push({...affected});
  if(affected) uncertain=true;
  const e=Error(message+(affected || unresolved.length?' Use Check pending changes before trying again.':'')); e.unknown=true; return e;
}
async function post(path, body='', login=false) {
  if(!login && (!token || Date.now()>=expires)) { forgetSession(); throw Error('Admin session expired; login again. Draft text is retained.'); }
  const controller=new AbortController(), timer=setTimeout(()=>controller.abort(),5000);
  const session=token;
  let response, value;
  try {
    response=await fetch(path,{method:'POST',cache:'no-store',credentials:'omit',redirect:'error',
      headers:login?{}:{'X-Mast-Session':session},body,signal:controller.signal});
    value=await response.text();
  } catch(e) { throw unknown('Admin connection lost or timed out; command outcome unknown.'); }
  finally { clearTimeout(timer); }
  if(!login && token!==session) throw unknown('Session ended while a command was in flight; outcome unknown.');
  if(response.status>=500 && response.status!==503) throw unknown(value);
  if(!response.ok) {
    if(response.status===403 && !login) forgetSession();
    throw Error('HTTP '+response.status+': '+value);
  }
  return value;
}
function validateCommand(text) {
  if(!/^[\x20-\x7e]{1,162}$/.test(text)) throw Error('Native command requires 1..162 printable ASCII bytes.');
  const words=text.trim().toLowerCase().split(/ +/);
  if(words[0]==='password' && !(words.length===1 || (words.length===2 && words[1]==='help')))
    throw Error('Management password changes require encrypted Management RF. Only password status/help are available over HTTP.');
  if(words[0]==='role' && words[1]==='password')
    throw Error('Role administrator password changes are unavailable over HTTP; use the file-based role-password CLI over authenticated encrypted Management RF.');
  if((words[0]==='telemetry' && words[1]==='endpoint' && ['ca','token'].includes(words[2])) ||
     (words[0]==='bot' && words[1]==='https' && ['ca','token'].includes(words[2])) ||
     (words[0]==='key' && words.length>2 && !(words.length===3 && ['pending','cancel'].includes(words[2]))))
    throw Error('CA, endpoint-token and private-key import are unavailable over HTTP. Use the owner CLI over encrypted Management RF.');
  if((words[0]==='wifi' && ['ssid','password','forget'].includes(words[1])) ||
     (words[0]==='wifi' && words.length>=3) ||
     (words[0]==='get' && words[1]==='wifi.pwd') ||
     (words[0]==='set' && words[1]?.startsWith('wifi.')))
    throw Error('WiFi secrets and setters require encrypted Management RF; this HTTP page cannot provision them.');
  if(words[0]==='role' && words[1]==='channel' && words.length>4 && !(words.length===5 && words[4]==='off'))
    throw Error('Channel keys are unavailable over HTTP. Use the owner CLI over encrypted Management RF.');
}
async function cmd(text, raw=false) {
  validateCommand(text);
  const previous=inFlight;
  inFlight={command:text,runtime:/^source wasm /.test(text)?'wasm':'lua',
    snapshot:uploaded?{...uploaded}:uploadAttempt?{...uploadAttempt}:null,restore:staged?{id:staged.id,kind:staged.kind,scope:staged.scope,principal:staged.principal}:null};
  inFlight.mutation=!nativeRead(text);
  if(inFlight.mutation) lastMutation={...inFlight};
  try {
    const value=await post('/admin/command',text);
    if(!raw && /^Error:/.test(value)) {
      if(/unknown|uncertain|partial changes possible/i.test(value)) throw unknown(value);
      throw Error(value);
    }
    return value;
  } finally {
    inFlight=previous;
  }
}
function confirmed(message, inspection=false) {
  if(uncertain && !inspection) throw Error('A previous change is unconfirmed. Use Check pending changes before making another change.');
  return window.confirm(message);
}
async function action(label, work) {
  if(busy) return;
  lastMutation=null;
  busy=true; updateControls(); $('operation').textContent=label; show('Working...','muted');
  try { const text=await work(); if(text!==undefined) report(text); }
  catch(e) { show(e.message,e.unknown?'warning':'error'); }
  finally { busy=false; updateControls(); }
}
function report(text) {
  if(/^(UNKNOWN\b)|outcome unknown/i.test(text)) { unknown(text); show(text,'warning'); return; }
  const pending=/^(Accepted|Queued|PENDING|BUSY|UPLOADING|STAGED|ACK|RECEIVED|Removed corrupt)/.test(text);
  show((pending?'Pending: ':'')+text,pending?'warning':'muted');
}
const wait=ms=>new Promise(resolve=>setTimeout(resolve,ms));
async function readGroup(commands, target) {
  const lines=[];
  for(const text of commands) {
    delete seen[text];
    try { const value=await cmd(text); seen[text]=value; lines.push(text+': '+value); }
    catch(e) { lines.push(text+': '+e.message); $(target).textContent=lines.join('\n'); throw e; }
    $(target).textContent=lines.join('\n');
  }
  return lines.join('\n');
}
function parseStatus(text) {
  const m=/^roles applied=(\d+) saved=(\d+) generation=(\d+) bot-saved=([01]) PHY=(\d+),(\d+),(\d+),(\d+),(\d+) effective-gen=(\d+) temp=([01])$/.exec(text);
  if(!m || +m[1]>15 || +m[2]>15) throw Error('Unexpected native status shape; use the native command console.');
  return {applied:+m[1],saved:+m[2],generation:m[3],botSaved:m[4],phy:m.slice(5,10),effectiveGeneration:m[10],temp:m[11]};
}
function renderRoles(status, bot) {
  $('role-generation').textContent='Settings revision '+status.generation;
  $('role-rows').replaceChildren();
  const b=/^bot applied=([01]) saved=([01]) ready=([01]) state=(.*)$/.exec(bot);
  for(const [name,bit] of [['repeater',1],['room',2],['companion',4],['observer',8],['bot',0],['management',null],['KISS',null]]) {
    const row=document.createElement('tr'), running=bit===null?'On':bit?((status.applied&bit)?'Enabled':'Disabled'):b?(b[1]==='1'?(b[3]==='1'?'Ready':'Not ready: '+b[4]):'Disabled'):'Unknown';
    const saved=bit===null?'On':bit?((status.saved&bit)?'Enabled':'Disabled'):b?(b[2]==='1'?'Enabled':'Disabled'):(status.botSaved==='1'?'Enabled':'Disabled');
    const names={repeater:'Repeater',room:'Room',companion:'Companion',observer:'Observer',bot:'Command bot',management:'Management',KISS:'KISS'};
    for(const text of [names[name],running,saved]) { const td=document.createElement('td'); td.textContent=text; row.appendChild(td); }
    const td=document.createElement('td');
    if(bit!==null) {
      const button=document.createElement('button'); button.type='button'; button.textContent=saved==='Enabled'?'Disable after restart':'Enable after restart';
      button.onclick=()=>action('Saving '+name+' next-boot selection',async()=>{
        if(!confirmed('Change '+name+' next-boot selection to '+(saved==='Enabled'?'disabled':'enabled')+'? Running roles are unchanged until apply/reboot.')) return 'Cancelled; no role change sent.';
        // Read the shared mask again rather than overwriting another owner's saved selection.
        const current=parseStatus(await cmd('status'));
        const mask=saved==='Enabled'?(current.saved&~bit):(current.saved|bit);
        const result=await cmd(bit?'roles '+mask:'bot '+(saved==='Enabled'?'off':'on'));
        await refresh(); return result;
      }); td.appendChild(button);
    } else td.textContent='Always enabled';
    row.appendChild(td); $('role-rows').appendChild(row);
  }
}
async function refresh() {
  const status=parseStatus(await cmd('status')); effective=status;
  let bot; try { bot=await cmd('bot status'); } catch(e) { if(e.unknown || !token) throw e; bot=e.message; }
  renderRoles(status,bot);
  const [frequency,bandwidth,sf,cr,power]=status.phy;
  $('radio-status').textContent=(Number(frequency)/1000000).toFixed(3)+' MHz | '+Number(bandwidth)/1000+' kHz | SF'+sf+' | CR 4/'+cr+' | '+power+' dBm\n'+(status.temp==='1'?'Temporary channel':'Current channel');
  return 'Role and radio status updated.';
}
async function inventory() {
  supported.source=false;
  const runtimes=await cmd('source api runtimes');
  supported.wasm=/\bwamr-2\.4\.1\/meshcore-v1\b/.test(runtimes);
  await readGroup(['source status','source hash','source metadata'].map(sourceCommand).concat('bot status'),'program-status');
  await readGroup(['source api','source api package','source api modules','source api fetch'].map(sourceCommand),'capabilities');
  const api=seen[sourceCommand('source api')]||'';
  supported.source=programRuntime()==='wasm'?supported.wasm && /^API meshcore-v1 runtime=wamr-2\.4\.1 /.test(api):/^API named-commands-v1 lua=5\.5\.1 /.test(api);
  $('runtime-note').textContent=supported.source?(programRuntime()==='wasm'?'Wasm is available. Choose a module or package to upload.':'Lua is available. Edit or load a program below.'):'This firmware does not support the selected runtime.';
  return 'Program status updated.';
}
function sourceBytes() {
  if(programRuntime()==='wasm') {
    if(!wasmBytes) throw Error('Choose a Wasm binary file before uploading.');
    return wasmBytes.slice();
  }
  const bytes=new TextEncoder().encode($('source').value);
  if(!bytes.length || bytes.length>4096 || bytes.includes(0) || bytes[0]===27) throw Error('Source must be 1..4096 UTF-8 bytes, without NUL or binary bytecode.');
  return bytes;
}
function draftInfo() { const b=new TextEncoder().encode($('source').value); $('draft-info').textContent=b.length+' / 4096 bytes. Download unsaved edits before closing this tab.'; }
async function uploadSource() {
  if(!supported.source) throw Error('Refresh capabilities before uploading source.');
  const bytes=sourceBytes(), hash=sha256(bytes), id=hash.slice(0,16), count=Math.ceil(bytes.length/48);
  if(!confirmed('Upload this '+bytes.length+'-byte draft? Install it separately to activate its commands.\nSHA256 '+hash)) return 'Cancelled; no source upload sent.';
  uploaded=null;
  uploadAttempt={id,hash,size:bytes.length,runtime:programRuntime()};
  const admission=await sourceCmd(`source begin ${id} ${bytes.length} ${hash}`);
  const match=new RegExp('^ACK '+id+' next=(\\d+)$').exec(admission);
  if(!match || +match[1]>count) throw Error('Upload admission/resume index invalid; inspect source status.');
  for(let n=+match[1];n<count;n++) {
    if(await sourceCmd(`source chunk ${id} ${n} ${hex(bytes.slice(n*48,(n+1)*48))}`)!==`ACK ${id} next=${n+1}`)
      throw unknown('Source chunk acknowledgement mismatch.');
    $('upload-info').textContent='Uploading: '+(n+1)+' / '+count+' chunks';
  }
  uploaded={id,hash,size:bytes.length,runtime:programRuntime()};
  $('upload-info').textContent='Uploaded '+bytes.length+' bytes. Ready to install.\nSHA256 '+hash+'\nLater edits need another upload.';
  return 'Draft uploaded. Select Install uploaded draft to activate it.';
}
function manifest(text) {
  const m=/^SHA256 ([0-9a-f]{64}) gen=(\d+)$/.exec(text);
  if(!m) throw Error('Invalid durable source manifest.');
  return {hash:m[1],generation:m[2],text};
}
async function sourceOutcome(expected, previous) {
  for(let n=0;n<30;n++) {
    await wait(500);
    const status=await sourceCmd('source status'); $('program-status').textContent=status;
    const current=manifest(await sourceCmd('source hash'));
    const terminal=/source durably saved and active|source saved; bot disabled; loads on next enabled boot/.test(status);
    if(terminal && current.text!==previous && (!expected || current.hash===expected)) return status+'\n'+current.text;
  }
  throw unknown('Source activation remains pending or was superseded; inspect source status and hash.');
}
async function installSource() {
  if(!uploaded) throw Error('Explicitly upload/resume the draft first.');
  const u={...uploaded};
  if(u.runtime!==programRuntime()) throw Error('Runtime changed; upload the selected runtime explicitly.');
  if(!confirmed('Install the uploaded '+u.runtime+' program ('+u.size+' bytes)? It replaces custom commands and cancels running work. Edits made after upload are not included.\nSHA256 '+u.hash)) return 'Cancelled; no install sent.';
  const before=manifest(await sourceCmd('source hash'));
  const status=await sourceCmd('source status');
  const m=/\bupload=([0-9a-f]{16}) next=(\d+)\/(\d+);/.exec(status);
  if(!m || m[1]!==u.id || +m[2]!==+m[3] || +m[3]!==Math.ceil(u.size/48)) throw Error('Uploaded snapshot changed/incomplete; inspect status and explicitly upload/resume.');
  if(await sourceCmd(`source begin ${u.id} ${u.size} ${u.hash}`)!==`ACK ${u.id} next=${Math.ceil(u.size/48)}`)
    throw Error('Uploaded hash/size manifest changed; no source commit sent. Inspect status and explicitly upload/resume.');
  const result=await sourceCmd('source commit '+u.id); uploaded=null;
  if(!result.startsWith('Accepted')) throw unknown('Source verification admission was not recognized: '+result);
  return await sourceOutcome(u.hash,before.text);
}
async function readSource() {
  const old=$('source').value;
  if(programRuntime()==='lua' && old && !window.confirm('Replace this editor draft with verified durable source? Download unsaved edits first.')) return 'Cancelled; editor unchanged.';
  const before=manifest(await sourceCmd('source hash')), status=await sourceCmd('source status');
  const m=/\bsize=(\d+) /.exec(status), size=m?+m[1]:0;
  if(!size || size>4096) throw Error('Durable source exceeds the editable/download envelope or has no valid size. Bundled handlers may be larger; use metadata and live !plugins.');
  const bytes=new Uint8Array(size);
  for(let n=0;n<Math.ceil(size/48);n++) {
    const result=await sourceCmd('source read '+n);
    if(!result.startsWith('DATA ')) throw Error('Source readback missing DATA at chunk '+n);
    const chunk=unhex(result.slice(5)), expected=Math.min(48,size-n*48);
    if(chunk.length!==expected) throw Error('Source readback length changed; editor unchanged.');
    bytes.set(chunk,n*48);
  }
  if(sha256(bytes)!==before.hash || await sourceCmd('source hash')!==before.text) throw Error('Source hash/generation changed or digest failed; editor unchanged.');
  if(programRuntime()==='wasm') {
    download(bytes,'meshcore-durable.mcbot');
    return 'Verified Wasm program downloaded. Lua draft unchanged.';
  }
  if($('source').value!==old) throw Error('Editor changed during read; downloaded source was not substituted.');
  $('source').value=new TextDecoder('utf-8',{fatal:true}).decode(bytes); draftInfo();
  return 'Verified installed source loaded into the editor.';
}
async function recoverSource(operation) {
  const descriptions={rollback:'Restore the previous program and help text?',remove:'Remove custom Lua commands and restore bundled commands?',retry:'Retry loading the selected program?'};
  if(programRuntime()==='wasm') descriptions.remove='Remove the Wasm package? Lua commands will stay unchanged.';
  if(!confirmed(descriptions[operation]+' Running commands can be cancelled; stored bot data remains.')) return 'Cancelled; no source recovery sent.';
  let before=''; try { before=manifest(await sourceCmd('source hash')).text; } catch(e) { if(operation!=='remove' || e.unknown || !token) throw e; }
  const result=await sourceCmd('source '+operation); uploaded=null;
  if(operation==='retry') return result+'\nCheck activation to see whether the selected program is running.';
  if(!/^(Accepted|Removed corrupt)/.test(result)) throw unknown('Source recovery admission was not recognized: '+result);
  return await sourceOutcome(null,before);
}
function download(bytes,name,type='application/octet-stream') {
  const url=URL.createObjectURL(new Blob([bytes],{type})), a=document.createElement('a');
  a.href=url; a.download=name; a.click(); setTimeout(()=>URL.revokeObjectURL(url),1000);
}
const scopes=['caller','conversation','bot','channel'], kinds={BKD:'kv',BTD:'timers',BRD:'reminders'};
function backupEnvelope(bytes) {
  const magic=String.fromCharCode(...bytes.slice(0,3)), kind=kinds[magic], scope=scopes[bytes[36]], principal=hex(bytes.slice(37,69));
  if(bytes.length!==2422 || bytes[3]!==1 || !kind || !scope || sha256(bytes.slice(0,-32))!==hex(bytes.slice(-32)))
    throw Error('Backup must be a valid 2422-byte BKD/BTD/BRD v1 file with matching content digest.');
  if((scope==='bot')!==(principal==='0'.repeat(64)) || (kind==='reminders' && scope!=='caller')) throw Error('Backup scope/principal is invalid.');
  return {kind,scope,principal,bot:hex(bytes.slice(4,36)),count:bytes[69],hash:sha256(bytes)};
}
function backupDescription(meta) { return meta.kind+' · '+meta.scope+' · '+meta.count+' records\nPrincipal '+meta.principal+'\nBot '+meta.bot+'\nSHA256 '+meta.hash; }
async function botKey() { const key=await cmd('bot key'); if(!/^KEY [0-9a-f]{64}$/.test(key)) throw Error('Running persistent bot identity unavailable.'); return key.slice(4); }
async function dataOutcome(expected,id) {
  for(let n=0;n<60;n++) {
    const status=await cmd('data status'); $('storage-status').textContent=status;
    if(status.startsWith(expected+' ') && (!id || status===expected+' '+id)) return status;
    if(status.startsWith('UNKNOWN')) throw unknown('Bot-data result '+status+'; export/read back the affected scope.');
    if(!/^(BUSY|PENDING) /.test(status)) throw Error('Bot-data result was not '+expected+': '+status);
    await wait(500);
  }
  throw unknown('Bot-data operation remains pending.');
}
async function exportData() {
  const kind=$('data-kind').value, scope=$('data-scope').value, principal=$('principal').value;
  if(!/^[0-9a-f]{64}$/.test(principal) || (scope==='bot')!==(principal==='0'.repeat(64)) || (kind==='reminders' && scope!=='caller'))
    throw Error('Select caller for reminders. Use full nonzero user/channel identity, or 64 zeros only for bot-global.');
  if(!confirmed('Export private '+kind+' data for '+scope+' principal '+principal+' over plaintext HTTP and download an unencrypted file? Protect the downloaded file; no credentials/identity are included. This replaces the RAM transfer buffer, allowing readback after an uncertain restore.',true)) return 'Cancelled; no export sent.';
  const key=await botKey(), result=await cmd(`data export ${kind} ${scope} ${principal}`);
  if(!result.startsWith('PENDING')) throw Error('Data export was not admitted: '+result);
  const status=await dataOutcome('EXPORTED'), m=/^EXPORTED ([0-9a-f]{64}) ([0-9a-f]{16})$/.exec(status);
  if(!m || m[2]!==m[1].slice(0,16)) throw Error('Invalid export manifest.');
  const bytes=new Uint8Array(2422);
  for(let n=0;n<Math.ceil(bytes.length/48);n++) {
    const value=await cmd(`data read ${m[2]} ${n}`);
    if(!value.startsWith('DATA ')) throw Error('Invalid export chunk '+n);
    const chunk=unhex(value.slice(5));
    if(chunk.length!==Math.min(48,bytes.length-n*48)) throw Error('Export chunk length mismatch.');
    bytes.set(chunk,n*48);
  }
  const meta=backupEnvelope(bytes);
  if(meta.hash!==m[1] || meta.kind!==kind || meta.scope!==scope || meta.principal!==principal || meta.bot!==key ||
      await cmd('data status')!==status || await botKey()!==key) throw Error('Export identity/scope/hash or frozen status changed; no file downloaded.');
  download(bytes,`meshcore-${kind}-${scope}-${m[2]}.${{kv:'bkd',timers:'btd',reminders:'brd'}[kind]}`);
  return 'Verified private backup downloaded. Protect this file; values are not displayed here.\n'+backupDescription(meta);
}
async function inspectBackup() {
  staged=null; backup=null; $('backup-info').textContent='No backup inspected.';
  const file=$('backup').files[0];
  if(!file) return 'No backup selected.';
  if(file.size!==2422) throw Error('Backup file must be exactly 2422 bytes.');
  const bytes=new Uint8Array(await file.arrayBuffer()), meta=backupEnvelope(bytes);
  backup={bytes,...meta}; $('backup-info').textContent=backupDescription(meta);
  return 'Backup envelope inspected locally. Native staging still checks records, bot identity and storage headroom.';
}
async function stageData() {
  if(!backup) throw Error('Select and inspect a backup first.');
  const b=backup, id=b.hash.slice(0,16);
  if(b.kind!=='kv' && !$('no-rearm').checked) throw Error('Scheduler staging requires explicit no-rearm acknowledgement.');
  if(!confirmed('Upload private backup over plaintext HTTP and replace the current RAM transfer buffer?\n'+backupDescription(b)+'\nStaging validates only; it does not restore.')) return 'Cancelled; no backup staged.';
  if(await botKey()!==b.bot) throw Error('Backup belongs to a different bot identity; no data uploaded.');
  staged=null;
  if(await cmd(`data begin ${id} ${b.hash}`)!==`UPLOADING ${id} bytes=2422`) throw unknown('Backup upload admission mismatch.');
  for(let n=0;n<Math.ceil(b.bytes.length/48);n++) {
    if(await cmd(`data chunk ${id} ${n} ${hex(b.bytes.slice(n*48,(n+1)*48))}`)!==`RECEIVED ${Math.min((n+1)*48,b.bytes.length)}`)
      throw unknown('Backup chunk acknowledgement mismatch.');
  }
  if(await cmd('data stage '+id)!=='PENDING '+id) throw unknown('Backup staging admission mismatch.');
  const result=await dataOutcome('STAGED',id); staged={...b,id};
  return result+'\nBackup validated. Select Restore backup to apply it.';
}
async function restoreData() {
  if(!staged) throw Error('Explicitly stage this backup first.');
  const b=staged, scheduler=b.kind!=='kv';
  if(scheduler && !$('no-rearm').checked) throw Error('Scheduler restore requires no-rearm acknowledgement.');
  if(!confirmed('Restore '+backupDescription(b)+'\n'+(scheduler?'Merge this scheduler family, never rearm. Matching pending records are cancelled; UNKNOWN means the committed outcome is unconfirmed. Already sent messages cannot be undone.':'Replace ALL KV keys in this scope, deleting newer keys absent from the backup.')+'\nRead/export affected state before another restore if the result is uncertain.')) return 'Cancelled; no restore sent.';
  if(await botKey()!==b.bot || await cmd('data status')!=='STAGED '+b.id) throw Error('Bot identity or staged data changed; explicitly inspect and stage again.');
  const admission=await cmd('data restore '+b.id+(scheduler?' no-rearm':'')); staged=null;
  if(admission!=='PENDING '+b.id) throw unknown('Restore admission mismatch: '+admission);
  return await dataOutcome('COMMITTED',b.id);
}
const readonly=/^(?:status|ver|board|job|help(?: \S+)?|get (?:name|owner\.info|radio|freq|tx|cad|wifi\.(?:enabled|ssid|ip|status))|password(?: help)?|roles|role-path|room access|companion (?:stats|errors|help)|wifi (?:status|help)|auth (?:status(?: [0-9a-f]{64})?|peer [1-4])|bot (?:status|key|stats|log|diagnostics|admission|policy|mesh|limits|contention|name|destination(?: [1-4])?|channel-wait|shared|home|https(?: status)?|reminders|events|discovery(?: status)?|forward(?: from|to)?|help)|role (?:help|config \S+|name \S+|key \S+(?: pending)?|channel \S+ \d+)|key (?:help|\S+(?: pending)?)|source (?:status|hash|metadata|help|helptext|api(?: [a-z]+)?|read \d+)|data (?:status|help|read [0-9a-f]{16} \d+)|telemetry(?: (?:status|counts|times|tls|tls-heap|tls-blocks|help|endpoint (?:status|host|address|path|port)))?)$/;
const readonlyStats=/^(?:get stats|stats(?: (?:help|radio|signal|tx|airtime|admission|sensors|memory|psram|bot|vm|observer|companion))?)$/;
function nativeRead(text) {
  const normalized=text.trim().replace(/ +/g,' ').replace(/^source wasm /,'source ');
  return readonly.test(normalized) || readonlyStats.test(normalized);
}
async function nativeWrite(text,warning) {
  validateCommand(text);
  if(!confirmed(warning || 'Run "'+text+'"? This command can change settings or stop running work.')) return 'Cancelled; no write sent.';
  const result=await cmd(text);
  if(/^source (?:wasm )?cancel$/.test(text)) uploaded=null;
  if(text==='data clear') staged=null;
  if(/^(radio |tempradio |apply$|reboot$)/.test(text)) {
    effective=null;
    $('radio-status').textContent='Radio change pending. Check pending changes, then refresh status.';
  }
  return result;
}
function integer(id,min,max) {
  const text=$(id).value;
  if(!/^\d+$/.test(text) || +text<min || +text>max) throw Error(id+' requires an integer '+min+'..'+max+'.');
  return text;
}
function radioCommand(temporary) {
  const profile=[integer('freq',150000000,960000000),$('bw').value,$('sf').value,$('cr').value,integer('power',0,22)].join(' ');
  return (temporary?'tempradio '+integer('seconds',1,3600):'radio')+' '+profile;
}
function bind(id,label,work) { $(id).onclick=()=>action(label,work); }
$('login-form').onsubmit=e=>{e.preventDefault();action('Logging into mast administration',async()=>{
  const password=$('password').value; $('password').value='';
  const value=await post('/admin/login',password,true);
  if(!/^[0-9a-f]{32}$/.test(value)) throw Error('Login returned an invalid session token.');
  token=value; expires=Date.now()+600000;
  await refresh(); return 'Logged in.';
});};
bind('logout','Logging out',async()=>{
  try { return await post('/admin/logout'); } finally { forgetSession(); }
});
bind('refresh','Reading role and radio state',refresh);
async function updateRequest(method,body='',headers={}) {
  if(!token || Date.now()>=expires) {forgetSession();throw Error('Login again before checking the firmware update.');}
  const session=token,controller=new AbortController(),timer=setTimeout(()=>controller.abort(),method==='GET'?5000:190000);
  try {
    const response=await fetch('/admin/update'+(method==='REBOOT'?'/reboot':''),{method:method==='REBOOT'?'POST':method,
      headers:{'X-Mast-Session':session,...headers},body:method==='GET'?undefined:body,cache:'no-store',credentials:'omit',redirect:'error',signal:controller.signal});
    const text=await response.text();
    if(response.status===403) {
      if(!text.startsWith('{')) forgetSession();
      throw Error('Firmware update authorization failed; check the signed operator manifest and session. No application verification confirmed.');
    }
    if(response.status>=500) throw unknown('Firmware connection failed; update outcome unknown.');
    if(!response.ok) throw Error('Firmware HTTP '+response.status+': '+text);
    if(token!==session) throw unknown('Firmware session ended during the request; outcome unknown.');
    const value=JSON.parse(text);
    if(!['idle','receiving','failed','verified'].includes(value.state) || !Number.isSafeInteger(value.size) || value.size<0 ||
       !Number.isSafeInteger(value.received) || value.received<0 || value.received>value.size ||
       typeof value.target!=='string' || typeof value.reboot_ready!=='boolean' || !['healthy','pending'].includes(value.boot_health) ||
       typeof value.rollback_supported!=='boolean') throw Object.assign(Error('Invalid firmware status readback; no update outcome confirmed.'),{readback:true});
    firmwareStatus=value;
    const states={idle:'No update in progress',receiving:'Receiving update',failed:'Update failed',verified:'Verified; ready to restart'};
    const lines=['Update: '+states[value.state],'Device target: '+value.target,'Boot health: '+(value.boot_health==='healthy'?'Healthy':'Waiting for startup checks'),
      'Rollback support: '+(value.rollback_supported?'Supported':'Unavailable')];
    if(value.size) lines.push('Received: '+value.received+' / '+value.size+' bytes');
    if(value.error) lines.push('Error: '+value.error);
    if(value.recovery) lines.push('Recovery: '+value.recovery);
    if(value.running_hash_error) lines.push('Running image: '+value.running_hash_error);
    else if(value.running_sha256) lines.push('Running image: '+(Number.isSafeInteger(value.running_size)&&value.running_size>0?value.running_size+' bytes':'size unavailable')+'\nSHA256 '+value.running_sha256);
    if(value.sha256) lines.push('Update SHA256 '+value.sha256);
    $('firmware-detail').textContent=lines.join('\n');
    return value;
  } catch(e) {
    if(e.name==='AbortError' || e instanceof TypeError) throw unknown('Firmware connection lost or timed out; update outcome unknown.');
    if(method!=='GET' && (e.readback || e instanceof SyntaxError)) throw unknown('Invalid firmware mutation reply; update outcome unknown.');
    throw e;
  } finally {clearTimeout(timer);}
}
function firmwareConfirmed(item,status) {
  if(item.command==='firmware upload') return status.state==='verified' && status.reboot_ready &&
    status.sha256===item.image.sha256 && status.target===item.image.target && status.size===item.image.size && status.received===item.image.size;
  return item.command==='firmware reboot' && status.state==='idle' && !status.reboot_ready && status.size===0 && status.received===0 &&
    status.running_sha256===item.image.sha256 && status.running_size===item.image.size && status.boot_health==='healthy' && !status.running_hash_error;
}
bind('firmware-status','Reading firmware update and boot health',async()=>{
  const status=await updateRequest('GET');
  for(let n=unresolved.length-1;n>=0;n--) if(unresolved[n].image && firmwareConfirmed(unresolved[n],status)) unresolved.splice(n,1);
  uncertain=unresolved.length>0;
  const running=expectedFirmware && firmwareConfirmed({command:'firmware reboot',image:expectedFirmware},status);
  return running?'The uploaded image is running and healthy.':status.state==='verified'?
    'Update verified. Restart to activate it.':
    'Firmware status updated.'+(uncertain?' The previous update is still unconfirmed.':'');
});
bind('firmware-upload','Uploading signed application',async()=>{
  const image=$('firmware-image').files[0],file=$('firmware-manifest').files[0];
  if(!firmwareStatus || !['idle','failed'].includes(firmwareStatus.state)) throw Error('Check firmware update capability and state first.');
  if(!image || !file || !image.size || image.size>0x330000 || file.size>2048) throw Error('Choose an application up to 3342336 bytes and its bounded signed JSON manifest.');
  const bytes=new Uint8Array(await image.arrayBuffer()),meta=JSON.parse(await file.text());
  if(!meta || Array.isArray(meta) || Object.keys(meta).length!==4 || Object.keys(meta).some(k=>!['target','size','sha256','signature'].includes(k)))
    throw Error('Choose the offline signed release manifest with target, size, sha256 and signature only. Private signing keys are never accepted.');
  const marker=new TextEncoder().encode('meshcore-esp-target-v1:'+meta.target+':');
  const hasMarker=bytes.some((byte,n)=>byte===marker[0] && marker.every((value,offset)=>bytes[n+offset]===value));
  if(!/^[a-z0-9][a-z0-9-]{0,62}$/.test(meta.target) || meta.target!==firmwareStatus.target || meta.size!==bytes.length ||
     !/^[0-9a-f]{64}$/.test(meta.sha256) || !/^[0-9a-f]{128}$/.test(meta.signature) || sha256(bytes)!==meta.sha256 ||
     bytes.length<24 || bytes[0]!==0xe9 || bytes[12]!==9 || bytes[13]!==0 || !hasMarker)
    throw Error('Application target, size, ESP32-S3 image header or digest differs from the signed manifest; no upload sent.');
  if(!confirmed('Upload this '+meta.target+' application ('+meta.size+' bytes)? It will be verified before a separate restart. Saved roles and bot data are retained.\nSHA256 '+meta.sha256)) return 'Cancelled; no firmware upload sent.';
  expectedFirmware={target:meta.target,size:meta.size,sha256:meta.sha256};
  inFlight={command:'firmware upload',runtime:'firmware',mutation:true,image:{...expectedFirmware}}; lastMutation={...inFlight};
  try {
    $('firmware-detail').textContent='Uploading '+meta.size+' bytes. Waiting for verification.\nSHA256 '+meta.sha256;
    const status=await updateRequest('POST',bytes,{'Content-Type':'application/octet-stream','X-Mast-Target':meta.target,'X-Mast-SHA256':meta.sha256,'X-Mast-Signature':meta.signature});
    if(status.state!=='verified' || !status.reboot_ready || status.target!==meta.target || status.size!==meta.size || status.received!==meta.size)
      throw unknown('Firmware upload reply does not confirm verification of the submitted image.');
    if(status.sha256 && status.sha256!==meta.sha256) throw unknown('Firmware upload digest readback differs from the submitted signed file.');
    return 'Update verified. Restart to activate it, then log in and check firmware status.';
  } finally {inFlight=null;}
});
bind('firmware-reboot','Rebooting into verified application',async()=>{
  if(!firmwareStatus?.reboot_ready || !expectedFirmware) throw Error('This tab has no verified image snapshot; inspect the release manifest and update status before rebooting through the owner CLI.');
  if(!confirmed('Reboot every role into the verified application? Reconnect and login to check its running hash and boot health.')) return 'Cancelled; no reboot sent.';
  inFlight={command:'firmware reboot',runtime:'firmware',mutation:true,image:{...expectedFirmware}}; lastMutation={...inFlight};
  try {
    await updateRequest('REBOOT');
    throw unknown('Firmware reboot admitted; running application and health are not yet confirmed. Reconnect and login, then check firmware status.');
  } finally {inFlight=null;}
});
bind('inspect','Checking pending changes',async()=>{
  if(unresolved.some(x=>x.image)) {
    const status=await updateRequest('GET');
    for(let n=unresolved.length-1;n>=0;n--) if(unresolved[n].image && firmwareConfirmed(unresolved[n],status)) unresolved.splice(n,1);
    if(unresolved.some(x=>x.image)) return 'Firmware outcome remains unconfirmed; writes stay blocked. Check the signed image digest and running boot health.\n'+$('firmware-detail').textContent;
    if(!unresolved.length) {uncertain=false;return 'Firmware update confirmed.\n'+$('firmware-detail').textContent;}
  }
  const commands=['status','job'];
  for(const item of unresolved) {
    if(item.command.startsWith('source ')) commands.push(item.runtime==='wasm'?'source wasm status':'source status',item.runtime==='wasm'?'source wasm hash':'source hash');
    else if(item.command.startsWith('data ')) commands.push('data status');
  }
  await readGroup([...new Set(commands)],'role-detail');
  parseStatus(seen.status);
  for(let n=unresolved.length-1;n>=0;n--) {
    const item=unresolved[n], prefix=item.runtime==='wasm'?'source wasm ':'source ';
    let resolved=false;
    if(new RegExp('^'+prefix+'commit [0-9a-f]{16}$').test(item.command) && item.snapshot) {
      const current=manifest(seen[prefix+'hash']||'');
      const status=/^gen=(\d+) active=[0-3] prev=[0-3] size=(\d+) upload=(?:none|[0-9a-f]{16}) next=\d+\/\d+; source (?:durably saved and active|saved; bot disabled; loads on next enabled boot)$/.exec(seen[prefix+'status']||'');
      resolved=!!status && current.hash===item.snapshot.hash && current.generation===status[1] && +status[2]===item.snapshot.size;
    } else if(item.snapshot && item.command.startsWith(prefix)) {
      const status=/^gen=\d+ active=[0-3] prev=[0-3] size=\d+ upload=([0-9a-f]{16}) next=(\d+)\/(\d+); .+$/.exec(seen[prefix+'status']||'');
      const chunk=new RegExp('^'+prefix+'chunk ([0-9a-f]{16}) (\\d+) [0-9a-f]+$').exec(item.command);
      const begin=new RegExp('^'+prefix+'begin '+item.snapshot.id+' '+item.snapshot.size+' '+item.snapshot.hash+'$').test(item.command);
      resolved=!!status && status[1]===item.snapshot.id && +status[3]===Math.ceil(item.snapshot.size/48) &&
        (begin || (chunk && chunk[1]===status[1] && (+status[2]===+chunk[2] || +status[2]===+chunk[2]+1)));
    } else if(/^data restore [0-9a-f]{16}(?: no-rearm)?$/.test(item.command) && item.restore) {
      resolved=seen['data status']==='COMMITTED '+item.restore.id;
    }
    if(resolved) unresolved.splice(n,1);
  }
  uncertain=unresolved.length>0 || /UNKNOWN|outcome unknown|waiting for acceptance|PENDING|BUSY/.test($('role-detail').textContent);
  return (uncertain?'Changes are still unconfirmed. Further changes are blocked; check the affected settings or export the data.':'Changes confirmed. Controls are unlocked.')+
    '\n'+unresolved.map(x=>x.command+' ['+x.runtime+']'+(x.restore?' '+x.restore.kind+'/'+x.restore.scope+'/'+x.restore.principal:'')).join('\n')+'\n'+$('role-detail').textContent;
});
bind('role-read','Reading selected role',async()=>{
  const role=$('role').value;
  return await readGroup(['role config '+role,'role name '+role],'role-detail');
});
bind('role-save','Saving role name',async()=>{
  const name=$('role-name').value;
  if(!/^[\x20-\x39\x3b-\x7e]{1,31}$/.test(name)) throw Error('Name requires 1..31 printable ASCII bytes without colon.');
  return await nativeWrite('role name '+$('role').value+' '+name,'Save this name? The role keeps its identity.');
});
bind('role-advert','Sending role advert',()=>nativeWrite('role advert '+$('role').value+' zerohop','Send a zero-hop advert for this role?'));
bind('radio-copy','Copying effective profile',()=>{
  if(!effective) throw Error('Refresh radio status first.');
  if(!window.confirm('Replace the form with the current radio settings? Temporary settings may be active.')) return 'Cancelled; form unchanged.';
  ['freq','bw','sf','cr','power'].forEach((id,n)=>$(id).value=effective.phy[n]);
  return 'Current settings loaded into the form.';
});
for(const [id,temp] of [['radio-save',false],['radio-temp',true]]) bind(id,'Changing shared PHY',async()=>{
  const text=radioCommand(temp);
  return await nativeWrite(text,(temp?'Retune temporarily, then return to the saved channel?':'Save and retune this channel for all roles?')+'\n'+text+'\nRF administration may disconnect. Queued packets on the old channel will be cancelled (STALE).');
});
bind('inventory','Reading program inventory / capabilities',inventory);
bind('hello','Creating a hello draft',()=>{
  if($('source').value && !window.confirm('Replace this editor draft with the hello template? Download unsaved edits first.')) return 'Cancelled; draft retained.';
  $('source').value="function hello(name) reply('Hello '..name) end"; draftInfo(); return 'Hello example ready. Upload and install it to activate the command.';
});
$('source').oninput=draftInfo;
$('program-runtime').onchange=()=>{uploaded=null; supported.source=false; return action('Reading selected runtime',inventory);};
$('wasm-file').onchange=()=>action('Reading Wasm binary file',async()=>{
  wasmBytes=null; uploaded=null;
  const file=$('wasm-file').files[0];
  if(!file || !file.size || file.size>4096) throw Error('Wasm file must contain 1..4096 bytes.');
  const bytes=new Uint8Array(await file.arrayBuffer());
  let offset=0;
  const magic=[0,97,115,109,1,0,0,0];
  if(bytes[0]!==0) {
    const end=bytes.indexOf(10);
    if(end<0 || end>255) throw Error('Wasm package requires a bounded metadata header.');
    const header=String.fromCharCode(...bytes.slice(0,end));
    if(!header.startsWith('--@meshcore-bot/1;') || !header.includes(';runtime=wamr-2.4.1;api=meshcore-v1;'))
      throw Error('Selected file does not declare the supported Wasm runtime/API.');
    offset=end+1;
  }
  if(!magic.every((b,n)=>bytes[offset+n]===b)) throw Error('Expected portable Wasm version 1, not Lua source or AOT.');
  wasmBytes=bytes;
  $('wasm-info').textContent=file.name+' · '+bytes.length+' binary bytes · SHA256 '+sha256(bytes)+' · not installed';
  return 'Wasm file selected. Upload and install it to activate its commands.';
});
bind('source-hash','Hashing editor draft',()=>{const b=sourceBytes();return 'Editor draft '+b.length+' bytes · SHA256 '+sha256(b);});
bind('source-draft','Downloading local draft',()=>{const wasm=programRuntime()==='wasm';download(wasm?sourceBytes():new TextEncoder().encode($('source').value),wasm?'meshcore-draft.mcbot':'meshcore-draft.lua',wasm?'application/octet-stream':'text/plain');return 'Local draft downloaded; no device command sent.';});
bind('source-read','Reading verified durable source',readSource);
bind('source-stage','Uploading / resuming draft snapshot',uploadSource);
bind('source-install','Verifying / installing uploaded snapshot',installSource);
for(const op of ['rollback','remove','retry']) bind('source-'+op,'Recovering source: '+op,()=>recoverSource(op));
bind('help-read','Reading source help',async()=>{
  const old=$('helptext').value;
  if(old && !window.confirm('Replace local source help edits with saved help?')) return 'Cancelled; help edits retained.';
  const text=await sourceCmd('source helptext',true);
  if($('helptext').value!==old) throw Error('Help editor changed during read; edits retained.');
  $('helptext').value=text==='(no source help)'?'':text; return 'Saved help read as plain text.';
});
bind('help-save','Saving help text',()=>nativeWrite(sourceCommand('source help '+($('helptext').value||'-')),'Save this help text? Program code will stay unchanged.'));
bind('storage-read','Reading storage / backup capabilities',async()=>{
  supported.data=false;
  await readGroup(['source api storage','source api data','source api notes','source api reminders','bot limits','data status'],'storage-status');
  supported.data=/^Data kv=1 timers=1 reminders=1 scope=single bytes=2422 version=1 /.test(seen['source api data']||'');
  return supported.data?'Backup controls are ready.':'This firmware does not support these backup controls.';
});
$('data-scope').onchange=()=>{if($('data-scope').value==='bot') $('principal').value='0'.repeat(64);};
$('backup').onchange=()=>action('Inspecting local backup envelope',inspectBackup);
bind('data-export','Exporting private scoped data',exportData);
bind('data-stage','Staging private backup',stageData);
bind('data-restore','Restoring explicitly staged scope',restoreData);
bind('telemetry-read','Reading telemetry readiness',async()=>{
  supported.telemetry=false;
  await readGroup(['telemetry status','telemetry endpoint status'],'telemetry-status');
  supported.telemetry=/^Telemetry on=[01] interval=\d+ /.test(seen['telemetry status']||'');
  return supported.telemetry?'Telemetry controls supported; endpoint trust/credentials remain restricted.':'Telemetry unavailable; no configuration controls enabled.';
});
bind('telemetry-save','Saving telemetry publishing',()=>nativeWrite('telemetry '+$('telemetry-on').value,'Persist and apply telemetry publishing state? Enabled publishing sends device metrics to the configured endpoint; inspect readiness/errors.'));
bind('interval-save','Saving telemetry interval',()=>nativeWrite('telemetry interval '+integer('interval',30,86400),'Persist and apply the metrics publishing interval?'));
bind('service-read','Reading services',()=>readGroup(['bot home','bot shared','bot reminders','bot discovery','bot mesh','companion stats','companion errors','wifi status'],'service-status'));
bind('grant-read','Reading bot grant',()=>readGroup(['bot '+$('grant').value],'service-status'));
bind('grant-save','Saving bot permission',()=>{
  const grant=$('grant').value,on=$('grant-on').value==='on';
  const permissions={shared:['shared data and timers','Commands can access shared bot and channel data.'],
    home:['the configured HTTPS service','Commands can call the configured service.'],
    reminders:['private reminders','Reminders can send private messages without a new request.'],
    discovery:['Base telemetry and path discovery','Base telemetry is shared with signed contacts.'],
    'channel-wait':['channel follow-up messages','Channel senders are not individually authenticated.']};
  if(!permissions[grant]) throw Error('Choose a bot permission.');
  const [name,detail]=permissions[grant];
  return nativeWrite('bot '+grant+' '+(on?'on':'off'),(on?'Allow ':'Disable ')+name+'?\n'+(on?detail:'Running work that needs this permission will stop.'));
});
$('role').onchange=updateControls;
$('command-form').onsubmit=e=>{e.preventDefault(); action('Running native command',async()=>{
  const text=$('command').value; validateCommand(text);
  return nativeRead(text)?await cmd(text,text==='source helptext'):await nativeWrite(text);
});};
document.querySelectorAll('[data-read]').forEach(button=>button.onclick=()=>action('Reading '+button.dataset.read,async()=>{
  const value=await cmd(sourceCommand(button.dataset.read)); $(button.dataset.target).textContent=value; return value;
}));
document.querySelectorAll('[data-write]').forEach(button=>button.onclick=()=>action('Native change: '+button.dataset.write,()=>nativeWrite(sourceCommand(button.dataset.write),button.dataset.warning)));
setInterval(()=>{
  if(!token) return;
  const remaining=expires-Date.now();
  if(remaining<=0) { forgetSession(); show('Admin session expired. Login again; drafts remain in this tab. An in-flight command may have an unknown outcome.','warning'); }
  else $('session').textContent='Logged in · expires in '+Math.ceil(remaining/60000)+' min';
},1000);
window.addEventListener('beforeunload',event=>{if($('source').value) {event.preventDefault();event.returnValue='';}});
updateControls();
</script></body></html>)html";
