import {pbkdf2Sync} from "node:crypto";
import {defineConfig} from "vitest/config";
import {cloudflareTest} from "@cloudflare/vitest-pool-workers";
import fixture from "./test/native-fixtures.json" with {type: "json"};
const users = {
  alice: {name: "Alice", salt: "ab".repeat(16), iterations: 100000, aliases: ["A", "SharedB"],
    hash: pbkdf2Sync("fixture password only", Buffer.from("ab".repeat(16), "hex"), 100000, 32, "sha256").toString("hex")},
  bob: {name: "Bob", salt: "cd".repeat(16), iterations: 100000, aliases: ["SharedB"],
    hash: pbkdf2Sync("another fixture password", Buffer.from("cd".repeat(16), "hex"), 100000, 32, "sha256").toString("hex")},
};
export default defineConfig({
  plugins: [cloudflareTest({
    wrangler: {configPath: "./wrangler.jsonc"},
    miniflare: {compatibilityDate: "2026-08-15", bindings: {
      MODE: "opaque",
      ALIASES: JSON.stringify({
        A: {backend: "native", publicKey: fixture.room.publicKey, name: "A", password: "room"},
        SharedB: {backend: "native", publicKey: fixture.otherRoom.publicKey, name: "SharedB", password: "room"},
      }),
      ROOM_KEYS: JSON.stringify({A: fixture.room.key, SharedB: fixture.otherRoom.key}),
      FRONTENDS: JSON.stringify({one: {token: "one", aliases: ["A", "SharedB"]}}),
      WEB_USERS: JSON.stringify(users),
    }},
  })],
  test: {include: ["test/accounts.test.ts"], testTimeout: 10000},
});
