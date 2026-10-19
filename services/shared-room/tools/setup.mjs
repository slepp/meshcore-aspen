#!/usr/bin/env node
// Deploys your own shared-room Worker with hosted rooms. See SELF_HOSTING.md.
import {readFile, writeFile} from "node:fs/promises";
import {createInterface} from "node:readline/promises";
import {
  ToolError, fail, finish, files, wrangler, readConfig, privateFile, writePrivate, exists,
  newKek, parseKek, newAdminToken, sha256Hex, serviceUrl, call, describeFailure, hostOf,
} from "./deploy-common.mjs";

const usage = `Usage: node tools/setup.mjs [--name WORKER] [--route HOST] [--signup code|open|closed]
                          [--url ORIGIN] [--mint]
       node tools/setup.mjs --restore-kek FILE
       node tools/setup.mjs --rotate-kek`;

function parse(argv) {
  const out = {};
  const value = i => {if (i + 1 >= argv.length || argv[i + 1].startsWith("--")) fail(usage); return argv[i + 1];};
  for (let i = 0; i < argv.length; i++) {
    switch (argv[i]) {
      case "--name": out.name = value(i++); break;
      case "--route": out.route = value(i++); break;
      case "--signup": out.signup = value(i++); break;
      case "--url": out.url = value(i++); break;
      case "--restore-kek": out.restore = value(i++); break;
      case "--rotate-kek": out.rotate = true; break;
      case "--mint": out.mint = true; break;
      case "--help": out.help = true; break;
      default: fail(usage);
    }
  }
  return out;
}

async function putSecret(name, text) {
  // The value travels on stdin only.
  const r = await wrangler(["secret", "put", name, "-c", files.localConfig], text);
  if (r.code !== 0) fail(`Wrangler could not store ${name}. Check "npx wrangler whoami" and retry.`);
}

async function secretNames() {
  const r = await wrangler(["secret", "list", "--format", "json", "-c", files.localConfig]);
  if (r.code !== 0) return new Set();
  try {return new Set(JSON.parse(r.stdout).map(s => s.name));} catch {return new Set();}
}

async function ask(question, fallback) {
  if (!process.stdin.isTTY) return fallback;
  const rl = createInterface({input: process.stdin, output: process.stdout});
  try {return (await rl.question(`${question} [${fallback}]: `)).trim() || fallback;}
  finally {rl.close();}
}

async function localConfig(opts) {
  if (await exists(files.localConfig)) {
    const config = await readConfig(files.localConfig);
    if (opts.name && opts.name !== config.name)
      fail(`wrangler.local.jsonc already names the Worker "${config.name}". Delete it to start a new deployment.`);
    return config;
  }
  const name = opts.name ?? await ask("Worker name", "aspen-rooms");
  if (!/^[a-z0-9][a-z0-9-]{0,62}$/.test(name)) fail("Worker names use lowercase letters, digits and hyphens.");
  const route = opts.route ?? (process.stdin.isTTY ? await ask("Custom domain (blank for workers.dev)", "") : "");
  if (route && !/^[a-z0-9]([a-z0-9.-]*[a-z0-9])?$/.test(route)) fail("The custom domain is not a valid host name.");
  const signup = opts.signup ?? "code";
  if (!["code", "open", "closed"].includes(signup)) fail("--signup takes code, open or closed.");
  const base = await readConfig(files.baseConfig);
  delete base.routes;
  const config = {...base, name, workers_dev: !route, keep_vars: true,
    observability: {enabled: false}, vars: {...base.vars, SIGNUP_MODE: signup}};
  if (route) config.routes = [{pattern: route, custom_domain: true}];
  await writeFile(files.localConfig, "// Local deployment. Git-ignored; edit vars here, then re-run setup.\n" +
    JSON.stringify(config, null, 2) + "\n", {mode: 0o644});
  return config;
}

