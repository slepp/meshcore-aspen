#!/usr/bin/env node
import {createHash, randomBytes} from "node:crypto";
import {parseArgs} from "node:util";
import {resolve} from "node:path";
import {fileURLToPath} from "node:url";
import {privateFile, savePrivateFile, uploadSecret, validateUsers} from "./web-users.mjs";

const hash = value => createHash("sha256").update(value).digest("hex");
export function validateEnrollments(entries) {
  if (!entries || typeof entries !== "object" || Array.isArray(entries)) throw Error("Invalid enrollment file");
  for (const [id, entry] of Object.entries(entries))
    if (!/^[a-f0-9]{64}$/.test(id) || !entry || Object.keys(entry).sort().join(",") !== "expires,grant,username" ||
        !/^[a-z0-9][a-z0-9_-]{2,63}$/.test(entry.username) || !/^[a-f0-9]{64}$/.test(entry.grant) ||
        !Number.isSafeInteger(entry.expires) || entry.expires <= 0) throw Error("Invalid enrollment file");
  return entries;
}
export async function main(args) {
  const {values} = parseArgs({args, options: {
    users: {type: "string"}, username: {type: "string"}, origin: {type: "string"},
    enrollments: {type: "string"}, link: {type: "string"}, upload: {type: "boolean"}, account: {type: "string"},
  }});
  if (!values.enrollments) throw Error("Use --enrollments PRIVATE_JSON and --upload, or --users PRIVATE_JSON --username USER --origin HTTPS_ORIGIN --link PRIVATE_FILE");
  const path = resolve(values.enrollments);
  const outputPaths = [path, ...(values.users ? [resolve(values.users)] : []), ...(values.link ? [resolve(values.link)] : [])];
  if (new Set(outputPaths).size !== outputPaths.length) throw Error("Use separate account, enrollment and link files");
  let entries = {};
  try {entries = validateEnrollments(JSON.parse((await privateFile(path, 128 * 1024)).toString("utf8")));}
  catch (error) {if (error.code !== "ENOENT") throw error;}
  if (values.username) {
    if (!values.users || !values.origin || !values.link) throw Error("Use --users, --origin and --link to create an enrollment");
    const origin = new URL(values.origin);
    if (origin.origin !== values.origin || origin.protocol !== "https:")
      throw Error("Use an HTTPS origin without a path, query or fragment");
    const users = validateUsers(JSON.parse((await privateFile(resolve(values.users), 128 * 1024)).toString("utf8")));
    const account = users[values.username];
    if (!account) throw Error("Account does not exist in the private account file");
    const now = Math.floor(Date.now() / 1000), token = randomBytes(32).toString("hex");
    entries = Object.fromEntries(Object.entries(entries).filter(([, entry]) => entry.expires > now && entry.username !== values.username));
    entries[hash(token)] = {username: values.username, grant: hash(JSON.stringify([values.username, account])), expires: now + 1800};
    validateEnrollments(entries);
    await savePrivateFile(path, JSON.stringify(entries, null, 2) + "\n");
    await savePrivateFile(resolve(values.link), `${origin.origin}/#enroll=${token}&user=${encodeURIComponent(values.username)}\n`);
    console.log("One-use enrollment link saved privately; it expires in 30 minutes. Deliver it only to the account owner.");
  } else if (!values.upload) throw Error("Use --username to create an enrollment or --upload to publish existing enrollment hashes");
  if (values.upload) {await uploadSecret(entries, values.account, "WEB_ENROLLMENTS"); console.log("WEB_ENROLLMENTS uploaded; enrollment tokens were not printed.");}
}
if (process.argv[1] && resolve(process.argv[1]) === fileURLToPath(import.meta.url)) {
  main(process.argv.slice(2)).catch(error => {
    const known = error instanceof Error && /^(Use |Invalid enrollment file|Account does not exist|Could not inspect|Worker account upload)/.test(error.message);
    console.error(known ? error.message : "Passkey enrollment setup failed; check private files and Wrangler access.");
    process.exitCode = 1;
  });
}
