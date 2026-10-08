#!/usr/bin/env node
import {constants} from "node:fs";
import {lstat, mkdir, open, rename, unlink} from "node:fs/promises";
import {pbkdf2Sync, randomBytes} from "node:crypto";
import {parseArgs} from "node:util";
import {dirname, resolve} from "node:path";
import {spawn} from "node:child_process";
import {fileURLToPath} from "node:url";
import {checkSecretSlots} from "./bindings.mjs";

const userID = /^[a-z0-9][a-z0-9_-]{2,63}$/, aliasID = /^[a-zA-Z0-9_-]{1,64}$/;
export function validateUsers(users) {
  if (!users || typeof users !== "object" || Array.isArray(users)) throw Error("Invalid account file");
  for (const [username, user] of Object.entries(users)) {
    if (!userID.test(username) || !user || Object.keys(user).sort().join(",") !== "aliases,hash,iterations,name,salt" ||
        typeof user.name !== "string" || !user.name.isWellFormed() || user.name !== user.name.trim() ||
        !Buffer.byteLength(user.name) || Buffer.byteLength(user.name) > 24 || /[\u0000-\u001f\u007f-\u009f:]/u.test(user.name) ||
        !/^[a-f0-9]{32}$/.test(user.salt) || !/^[a-f0-9]{64}$/.test(user.hash) || user.iterations !== 100000 ||
        !Array.isArray(user.aliases) || !user.aliases.length || user.aliases.some(alias => !aliasID.test(alias)) ||
        new Set(user.aliases).size !== user.aliases.length) throw Error("Invalid account file");
  }
  return users;
}
export function passwordRecord(password, name, aliases) {
  if (typeof password !== "string" || !password.isWellFormed() || Buffer.byteLength(password) < 16 ||
      Buffer.byteLength(password) > 256 || /[\u0000\r\n]/u.test(password))
    throw Error("Use an account password of 16..256 UTF-8 bytes without line breaks");
  const salt = randomBytes(16), input = Buffer.from(password);
  try {return {name, salt: salt.toString("hex"), iterations: 100000, aliases,
    hash: pbkdf2Sync(input, salt, 100000, 32, "sha256").toString("hex")};}
  finally {input.fill(0);}
}
async function privateFile(path, maxBytes) {
  const file = await open(path, constants.O_RDONLY | constants.O_NOFOLLOW);
  try {
    const info = await file.stat();
    if (!info.isFile() || info.size > maxBytes || (info.mode & 0o077) ||
        process.getuid && info.uid !== process.getuid()) throw Error("Use an operator-owned private file (0600 or 0400)");
    return await file.readFile();
  } finally {await file.close();}
}
async function hiddenPassword(label) {
  if (!process.stdin.isTTY) throw Error("Use a terminal password prompt or --password-file PRIVATE_FILE");
  process.stderr.write(label);
  const wasRaw = process.stdin.isRaw;
  process.stdin.setRawMode(true); process.stdin.resume(); process.stdin.setEncoding("utf8");
  let password = "";
  return new Promise((resolvePassword, reject) => {
    const finish = error => {
      process.stdin.removeListener("data", read);
      process.stdin.setRawMode(wasRaw); process.stdin.pause(); process.stderr.write("\n");
      if (error) reject(error); else resolvePassword(password);
    };
    const read = data => {
      for (const char of data) {
        if (char === "\u0003" || char === "\u0004") {finish(Error("Account password entry cancelled")); return;}
        if (char === "\r" || char === "\n") {finish(); return;}
        if (char === "\u007f" || char === "\b") password = Array.from(password).slice(0, -1).join("");
        else if (char >= " " && char !== "\u001b") password += char;
      }
    };
    process.stdin.on("data", read);
  });
}
async function upload(users, account) {
  if (!/^[a-f0-9]{32}$/.test(account ?? "")) throw Error("Use --account CLOUDFLARE_ACCOUNT_ID with --upload");
  const wrangler = fileURLToPath(new URL("../node_modules/wrangler/bin/wrangler.js", import.meta.url));
  const config = fileURLToPath(new URL("../wrangler.jsonc", import.meta.url));
  const env = {...process.env, CLOUDFLARE_ACCOUNT_ID: account, WRANGLER_LOG_SANITIZE: "true",
    WRANGLER_LOG: "error", WRANGLER_SEND_METRICS: "false"};
  await checkSecretSlots(async args => {
    const child = spawn(process.execPath, [wrangler, ...args, "--config", config], {
      env: {...env, WRANGLER_LOG: "log"}, stdio: ["ignore", "pipe", "pipe"],
    });
    let output = "";
    child.stdout.on("data", chunk => {output += chunk;}); child.stderr.resume();
    const code = await new Promise((resolveCode, reject) => {child.once("error", reject); child.once("exit", resolveCode);});
    if (code !== 0) throw Error("Could not inspect the active Worker; no accounts were uploaded");
    return JSON.parse(output);
  });
  const child = spawn(process.execPath, [wrangler, "secret", "bulk", "--name", "aspen-shared-room", "--config", config], {
    env, stdio: ["pipe", "ignore", "ignore"],
  });
  let inputError;
  child.stdin.on("error", error => {inputError = error;});
  const done = new Promise((resolveCode, reject) => {child.once("error", reject); child.once("exit", resolveCode);});
  child.stdin.end(JSON.stringify({WEB_USERS: JSON.stringify(users)}));
  if (await done !== 0 || inputError) throw Error("Worker account upload was not confirmed");
}
export async function main(args) {
  const {values} = parseArgs({args, options: {
    file: {type: "string"}, username: {type: "string"}, name: {type: "string"}, aliases: {type: "string"},
    "password-file": {type: "string"}, remove: {type: "boolean"}, upload: {type: "boolean"}, account: {type: "string"},
  }});
  if (!values.file) throw Error("Use --file PRIVATE_ACCOUNT_JSON and --username USER, or --upload --account ACCOUNT_ID");
  const path = resolve(values.file), parent = dirname(path);
  await mkdir(parent, {recursive: true, mode: 0o700});
  const directory = await lstat(parent);
  if (!directory.isDirectory() || (directory.mode & 0o077) ||
      process.getuid && directory.uid !== process.getuid()) throw Error("Use an operator-owned private account directory (0700)");
  let users = Object.create(null);
  try {users = validateUsers(JSON.parse((await privateFile(path, 128 * 1024)).toString("utf8")));}
  catch (error) {if (error.code !== "ENOENT") throw error;}
  if (values.username) {
    if (!userID.test(values.username)) throw Error("Use a lowercase username of 3..64 letters, digits, hyphens or underscores");
    if (values.remove) {
      if (!Object.hasOwn(users, values.username)) throw Error("Account does not exist in the private file");
      delete users[values.username];
    } else {
      if (!values.name || !values.aliases) throw Error("Creating an account requires --name and --aliases ALIAS[,ALIAS]");
      let password;
      if (values["password-file"]) {
        const raw = await privateFile(resolve(values["password-file"]), 1024);
        try {password = new TextDecoder("utf-8", {fatal: true}).decode(raw).replace(/\r?\n$/, "");} finally {raw.fill(0);}
      } else {
        password = await hiddenPassword("Account password (not echoed): ");
        if (password !== await hiddenPassword("Confirm account password: ")) throw Error("Account passwords did not match");
      }
      users[values.username] = passwordRecord(password, values.name, values.aliases.split(","));
      password = undefined;
    }
    validateUsers(users);
    const content = JSON.stringify(users, null, 2) + "\n", temporary = path + "." + randomBytes(8).toString("hex") + ".tmp";
    let created = false;
    try {
      const file = await open(temporary, constants.O_WRONLY | constants.O_CREAT | constants.O_EXCL | constants.O_NOFOLLOW, 0o600);
      created = true;
      try {await file.writeFile(content); await file.sync();} finally {await file.close();}
      await rename(temporary, path); created = false;
      const dir = await open(parent, constants.O_RDONLY);
      try {await dir.sync();} finally {await dir.close();}
      if ((await privateFile(path, 128 * 1024)).toString("utf8") !== content) throw Error("Saved account file did not match");
    } finally {if (created) await unlink(temporary);}
    console.log("Private account file saved; passwords were not stored in plaintext.");
  } else if (!values.upload) throw Error("Use --username to edit an account, or --upload to publish the existing file");
  if (values.upload) {await upload(users, values.account); console.log("WEB_USERS uploaded; account passwords and room grants now control desktop access.");}
}
if (process.argv[1] && resolve(process.argv[1]) === fileURLToPath(import.meta.url)) {
  main(process.argv.slice(2)).catch(error => {
    // Parser/process errors may contain private input; only our fixed messages leave this process.
    const known = error instanceof Error && /^(Use |Invalid account file|Account |Creating an account|Could not inspect|Worker account upload|Saved account file)/.test(error.message);
    console.error(known ? error.message : "Account setup failed; check private-file access and the local Wrangler installation.");
    process.exitCode = 1;
  });
}
