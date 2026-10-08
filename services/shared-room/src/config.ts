import type {Room} from "./room";

export interface Alias {backend: string; publicKey: string; name: string; password: string}
export interface Frontend {token: string; aliases: string[]; region?: string}
export interface Env {
  ROOMS: DurableObjectNamespace<Room>;
  ALIASES?: string;
  FRONTENDS?: string;
  HISTORY_LIMIT?: string;
  MODE?: string;
  ROOM_KEYS?: string;
  WEB_USERS?: string;
  ASSETS?: Fetcher;
}
export interface Connection {alias: string; frontend: string; credential: string}
export class ApiError extends Error {
  constructor(public status: number, message: string) {super(message);}
}
export function fail(status: number, message: string): never {throw new ApiError(status, message);}
export function wellFormed(value: string): boolean {
  // Reject unpaired UTF-16 surrogates; native clients and coreJSON use UTF-8.
  for (let i = 0; i < value.length; i++) {
    const unit = value.charCodeAt(i);
    if (unit >= 0xd800 && unit <= 0xdbff) {
      const next = value.charCodeAt(++i);
      if (!(next >= 0xdc00 && next <= 0xdfff)) return false;
    } else if (unit >= 0xdc00 && unit <= 0xdfff) return false;
  }
  return true;
}
/** Public native hashtag region only. No wildcard, hierarchy or private key. */
export function publicRegion(value: unknown): string | undefined {
  if (value === undefined) return;
  if (typeof value !== "string" || !wellFormed(value)) fail(503, "Invalid public frontend region");
  const name = value.replace(/^#/, ""), bytes = new TextEncoder().encode(name);
  if (!bytes.length || bytes.length > 30 || name.startsWith("$") || name.startsWith("#") ||
      !bytes.every(c => c === 45 || c === 35 || c === 36 || (c >= 48 && c <= 57) || c >= 65))
    fail(503, "Use a public named frontend region; private regions require a key handoff");
  return name;
}
export function frontendRegion(env: Env, frontend: string): string | undefined {
  const entries = JSON.parse(env.FRONTENDS ?? "{}") as Record<string, Frontend>;
  return publicRegion(entries[frontend]?.region);
}
export function aliases(env: Env): Record<string, Alias> {
  const entries = JSON.parse(env.ALIASES ?? "{}") as Record<string, Alias>;
  if (!entries || typeof entries !== "object" || Array.isArray(entries)) fail(503, "Invalid ALIASES configuration");
  const keys = new Set<string>();
  for (const [name, alias] of Object.entries(entries)) {
    if (!/^[a-zA-Z0-9_-]{1,64}$/.test(name) || !alias ||
        !/^[a-zA-Z0-9_-]{1,64}$/.test(alias.backend) ||
        !/^[a-f0-9]{64}$/.test(alias.publicKey) || typeof alias.name !== "string" || !alias.name ||
        new TextEncoder().encode(alias.name).length > 31 || alias.name.includes("\0") || !wellFormed(alias.name) ||
        typeof alias.password !== "string" || keys.has(alias.publicKey)) {
      fail(503, "Invalid or duplicate advertised identity in ALIASES");
    }
    keys.add(alias.publicKey);
  }
  return entries;
}
export function equalToken(a: string, b: string): boolean {
  let different = a.length ^ b.length;
  for (let i = 0; i < Math.max(a.length, b.length); i++) different |= (a.charCodeAt(i) || 0) ^ (b.charCodeAt(i) || 0);
  return different === 0;
}
export async function credential(token: string): Promise<string> {
  const digest = await crypto.subtle.digest("SHA-256", new TextEncoder().encode(token));
  return Array.from(new Uint8Array(digest), byte => byte.toString(16).padStart(2, "0")).join("");
}
export async function authorize(request: Request, env: Env, alias: string): Promise<Connection> {
  const token = request.headers.get("Authorization")?.match(/^Bearer (.+)$/)?.[1];
  if (!token || token.length > 512) fail(401, "Frontend bearer token required");
  const frontends = JSON.parse(env.FRONTENDS ?? "{}") as Record<string, Frontend>;
  for (const [name, entry] of Object.entries(frontends)) {
    if (entry.token && equalToken(entry.token, token)) {
      if (!entry.aliases.includes(alias)) fail(403, "Frontend is not allowed to serve this alias");
      return {alias, frontend: name, credential: await credential(entry.token)};
    }
  }
  return fail(401, "Unknown frontend token");
}
export function errorResponse(error: unknown): Response {
  const known = error instanceof ApiError;
  if (!known) console.error("Shared room operation failed", error);
  return Response.json({error: known ? error.message : "Room operation failed"}, {status: known ? error.status : 500,
    headers: {"Cache-Control": "no-store", "X-Content-Type-Options": "nosniff"}});
}
