// Exercise the shipped browser decoder against native API data, without a radio.
const assert = require('node:assert/strict');
const fs = require('node:fs');
const vm = require('node:vm');
const script = fs.readFileSync(process.argv[2], 'utf8').split('<script>')[1].split('</script>')[0];
const snapshot = JSON.parse(fs.readFileSync(process.argv[3], 'utf8'));
const elements = new Map();
class Element {
  constructor() { this.children = []; this.style = {}; this.clientWidth = 0; this.value = 'all'; this.replacements = 0; }
  addEventListener() {}
  setAttribute() {}
  appendChild(child) { this.children.push(child); }
  replaceChildren(...children) { this.children = children; this.replacements++; }
  set innerHTML(value) { assert.fail('Radio data interpreted as HTML: ' + value); }
}
const element = id => {
  if (!elements.has(id)) elements.set(id, new Element());
  return elements.get(id);
};
const context = vm.createContext({
  document: {getElementById: element, createElement: () => new Element(), addEventListener() {}, hidden: false},
  window: {addEventListener() {}}, location: {protocol: 'http:', host: 'radio.local'},
  WebSocket: class { close() {} }, Intl, Date, Math, console, setTimeout() {}, clearTimeout() {},
});
vm.runInContext(script, context);
const call = (name, ...args) => {
  context.args = args;
  return vm.runInContext(name + '(...args)', context);
};
const identities = call('knownIdentities', {roles: [
  {name: '<img src=x onerror=alert(1)>', public_key: 'aa1122' + '00'.repeat(29)},
  {name: 'Second', public_key: 'aa3344' + '00'.repeat(29)},
  {name: 'Single', public_key: 'bb5566' + '00'.repeat(29)},
  {name: 'Invalid', public_key: '<script>'},
]});
assert.equal(call('prefixLabel', 'aa', identities), 'aa [2 matches]');
assert.equal(call('prefixLabel', 'aa1122', identities), 'aa1122 (<img src=x onerror=alert(1)>)');
assert.equal(call('prefixLabel', 'bb', identities), 'bb (Single)');
assert.equal(call('prefixLabel', 'cc', identities), 'cc');
assert.equal(call('prefixLabel', '', identities), '');
const duplicate = call('knownIdentities', {roles: [
  {name: 'Same', public_key: 'bb'.repeat(32)},
  {name: 'Same', public_key: 'bb'.repeat(32)},
]});
assert.equal(call('prefixLabel', 'bb', duplicate), 'bb (Same)', 'deduplicate the same public identity');
const aliases = call('knownIdentities', {roles: [
  {name: 'One', public_key: 'bb'.repeat(32)},
  {name: 'Other', public_key: 'bb'.repeat(32)},
]});
assert.equal(call('prefixLabel', 'bb', aliases), 'bb', 'do not pick an arbitrary name');
const decode = (hex, length = hex.length / 2, names = identities) =>
  call('decodePacket', {preview_hex: hex, length}, names);
// Native v0: header, optional two little-endian transport codes, packed path,
// then the payload (ObserverWire.cpp / meshcore-go v1.5.0 PacketFromBytes).
assert.equal(decode('0900bbaa0000'),
  'Private text / Flood / v0 / encrypted\nPath 0 × 1 B\nsrc aa [2 matches] → dst bb (Single)');
for (const [packed, path, width] of [[1, 'bb', 1], [65, 'bb55', 2], [129, 'bb5566', 3]]) {
  const wire = '0a' + packed.toString(16).padStart(2, '0') + path + 'bbaa0000';
  assert.match(decode(wire), new RegExp('Direct[^\\n]*\\nRoute 1 × ' + width + ' B'));
  assert.match(decode(wire), /\(Single\)/);
}
for (const route of [0, 3]) {
  assert.match(decode((8 | route).toString(16).padStart(2, '0') + '3412cdab00bbaa0000'), /transport 0x1234, 0xabcd/);
}
for (const version of [1, 2, 3]) {
  assert.match(decode((9 | version << 6).toString(16) + '00bbaa0000'), /Unsupported payload version/);
  assert.doesNotMatch(decode((9 | version << 6).toString(16) + '00bbaa0000'), /src|Single/);
}
assert.match(decode('310001'), /Unknown type 12/);
assert.match(decode('3e0001'), /Custom \/ Direct/);
assert.equal(decode(''), 'No packet bytes');
assert.equal(decode('', 10), 'Packet preview unavailable');
for (const hex of ['0', 'zz', '00<script>', '00'.repeat(17)]) assert.match(decode(hex), /Malformed/);
for (const length of [-1, 256, 1.5, null, 0]) assert.match(decode('0900', length), /Malformed/);
for (const hex of ['09', '0800', '08', '0900', '0a01', '0ac000', '0a6100', '0a9600'])
  assert.match(decode(hex), /Malformed/, hex);
