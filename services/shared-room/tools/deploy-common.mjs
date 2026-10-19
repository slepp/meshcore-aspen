// Shared helpers for setup.mjs, admin.mjs and doctor.mjs.
// Secrets move through files with mode 0600 and child-process stdin only:
// never through argv, environment variables or output.
import {chmod, mkdir, open, readFile, stat, writeFile} from "node:fs/promises";
import {spawn} from "node:child_process";
import {createHash, randomBytes} from "node:crypto";
import {join} from "node:path";
import {fileURLToPath} from "node:url";

export const root = fileURLToPath(new URL("..", import.meta.url));
export const deployDir = join(root, ".deploy");
export const files = {
  kek: join(deployDir, "room-kek.json"),
  token: join(deployDir, "admin-token"),
  url: join(deployDir, "url"),
  localConfig: join(root, "wrangler.local.jsonc"),
  baseConfig: join(root, "wrangler.jsonc"),
};

export class ToolError extends Error {}
export function fail(message) {throw new ToolError(message);}

export function wranglerEntry() {
  return process.env.ASPEN_WRANGLER || join(root, "node_modules/wrangler/bin/wrangler.js");
}

// Runs Wrangler with `input` on stdin. Returns {code, stdout}. Output is
// captured, never echoed, because some commands print identifiers.
export function wrangler(args, input) {
  return new Promise((resolve, reject) => {
    const child = spawn(process.execPath, [wranglerEntry(), ...args], {
      cwd: root, stdio: ["pipe", "pipe", "pipe"],
      env: {...process.env, WRANGLER_SEND_METRICS: "false", WRANGLER_LOG_SANITIZE: "true"},
    });
    let stdout = "";
    child.stdout.setEncoding("utf8");
    child.stdout.on("data", data => {stdout += data;});
    child.stderr.resume();
    child.stdin.on("error", () => {});
    child.once("error", reject);
    child.once("exit", code => resolve({code, stdout}));
    child.stdin.end(input ?? "");
  });
}

export function stripJsonc(text) {
  let out = "", i = 0;
  while (i < text.length) {
    const c = text[i], n = text[i + 1];
    if (c === '"') {
      let j = i + 1;
      while (j < text.length && text[j] !== '"') j += text[j] === "\\" ? 2 : 1;
      out += text.slice(i, j + 1); i = j + 1;
    } else if (c === "/" && n === "/") {
      while (i < text.length && text[i] !== "\n") i++;
    } else if (c === "/" && n === "*") {
      const end = text.indexOf("*/", i + 2);
      i = end < 0 ? text.length : end + 2;
    } else {out += c; i++;}
  }
  return out.replace(/,(\s*[}\]])/g, "$1");
}

export async function readConfig(path) {
  try {return JSON.parse(stripJsonc(await readFile(path, "utf8")));}
  catch {fail(`Cannot read ${path} as JSON.`);}
}

export async function privateFile(path, what) {
  let info;
  try {info = await stat(path);} catch {fail(`${what} not found: ${path}`);}
  if (!info.isFile() || (info.mode & 0o077) || (process.getuid && info.uid !== process.getuid()))
    fail(`${what} must be an operator-owned regular file with mode 0600: ${path}`);
  return readFile(path, "utf8");
}

export async function exists(path) {
  try {await stat(path); return true;} catch {return false;}
}

export async function writePrivate(path, text) {
  await mkdir(deployDir, {recursive: true, mode: 0o700});
  await chmod(deployDir, 0o700);
  const handle = await open(path, "w", 0o600);
  try {await handle.writeFile(text);} finally {await handle.close();}
  await chmod(path, 0o600);
}

export function newKek(id = "k1") {return {id, key: randomBytes(32).toString("hex")};}
export function parseKek(text) {
  let kek;
  try {kek = JSON.parse(text);} catch {fail("ROOM_KEK file is not valid JSON.");}
  const keys = kek && typeof kek.keys === "object" && kek.keys ? kek.keys : null;
  if (!keys || typeof kek.current !== "string" || !Object.hasOwn(keys, kek.current) ||
      Object.values(keys).some(v => !/^[a-f0-9]{64}$/.test(v)) ||
      Object.keys(keys).some(k => !/^[a-z0-9_-]{1,16}$/.test(k)))
    fail('ROOM_KEK file must look like {"current":"k1","keys":{"k1":"<64 hex>"}}.');
  return kek;
}
export const sha256Hex = text => createHash("sha256").update(text).digest("hex");
export const newAdminToken = () => randomBytes(32).toString("hex");

// https for any host; plain http only for a loopback development server.
export function serviceUrl(value) {
  let url;
  try {url = new URL(value);} catch {fail("The service URL is not valid.");}
  const loopback = ["127.0.0.1", "localhost", "[::1]"].includes(url.hostname);
  if (url.username || url.password || url.search || url.hash || url.pathname !== "/" ||
      !(url.protocol === "https:" || (url.protocol === "http:" && loopback)))
    fail("Use the service origin, such as https://rooms.example.workers.dev.");
  return url;
}

export async function call(base, path, {body, token} = {}) {
  const res = await fetch(new URL(path, base), {
    method: body === undefined ? "GET" : "POST", redirect: "error",
    headers: {...(body === undefined ? {} : {"content-type": "application/json"}),
      ...(token ? {authorization: `Bearer ${token}`} : {})},
    body: body === undefined ? undefined : JSON.stringify(body),
    signal: AbortSignal.timeout(15000),
  });
  let json = null;
  try {json = await res.json();} catch {/* non-JSON body */}
  return {status: res.status, json};
}

export function describeFailure(result) {
  const j = result.json;
  return j && j.e ? `${j.e}${j.m ? `: ${j.m}` : ""}` : `HTTP ${result.status}`;
}

export function hostOf(base) {return new URL(base).host;}

export async function readStdin() {
  const chunks = [];
  for await (const chunk of process.stdin) chunks.push(chunk);
  return Buffer.concat(chunks).toString("utf8").trim();
}

export function finish(error) {
  console.error(error instanceof ToolError ? error.message : "Unexpected failure; no details are printed to protect secrets.");
  process.exitCode = 1;
}
