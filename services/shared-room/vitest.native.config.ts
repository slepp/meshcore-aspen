import { defineConfig } from "vitest/config";
import { cloudflareTest } from "@cloudflare/vitest-pool-workers";
import fixture from "./test/native-fixtures.json" with { type: "json" };
export function nativeConfig(independent = false, regional = false) {
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
              one: { token: "one", aliases: ["A", "SharedB"], ...(regional ? {region: "ab"} : {}) },
              two: { token: "two", aliases: ["A", "SharedB"], ...(regional ? {region: "#ab"} : {}) },
              ...(regional ? {
                edm: {token: "edm", aliases: ["A"], region: "edm"},
                upper: {token: "upper", aliases: ["A"], region: "AB"},
                plain: {token: "plain", aliases: ["A"]},
                private: {token: "private", aliases: ["A"], region: "$private"},
              } : {}),
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
    test: { include: regional ? ["test/native-region.test.ts"] : independent ? ["test/native-independent.test.ts"] :
      ["test/native-room.test.ts", "test/native-retry.test.ts", "test/profiles.test.ts"], testTimeout: 10000 },
  });
}
export default nativeConfig();
