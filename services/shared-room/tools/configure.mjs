#!/usr/bin/env node
// Operator-run handoff. Secret values never belong in command arguments/logs.
import {readFile, stat} from "node:fs/promises";
import {spawn} from "node:child_process";
import {fileURLToPath} from "node:url";

const args = process.argv.slice(2);
const file = args[args.indexOf("--file") + 1];
const account = args[args.indexOf("--account") + 1];
const keyFile = args[args.indexOf("--keys") + 1];
const validate = args.includes("--validate");
function reject(message) {throw new Error(message);}
function record(value) {return value && typeof value === "object" && !Array.isArray(value);}
const id = /^[a-zA-Z0-9_-]{1,64}$/;

try {
  if (!args.includes("--file") || !file || file.startsWith("--") ||
      !args.includes("--keys") || !keyFile || keyFile.startsWith("--") ||
      (!validate && (!args.includes("--account") || !/^[a-f0-9]{32}$/.test(account ?? ""))))
    reject("Use --file PRIVATE_JSON --keys PRIVATE_WORKER_KEYS --account ACCOUNT_ID, or add --validate without --account.");
  const info = await stat(file);
  if (!info.isFile() || info.size > 128 * 1024 || (info.mode & 0o077) ||
      (process.getuid && info.uid !== process.getuid()))
    reject("Configuration must be an operator-owned private file (0600 or 0400), at most 128KiB.");
  let config;
  try {config = JSON.parse(await readFile(file, "utf8"));}
  catch {reject("Cannot read a valid configuration JSON file.");}
  if (!record(config) || Object.keys(config).sort().join(",") !== "ALIASES,FRONTENDS" ||
      !record(config.ALIASES) || !record(config.FRONTENDS) ||
      !Object.keys(config.ALIASES).length || !Object.keys(config.FRONTENDS).length)
    reject("Configuration must contain nonempty ALIASES and FRONTENDS objects only.");
  const keys = new Set(), tokens = new Set();
  for (const [name, alias] of Object.entries(config.ALIASES)) {
    if (!id.test(name) || !record(alias) || !id.test(alias.backend ?? "") ||
        !/^[a-f0-9]{64}$/.test(alias.publicKey ?? "") || keys.has(alias.publicKey) ||
        typeof alias.name !== "string" || !alias.name || !alias.name.isWellFormed() ||
        Buffer.byteLength(alias.name) > 31 || alias.name.includes("\0") ||
        typeof alias.password !== "string" || !/^[\x20-\x7e]{0,15}$/.test(alias.password) ||
        Object.keys(alias).sort().join(",") !== "backend,name,password,publicKey")
      reject("Invalid/duplicate alias identity or native room name/password.");
    keys.add(alias.publicKey);
  }
  for (const [name, frontend] of Object.entries(config.FRONTENDS)) {
    if (!id.test(name) || !record(frontend) || typeof frontend.token !== "string" ||
        !/^[\x21-\x7e]{32,256}$/.test(frontend.token) || tokens.has(frontend.token) ||
        !Array.isArray(frontend.aliases) || !frontend.aliases.length ||
        new Set(frontend.aliases).size !== frontend.aliases.length ||
        frontend.aliases.some(alias => !Object.hasOwn(config.ALIASES, alias)) ||
        Object.keys(frontend).sort().join(",") !== "aliases,token")
      reject("Invalid/duplicate frontend token or alias grant.");
    tokens.add(frontend.token);
  }
  const keyInfo = await stat(keyFile);
  if (!keyInfo.isFile() || keyInfo.size > 128 * 1024 || (keyInfo.mode & 0o077) ||
      (process.getuid && keyInfo.uid !== process.getuid()))
    reject("Configuration must use operator-owned private key files (0600 or 0400).");
  let keyConfig;
  try {keyConfig = JSON.parse(await readFile(keyFile, "utf8"));}
  catch {reject("Cannot read a valid private Worker-key JSON file.");}
  if (!record(keyConfig) || keyConfig.worker !== "aspen-shared-room" || keyConfig.keyLocation !== "worker" ||
      !record(keyConfig.expandedKeys) || Object.keys(keyConfig).sort().join(",") !== "expandedKeys,keyLocation,worker" ||
      Object.keys(keyConfig.expandedKeys).sort().join(",") !== Object.keys(config.ALIASES).sort().join(","))
    reject("Invalid/duplicate Worker-key target or alias set.");
  const wasm = new WebAssembly.Instance(new WebAssembly.Module(await readFile(new URL("../src/native-crypto.wasm", import.meta.url))), {}).exports;
  const memory = new Uint8Array(wasm.memory.buffer, wasm.mc_arena(), 2048);
  for (const [alias, raw] of Object.entries(keyConfig.expandedKeys)) {
    if (typeof raw !== "string" || !/^[a-f0-9]{128}$/.test(raw)) reject("Invalid/duplicate expanded Worker key.");
    const key = Buffer.from(raw, "hex");
    try {
      if ((key[0] & 7) || (key[31] & 0xc0) !== 0x40) reject("Invalid/duplicate expanded Worker key.");
      memory.set(key); wasm.mc_pub();
      if (Buffer.from(memory.slice(64,96)).toString("hex") !== config.ALIASES[alias].publicKey)
        reject("Invalid/duplicate Worker key and advertised public key pairing.");
    } finally {memory.fill(0);key.fill(0);}
  }
  if (validate) {console.log("Private opaque configuration and Worker key pairings validated; no upload performed.");}
  else {
    // Ordinary tool stdin is not a secret handoff. This process is run by the
    // operator and reads their private mount/file locally, outside model input.
    const wrangler = fileURLToPath(new URL("../node_modules/wrangler/bin/wrangler.js", import.meta.url));
    const workerConfig = fileURLToPath(new URL("../wrangler.jsonc", import.meta.url));
    const child = spawn(process.execPath, [wrangler, "secret", "bulk", "--name", "aspen-shared-room", "--config", workerConfig], {
      stdio: ["pipe", "inherit", "inherit"],
      env: {...process.env, CLOUDFLARE_ACCOUNT_ID: account, WRANGLER_LOG_SANITIZE: "true", WRANGLER_LOG: "error", WRANGLER_SEND_METRICS: "false"},
    });
    child.stdin.on("error", () => {});
    const done = new Promise((resolve, fail) => {child.once("error", fail); child.once("exit", code => resolve(code));});
    child.stdin.end(JSON.stringify({ALIASES: JSON.stringify(config.ALIASES), FRONTENDS: JSON.stringify(config.FRONTENDS), ROOM_KEYS: JSON.stringify(keyConfig.expandedKeys)}));
    if (await done !== 0) reject("Wrangler did not confirm the secret upload.");
    console.log("ALIASES/FRONTENDS/ROOM_KEYS uploaded to aspen-shared-room. Deploy the opaque Worker before enabling frontend connections.");
  }
} catch (error) {
  // Only our fixed validation messages are printable; never echo parser input,
  // external process errors, environment or configuration object values.
  const message = error instanceof Error && /^(Use |Configuration must |Cannot read |Invalid\/duplicate |Wrangler did not )/.test(error.message)
    ? error.message : "Private configuration handoff failed; inspect local file/access and Wrangler installation.";
  console.error(message); process.exitCode = 1;
}
