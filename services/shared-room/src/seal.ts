// Key custody for hosted rooms: ROOM_KEK parsing, per-room AES-GCM sealing,
// self-authenticating alias ids and keyed client counters. Secrets never leave
// this module except as the decrypted room key handed to its Room Durable Object.
import {equalToken, type Env} from "./config";
import {fromHex, toHex} from "./native-crypto";
import {sha256Hex} from "./node-protocol";

const encoder = new TextEncoder();
export interface Kek {current: string; keys: Record<string, Uint8Array>}
const B32 = "abcdefghijklmnopqrstuvwxyz234567";

let parsedFor: string | undefined, parsed: Kek | undefined;
/** Parse the ROOM_KEK secret `{"current":"k1","keys":{"k1":"<64 hex>"}}`, or undefined when absent or malformed. */
export function parseKek(env: Pick<Env, "ROOM_KEK">): Kek | undefined {
  const raw = env.ROOM_KEK;
  if (typeof raw !== "string" || !raw) return;
  if (raw === parsedFor) return parsed;
  let result: Kek | undefined;
  try {
    const value = JSON.parse(raw) as {current?: unknown; keys?: Record<string, unknown>};
    if (typeof value.current === "string" && value.keys && typeof value.keys === "object" && !Array.isArray(value.keys)) {
      const keys: Record<string, Uint8Array> = {};
      for (const [id, hex] of Object.entries(value.keys)) {
        if (!/^[a-zA-Z0-9_-]{1,16}$/.test(id) || typeof hex !== "string" || !/^[a-f0-9]{64}$/.test(hex)) throw new Error("kek");
        keys[id] = fromHex(hex);
      }
      if (keys[value.current]) result = {current: value.current, keys};
    }
  } catch {result = undefined;}
  parsedFor = raw; parsed = result;
  return result;
}

export function b32(bytes: Uint8Array): string {
  let bits = 0, acc = 0, out = "";
  for (const b of bytes) {
    acc = (acc << 8) | b; bits += 8;
    while (bits >= 5) {out += B32[(acc >> (bits - 5)) & 31]; bits -= 5;}
    acc &= (1 << bits) - 1;
  }
  if (bits) out += B32[(acc << (5 - bits)) & 31];
  return out;
}

async function hkdf(secret: Uint8Array, salt: Uint8Array, info: string, bytes: number): Promise<Uint8Array> {
  const base = await crypto.subtle.importKey("raw", secret as BufferSource, "HKDF", false, ["deriveBits"]);
  return new Uint8Array(await crypto.subtle.deriveBits({name: "HKDF", hash: "SHA-256", salt: salt as BufferSource,
    info: encoder.encode(info)}, base, bytes * 8));
}
async function hmac(key: Uint8Array, data: Uint8Array | string): Promise<Uint8Array> {
  const k = await crypto.subtle.importKey("raw", key as BufferSource, {name: "HMAC", hash: "SHA-256"}, false, ["sign"]);
  return new Uint8Array(await crypto.subtle.sign("HMAC", k, typeof data === "string" ? encoder.encode(data) : data as BufferSource));
}

