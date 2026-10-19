// Gates that run before any storage is touched: an in-memory token bucket per
// client key and a write budget for the Registry. Both are per isolate and best
// effort; the Registry holds the authoritative counters once a signature passes.
import type {Env} from "./config";

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

const budget = {day: 0, base: 0, pending: 0, flushedAt: 0, minute: 0, minuteCount: 0};
export type BudgetFlush = (day: number, count: number) => Promise<number>;
/** Test seam: clears every per-isolate counter. */
export function resetGates(): void {
  buckets.clear();
  Object.assign(budget, {day: 0, base: 0, pending: 0, flushedAt: 0, minute: 0, minuteCount: 0});
}
export function intSetting(value: string | undefined, fallback: number, min = 1, max = 1_000_000_000): number {
  const n = Number(value);
  return value !== undefined && value !== "" && Number.isSafeInteger(n) && n >= min && n <= max ? n : fallback;
}
/**
 * Counts one Registry-writing redeem request. Over the daily write budget the
 * service answers `closed`; above 600 anonymous redeems per minute it answers
 * `rate`. The count reaches the Registry at most once a minute per isolate.
 */
export async function admitRedeem(env: Env, flush: BudgetFlush, nowMs = Date.now()): Promise<"ok" | "closed" | "rate"> {
  const now = Math.floor(nowMs / 1000), day = Math.floor(now / 86400), minute = Math.floor(now / 60);
  if (budget.day !== day) Object.assign(budget, {day, base: 0, pending: 0, flushedAt: 0});
  if (budget.minute !== minute) Object.assign(budget, {minute, minuteCount: 0});
  if (++budget.minuteCount > 600) return "rate";
  const cap = intSetting(env.REGISTRY_WRITES_PER_DAY, 50000);
  if (budget.base + budget.pending + 1 > cap) return "closed";
  budget.pending++;
  if (now - budget.flushedAt >= 60) {
    const n = budget.pending;
    budget.flushedAt = now;
    try {
      budget.base = await flush(day, n);
      budget.pending -= n;
    } catch {/* keep counting in memory */}
  }
  return "ok";
}
