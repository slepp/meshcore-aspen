#!/usr/bin/env node
// Device-side simulator for the shared-room node protocol (aspen-node.v1).
// It signs redeem requests exactly as an Aspen node does and drives the control
// route, so a Worker can be exercised without a radio. Use it against a local
// `wrangler dev` or your own deployment; it never prints tokens or keys unless
// you ask for them.
//
//   node tools/node-sim.mjs info --base http://127.0.0.1:8787
//   node tools/node-sim.mjs walkthrough --base http://127.0.0.1:8787 --admin-token-file .deploy/admin-token
//   node tools/node-sim.mjs create CODE "My Room" --base URL --seed-file node.seed --save room.json
//
// `wrangler dev` presents the host of the first route in wrangler.jsonc to the Worker, so the signed
// origin differs from the address you dial; pass --origin https://<that host> (or use a config without routes).
// Seeds are 32 random bytes in hex. A missing seed file is created with mode 0600.
import {createHash, createPrivateKey, createPublicKey, randomBytes, sign} from "node:crypto";
import {existsSync, readFileSync, writeFileSync} from "node:fs";
import {parseArgs} from "node:util";

export const PROTOCOL = "aspen-node.v1";
const CROCKFORD = "0123456789ABCDEFGHJKMNPQRSTVWXYZ";
const sha256 = data => createHash("sha256").update(data).digest("hex");

export function canonicalCode(input) {
  let out = "";
  for (const ch of String(input).toUpperCase()) {
    if (ch === "-" || /\s/.test(ch)) continue;
    const c = ch === "O" ? "0" : ch === "I" || ch === "L" ? "1" : ch;
    if (!CROCKFORD.includes(c)) throw new Error("Code has an invalid character.");
    out += c;
  }
  if (out.length !== 16) throw new Error("Code must have 16 symbols.");
  return out;
}

export function nodeFromSeed(seedHex) {
  const seed = Buffer.from(seedHex, "hex");
  if (seed.length !== 32) throw new Error("Seed must be 32 bytes of hex.");
  const priv = createPrivateKey({key: Buffer.concat([Buffer.from("302e020100300506032b657004220420", "hex"), seed]), format: "der", type: "pkcs8"});
  const pub = createPublicKey(priv).export({format: "der", type: "spki"}).subarray(-32).toString("hex");
  return {priv, pub};
}

/** Signed redeem body for op join, create or convert. */
export function redeemBody(node, origin, op, {code, name, pub, key, pw, avoid, ts = Math.floor(Date.now() / 1000)}) {
  const open = String(code).toUpperCase() === "OPEN";
  const canonical = open ? "OPEN" : canonicalCode(code);
  const codeHash = open ? "open" : sha256("aspen-node.v1.code\n" + canonical).slice(0, 32);
  const subject = op === "join" ? "-" : op === "create" ? sha256(name).slice(0, 16) : pub;
  const message = [PROTOCOL, origin, op, codeHash, node.pub, String(ts), subject].join("\n");
  const body = {p: PROTOCOL, op, code: canonical, node: node.pub, ts, sig: sign(null, Buffer.from(message), node.priv).toString("hex")};
  if (op !== "join") body.name = name;
  if (op === "convert") Object.assign(body, {pub, key, pw: pw ?? ""});
  if (avoid) body.avoid = avoid;
  return body;
}

export function originOf(base) {return "https://" + new URL(base).hostname.toLowerCase();}

async function post(base, path, body, token) {
  const headers = {"Content-Type": "application/json"};
  if (token) headers.Authorization = "Bearer " + token;
  const response = await fetch(new URL(path, base), {method: "POST", headers, body: JSON.stringify(body)});
  const text = await response.text();
  let json;
  try {json = JSON.parse(text);} catch {json = {ok: 0, e: "not_json", m: text.slice(0, 80)};}
  return {status: response.status, body: json};
}

export function loadSeed(file) {
  if (!file) return randomBytes(32).toString("hex");
  if (!existsSync(file)) writeFileSync(file, randomBytes(32).toString("hex") + "\n", {mode: 0o600});
  return readFileSync(file, "utf8").trim();
}
const hide = body => body && (body.token || body.pw) ? {...body, ...(body.token ? {token: "ar1_****"} : {}), ...(body.pw ? {pw: "****"} : {})} : body;

