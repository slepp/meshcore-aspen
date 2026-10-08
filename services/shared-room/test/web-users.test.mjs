import {test} from "node:test";
import assert from "node:assert/strict";
import {mkdtemp, readFile, rm, stat, writeFile, chmod, symlink} from "node:fs/promises";
import {tmpdir} from "node:os";
import {join} from "node:path";
import {pbkdf2Sync} from "node:crypto";
import {main, passwordRecord, validateUsers} from "../tools/web-users.mjs";

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
