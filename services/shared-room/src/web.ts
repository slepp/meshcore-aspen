import {credential, fail, wellFormed, type Alias} from "./config";

export const WEB_PROTOCOL = "aspen-room.web.v1";
export const WEB_SESSION_SECONDS = 30 * 24 * 60 * 60;
export const POST_BYTES = 151;
export const PAGE_SIZE = 100;
export interface WebSession {
  token: string;
  alias: string;
  author: string;
  name: string;
  credential: string;
  expires: number;
  username?: string | null;
}
export interface WebConnection extends WebSession {
  kind: "web";
  cursor: number;
}
export function isWebConnection(value: unknown): value is WebConnection {
  return !!value && typeof value === "object" && "kind" in value && value.kind === "web";
}
export function webPath(path: string): RegExpMatchArray | null {
  return path.match(/^\/v1\/web\/rooms\/([a-zA-Z0-9_-]{1,64})\/(challenge|login|session|logout|history|posts|socket)$/);
}
export function sameOrigin(request: Request): void {
  const url = new URL(request.url);
  if (request.headers.get("Origin") !== url.origin) fail(403, "Use the room service's own website");
  if (url.protocol !== "https:" && !["localhost", "127.0.0.1", "[::1]"].includes(url.hostname))
    fail(403, "Web room login requires HTTPS");
}
export function webText(value: unknown, label: string, bytes: number): string {
  if (typeof value !== "string" || !value.length || !wellFormed(value) || value.includes("\0") ||
      new TextEncoder().encode(value).length > bytes) fail(400, `Invalid ${label}; use 1..${bytes} UTF-8 bytes`);
  return value;
}
export function displayName(value: unknown): string {
  const name = webText(value, "display name", 24);
  if (name !== name.trim() || /[\u0000-\u001f\u007f-\u009f:]/.test(name)) fail(400, "Use a display name without colons or control characters");
  return name;
}
export async function webCredential(alias: Alias): Promise<string> {
  return credential(JSON.stringify([alias.backend, alias.publicKey, alias.password]));
}
export function cookieName(alias: string): string {return `aspen_room_${alias}`;}
export function sessionCookie(alias: string, token: string, maxAge = WEB_SESSION_SECONDS): string {
  return `${cookieName(alias)}=${token}; Path=/v1/web/rooms/${alias}/; Max-Age=${maxAge}; HttpOnly; Secure; SameSite=Strict`;
}
export function cookieToken(request: Request, alias: string): string {
  const found = (request.headers.get("Cookie") ?? "").split(";").map(p => p.trim())
    .filter(p => p.startsWith(`${cookieName(alias)}=`));
  const token = found[0]?.slice(cookieName(alias).length + 1);
  if (found.length !== 1 || !token || !/^[a-f0-9]{64}$/.test(token)) fail(401, "Join this room to read or send messages");
  return token;
}
export function sequence(value: string | null, label: string): number {
  if (value === null || !/^(0|[1-9][0-9]*)$/.test(value)) fail(400, `Invalid ${label}`);
  const n = Number(value);
  if (!Number.isSafeInteger(n)) fail(400, `Invalid ${label}`);
  return n;
}
export async function webBody(request: Request, maxBytes = 4096): Promise<Record<string, unknown>> {
  if (request.headers.get("Content-Type")?.split(";")[0].trim().toLowerCase() !== "application/json")
    fail(415, "Use application/json");
  const reader = request.body?.getReader();
  if (!reader) fail(400, "JSON body required");
  const chunks: Uint8Array[] = [];
  let size = 0;
  while (true) {
    const chunk = await reader.read();
    if (chunk.done) break;
    size += chunk.value.length;
    if (size > maxBytes) {await reader.cancel(); fail(413, `Web request exceeds ${maxBytes} bytes`);}
    chunks.push(chunk.value);
  }
  const bytes = new Uint8Array(size);
  let offset = 0;
  for (const chunk of chunks) {bytes.set(chunk, offset); offset += chunk.length;}
  let value: unknown;
  try {value = JSON.parse(new TextDecoder("utf-8", {fatal: true, ignoreBOM: true}).decode(bytes));}
  catch {return fail(400, "Invalid JSON");}
  if (!value || typeof value !== "object" || Array.isArray(value)) fail(400, "JSON object required");
  return value as Record<string, unknown>;
}
export function webResponse(value: unknown, status = 200, cookie?: string): Response {
  return Response.json(value, {status, headers: {
    "Cache-Control": "no-store",
    "X-Content-Type-Options": "nosniff",
    ...(cookie ? {"Set-Cookie": cookie} : {}),
  }});
}
