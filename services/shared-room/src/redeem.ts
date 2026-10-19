// Public node routes: GET /v1/info, POST /v1/node/redeem and the hosted-room
// control route. Handlers verify signatures and tokens here and call the
// Registry and Room objects over internal RPC. Error text is fixed and nothing
// from a request body, header or secret is ever logged.
import {aliases, type Env} from "./config";
import {admitMinute, admitRegistryWrite, intSetting, memoryLimit} from "./limits";
import {NativeCrypto, fromHex, toHex} from "./native-crypto";
import {NodeError, canonicalCode, codeHash, createSubject, nodeFail, nodeJson, partsResponse, sha256Hex, signedMessage,
  validName, validPassword, type NodeOp} from "./node-protocol";
import {NODE_PROTOCOL, SERVICE_VERSION} from "./protocol";
import {clientKey, newAliasId, parseKek, type Kek} from "./seal";
import type {ClaimParams, ClaimResult} from "./registry";
import type {HostedGrant} from "./hosted";

const REDEEM_BYTES = 2048, CONTROL_BYTES = 1024;
let nativeCrypto: NativeCrypto | undefined;
const crypto_ = (): NativeCrypto => nativeCrypto ??= new NativeCrypto();

export type SignupMode = "code" | "open" | "closed";
export function signupMode(env: Env, kek: Kek | undefined): SignupMode {
  if (!kek) return "closed";
  return env.SIGNUP_MODE === "open" ? "open" : env.SIGNUP_MODE === "closed" ? "closed" : "code";
}
export const registryOf = (env: Env) => env.REGISTRY.getByName("registry");

/** Reads a bounded JSON object body; every failure is `bad_request`. */
export async function readObject(request: Request, max: number): Promise<Record<string, unknown>> {
  if (request.method !== "POST" || !/^application\/json\b/i.test(request.headers.get("Content-Type") ?? "")) nodeFail("bad_request");
  const declared = Number(request.headers.get("Content-Length"));
  if (declared > max) nodeFail("bad_request");
  const reader = request.body?.getReader();
  if (!reader) nodeFail("bad_request");
  const chunks: Uint8Array[] = [];
  let size = 0;
  for (;;) {
    const {done, value} = await reader.read();
    if (done) break;
    size += value.length;
    if (size > max) {await reader.cancel(); nodeFail("bad_request");}
    chunks.push(value);
  }
  const bytes = new Uint8Array(size);
  let at = 0;
  for (const c of chunks) {bytes.set(c, at); at += c.length;}
  let value: unknown;
  try {value = JSON.parse(new TextDecoder("utf-8", {fatal: true, ignoreBOM: true}).decode(bytes));} catch {nodeFail("bad_request");}
  if (!value || typeof value !== "object" || Array.isArray(value)) nodeFail("bad_request");
  return value as Record<string, unknown>;
}

export function info(env: Env): Response {
  return nodeJson({service: "aspen-shared-room", version: SERVICE_VERSION, protocol: NODE_PROTOCOL,
    signup: signupMode(env, parseKek(env)), time: Math.floor(Date.now() / 1000)});
}

const FIELDS: Record<NodeOp, string[]> = {
  join: ["p", "op", "code", "node", "ts", "sig", "avoid"],
  create: ["p", "op", "code", "node", "ts", "sig", "avoid", "name"],
  convert: ["p", "op", "code", "node", "ts", "sig", "avoid", "name", "pub", "key", "pw"],
};
const hexOf = (value: unknown, bytes: number): string => {
  if (typeof value !== "string" || !new RegExp(`^[a-f0-9]{${bytes * 2}}$`).test(value)) nodeFail("bad_request");
  return value;
};

async function ipClient(request: Request, env: Env, kek: Kek | undefined): Promise<string> {
  const ip = request.headers.get("CF-Connecting-IP");
  if (!ip || ip.length > 64) nodeFail("bad_request");
  return clientKey(kek, ip, Math.floor(Date.now() / 1000));
}

