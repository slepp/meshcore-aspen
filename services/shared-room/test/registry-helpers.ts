import {env, runInDurableObject} from "cloudflare:test";
import {afterEach, expect} from "vitest";
import worker from "../src/index";
import type {Env} from "../src/config";
import {resetGates} from "../src/limits";
import {NativeCrypto, fromHex, toHex} from "../src/native-crypto";
import {codeHash, createSubject, formatCode, signedMessage, type NodeOp} from "../src/node-protocol";
const FIXTURE_ADMIN = "fixture-admin-token";
import {fixture, history} from "./native-helpers";
export {fixture, history};

export const bindings = env as unknown as Env;
export const crypto = new NativeCrypto();
export const ORIGIN = "https://rooms.test";
export const sockets: WebSocket[] = [];
afterEach(() => {resetGates(); for (const ws of sockets.splice(0)) ws.close();});

let counter = 0;
/** Fresh client address per call so per-client limits do not couple tests. */
export const freshIp = (): string => `198.51.100.${(++counter % 250) + 1}`;
export interface TestNode {key: Uint8Array; pub: string}
export function nodeFrom(seedByte: number): TestNode {
  const made = crypto.seed(new Uint8Array(32).fill(seedByte));
  return {key: made.key, pub: toHex(made.publicKey)};
}
export const nodeA = nodeFrom(0x11), nodeB = nodeFrom(0x22), nodeC = nodeFrom(0x33);

export async function call(path: string, init: RequestInit & {ip?: string; env?: Partial<Env>} = {}): Promise<Response> {
  const {ip, env: override, ...rest} = init;
  const headers = new Headers(rest.headers);
  headers.set("CF-Connecting-IP", ip ?? freshIp());
  const request = new Request(ORIGIN + path, {...rest, headers, body: rest.method === "GET" || rest.method === "HEAD" ? undefined : rest.body});
  return worker.fetch(request, {...bindings, ...override} as Env);
}
export interface Reply {status: number; body: any; headers: Headers}
export async function json(path: string, body: unknown, init: {ip?: string; env?: Partial<Env>; token?: string; method?: string} = {}): Promise<Reply> {
  const headers: Record<string, string> = {"Content-Type": "application/json"};
  if (init.token) headers.Authorization = `Bearer ${init.token}`;
  const method = init.method ?? "POST";
  const r = await call(path, {method, headers, body: typeof body === "string" ? body : JSON.stringify(body), ip: init.ip, env: init.env});
  const text = await r.text();
  let parsed: any; try {parsed = JSON.parse(text);} catch {parsed = text;}
  return {status: r.status, body: parsed, headers: r.headers};
}
export const admin = (action: string, body: unknown, ip?: string) => json(`/v1/admin/${action}`, body, {token: FIXTURE_ADMIN, ip});

export const now = (): number => Math.floor(Date.now() / 1000);
export async function mintCreate(count = 1): Promise<string[]> {
  const r = await admin("mint", {kind: "create", count});
  expect(r.status, JSON.stringify(r.body)).toBe(200);
  return r.body.codes;
}

