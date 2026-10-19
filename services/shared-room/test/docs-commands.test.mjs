// Every `cloudroom ...` command shown in the operator docs must follow the
// grammar of the Management console. Placeholders (CODE, HOST, ALIAS, ...) and
// bracketed alternatives are wildcards; concrete values are checked.
import {test} from "node:test";
import assert from "node:assert/strict";
import {readFile} from "node:fs/promises";
import {fileURLToPath} from "node:url";

const repo = fileURLToPath(new URL("../../../", import.meta.url));
const docs = [
  "services/shared-room/SELF_HOSTING.md", "services/shared-room/NATIVE.md", "services/shared-room/README.md",
  "services/shared-room/TRUST.md", "services/shared-room/WEB.md", "firmware/esp32/PUBLIC_SETUP.md",
  "README.md", "ARCHITECTURE.md", "NODE_BACKUP.md", "SECURITY.md", "RELEASING.md", "release/README.md",
];

const placeholders = new Set(["CODE", "HOST", "ALIAS", "NAME", "PW", "ID", "N", "FILE"]);
const wild = t => /[\[\]|{}]/.test(t) || placeholders.has(t.split("@")[0]) || placeholders.has(t.split("@")[1] ?? "") && t.includes("@");
const hostOk = h => h.length <= 36 && /^([a-z0-9]([a-z0-9-]{0,61}[a-z0-9])?)(\.[a-z0-9]([a-z0-9-]{0,61}[a-z0-9])?)*(:\d+)?$/.test(h) && !h.includes(":");
const codeOk = t => {
  if (wild(t)) return true;
  const [code, host, ...extra] = t.split("@");
  if (extra.length || !/^[0-9A-HJKMNP-TV-Z-]+$/i.test(code) || code.replaceAll("-", "").length !== 16) return false;
  return host === undefined || hostOk(host) || wild(host);
};
const aliasOk = t => wild(t) || /^[1-2]$/.test(t) || /^[a-z0-9_-]{1,64}$/.test(t);
const page = t => wild(t) || /^\d{1,2}$/.test(t);

const grammar = {
  status: a => a.length === 0,
  error: a => a.length === 0,
  list: a => a.length <= 1 && a.every(page),
  probe: a => a.length <= 1 && a.every(h => wild(h) || hostOk(h)),
  join: a => a.length === 1 && codeOk(a[0]),
  create: a => a.length >= 2 && (a[0].toUpperCase() === "OPEN" || codeOk(a[0])) && a.slice(1).join(" ").length <= 31,
  convert: a => a.length >= 1 && codeOk(a[0]) && (a.length === 1 || (a[1] === "yes" && a.length <= 3 && (a[2] === undefined || wild(a[2]) || hostOk(a[2])))),
  invite: a => a.length === 0 || (a.length === 1 && (a[0] === "owner" || wild(a[0]))),
  nodes: a => a.length <= 1 && a.every(page),
  revoke: a => a.length === 1 && (wild(a[0]) || /^[0-9a-f]{8,16}$/.test(a[0])),
  password: a => a.length <= 1,
  unlink: a => { const r = a.filter(x => !["delete", "local", "force"].includes(x)); return r.length <= 1 && r.every(aliasOk); },
  share: a => ["on", "off"].includes(a[0]) && (a.length === 1 || (a.length === 2 && a[1] === "force")) || wild(a[0] ?? ""),
  result: a => a.length === 0,
  apply: a => a.length === 0,
  advertise: a => a.length <= 1 && a.every(aliasOk),
  enable: a => a.length === 1 && (["on", "off"].includes(a[0]) || wild(a[0])),
  config: a => ["api", "status", "hash", "retain", "begin", "chunk", "commit", "abort"].includes(a[0]),
  configure: a => a.length === 1,
};

function commands(text) {
  const found = [];
  let fenced = false;
  for (const [index, line] of text.split("\n").entries()) {
    if (line.startsWith("```")) {fenced = !fenced; continue;}
    if (fenced) {
      const m = line.match(/^(?:\s*)(cloudroom (.+))$/);
      if (m) found.push({line: index + 1, text: m[1]});
    } else for (const m of line.matchAll(/`(cloudroom [^`]+)`/g)) found.push({line: index + 1, text: m[1]});
  }
  return found;
}

test("documented cloudroom commands follow the Management grammar", async () => {
  let checked = 0;
  const bad = [];
  for (const doc of docs) {
    for (const {line, text} of commands(await readFile(repo + doc, "utf8"))) {
      const tokens = text.replace(/\\\|/g, "|").trim().split(/\s+/).slice(1);
      // Prose fragments such as `cloudroom` followed by a sentence are not commands.
      if (!tokens.length || !(tokens[0] in grammar)) {
        if (/^[a-z]+$/.test(tokens[0] ?? "") && !tokens[0].startsWith("-")) bad.push(`${doc}:${line}: unknown verb in "${text}"`);
        continue;
      }
      const args = tokens.slice(1);
      // A bare verb or a bracketed synopsis is a reference, not a command line.
      if (!args.length || /[\[|]/.test(text)) {checked++; continue;}
      // Optional-argument markers in usage tables are wildcards.
      const concrete = args.map(a => a.replace(/^\[|\]$/g, ""));
      if (!grammar[tokens[0]](concrete.filter(a => a !== "")) ) bad.push(`${doc}:${line}: "${text}"`);
      checked++;
    }
  }
  assert.deepEqual(bad, []);
  assert.ok(checked >= 30, `only ${checked} commands checked`);
});

test("the node guide quotes the fixed reply texts", async () => {
  const native = await readFile(repo + "services/shared-room/NATIVE.md", "utf8");
  for (const text of [
    "Code ABCD-EFGH-JKMN-PQRS (10 min, once). Run: cloudroom join ABCD-EFGH-JKMN-PQRS@",
    "Linked. Restarts in 10 s. Ask the room owner for the password. If the room is not in your app, run \"cloudroom advertise 1\".",
    "Key goes to aspen-rooms.example.workers.dev; it can speak as the room. Admin logins stay. Add \"yes aspen-rooms.example.workers.dev\".",
    "Error: link busy; send \"cloudroom result\"",
  ]) assert.ok(native.includes(text), `missing: ${text}`);
});
