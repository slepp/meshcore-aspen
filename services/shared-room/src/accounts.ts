import {aliases, credential, equalToken, fail, wellFormed, type Env} from "./config";
import {fromHex, toHex} from "./native-crypto";
import {displayName} from "./web";

export const DEVICE_PROTOCOL = "aspen-room.device.v1";
export interface WebUser {
  name: string;
  salt: string;
  hash: string;
  iterations: number;
  aliases: string[];
}
export function username(value: unknown): string {
  if (typeof value !== "string" || !/^[a-z0-9][a-z0-9_-]{2,63}$/.test(value))
    fail(400, "Use a username of 3..64 lowercase letters, digits, hyphens or underscores");
  return value;
}
export function webUsers(env: Env): Record<string, WebUser> | undefined {
  if (env.WEB_USERS === undefined) return;
  let value: unknown;
  try {value = JSON.parse(env.WEB_USERS);} catch {return fail(503, "Invalid WEB_USERS configuration");}
  if (!value || typeof value !== "object" || Array.isArray(value)) fail(503, "Invalid WEB_USERS configuration");
  const rooms = aliases(env), result: Record<string, WebUser> = Object.create(null);
  for (const [id, entry] of Object.entries(value)) {
    if (!/^[a-z0-9][a-z0-9_-]{2,63}$/.test(id) || !entry || typeof entry !== "object" || Array.isArray(entry))
      fail(503, "Invalid WEB_USERS account");
    const user = entry as WebUser;
    if (Object.keys(user).sort().join(",") !== "aliases,hash,iterations,name,salt" ||
        !/^[a-f0-9]{32}$/.test(user.salt) || !/^[a-f0-9]{64}$/.test(user.hash) ||
        user.iterations !== 100000 || !Array.isArray(user.aliases) || !user.aliases.length ||
        new Set(user.aliases).size !== user.aliases.length || user.aliases.some(a => !Object.hasOwn(rooms, a)))
      fail(503, "Invalid WEB_USERS password hash or room grant");
    try {displayName(user.name);} catch {return fail(503, "Invalid WEB_USERS display name");}
    result[id] = user;
  }
  return result;
}
export async function checkPassword(password: unknown, user?: WebUser): Promise<boolean> {
  if (typeof password !== "string" || !wellFormed(password) || new TextEncoder().encode(password).length > 256)
    fail(403, "Incorrect username, password or room access");
  // Unknown users take the same bounded password calculation.
  const salt = user?.salt ?? "00".repeat(16), expected = user?.hash ?? "00".repeat(32);
  const key = await crypto.subtle.importKey("raw", new TextEncoder().encode(password), "PBKDF2", false, ["deriveBits"]);
  const digest = await crypto.subtle.deriveBits({name: "PBKDF2", hash: "SHA-256", salt: new Uint8Array(fromHex(salt)).buffer,
    iterations: 100000}, key, 256);
  return equalToken(toHex(new Uint8Array(digest)), expected) && !!user;
}
export function deviceChallenge(origin: string, alias: string, user: string, publicKey: string, nonce: string): string {
  const message = [DEVICE_PROTOCOL, origin, alias, user, publicKey, nonce].join("\n");
  if (new TextEncoder().encode(message).length > 512) fail(400, "Room service origin exceeds the device signing limit");
  return message;
}
export async function accountCredential(env: Env, alias: string, user: string): Promise<string> {
  const account = webUsers(env)?.[user];
  if (!account?.aliases.includes(alias)) fail(401, "Account room access changed; sign in again");
  return credential(JSON.stringify([aliases(env)[alias], user, account]));
}