export async function walkthrough(base, adminToken, log = console.log, originOverride) {
  const origin = originOverride ?? originOf(base);
  const a = nodeFromSeed(randomBytes(32).toString("hex")), b = nodeFromSeed(randomBytes(32).toString("hex"));
  const step = async (label, promise, expect = 200) => {
    const r = await promise;
    log(`${label}: ${r.status} ${JSON.stringify(hide(r.body))}`);
    if (r.status !== expect) throw new Error(`${label} failed`);
    return r.body;
  };
  const info = await fetch(new URL("/v1/info", base)).then(r => r.json());
  log(`info: ${JSON.stringify(info)}`);
  const minted = await step("mint create code", post(base, "/v1/admin/mint", {kind: "create", count: 1}, adminToken));
  const room = await step("create (node A)", post(base, "/v1/node/redeem", redeemBody(a, origin, "create", {code: minted.codes[0], name: "Sim Room"})));
  const control = (token, body) => post(base, `/v1/aliases/${room.alias}/control`, body, token);
  const invite = await step("invite (node A)", control(room.token, {op: "invite"}));
  const joined = await step("join (node B)", post(base, "/v1/node/redeem", redeemBody(b, origin, "join", {code: invite.code})));
  const nodes = await step("nodes (node A)", control(room.token, {op: "nodes"}));
  await step("revoke (node A revokes B)", control(room.token, {op: "revoke", id: b.pub.slice(0, 8)}));
  await step("old token refused", control(joined.token, {op: "nodes"}), 401);
  const left = await step("leave (node A, last owner)", control(room.token, {op: "leave"}));
  log(`summary: ${nodes.total} nodes seen, room deleted=${left.deleted}`);
}

async function main() {
  const {values, positionals} = parseArgs({allowPositionals: true, options: {
    base: {type: "string"}, "seed-file": {type: "string"}, "admin-token-file": {type: "string"}, save: {type: "string"},
    "show-token": {type: "boolean"}, origin: {type: "string"}, pub: {type: "string"}, "key-file": {type: "string"}, pw: {type: "string"}, avoid: {type: "string"}}});
  const [cmd, ...rest] = positionals;
  if (!cmd || !values.base) {
    console.error("Usage: node-sim.mjs info|walkthrough|create|join|convert|control --base URL [options]");
    process.exit(2);
  }
  const base = values.base, origin = values.origin ?? originOf(base);
  const print = r => {
    console.log(JSON.stringify(values["show-token"] ? r.body : hide(r.body)));
    if (values.save && r.body.ok) writeFileSync(values.save, JSON.stringify(r.body) + "\n", {mode: 0o600});
    if (r.status !== 200) process.exitCode = 1;
  };
  if (cmd === "info") {console.log(await (await fetch(new URL("/v1/info", base))).text()); return;}
  if (cmd === "walkthrough") {
    const file = values["admin-token-file"];
    const token = file ? readFileSync(file, "utf8").trim() : process.env.ADMIN_TOKEN;
    if (!token) throw new Error("Give --admin-token-file or set ADMIN_TOKEN.");
    await walkthrough(base, token, console.log, values.origin);
    return;
  }
  const node = nodeFromSeed(loadSeed(values["seed-file"]));
  if (cmd === "join") print(await post(base, "/v1/node/redeem", redeemBody(node, origin, "join", {code: rest[0], avoid: values.avoid})));
  else if (cmd === "create") print(await post(base, "/v1/node/redeem", redeemBody(node, origin, "create", {code: rest[0], name: rest[1], avoid: values.avoid})));
  else if (cmd === "convert") {
    const key = readFileSync(values["key-file"], "utf8").trim();
    print(await post(base, "/v1/node/redeem", redeemBody(node, origin, "convert", {code: rest[0], name: rest[1], pub: values.pub, key, pw: values.pw})));
  } else if (cmd === "control") {
    const [alias, token, op, extra] = rest;
    print(await post(base, `/v1/aliases/${alias}/control`, {op, ...(extra ? JSON.parse(extra) : {})}, token));
  } else {
    console.error("Unknown command.");
    process.exit(2);
  }
}
if (import.meta.url === `file://${process.argv[1]}`) main().catch(error => {console.error(error.message); process.exit(1);});
