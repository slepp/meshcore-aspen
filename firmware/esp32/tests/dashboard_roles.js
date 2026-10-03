// Exercise the shipped UI against the actual native-role status fixture.
const assert = require('node:assert/strict');
const fs = require('node:fs');
const vm = require('node:vm');
const script = fs.readFileSync(process.argv[2], 'utf8').split('<script>')[1].split('</script>')[0];
const snapshot = JSON.parse(fs.readFileSync(process.argv[3], 'utf8'));
assert.deepEqual(snapshot.roles.map(r => r.role), ['repeater', 'room', 'companion', 'observer', 'bot', 'management']);
assert.equal(new Set(snapshot.roles.map(r => r.public_key)).size, 6);
assert.equal(snapshot.roles[2].name, 'caf\u00e9');
for (const role of snapshot.roles) {
  assert.match(role.public_key, /^[0-9a-f]{64}$/);
  assert.equal(role.ready, role.role !== 'management');
  assert.equal(role.fault, null);
}
const elements = new Map();
class Element {
  constructor() { this.children = []; this.style = {}; this.clientWidth = 0; this.value = 'all'; }
  addEventListener() {}
  setAttribute() {}
  appendChild(child) { this.children.push(child); }
  replaceChildren(...children) { this.children = children; }
  set innerHTML(value) { assert.fail('Device data must not become HTML: ' + value); }
}
const element = id => {
  if (!elements.has(id)) elements.set(id, new Element());
  return elements.get(id);
};
let socket;
vm.runInNewContext(script, {
  document: {getElementById: element, createElement: () => new Element(), addEventListener() {}, hidden: false},
  window: {addEventListener() {}}, location: {protocol: 'http:', host: 'radio.local'},
  WebSocket: class { constructor() { socket = this; } close() { assert.fail('Valid role snapshot rejected'); } },
  Intl, Date, Math, console, setTimeout() {}, clearTimeout() {},
});
const show = data => socket.onmessage({data: JSON.stringify(data)});
show(snapshot);
assert.equal(element('role-panel').hidden, false);
assert.equal(element('roles').children.length, 6);
assert.equal(element('roles').children[4].children[0].textContent, 'KISS bot');
assert.equal(element('roles').children[5].children[0].textContent, 'Management');
assert.match(element('roles').children[5].children[2].textContent, /unprovisioned/);
assert.equal(snapshot.roles[5].profile_generation, '0');
const keyDetails = element('roles').children[0].children[3].children[0];
assert.equal(keyDetails.children[0].textContent, snapshot.roles[0].public_key.slice(0, 12) + '...');
assert.equal(keyDetails.children[1].textContent, snapshot.roles[0].public_key);
snapshot.scheduler.owner_slot = snapshot.roles[0].source_slot;
show(snapshot);
assert.equal(element('owner').textContent, 'Radio settings owner: Repeater');
const changed = structuredClone(snapshot);
changed.roles[1].name = '<img src=x onerror=alert(1)>';
changed.roles[0].ready = false;
changed.roles[0].state = 'fault';
changed.roles[0].fault = 'identity read failed';
changed.roles[0].public_key = null;
show(changed);
assert.equal(element('roles').children[1].children[1].textContent, changed.roles[1].name);
assert.equal(element('roles').children[0].children[2].textContent, 'fault / identity read failed');
assert.equal(element('roles').children[0].children[3].children[0].textContent, 'Unavailable');
const event = {direction: 'tx', state: 2, reason: 0, length: 3, at_ms: snapshot.uptime_ms,
  source_slot: snapshot.roles[0].source_slot, source_generation: snapshot.roles[0].source_generation,
  queue_ms: 0, rf_ms: 20, estimated_ms: 20, rssi_dbm: null, snr_db: null,
  preview_hex: '010203', preview_truncated: false};
snapshot.history.events = [event];
show(snapshot);
assert.match(element('events').children[0].children[2].textContent, /Repeater/);
event.source_generation++;
show(snapshot);
assert.doesNotMatch(element('events').children[0].children[2].textContent, /Repeater/);
delete snapshot.roles;
show(snapshot);
assert.equal(element('role-panel').hidden, true);
assert.equal(element('roles').children.length, 0);
console.log('Dashboard roles: native public identities, names, faults, generation labels and radio-only hiding passed');