assert.match(decode('090000', 187), /invalid path\/payload length/);
assert.equal(decode('09000000', 186),
  'Private text / Flood / v0 / encrypted\nPath 0 × 1 B\nsrc 00 → dst 00',
  'truncated ciphertext does not add a redundant preview warning');
assert.match(decode('090000'), /short private envelope/);
assert.match(decode('0a0f' + 'bb'.repeat(14), 21), /Preview missing path bytes/);
assert.doesNotMatch(decode('0a4101', 10), /Single/, 'incomplete two-byte path hash must not resolve');
assert.match(decode('0900bb', 20), /Preview missing source\/destination/);
assert.equal(decode('1100' + 'bb5566'.repeat(4) + 'bb55', 102),
  'Advert / Flood / v0\nPath 0 × 1 B\nkey ' + 'bb5566'.repeat(4) + 'bb55');
assert.match(decode('110001'), /short advert/);
assert.equal(decode('1d00bb', 37),
  'Anonymous request / Flood / v0 / encrypted\nPath 0 × 1 B\nsrc anonymous → dst bb (Single)');
assert.equal(decode('1500bb0000'), 'Group text / Flood / v0 / encrypted\nPath 0 × 1 B\nchannel bb');
assert.doesNotMatch(decode('1500bb0000'), /Single/, 'channel hashes are not contact prefixes');
assert.match(decode('0d0012345678'), /ref 12345678/);
assert.match(decode('0d0012'), /ACK needs 4 bytes/);
assert.match(decode('2601bb' + '00'.repeat(9)), /Signal 1 B: bb/);
assert.doesNotMatch(decode('2601bb' + '00'.repeat(9)), /Single|Route/);
assert.match(decode('0900BBaa0000'), /dst bb \(Single\)/);
for (let header = 0; header < 256; header++) {
  for (let packed = 0; packed < 256; packed++) {
    assert.equal(typeof decode(header.toString(16).padStart(2, '0') +
      packed.toString(16).padStart(2, '0') + '00'), 'string');
  }
}
for (const [milliseconds, expected] of [
  [-1, '0s'], [0, '0s'], [999, '0s'], [1000, '1s'], [59999, '59s'],
  [60000, '1m'], [3599999, '59m'], [3600000, '1h'], [86399999, '23h'],
  [86400000, '1d'], [172800000, '2d'], [NaN, '-'],
]) assert.equal(call('activityAge', milliseconds), expected);
snapshot.roles = [
  {role: 'repeater', name: '<img src=x onerror=alert(1)>', public_key: 'bb'.repeat(32),
    source_slot: 4, source_generation: 1},
];
snapshot.uptime_ms = 59999;
snapshot.history.events = [{sequence: 1, at_ms: 0, direction: 'rx', state: 0, length: 6,
  queue_ms: 0, rf_ms: null, estimated_ms: 20, rssi_dbm: -70, snr_db: 4.25,
  preview_hex: '0900bbaa0000', preview_truncated: false}];
call('renderEvents', snapshot);
const row = element('events').children[0], age = row.children[0];
assert.equal(age.textContent, '59s');
assert.equal(row.children[1].textContent, 'Received over RF');
assert.equal(row.children[5].textContent, '-70 dBm / 4.25 dB');
assert.match(row.children[6].textContent, /<img src=x onerror=alert\(1\)>/);
const replacements = element('events').replacements;
snapshot.uptime_ms = 60000;
call('renderEvents', snapshot);
assert.equal(element('events').children[0], row, 'age-only update preserves rows');
assert.equal(age.textContent, '1m');
snapshot.uptime_ms += 500;
snapshot.roles[0].ready = true;
call('renderEvents', snapshot);
assert.equal(element('events').replacements, replacements, 'subsecond/role status update must not rebuild history');
for (const direction of ['tx', 'local', 'other']) {
  snapshot.history.events[0].direction = direction;
  snapshot.history.events[0].state = 4;
  call('renderEvents', snapshot);
  assert.equal(element('events').children[0].children[5].textContent, '-');
}
assert.equal(call('decodePacket', {preview_hex: null, length: 3}, identities), 'Malformed packet preview');
snapshot.history.events[0].direction = 'tx';
call('renderEvents', snapshot);
assert.equal(element('events').children[0].children[1].textContent, 'Unconfirmed');
snapshot.history.events[0].state = 2;
call('renderEvents', snapshot);
assert.equal(element('events').children[0].children[1].textContent, 'Sent over RF');
element('filter').value = 'rx';
call('renderEvents', snapshot);
assert.equal(element('events').children.length, 0);
assert.equal(element('events-empty').hidden, false);
element('filter').value = 'all';
snapshot.history.events[0].direction = 'rx';
snapshot.history.events[0].rssi_dbm = 127;
snapshot.history.events[0].snr_db = -32;
call('renderEvents', snapshot);
assert.equal(element('events').children[0].children[5].textContent, '-', 'local-loopback marker is not an RF reading');
snapshot.history.events.unshift({...snapshot.history.events[0], sequence: 2, at_ms: 60000,
  length: 4, preview_hex: '09000000'});
