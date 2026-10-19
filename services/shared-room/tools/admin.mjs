#!/usr/bin/env node
// Operator commands for your own shared-room Worker (the /v1/admin API).
import {readFile} from "node:fs/promises";
import {
  fail, finish, files, privateFile, serviceUrl, call, describeFailure, readStdin,
} from "./deploy-common.mjs";

const usage = `Usage: node tools/admin.mjs [--url ORIGIN] [--token-stdin] COMMAND
  mint [--count N] [--ttl SECONDS]     create codes (1-20, 60-2592000 s; default 7 days)
  rooms [--offset N]                   list hosted rooms
  disable|enable ALIAS                 stop or resume a room
  delete ALIAS --yes                   remove a room and its data
  codes [--offset N]                   list codes (hash prefix only)
  revoke-code H                        revoke the code whose hash starts with H (8 hex or more)
  ban NODE [--until UNIX] | unban NODE block a node key (64 hex)
  invite ALIAS                         owner code for a room that lost its owner (15 min)
  audit [--offset N]                   operator and room events`;

const alias = v => /^[a-z0-9_-]{1,64}$/.test(v ?? "") ? v : fail(usage);
const hex = (v, min, max) => new RegExp(`^[a-f0-9]{${min},${max}}$`).test(v ?? "") ? v : fail(usage);
const int = (v, lo, hi) => {
  const n = Number(v);
  return Number.isInteger(n) && n >= lo && n <= hi ? n : fail(usage);
};

function takeOptions(args, allowed) {
  const opts = {}, rest = [];
  for (let i = 0; i < args.length; i++) {
    const a = args[i];
    if (!a.startsWith("--")) {rest.push(a); continue;}
    const key = a.slice(2);
    if (!(key in allowed)) fail(usage);
    if (allowed[key] === "flag") opts[key] = true;
    else {if (++i >= args.length) fail(usage); opts[key] = args[i];}
  }
  return {opts, rest};
}

const when = t => new Date(t * 1000).toISOString().replace(/\.\d+Z$/, "Z");

async function main() {
  const argv = process.argv.slice(2);
  const g = takeOptions(argv, {url: "value", "token-stdin": "flag", count: "value", ttl: "value",
    offset: "value", until: "value", yes: "flag"});
  const [command, ...args] = g.rest;
  if (!command) fail(usage);
  const base = serviceUrl(g.opts.url ?? (await readFile(files.url, "utf8").catch(() => "")).trim());
  const token = (g.opts["token-stdin"] ? await readStdin() : (await privateFile(files.token, "Admin token file")).trim());
  const offset = g.opts.offset === undefined ? 0 : int(g.opts.offset, 0, 1e9);
  const send = async (path, body) => {
    const r = await call(base, path, {body, token});
    if (r.status !== 200 || !r.json?.ok) fail(`Refused: ${describeFailure(r)}`);
    return r.json;
  };

  switch (command) {
    case "mint": {
      const j = await send("/v1/admin/mint", {kind: "create",
        count: g.opts.count === undefined ? 1 : int(g.opts.count, 1, 20),
        ...(g.opts.ttl === undefined ? {} : {ttl: int(g.opts.ttl, 60, 2592000)})});
      for (const code of j.codes) console.log(code);
      console.log(`Expires ${when(j.expires)}. Each code creates one room: cloudroom create CODE@${base.host} NAME`);
      break;
    }
    case "rooms": {
      const j = await send("/v1/admin/rooms", {offset});
      for (const r of j.rooms) console.log(`${r.alias}  ${r.state}  created ${when(r.created)}  active ${when(r.last_active)}  owner ${r.owner}  ${JSON.stringify(r.name)}`);
      if (j.next !== null) console.log(`More: --offset ${j.next}`);
      break;
    }
    case "disable": case "enable": {
      await send("/v1/admin/room", {alias: alias(args[0]), action: command});
      console.log(`Room ${command}d.`);
      break;
    }
    case "delete": {
      if (!g.opts.yes) fail("Deleting a room removes its history and key for every node. Add --yes to continue.");
      await send("/v1/admin/room", {alias: alias(args[0]), action: "delete"});
      console.log("Room deleted.");
      break;
    }
    case "codes": {
      const j = await send("/v1/admin/codes", {offset});
      for (const c of j.codes) console.log(`${c.h}  ${c.kind}  ${c.state}  expires ${when(c.expires)}  issuer ${c.issuer}${c.alias ? `  room ${c.alias}` : ""}`);
      if (j.next !== null) console.log(`More: --offset ${j.next}`);
      break;
    }
    case "revoke-code": {
      await send("/v1/admin/code", {h: hex(args[0], 8, 64), action: "revoke"});
      console.log("Code revoked.");
      break;
    }
    case "ban": {
      await send("/v1/admin/node", {node: hex(args[0], 64, 64), action: "ban",
        ...(g.opts.until === undefined ? {} : {until: int(g.opts.until, 1, 4e9)})});
      console.log("Node banned; its tokens stop working in every room.");
      break;
    }
    case "unban": {
      await send("/v1/admin/node", {node: hex(args[0], 64, 64), action: "unban"});
      console.log("Node unbanned.");
      break;
    }
    case "invite": {
      const j = await send("/v1/admin/invite", {alias: alias(args[0]), role: "owner"});
      console.log(j.code);
      console.log(`Owner code, valid ${j.ttl} s, works once. On the replacement node: cloudroom join ${j.code}@${base.host}`);
      break;
    }
    case "audit": {
      const j = await send("/v1/admin/audit", {offset});
      for (const r of j.rows) console.log(`${when(r.ts)}  ${r.actor}  ${r.action}  ${r.target}  ${r.result}`);
      if (j.next !== null) console.log(`More: --offset ${j.next}`);
      break;
    }
    default: fail(usage);
  }
}

main().catch(finish);
