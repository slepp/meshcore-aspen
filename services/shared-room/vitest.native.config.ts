import { defineConfig } from "vitest/config";
import { cloudflareTest } from "@cloudflare/vitest-pool-workers";
import fixture from "./test/native-fixtures.json" with { type: "json" };
export function nativeConfig(independent = false) {
  return defineConfig({
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
              ...(independent ? {B: {
                backend: "other",
                publicKey: fixture.thirdRoom.publicKey,
                name: "B",
                password: "private",
              }} : {}),
            }),
            ROOM_KEYS: JSON.stringify({
              A: fixture.room.key,
              SharedB: fixture.otherRoom.key,
              ...(independent ? {B: fixture.thirdRoom.key} : {}),
            }),
            FRONTENDS: JSON.stringify({
              one: { token: "one", aliases: ["A", "SharedB"] },
              two: { token: "two", aliases: ["A", "SharedB"] },
              ...(independent ? {
                multi: { token: "multi", aliases: ["A", "B"] },
                aonly: { token: "aonly", aliases: ["A"] },
                bonly: { token: "bonly", aliases: ["B"] },
              } : {}),
            }),
          },
        },
      }),
    ],
    test: { include: [independent ? "test/native-independent.test.ts" : "test/native-room.test.ts"], testTimeout: 10000 },
  });
}
export default nativeConfig();
