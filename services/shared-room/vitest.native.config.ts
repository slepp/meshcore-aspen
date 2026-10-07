import { defineConfig } from "vitest/config";
import { cloudflareTest } from "@cloudflare/vitest-pool-workers";
import fixture from "./test/native-fixtures.json" with { type: "json" };
export default defineConfig({
  plugins: [
    cloudflareTest({
      wrangler: { configPath: "./wrangler.jsonc" },
      miniflare: {
        compatibilityDate: "2026-08-15",
        bindings: {
          MODE: "opaque",
          ALIASES: JSON.stringify({
            A: {
              backend: "native",
              publicKey: fixture.room.publicKey,
              name: "A",
              password: "room",
            },
            SharedB: {
              backend: "native",
              publicKey: fixture.otherRoom.publicKey,
              name: "SharedB",
              password: "room",
            },
            B: {
              backend: "other",
              publicKey: fixture.thirdRoom.publicKey,
              name: "B",
              password: "private",
            },
          }),
          ROOM_KEYS: JSON.stringify({
            A: fixture.room.key,
            SharedB: fixture.otherRoom.key,
            B: fixture.thirdRoom.key,
          }),
          FRONTENDS: JSON.stringify({
            one: { token: "one", aliases: ["A", "SharedB", "B"] },
            two: { token: "two", aliases: ["A", "SharedB", "B"] },
          }),
        },
      },
    }),
  ],
  test: { include: ["test/native-room.test.ts"], testTimeout: 10000 },
});
