import {test} from "node:test";
import assert from "node:assert/strict";
import {mkdtemp, readFile, rm, stat, writeFile, chmod, symlink} from "node:fs/promises";
import {tmpdir} from "node:os";
import {join} from "node:path";
import {pbkdf2Sync, createHash} from "node:crypto";
import {main, passwordRecord, validateUsers} from "../tools/web-users.mjs";
import {main as passkeyMain, validateEnrollments} from "../tools/web-passkeys.mjs";

test("account tool hashes passwords, saves private files and supports account removal", async () => {
  const root = await mkdtemp(join(tmpdir(), "aspen-accounts-")), path = join(root, "users.json"), password = "fixture password only";
  try {
    const passwordFile = join(root, "password.txt");
    await writeFile(passwordFile, password, {mode: 0o600});
    await main(["--file", path, "--username", "alice", "--name", "Alice", "--aliases", "A,B", "--password-file", passwordFile]);
    const raw = await readFile(path, "utf8"), users = validateUsers(JSON.parse(raw));
    assert.equal(raw.includes(password), false);
    assert.equal((await stat(path)).mode & 0o777, 0o600);
    assert.equal(users.alice.hash, pbkdf2Sync(password, Buffer.from(users.alice.salt, "hex"), 100000, 32, "sha256").toString("hex"));
    await main(["--file", path, "--username", "alice", "--remove"]);
    assert.deepEqual(JSON.parse(await readFile(path, "utf8")), {});
    await chmod(passwordFile, 0o644);
    await assert.rejects(main(["--file", path, "--username", "alice", "--name", "Alice", "--aliases", "A", "--password-file", passwordFile]), /private file/);
    const link = join(root, "password-link");
    await symlink(passwordFile, link);
    await assert.rejects(main(["--file", path, "--username", "alice", "--name", "Alice", "--aliases", "A", "--password-file", link]));
    for (const input of ["short", "x\n".repeat(20), "x".repeat(257)])
      assert.throws(() => passwordRecord(input, "Alice", ["A"]), /16..256/);
    assert.throws(() => validateUsers({Alice: passwordRecord(password, "Alice", ["A"])}), /Invalid/);
  } finally {await rm(root, {recursive: true, force: true});}
});

test("passkey accounts and first-passkey enrollment links stay private and bounded", async () => {
  const root = await mkdtemp(join(tmpdir(), "aspen-passkeys-")), usersPath = join(root, "users.json"),
    enrollments = join(root, "enrollments.json"), link = join(root, "link.txt"), origin = "https://aspen.example";
  const hash = value => createHash("sha256").update(value).digest("hex");
  try {
    await main(["--file", usersPath, "--username", "alice", "--name", "Alice", "--aliases", "A", "--passkey"]);
    const users = validateUsers(JSON.parse(await readFile(usersPath, "utf8")));
    const args = ["--users", usersPath, "--username", "alice", "--origin", origin, "--enrollments", enrollments, "--link", link];
    await passkeyMain(args);
    const entries = validateEnrollments(JSON.parse(await readFile(enrollments, "utf8")));
    const url = new URL((await readFile(link, "utf8")).trim()), fragment = new URLSearchParams(url.hash.slice(1));
    const token = fragment.get("enroll");
    assert.equal(url.origin, origin); assert.equal(fragment.get("user"), "alice");
    assert.match(token, /^[a-f0-9]{64}$/);
    assert.deepEqual(Object.keys(entries), [hash(token)]);
    assert.equal(entries[hash(token)].grant, hash(JSON.stringify(["alice", users.alice])));
    assert(entries[hash(token)].expires - Math.floor(Date.now()/1000) <= 1800);
    assert(entries[hash(token)].expires - Math.floor(Date.now()/1000) >= 1799);
    assert.equal((await stat(link)).mode & 0o777, 0o600);
    assert.equal((await stat(enrollments)).mode & 0o777, 0o600);
    assert.equal((await readFile(enrollments, "utf8")).includes(token), false);
    await passkeyMain(args);
    const replacement = validateEnrollments(JSON.parse(await readFile(enrollments, "utf8")));
    assert.equal(Object.keys(replacement).length, 1); assert(!Object.hasOwn(replacement, hash(token)));
    await assert.rejects(passkeyMain([...args.slice(0, -2), "--link", usersPath]), /separate/);
    await chmod(enrollments, 0o644);
    await assert.rejects(passkeyMain(args), /private file/);
    assert.throws(() => validateEnrollments({["aa".repeat(32)]: {username: "alice", grant: "00", expires: 0}}), /Invalid/);
  } finally {await rm(root, {recursive: true, force: true});}
});
