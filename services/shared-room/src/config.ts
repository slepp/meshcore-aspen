import type {Room} from "./room";

export interface Alias {backend: string; publicKey: string; name: string; password: string}
export interface Frontend {token: string; aliases: string[]}
export interface Env {
  ROOMS: DurableObjectNamespace<Room>;
  ALIASES: string;
  FRONTENDS: string;
  HISTORY_LIMIT?: string;
}
export interface Connection {alias: string; frontend: string; credential: string}
export class ApiError extends Error {
  constructor(public status: number, message: string) {super(message);}
}
export function fail(status: number, message: string): never {throw new ApiError(status, message);}
export function aliases(env: Env): Record<string, Alias> {
  const entries = JSON.parse(env.ALIASES) as Record<string, Alias>;
  if (!entries || typeof entries !== "object" || Array.isArray(entries)) fail(503, "Invalid ALIASES configuration");
  const keys = new Set<string>();
  for (const [name, alias] of Object.entries(entries)) {
    if (!/^[a-zA-Z0-9_-]{1,64}$/.test(name) || !alias ||
        !/^[a-zA-Z0-9_-]{1,64}$/.test(alias.backend) ||
        !/^[a-f0-9]{64}$/.test(alias.publicKey) || typeof alias.name !== "string" || !alias.name ||
        new TextEncoder().encode(alias.name).length > 32 || typeof alias.password !== "string" || keys.has(alias.publicKey)) {
      fail(503, "Invalid or duplicate advertised identity in ALIASES");
    }
    keys.add(alias.publicKey);
  }
  return entries;
}
function equalToken(a: string, b: string): boolean {
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
  const frontends = JSON.parse(env.FRONTENDS) as Record<string, Frontend>;
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
  return Response.json({error: known ? error.message : "Room operation failed"}, {status: known ? error.status : 500});
}
