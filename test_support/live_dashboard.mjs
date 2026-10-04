import assert from 'node:assert/strict';
import { setTimeout as delay } from 'node:timers/promises';

const host = process.argv[2];
const seconds = Number(process.argv[3] ?? 30);
const count = Number(process.argv[4] ?? 2);
assert(host && Number.isFinite(seconds) && seconds > 0, 'Provide radio host and positive duration');
assert(count === 1 || count === 2, 'Select one or two live subscribers');
const url = new URL(`http://${host}/api/status`);
const live = new URL('/api/live', url);
live.protocol = 'ws:';
const subscribers = [];
let finished = false;

async function snapshot() {
  for (let attempt = 0; attempt < 3; attempt++) {
    const response = await fetch(url, {
      headers: { Connection: 'close' },
      signal: AbortSignal.timeout(5000),
    });
    if (response.status === 429) {
      await response.body.cancel();
      await delay(1000);
      continue;
    }
    assert.equal(response.status, 200, 'HTTP diagnostics must stay available');
    const data = await response.json();
    assert.equal(data.api_version, 1);
    return data;
  }
  throw new Error('HTTP diagnostics stayed rate-limited');
}

try {
  const initial = await snapshot();
  let fail;
  const failure = new Promise((_, reject) => { fail = reject; });
  for (let index = 0; index < count; index++) {
    const socket = new WebSocket(live);
    const subscriber = { socket, messages: 0, publication: -1, lastMessage: 0, maxClients: 0 };
    subscribers.push(subscriber);
    socket.addEventListener('message', event => {
      try {
        const data = JSON.parse(event.data);
        assert.equal(data.api_version, 1);
        assert(data.uptime_ms >= initial.uptime_ms, 'Radio rebooted during streaming');
        assert(data.publication > subscriber.publication, 'Stream repeated or reversed a snapshot');
        subscriber.publication = data.publication;
        subscriber.messages++;
        subscriber.lastMessage = Date.now();
        subscriber.maxClients = Math.max(subscriber.maxClients, data.kiss.connected);
      } catch (error) {
        fail(error);
      }
    });
    socket.addEventListener('error', event => {
      if (!finished) fail(new Error(`Subscriber ${index + 1} failed after ${subscriber.messages} updates: ${event.message ?? event.error?.message ?? 'connection error'}`));
    });
    socket.addEventListener('close', event => {
      if (!finished) fail(new Error(`Subscriber ${index + 1} disconnected after ${subscriber.messages} updates: ${event.code} ${event.reason}`));
    });
  }
  const controller = new AbortController();
  try {
    await Promise.race([
      failure,
      (async () => {
        const started = Date.now();
        while (Date.now() - started < seconds * 1000) {
          await delay(1000, undefined, { signal: controller.signal });
          if (Date.now() - started > 8000) {
            for (const subscriber of subscribers) {
              assert(Date.now() - subscriber.lastMessage < 8000, 'Live telemetry stalled');
            }
          }
        }
        for (const subscriber of subscribers) assert(subscriber.messages >= 2, 'Missing live updates');
        const final = await snapshot();
        assert(final.uptime_ms > initial.uptime_ms);
        console.log(JSON.stringify({
          dashboard: 'passed',
          subscribers: subscribers.map(({ messages, maxClients }) => ({ messages, maxClients })),
          freeHeap: final.memory.free_bytes,
          radioUptime: final.uptime_ms,
        }));
      })(),
    ]);
  } finally {
    controller.abort();
  }
} finally {
  finished = true;
  for (const { socket } of subscribers) socket.close();
}
