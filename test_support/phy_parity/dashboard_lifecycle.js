// Execute the shipped script with a deterministic browser clock and transport.
const assert = require('node:assert/strict');
const fs = require('node:fs');
const vm = require('node:vm');
const script = fs.readFileSync(process.argv[2], 'utf8').split('<script>')[1].split('</script>')[0];
const snapshot = JSON.parse(fs.readFileSync(process.argv[3], 'utf8'));
let now = 0, nextTimer = 0;
const timers = new Map(), elements = new Map(), connections = [];
class Element {
  constructor() { this.handlers = {}; this.style = {}; this.clientWidth = 0; this.value = 'all'; }
  addEventListener(name, callback) { this.handlers[name] = callback; }
  setAttribute() {}
  appendChild() {}
  replaceChildren() {}
}
const document = {
  hidden: false, handlers: {},
  getElementById(id) {
    if (!elements.has(id)) elements.set(id, new Element());
    return elements.get(id);
  },
  createElement: () => new Element(),
  addEventListener(name, callback) { this.handlers[name] = callback; },
};
class Socket {
  constructor(url) {
    assert.equal(url, 'ws://radio.local/api/live');
    assert.equal(connections.filter(c => !c.closed).length, 0, 'parallel stream');
    connections.push(this);
  }
  close() { this.closing = true; }
  finishClose() { this.closed = true; this.onclose(); }
  message(data = snapshot) { this.onmessage({data: JSON.stringify(data)}); }
}
function advance(milliseconds) {
  const until = now + milliseconds;
  for (;;) {
    const ready = [...timers].filter(([, t]) => t.at <= until).sort((a, b) => a[1].at - b[1].at)[0];
    if (!ready) break;
    now = ready[1].at; timers.delete(ready[0]); ready[1].callback();
  }
  now = until;
}
vm.runInNewContext(script, {
  document, window: {addEventListener() {}}, location: {protocol: 'http:', host: 'radio.local'},
  WebSocket: Socket, Intl, Date, Math, console,
  setTimeout(callback, delay) {
    const id = ++nextTimer; timers.set(id, {callback, at: now + delay}); return id;
  },
  clearTimeout(id) { timers.delete(id); },
  fetch() { assert.fail('dashboard must not poll HTTP'); },
});
const element = id => document.getElementById(id);
const click = id => element(id).handlers.click();
const visibility = hidden => { document.hidden = hidden; document.handlers.visibilitychange(); };
assert.equal(connections.length, 1);
connections[0].message();
assert.equal(element('connection').textContent, 'Live');
assert.equal(element('notice').hidden, true);
assert.equal(element('firmware-version').textContent,snapshot.firmware_version);
assert.equal(element('admin-link').hidden,true,'Standalone modem stats must not link to an unavailable admin page');
connections[0].message({...snapshot, roles:[{role:'management',name:'Admin',ready:true,public_key:'',source_slot:null}]});
assert.equal(element('admin-link').hidden,false,'On-device administration must be reachable from stats');
connections[0].message({...snapshot, profile:{...snapshot.profile,fault:true}});
assert.equal(element('profile-state').textContent,'TX disabled');
assert.equal(element('notice').hidden,false,'Radio faults must remain visible');
connections[0].message();
connections[0].message({...snapshot, memory: {
  ...snapshot.memory, dma_free_bytes: 321, dma_largest_bytes: 123, dma_minimum_bytes: 17,
}});
assert.equal(element('dma-heap').textContent, '321 / 123 / 17 B', 'DMA bytes must not round to KiB');
connections[0].message({...snapshot, memory: {
  ...snapshot.memory, dma_free_bytes: 0, dma_largest_bytes: 0, dma_minimum_bytes: 0,
}});
assert.equal(element('dma-heap').textContent, '0 / 0 / 0 B', 'zero is a valid exhausted heap reading');
connections[0].message({...snapshot, memory: {free_bytes: 66000, minimum_bytes: 8904}});
assert.equal(element('dma-heap').textContent, 'Unavailable', 'older snapshots must not invent DMA readings');
connections[0].message();
click('pause');
assert.equal(element('connection').textContent, 'Paused');
connections[0].message();
assert.equal(element('connection').textContent, 'Paused', 'late data must not resume paused UI');
click('pause'); visibility(false); click('refresh');
assert.equal(connections.length, 1, 'resume waits for the closing connection');
connections[0].finishClose();
advance(1500);
assert.equal(connections.length, 2);
connections[1].message();
visibility(true);
connections[1].finishClose();
advance(20000);
assert.equal(connections.length, 2, 'hidden page must not reconnect');
visibility(false); visibility(false);
assert.equal(connections.length, 3);
connections[2].message();
advance(6000);
assert.equal(element('connection').textContent, 'Stale');
assert.equal(connections[2].closing, true, 'stalled stream must close');
connections[2].finishClose();
advance(1500);
assert.equal(connections.length, 4);
connections[3].message();
assert.equal(element('connection').textContent, 'Live');
const priorHeading = element('device-name').textContent;
connections[3].message({api_version: 99});
assert.equal(connections[3].closing, true);
assert.equal(element('device-name').textContent, priorHeading, 'bad update must retain last valid snapshot');
connections[3].finishClose();
for (let attempt = 0; attempt < 8; ++attempt) {
  advance(11000);
  const current = connections.at(-1);
  current.finishClose();
}
assert.equal(connections.filter(c => !c.closed).length, 0);
click('pause');
advance(60000);
assert.equal(connections.filter(c => !c.closed).length, 0);
console.log('Dashboard push lifecycle: pause/hide/resume, no duplicate streams, stale/reconnect, invalid data passed');
