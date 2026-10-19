// Gates that run before any storage is touched: an in-memory token bucket per
// client key and a per-minute budget. Both are per isolate and best effort; the
// Registry holds the authoritative counters, including the daily write budget.

interface Window {start: number; count: number}
const buckets = new Map<string, Window>();
const BUCKET_CAP = 4096;

/** In-memory fixed-window counter. Returns the seconds to wait when the key is over its limit. */
export function memoryLimit(key: string, max: number, windowSeconds: number, now = Date.now() / 1000, cost = 1): number {
  let w = buckets.get(key);
  if (!w || now >= w.start + windowSeconds) w = {start: now, count: 0};
  buckets.delete(key); // refresh insertion order for least-recently-used eviction
  buckets.set(key, w);
  while (buckets.size > BUCKET_CAP) buckets.delete(buckets.keys().next().value!);
  if (w.count + cost > max) return Math.max(1, Math.ceil(w.start + windowSeconds - now));
  w.count += cost;
  return 0;
}

const budget = {minute: 0, minuteCount: 0};
/** Test seam: clears every per-isolate counter. */
export function resetGates(): void {
  buckets.clear();
  Object.assign(budget, {minute: 0, minuteCount: 0});
}
export function intSetting(value: string | undefined, fallback: number, min = 1, max = 1_000_000_000): number {
  const n = Number(value);
  return value !== undefined && value !== "" && Number.isSafeInteger(n) && n >= min && n <= max ? n : fallback;
}
/** Anonymous redeem requests per minute across this isolate; memory only. */
export function admitMinute(nowMs = Date.now()): boolean {
  const minute = Math.floor(nowMs / 60000);
  if (budget.minute !== minute) Object.assign(budget, {minute, minuteCount: 0});
  return ++budget.minuteCount <= 600;
}
