import {defineConfig} from "vitest/config";
import {cloudflareTest} from "@cloudflare/vitest-pool-workers";

const aliases = {
  A: {backend: "shared", publicKey: "11".repeat(32), name: "A", password: "room"},
  B: {backend: "shared", publicKey: "11" + "22".repeat(31), name: "B", password: "room-b"},
  YEG: {backend: "shared", publicKey: "33".repeat(32), name: "WelcomeYEG", password: "room"},
  YYC: {backend: "shared", publicKey: "44".repeat(32), name: "WelcomeYYC", password: "room"},
  C: {backend: "separate", publicKey: "55".repeat(32), name: "C", password: "private"},
};
export default defineConfig({
  plugins: [cloudflareTest({
    wrangler: {configPath: "./wrangler.jsonc"},
    miniflare: {
      compatibilityDate: "2026-08-15",
      bindings: {
        MODE: "decoded",
        ALIASES: JSON.stringify(aliases),
        FRONTENDS: JSON.stringify({local: {token: "local-token", aliases: ["A", "B", "C"]},
          remote: {token: "remote-token", aliases: ["A", "YEG", "YYC"]}}),
      },
    },
  })],
  test: {include: ["test/room.test.ts", "test/native-crypto.test.ts", "test/web.test.ts"], testTimeout: 10000},
});