function staticKeys(env: Env): Set<string> {
  try {return new Set(Object.values(aliases(env)).map(a => a.publicKey));} catch {return new Set();}
}

export async function redeem(request: Request, env: Env): Promise<Response> {
  const body = await readObject(request, REDEEM_BYTES);
  const kek = parseKek(env);
  const client = await ipClient(request, env, kek);
  const wait = memoryLimit("ip:" + client, 10, 60);
  if (wait) nodeFail("rate", {retryAfter: wait});
  if (env.NODE_LIMIT && !(await env.NODE_LIMIT.limit({key: client})).success) nodeFail("rate", {retryAfter: 60});
  const registry = registryOf(env);
  if (!admitMinute()) nodeFail("rate", {retryAfter: 60});
  if (!kek) nodeFail("closed");

  // Field validation. Unknown or missing fields are `bad_request`; texts never echo input.
  if (body.p !== NODE_PROTOCOL) nodeFail("bad_request");
  const op = body.op;
  if (op !== "join" && op !== "create" && op !== "convert") return nodeFail("bad_request");
  for (const key of Object.keys(body)) if (!FIELDS[op].includes(key)) nodeFail("bad_request");
  const required = op === "join" ? ["code", "node", "ts", "sig"] : op === "create" ? ["code", "node", "ts", "sig", "name"] :
    ["code", "node", "ts", "sig", "name", "pub", "key", "pw"];
  for (const key of required) if (body[key] === undefined) nodeFail("bad_request");
  const node = hexOf(body.node, 32), sig = hexOf(body.sig, 64);
  const ts = body.ts;
  if (typeof ts !== "number" || !Number.isSafeInteger(ts) || ts < 0) return nodeFail("bad_request");
  const open = op === "create" && typeof body.code === "string" && body.code.toUpperCase() === "OPEN";
  const canonical = open ? undefined : canonicalCode(body.code);
  if (!open && !canonical) nodeFail("bad_request");
  let avoid: number[] = [];
  if (body.avoid !== undefined) {
    if (typeof body.avoid !== "string" || !/^(?:[a-f0-9]{2}){0,8}$/.test(body.avoid)) nodeFail("bad_request");
    avoid = body.avoid ? Array.from(fromHex(body.avoid)) : [];
  }
  let name: string | undefined, pub: string | undefined, key: string | undefined, password: string | undefined;
  if (op !== "join") {
    if (!validName(body.name)) nodeFail("name");
    name = body.name;
  }
  if (op === "convert") {
    pub = hexOf(body.pub, 32); key = hexOf(body.key, 64);
    if (!validPassword(body.pw)) nodeFail("bad_request");
    password = body.pw;
  }
  const now = Math.floor(Date.now() / 1000);
  if (Math.abs(ts - now) > 300) nodeFail("clock", {now});

  // Signature over origin, op, code hash, node, ts and subject.
  const origin = "https://" + new URL(request.url).hostname.toLowerCase();
  const hash = open ? await sha256Hex(`aspen-node.v1.open\n${node}\n${op === "create" ? await createSubject(name!) : "-"}`) : await codeHash(canonical!);
  const subject = op === "join" ? "-" : op === "create" ? await createSubject(name!) : pub!;
  const message = signedMessage({origin, op, code: open ? "open" : hash.slice(0, 32), node, ts, subject});
  if (!crypto_().verify(fromHex(node), fromHex(sig), message)) nodeFail("bad_sig");
  // Only a signed request may write: count it against the daily Registry budget.
  if (!await admitRegistryWrite(env, (day, n) => registry.addWrites(day, n))) nodeFail("closed");

  const mode = signupMode(env, kek);
  if (open && mode !== "open") nodeFail(mode === "closed" ? "closed" : "code_invalid");
  if (mode === "closed" && op !== "join") nodeFail("closed");

  // Room material. Create generates the key; convert validates the one it was given.
  let pubHex = pub, keyHex = key, pubFirst: number | undefined;
  if (op === "convert") {
    let derived: string;
    try {derived = toHex(crypto_().publicKey(fromHex(key!)));} catch {return nodeFail("key");}
    const first = fromHex(pub!)[0];
    if (derived !== pub || first === 0 || first === 0xff) nodeFail("key");
    if (staticKeys(env).has(pub!)) nodeFail("exists");
    pubFirst = first;
  } else if (op === "create") {
    for (let i = 0; ; i++) {
      if (i === 16) nodeFail("internal");
      const made = crypto_().seed(crypto.getRandomValues(new Uint8Array(32)));
      const first = made.publicKey[0];
      if (first === 0 || first === 0xff || avoid.includes(first)) {made.key.fill(0); continue;}
      pubHex = toHex(made.publicKey); keyHex = toHex(made.key); pubFirst = first;
      made.key.fill(0);
      break;
    }
  }
  const pubHash = pubHex ? await sha256Hex(fromHex(pubHex)) : undefined;
  const alias = op === "join" ? "" : await newAliasId(kek);
  const claim: ClaimParams = {op, hash, open, node, ts, subject, client, signup: mode,
    maxRooms: intSetting(env.MAX_ROOMS, 2000), maxFrontends: intSetting(env.MAX_FRONTENDS, 16),
    avoid, alias, pubHash, pubFirst, name};
  const result: ClaimResult = await registry.claim(claim);
  if (result.k === "err") return nodeFail(result.e, result.retryAfter ? {retryAfter: result.retryAfter} : {});

  const stub = (a: string) => env.ROOMS.getByName("rt-" + a);
  let grant: HostedGrant | undefined;
  if (result.k === "replay" || result.k === "recover") {
    const reply = await stub(result.alias).addFrontend({alias: result.alias, node, role: result.k === "replay" ? result.role : 1, replay: true});
    if (!reply.ok || !reply.grant) {
      if (reply.e === "revoked") return nodeFail(result.k === "replay" ? "code_used" : "exists");
      return nodeFail(reply.e === "not_found" ? "code_invalid" : "closed");
    }
    grant = reply.grant;
    return nodeJson({ok: 1, alias: grant.alias, name: grant.name, pub: grant.pub, token: grant.token,
      role: grant.role === 1 ? "owner" : "frontend"});
  }
  try {
    const reply = op === "join" ?
      await stub(result.alias).addFrontend({alias: result.alias, node, role: result.role}) :
      await stub(result.alias).provision({alias: result.alias, name: name!, publicKey: pubHex!, key: keyHex!,
        password: op === "convert" ? password! : null, node});
    if (!reply.ok || !reply.grant) throw new NodeError(reply.e === "not_found" ? "code_invalid" : reply.e === "key" ? "key" : "closed");
    grant = reply.grant;
  } catch (error) {
    await registry.release({hash, node, alias: result.alias}).catch(() => undefined);
    if (error instanceof NodeError) throw error;
    return nodeFail("internal");
  }
  await registry.finish({hash, node, alias: grant.alias, role: grant.role,
    ...(op === "join" ? {} : {pubHash: await sha256Hex(fromHex(grant.pub)), pubFirst: fromHex(grant.pub)[0], name: grant.name})});
  return nodeJson({ok: 1, alias: grant.alias, name: grant.name, pub: grant.pub, token: grant.token,
    role: grant.role === 1 ? "owner" : "frontend"});
}

/** POST /v1/aliases/{alias}/control for a hosted id. */
export async function control(request: Request, env: Env, alias: string): Promise<Response> {
  const kek = parseKek(env);
  if (!kek) nodeFail("closed");
  const body = await readObject(request, CONTROL_BYTES);
  const token = request.headers.get("Authorization")?.match(/^Bearer (ar1_[a-f0-9]{64})$/)?.[1];
  if (!token) nodeFail("auth");
  const ip = request.headers.get("CF-Connecting-IP");
  const client = ip && ip.length <= 64 ? await clientKey(kek, ip, Math.floor(Date.now() / 1000)) : "none";
  const reply = await env.ROOMS.getByName("rt-" + alias).control(alias, token, body, {host: new URL(request.url).hostname.toLowerCase(), client});
  return partsResponse(reply);
}