const before = JSON.stringify(snapshot);
call('renderEvents', snapshot);
assert.equal(element('events').children[0].children[0].textContent, '0s');
assert.equal(element('events').children[1].children[0].textContent, '1m');
assert.equal(element('events').children[0].children[7].children[0].textContent, '09 00 00 00');
assert.equal(JSON.stringify(snapshot), before, 'retain newest-first order and all raw API fields');
const meshContacts = {
  capacity: 32, total: 3, truncated: false,
  items: [
    {public_key: 'd11122' + '00'.repeat(29), name: 'D1', type: 2},
    {public_key: 'd13344' + '00'.repeat(29), name: 'D4 <script>', type: 2},
    {public_key: 'bb5566' + '00'.repeat(29), name: 'Wye', type: 1},
  ],
};
const radios = call('knownIdentities', {contacts: meshContacts});
assert.equal(call('prefixLabel', 'd1', radios), 'd1 [2 matches]');
assert.equal(call('prefixLabel', 'd111', radios), 'd111 (D1)');
assert.equal(call('prefixLabel', 'd13344', radios), 'd13344 (D4 <script>)');
assert.equal(decode('0a82d11122d13344bbd10000', undefined, radios),
  'Private text / Direct / v0 / encrypted\nRoute 2 × 3 B: d11122 (D1) → d13344 (D4 <script>)\nsrc d1 [2 matches] → dst bb (Wye)');
const withRole = call('knownIdentities', {contacts: meshContacts,
  roles: [{public_key: meshContacts.items[0].public_key, name: 'Old role name'}]});
assert.equal(call('prefixLabel', 'd11122', withRole), 'd11122 (D1)');
for (const contactData of [
  {...meshContacts, total: 35, truncated: true},
  {...meshContacts, total: 4},
  {...meshContacts, items: [meshContacts.items[0], null, meshContacts.items[2]]},
]) {
  const partial = call('knownIdentities', {contacts: contactData});
  assert.equal(call('prefixLabel', 'd111', partial), 'd111 (D1?)');
  assert.equal(call('prefixLabel', meshContacts.items[0].public_key, partial),
    meshContacts.items[0].public_key + ' (D1)', 'full key does not need a candidate marker');
}
const tooMany = call('knownIdentities', {contacts: {...meshContacts, total: 100, truncated: true}});
assert.equal(call('prefixLabel', 'd1', tooMany), 'd1 [2 matches]');
assert.equal(call('prefixLabel', 'cc', tooMany), 'cc');
const screenshotContacts = call('knownIdentities', {contacts: {
  total: 40, truncated: true, items: [{public_key: 'b726' + '00'.repeat(30), name: 'SLP AR1', type: 2}],
}});
const compact = decode('014456000700d726b726559aed2005', 30, screenshotContacts);
assert.equal(compact,
  'Request / Flood / v0 / encrypted\nPath 4 × 2 B: 5600 → 0700 → d726 → b726 (SLP AR1?)\nsrc 9a → dst 55');
assert.equal(compact.split('\n').length, 3);
assert.ok(compact.length < 130, 'four-hop screenshot packet stays compact');
assert.doesNotMatch(compact, /incomplete|candidate|prefix match|preview|Repeater/);
const page = fs.readFileSync(process.argv[2], 'utf8');
assert.match(page, /\.packet-detail \{ white-space: pre-line;/);
assert.match(page, /#events td \{ vertical-align: top; padding-top: 8px; padding-bottom: 8px;/);
assert.equal((page.match(/Names match public-key prefixes/g) || []).length, 1);
snapshot.contacts = meshContacts;
snapshot.history.events = [{...snapshot.history.events[0], preview_hex: '0a82d11122d13344bbd10000', length: 12}];
call('renderEvents', snapshot);
assert.match(element('events').children[0].children[6].textContent, /D4 <script>/);
assert.match(element('contact-detail').textContent, /3 of 3/);
const contactRow = element('events').children[0];
snapshot.uptime_ms += 500;
call('renderEvents', snapshot);
assert.equal(element('events').children[0], contactRow);
snapshot.contacts.items[0].name = 'Renamed D1';
call('renderEvents', snapshot);
assert.match(element('events').children[0].children[6].textContent, /Renamed D1/);
snapshot.contacts = {...meshContacts, total: 35, truncated: true};
call('renderEvents', snapshot);
assert.match(element('events').children[0].children[6].textContent, /Renamed D1\?/);
assert.equal(element('contact-detail').textContent, 'Companion contacts: 3 of 35 (incomplete).');
delete snapshot.contacts;
call('renderEvents', snapshot);
assert.match(element('contact-detail').textContent, /unavailable/);
console.log('Dashboard activity: v0 bounds, routing widths, prefix ambiguity, text escaping, RF-only signal and age updates passed');