async function main() {
  const opts = parse(process.argv.slice(2));
  if (opts.help) {console.log(usage); return;}
  if (Number(process.versions.node.split(".")[0]) < 20) fail("Node 20 or newer is required.");
  const who = await wrangler(["whoami"]);
  if (who.code !== 0) fail('Wrangler is not signed in. Run "npx wrangler login", then retry.');

  if (opts.restore) {
    const kek = parseKek(await privateFile(opts.restore, "Backup file"));
    if (!await exists(files.localConfig)) fail("Run setup once for this checkout before restoring a key.");
    await writePrivate(files.kek, JSON.stringify(kek) + "\n");
    await putSecret("ROOM_KEK", JSON.stringify(kek));
    console.log("ROOM_KEK restored from the backup file.");
    return;
  }
  if (opts.rotate) {
    const kek = parseKek(await privateFile(files.kek, "ROOM_KEK file"));
    let n = Object.keys(kek.keys).length + 1;
    while (Object.hasOwn(kek.keys, `k${n}`)) n++;
    const next = newKek(`k${n}`);
    kek.keys[next.id] = next.key; kek.current = next.id;
    await writePrivate(files.kek, JSON.stringify(kek) + "\n");
    await putSecret("ROOM_KEK", JSON.stringify(kek));
    console.log(`Added key ${next.id}. New rooms use it; older rooms keep opening with their own key.`);
    console.log("Copy .deploy/room-kek.json to your password manager again.");
    return;
  }

  const config = await localConfig(opts);
  // The address file is written only after a deploy succeeds, so a failed first deploy stays a first run.
  const firstRun = !await exists(files.url);
  const newToken = !await exists(files.token);
  let created = false;
  if (!await exists(files.kek)) {
    const k = newKek();
    await writePrivate(files.kek, JSON.stringify({current: k.id, keys: {[k.id]: k.key}}) + "\n");
    created = true;
  }
  if (newToken) await writePrivate(files.token, newAdminToken() + "\n");
  const kek = parseKek(await privateFile(files.kek, "ROOM_KEK file"));
  const token = (await privateFile(files.token, "Admin token file")).trim();

  const dep = await wrangler(["deploy", "-c", files.localConfig]);
  if (dep.code !== 0) fail('Deploy failed. Run "npx wrangler deploy -c wrangler.local.jsonc" to see the error.');

  const have = await secretNames();
  if (created || !have.has("ROOM_KEK")) await putSecret("ROOM_KEK", JSON.stringify(kek));
  if (newToken || !have.has("ADMIN_TOKEN_HASH")) await putSecret("ADMIN_TOKEN_HASH", sha256Hex(token));

  const route = config.routes?.[0]?.pattern;
  const found = dep.stdout.match(/https:\/\/[a-z0-9.-]+\.workers\.dev/i)?.[0];
  const origin = opts.url ?? (route ? `https://${route}` : found);
  if (!origin) fail('Deployed, but the address was not reported. Rerun with --url https://YOUR-WORKER.workers.dev.');
  const base = serviceUrl(origin);
  await writePrivate(files.url, base.origin + "\n");

  let info;
  for (let i = 0; i < 6 && !info; i++) {
    try {
      const r = await call(base, "/v1/info");
      if (r.status === 200 && r.json?.protocol === "aspen-node.v1") info = r.json;
    } catch {/* custom domains can take a minute to answer */}
    if (!info) await new Promise(r => setTimeout(r, i ? 5000 : 1000));
  }
  if (!info) fail(`Deployed, but ${base.origin}/v1/info did not answer. Run: node tools/doctor.mjs`);
  console.log(`Worker ${config.name} runs at ${base.origin} (version ${info.version}, signup ${info.signup}).`);
  if (created || firstRun) console.log("Copy .deploy/room-kek.json to your password manager. Without it, hosted rooms cannot be recovered.");
  if (firstRun) console.log("The operator token is in .deploy/admin-token; node tools/admin.mjs reads it from there.");

  if (firstRun || opts.mint) {
    const r = await call(base, "/v1/admin/mint", {body: {kind: "create", count: 1}, token});
    const code = r.json?.codes?.[0];
    if (r.status !== 200 || !code) fail(`Could not mint a create code (${describeFailure(r)}). Run: node tools/doctor.mjs`);
    console.log("\nCreate your first room from the companion app's Management console on an Aspen node:");
    const host = hostOf(base);
    console.log(`  cloudroom create ${code}@${host} My Room`);
    if (Buffer.byteLength(host) > 36)
      console.log("This host name is longer than a direct message allows (36 bytes). Use a shorter custom domain, or upload a profile with admin.py configure.");
    console.log("The code works once and expires in 7 days.");
  }
}

main().catch(finish);