/** Sealed room secret: salt(16) || nonce(12) || ciphertext||tag, bound to the alias, public key and key id. */
export async function sealRoom(kek: Kek, alias: string, publicKey: string, key: Uint8Array, password: string):
    Promise<{kid: string; sealed: Uint8Array}> {
  const kid = kek.current, salt = crypto.getRandomValues(new Uint8Array(16)), nonce = crypto.getRandomValues(new Uint8Array(12));
  const pw = encoder.encode(password), plain = new Uint8Array(65 + pw.length);
  plain.set(key); plain[64] = pw.length; plain.set(pw, 65);
  const ck = await crypto.subtle.importKey("raw", await hkdf(kek.keys[kid], salt, `aspen-room.key.v1|${alias}`, 32) as BufferSource,
    "AES-GCM", false, ["encrypt"]);
  const body = new Uint8Array(await crypto.subtle.encrypt({name: "AES-GCM", iv: nonce as BufferSource,
    additionalData: encoder.encode(`${alias}|${publicKey}|${kid}`)}, ck, plain));
  plain.fill(0);
  const sealed = new Uint8Array(28 + body.length);
  sealed.set(salt); sealed.set(nonce, 16); sealed.set(body, 28);
  return {kid, sealed};
}
/** Fails closed (throws) when the blob, alias, public key or key id do not match. */
export async function openRoom(kek: Kek, alias: string, publicKey: string, kid: string, sealed: Uint8Array):
    Promise<{key: Uint8Array; password: string}> {
  const secret = kek.keys[kid];
  if (!secret || sealed.length < 28 + 65 + 16) throw new Error("sealed");
  const ck = await crypto.subtle.importKey("raw", await hkdf(secret, sealed.slice(0, 16), `aspen-room.key.v1|${alias}`, 32) as BufferSource,
    "AES-GCM", false, ["decrypt"]);
  const plain = new Uint8Array(await crypto.subtle.decrypt({name: "AES-GCM", iv: sealed.slice(16, 28) as BufferSource,
    additionalData: encoder.encode(`${alias}|${publicKey}|${kid}`)}, ck, sealed.slice(28) as BufferSource));
  if (plain.length < 65 || plain.length !== 65 + plain[64]) throw new Error("sealed");
  const key = plain.slice(0, 64), password = new TextDecoder().decode(plain.slice(65));
  plain.fill(0);
  return {key, password};
}

async function aliasKey(secret: Uint8Array): Promise<Uint8Array> {return hkdf(secret, new Uint8Array(0), "aspen-room.alias.v1", 32);}
/** `r` + 13 random symbols + 16 symbols of an 80-bit HMAC tag. */
export async function newAliasId(kek: Kek): Promise<string> {
  const head = "r" + b32(crypto.getRandomValues(new Uint8Array(8)));
  const tag = (await hmac(await aliasKey(kek.keys[kek.current]), "alias\n" + head)).slice(0, 10);
  return head + b32(tag);
}
/** True when the tag verifies under any key id in ROOM_KEK. No storage is touched. */
export async function validAliasId(kek: Kek, id: string): Promise<boolean> {
  if (!/^r[a-z2-7]{29}$/.test(id)) return false;
  const head = id.slice(0, 14);
  let ok = false;
  for (const secret of Object.values(kek.keys)) {
    const tag = b32((await hmac(await aliasKey(secret), "alias\n" + head)).slice(0, 10));
    if (equalToken(tag, id.slice(14))) ok = true;
  }
  return ok;
}

/** First four groups of an IPv6 address (its /64), with `::` expanded so every host in a prefix shares one key. */
export function ipv6Prefix(ip: string): string {
  const [head, tail] = ip.toLowerCase().split("::");
  const front = head ? head.split(":") : [], back = tail === undefined ? [] : tail ? tail.split(":") : [];
  const groups = tail === undefined ? front : [...front, ...Array(Math.max(0, 8 - front.length - back.length)).fill("0"), ...back];
  return groups.slice(0, 4).map(g => (parseInt(g || "0", 16) || 0).toString(16)).join(":");
}
/** Keyed IP counter key: /64 for IPv6, /32 for IPv4, rotating each UTC day. */
export async function clientKey(kek: Kek | undefined, ip: string, now: number): Promise<string> {
  let prefix = ip;
  const mapped = ip.match(/^::ffff:(\d{1,3}(?:\.\d{1,3}){3})$/i);
  if (mapped) prefix = mapped[1];
  else if (ip.includes(":")) prefix = ipv6Prefix(ip);
  const day = String(Math.floor(now / 86400));
  if (!kek) return "plain|" + prefix + "|" + day;
  const key = await hkdf(kek.keys[kek.current], new Uint8Array(0), "aspen-room.ipkey.v1", 32);
  return toHex((await hmac(key, prefix + "|" + day)).slice(0, 16));
}

export function newToken(): string {return "ar1_" + toHex(crypto.getRandomValues(new Uint8Array(32)));}
export const tokenHash = (token: string): Promise<string> => sha256Hex(token);
