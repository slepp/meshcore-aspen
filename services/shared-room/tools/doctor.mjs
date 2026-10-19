#!/usr/bin/env node
// Checks a self-hosted shared-room deployment. Prints findings; no secret values.
import {readFile} from "node:fs/promises";
import {finish, files, wrangler, readConfig, privateFile, parseKek, serviceUrl, call, exists, fail} from "./deploy-common.mjs";

let failed = false;
const ok = m => console.log(`ok    ${m}`);
const warn = m => console.log(`warn  ${m}`);
const bad = m => {failed = true; console.log(`FAIL  ${m}`);};

async function main() {
  const urlArg = process.argv.indexOf("--url");
  const origin = urlArg > 0 ? process.argv[urlArg + 1] : (await readFile(files.url, "utf8").catch(() => "")).trim();
  if (!origin) fail("No service address. Run node tools/setup.mjs, or pass --url ORIGIN.");
  const base = serviceUrl(origin);

  let info = null;
  try {
    const r = await call(base, "/v1/info");
    if (r.status === 200 && r.json?.protocol === "aspen-node.v1") {
      info = r.json;
      ok(`${base.origin} answers: ${info.service} ${info.version}, signup ${info.signup}`);
    } else bad(`${base.origin}/v1/info returned HTTP ${r.status}. Deploy the 0.1.20 Worker (node tools/setup.mjs).`);
  } catch {bad(`${base.origin} did not answer. Check the Worker name, route and DNS.`);}

  const source = await readFile(new URL("../src/protocol.ts", import.meta.url), "utf8").catch(() => "");
  const expected = source.match(/SERVICE_VERSION\s*=\s*"([^"]+)"/)?.[1];
  if (info && expected) info.version === expected ? ok(`Worker version matches this checkout (${expected})`)
    : warn(`Worker runs ${info.version}; this checkout is ${expected}. Run node tools/setup.mjs to deploy it.`);
  if (info?.signup === "open") warn("Signup is open: anyone can create rooms. Add a Cloudflare WAF rate-limit rule on /v1/node/*, or set SIGNUP_MODE=code.");
  if (info?.signup === "closed") warn("Signup is closed: ROOM_KEK is missing or SIGNUP_MODE=closed. Hosted rooms answer 503 until ROOM_KEK is set.");

  if (await exists(files.kek)) {
    try {parseKek(await privateFile(files.kek, "ROOM_KEK file")); ok(".deploy/room-kek.json is private and valid. Keep a copy in your password manager.");}
    catch (e) {bad(e.message);}
  } else warn("No .deploy/room-kek.json here. If this Worker was set up elsewhere, hosted rooms depend on that copy; setup --restore-kek FILE re-uploads it.");

  if (await exists(files.localConfig)) {
    const config = await readConfig(files.localConfig);
    config.observability?.enabled === false ? ok("Workers Logs are off in wrangler.local.jsonc")
      : warn("Workers Logs may be on. Set \"observability\": {\"enabled\": false} so request details stay out of logs.");
    const names = new Set((config.migrations ?? []).map(m => m.tag));
    names.has("v3") ? ok("Durable Object migration v3 (Registry) is configured") : bad("wrangler.local.jsonc lacks migration v3. Update this checkout and delete wrangler.local.jsonc to regenerate it.");
    config.durable_objects?.bindings?.some(b => b.name === "REGISTRY") ? ok("REGISTRY binding is configured") : bad("REGISTRY binding is missing from wrangler.local.jsonc.");
    const r = await wrangler(["secret", "list", "--format", "json", "-c", files.localConfig]);
    if (r.code === 0) {
      let secrets = new Set();
      try {secrets = new Set(JSON.parse(r.stdout).map(s => s.name));} catch {/* leave empty */}
      for (const name of ["ROOM_KEK", "ADMIN_TOKEN_HASH"])
        secrets.has(name) ? ok(`${name} is set`) : bad(`${name} is not set. Run node tools/setup.mjs.`);
    } else warn('Could not list secrets. Run "npx wrangler login".');
  } else warn("No wrangler.local.jsonc here; skipped local configuration checks.");

  if (await exists(files.token)) {
    const token = (await privateFile(files.token, "Admin token file")).trim();
    try {
      const r = await call(base, "/v1/admin/rooms", {body: {offset: 0}, token});
      r.status === 200 ? ok("Operator API accepts the admin token")
        : r.status === 404 ? bad("Operator API is off: ADMIN_TOKEN_HASH is not set on the Worker.")
        : bad(`Operator API refused the admin token (HTTP ${r.status}). Run setup again or use the token that matches ADMIN_TOKEN_HASH.`);
    } catch {bad("Operator API did not answer.");}
  } else warn("No .deploy/admin-token here; skipped the operator API check.");

  if (failed) process.exitCode = 1;
}

main().catch(finish);
