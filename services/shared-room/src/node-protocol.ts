// Node link protocol primitives: one-time codes, signed messages and fixed
// error texts. Pure functions; no storage and no secrets are logged here.
import {NODE_PROTOCOL} from "./protocol";

export const CROCKFORD = "0123456789ABCDEFGHJKMNPQRSTVWXYZ";
const encoder = new TextEncoder();

export const toHexBytes = (b: Uint8Array): string => Array.from(b, v => v.toString(16).padStart(2, "0")).join("");
export async function sha256Hex(text: string | Uint8Array): Promise<string> {
  const data = typeof text === "string" ? encoder.encode(text) : text;
  return toHexBytes(new Uint8Array(await crypto.subtle.digest("SHA-256", data)));
}

/** Canonical 16-symbol code, or undefined. `O` is 0, `I` and `L` are 1, `U` is rejected. */
export function canonicalCode(input: unknown): string | undefined {
  if (typeof input !== "string" || input.length > 64) return;
  let out = "";
  for (const ch of input.toUpperCase()) {
    if (ch === "-" || /\s/.test(ch)) continue;
    const c = ch === "O" ? "0" : ch === "I" || ch === "L" ? "1" : ch;
    if (c.length !== 1 || !CROCKFORD.includes(c)) return;
    out += c;
  }
  return out.length === 16 ? out : undefined;
}
export const formatCode = (canonical: string): string => canonical.match(/.{4}/g)!.join("-");
export function codeHash(canonical: string): Promise<string> {return sha256Hex("aspen-node.v1.code\n" + canonical);}
/** 80 random bits as 16 Crockford symbols. */
export function randomCode(): string {
  const bytes = crypto.getRandomValues(new Uint8Array(10));
  let bits = 0n;
  for (const b of bytes) bits = (bits << 8n) | BigInt(b);
  let out = "";
  for (let i = 15; i >= 0; i--) out += CROCKFORD[Number((bits >> BigInt(i * 5)) & 31n)];
  return out;
}

export type NodeOp = "join" | "create" | "convert";
export interface Signed {origin: string; op: NodeOp; code: string; node: string; ts: number; subject: string}
/** `code` is the first 32 hex characters of the code hash, or "open". */
export function signedMessage(s: Signed): Uint8Array {
  return encoder.encode([NODE_PROTOCOL, s.origin, s.op, s.code, s.node, String(s.ts), s.subject].join("\n"));
}
export async function createSubject(name: string): Promise<string> {return (await sha256Hex(name)).slice(0, 16);}

/** Name: 1..31 bytes printable ASCII excluding `: [ ] \ , ? *`. */
export function validName(value: unknown): value is string {
  return typeof value === "string" && value.length >= 1 && value.length <= 31 &&
    /^[\x20-\x7e]+$/.test(value) && !/[:\[\]\\,?*]/.test(value);
}
/** Room password: 0..15 printable ASCII. */
export function validPassword(value: unknown): value is string {
  return typeof value === "string" && value.length <= 15 && /^[\x20-\x7e]*$/.test(value);
}
export function generatedPassword(): string {
  const bytes = crypto.getRandomValues(new Uint8Array(10));
  return Array.from(bytes, b => CROCKFORD[b & 31]).join("");
}

export interface Failure {status: number; e: string; m: string}
const failures: Record<string, [number, string]> = {
  code_invalid: [400, "Code not valid for this request."],
  code_expired: [400, "Code expired."],
  code_used: [400, "Code already used."],
  bad_request: [400, "Request not understood."],
  name: [400, "Room name not accepted."],
  key: [400, "Room key not accepted."],
  bad_sig: [401, "Node signature rejected."],
  auth: [401, "Token not accepted."],
  cap: [403, "Server limit reached."],
  banned: [403, "Node refused."],
  forbidden: [403, "Owner role required."],
  disabled: [403, "Room is disabled."],
  too_new: [403, "Owner added less than 24 hours ago."],
  not_found: [404, "Not found."],
  clock: [409, "Node clock outside the allowed window."],
  collision: [409, "Room ID byte clashes with this node."],
  exists: [409, "Room already hosted."],
  last_owner: [409, "Other nodes still use this room."],
  self: [409, "Use leave for this node."],
  gone: [410, "Room deleted."],
  rate: [429, "Too many requests."],
  closed: [503, "Service is not accepting this request."],
  internal: [500, "Internal error."],
};
/** Thrown inside node handlers; the message is always fixed text. */
export class NodeError extends Error {
  readonly extra: Record<string, unknown>;
  constructor(readonly e: string, extra: Record<string, unknown> = {}) {
    super(e);
    this.extra = extra;
  }
  get status(): number {return (failures[this.e] ?? failures.internal)[0];}
  get text(): string {return (failures[this.e] ?? failures.internal)[1];}
}
export function nodeFail(e: string, extra: Record<string, unknown> = {}): never {throw new NodeError(e, extra);}
export function nodeJson(body: Record<string, unknown>, status = 200, headers: Record<string, string> = {}): Response {
  return new Response(JSON.stringify(body), {status, headers: {"Content-Type": "application/json",
    "Cache-Control": "no-store", "X-Content-Type-Options": "nosniff", ...headers}});
}
export interface FailureParts {status: number; body: Record<string, unknown>; retryAfter?: number}
/** Status and fixed-text body for a node, control or admin error. */
export function failureParts(error: NodeError): FailureParts {
  const extra = {...error.extra};
  const retryAfter = typeof extra.retryAfter === "number" ? extra.retryAfter : undefined;
  delete extra.retryAfter;
  return {status: error.status, body: {ok: 0, e: error.e, m: error.text, ...extra}, retryAfter};
}
export function partsResponse(parts: FailureParts): Response {
  return nodeJson(parts.body, parts.status, parts.retryAfter !== undefined ? {"Retry-After": String(parts.retryAfter)} : {});
}
/** Error body for any node, control or admin route. Logs only a fixed string and the error class. */
export function nodeError(error: unknown, route: string): Response {
  if (error instanceof NodeError) return partsResponse(failureParts(error));
  console.error("node route failed", route, error instanceof Error ? error.constructor.name : typeof error);
  return nodeJson({ok: 0, e: "internal", m: failures.internal[1]}, 500);
}
