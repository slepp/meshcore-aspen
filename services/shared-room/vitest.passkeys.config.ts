import {createHash, pbkdf2Sync} from "node:crypto";
import {defineConfig} from "vitest/config";
import {cloudflareTest} from "@cloudflare/vitest-pool-workers";
import fixture from "./test/native-fixtures.json" with {type: "json"};
const users = {
  alice: {name: "Alice", salt: "ab".repeat(16), iterations: 100000, aliases: ["A", "SharedB", "Independent"],
    hash: pbkdf2Sync("fixture password only", Buffer.from("ab".repeat(16), "hex"), 100000, 32, "sha256").toString("hex")},
  bob: {name: "Bob", salt: "cd".repeat(16), iterations: 100000, aliases: ["SharedB"],
    hash: pbkdf2Sync("another fixture password", Buffer.from("cd".repeat(16), "hex"), 100000, 32, "sha256").toString("hex")},
};
const hash = (text: string) => createHash("sha256").update(text).digest("hex");
export default defineConfig({
  plugins: [cloudflareTest({
    wrangler: {configPath: "./wrangler.jsonc"},
    miniflare: {compatibilityDate: "2026-08-15", bindings: {
      MODE: "opaque", WEB_AUTH_MODE: "passkey",
      PASSKEY_ORIGIN: "https://room.test", PASSKEY_RP_ID: "room.test",
      ALIASES: JSON.stringify({
        A: {backend: "native", publicKey: fixture.room.publicKey, name: "A", password: "room"},
        SharedB: {backend: "native", publicKey: fixture.otherRoom.publicKey, name: "SharedB", password: "room"},
        Independent: {backend: "independent", publicKey: "12".repeat(32), name: "Independent", password: "room"},
      }),
      ROOM_KEYS: JSON.stringify({A: fixture.room.key, SharedB: fixture.otherRoom.key}),
      FRONTENDS: JSON.stringify({one: {token: "one", aliases: ["A", "SharedB"]}}),
      WEB_USERS: JSON.stringify(users),
      WEB_ENROLLMENTS: JSON.stringify(Object.fromEntries(Object.entries(users).map(([username, account]) =>
        [hash(username === "alice" ? "aa".repeat(32) : "bb".repeat(32)),
          {username, grant: hash(JSON.stringify([username, account])), expires: 2000000000}]))),
    }},
  })],
  test: {include: ["test/passkeys.test.ts"], testTimeout: 15000},
});