export interface RedeemOptions {
  code: string; ts?: number; name?: string; pub?: string; key?: string; pw?: string; avoid?: string;
  origin?: string; ip?: string; env?: Partial<Env>; mutate?: (body: Record<string, unknown>) => void;
}
/** Builds and signs a redeem request exactly as a device does. */
export async function signedBody(node: TestNode, op: NodeOp, o: RedeemOptions): Promise<Record<string, unknown>> {
  const open = o.code === "OPEN";
  const canonical = open ? "OPEN" : o.code.replace(/-/g, "");
  const ts = o.ts ?? now();
  const subject = op === "join" ? "-" : op === "create" ? await createSubject(o.name!) : o.pub!;
  const hash = open ? "open" : (await codeHash(canonical)).slice(0, 32);
  const message = signedMessage({origin: o.origin ?? ORIGIN, op, code: hash, node: node.pub, ts, subject});
  const body: Record<string, unknown> = {p: "aspen-node.v1", op, code: canonical, node: node.pub, ts, sig: toHex(crypto.sign(node.key, message))};
  if (op !== "join") body.name = o.name;
  if (op === "convert") {body.pub = o.pub; body.key = o.key; body.pw = o.pw ?? "";}
  if (o.avoid !== undefined) body.avoid = o.avoid;
  o.mutate?.(body);
  return body;
}
export async function redeem(node: TestNode, op: NodeOp, o: RedeemOptions): Promise<Reply> {
  return json("/v1/node/redeem", await signedBody(node, op, o), {ip: o.ip, env: o.env});
}
export async function createRoom(node = nodeA, name = "Fleet", extra: Partial<RedeemOptions> = {}): Promise<any> {
  const [code] = await mintCreate();
  const r = await redeem(node, "create", {code, name, ...extra});
  expect(r.status, JSON.stringify(r.body)).toBe(200);
  return r.body;
}
export async function convertFixture(node = nodeA, pw = "room", name = "Converted"): Promise<any> {
  const [code] = await mintCreate();
  const r = await redeem(node, "convert", {code, name, pub: fixture.room.publicKey, key: fixture.room.key, pw});
  expect(r.status, JSON.stringify(r.body)).toBe(200);
  return r.body;
}
export async function control(alias: string, token: string, body: unknown, ip?: string): Promise<Reply> {
  return json(`/v1/aliases/${alias}/control`, body, {token, ip});
}
export async function invite(room: {alias: string; token: string}, role: "frontend" | "owner" = "frontend", ttl?: number): Promise<string> {
  const r = await control(room.alias, room.token, {op: "invite", role, ...(ttl ? {ttl} : {})});
  expect(r.status, JSON.stringify(r.body)).toBe(200);
  return r.body.code;
}
export async function joinRoom(room: {alias: string; token: string}, node: TestNode, role: "frontend" | "owner" = "frontend"): Promise<any> {
  const code = await invite(room, role);
  const r = await redeem(node, "join", {code});
  expect(r.status, JSON.stringify(r.body)).toBe(200);
  return r.body;
}
export const registryStub = () => bindings.REGISTRY.getByName("registry");
export async function registrySql<T = any>(query: string, ...params: any[]): Promise<T[]> {
  return runInDurableObject(registryStub(), (_i, s) => s.storage.sql.exec(query, ...params).toArray() as T[]);
}
export async function registryDump(): Promise<string> {
  return runInDurableObject(registryStub(), (_i, s) => {
    const out: Record<string, unknown> = {};
    for (const t of ["codes", "rooms", "members", "bans", "limits", "audit", "admin_audit"])
      out[t] = s.storage.sql.exec(`SELECT * FROM ${t}`).toArray();
    return JSON.stringify(out);
  });
}
export const hostedStub = (alias: string) => bindings.ROOMS.getByName("rt-" + alias);
export async function hostedSql<T = any>(alias: string, query: string, ...params: any[]): Promise<T[]> {
  return runInDurableObject(hostedStub(alias), (_i, s) => s.storage.sql.exec(query, ...params).toArray() as T[]);
}

export async function socket(alias: string, token: string, expected = 101): Promise<{ws: WebSocket; inbox: any[]; next: (label?: string) => Promise<any>; closed: Promise<number>; status: number}> {
  const r = await call(`/v1/aliases/${alias}/socket`, {headers: {Authorization: `Bearer ${token}`, Upgrade: "websocket",
    "Sec-WebSocket-Protocol": "aspen-room.v2.json"}});
  expect(r.status).toBe(expected);
  if (expected !== 101) {await r.text(); return {status: r.status} as any;}
  const ws = r.webSocket!;
  sockets.push(ws);
  const inbox: any[] = [], waiting: Array<(e: any) => void> = [];
  let onClose: (c: number) => void = () => undefined;
  const closed = new Promise<number>(resolve => {onClose = resolve;});
  ws.addEventListener("message", e => {
    const v = JSON.parse(e.data as string);
    if (waiting.length) waiting.shift()!(v); else inbox.push(v);
  });
  ws.addEventListener("close", e => onClose(e.code));
  ws.accept();
  const next = (label = "frame"): Promise<any> => inbox.length ? Promise.resolve(inbox.shift()) :
    new Promise((resolve, reject) => {
      const timer = setTimeout(() => reject(new Error(`Missing ${label}`)), 1500);
      waiting.push(v => {clearTimeout(timer); resolve(v);});
    });
  return {ws, inbox, next, closed, status: 101};
}
export async function rf(alias: string, token: string, wire: string): Promise<any> {
  const r = await json(`/v1/aliases/${alias}/operations`, {op: "rf", packet: wire}, {token});
  return r;
}
export {fromHex, formatCode};
