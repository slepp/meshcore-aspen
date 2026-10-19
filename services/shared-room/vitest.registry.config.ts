import {createHash} from "node:crypto";
import {defineConfig} from "vitest/config";
import {cloudflareTest} from "@cloudflare/vitest-pool-workers";
import fixture from "./test/native-fixtures.json" with {type: "json"};
// Hosted-room tests: a static room beside the Registry, a fixture KEK and
// admin token hash. Fixture values only; nothing here is a real secret.
export const FIXTURE_KEK = {current: "k1", keys: {k1: "42".repeat(32)}};
export const FIXTURE_ADMIN = "fixture-admin-token";
export default defineConfig({
  plugins: [cloudflareTest({
    wrangler: {configPath: "./wrangler.jsonc"},
    miniflare: {compatibilityDate: "2026-08-15", bindings: {
      MODE: "decoded",
      ALIASES: JSON.stringify({
        S: {backend: "static", publicKey: fixture.otherRoom.publicKey, name: "Static", password: "room"},
      }),
      ROOM_KEYS: JSON.stringify({S: fixture.otherRoom.key}),
      FRONTENDS: JSON.stringify({one: {token: "one", aliases: ["S"]}}),
      ROOM_KEK: JSON.stringify(FIXTURE_KEK),
      ADMIN_TOKEN_HASH: createHash("sha256").update(FIXTURE_ADMIN).digest("hex"),
      HOSTED_HISTORY_LIMIT: "20",
      ROOM_WRITES_PER_DAY: "60",
    }},
  })],
  test: {include: ["test/registry-*.test.ts"], testTimeout: 20000},
});
