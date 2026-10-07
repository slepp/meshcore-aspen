import {test} from "node:test";
import assert from "node:assert/strict";
import {readFile} from "node:fs/promises";
import {assertSecretSlots, checkSecretSlots, privateBindings} from "../tools/bindings.mjs";

test("checks the active binding types through placeholder removal, secret upload and redeploy", async () => {
  const config = JSON.parse(await readFile(new URL("../wrangler.jsonc", import.meta.url), "utf8"));
  assert.equal(config.keep_vars, true);
  for (const name of privateBindings) assert.equal(Object.hasOwn(config.vars, name), false);
  const deployments = [
    {created_on: "2026-01-02", versions: [{version_id: "current", percentage: 100}]},
    {created_on: "2026-01-01", versions: [{version_id: "older", percentage: 100}]},
  ];
  const untouched = [{name: "ROOMS", type: "durable_object_namespace"}, {name: "MODE", type: "plain_text"},
    {name: "HISTORY_LIMIT", type: "plain_text"}, {name: "OTHER_SECRET", type: "secret_text"}];
  const placeholders = [{name: "ALIASES", type: "plain_text"}, {name: "FRONTENDS", type: "plain_text"}];
  let bindings = [...untouched, ...placeholders];
  const calls = [];
  const run = async args => {
    calls.push(args);
    if (args[0] === "deployments") return [...deployments];
    assert.deepEqual(args, ["versions", "view", "current", "--json"]);
    return {resources: {bindings}};
  };
  await assert.rejects(checkSecretSlots(run), /ALIASES, FRONTENDS/);
  assert.equal(calls.every(args => args[0] === "deployments" || args[0] === "versions"), true);
  // The one-time code migration removes only the known plaintext placeholders.
  bindings = [...untouched];
  await checkSecretSlots(run);
  // Upload and later code deployment leave the private names as secret_text.
  // Throwing getters ensure the preflight never reads secret/plaintext values.
  bindings.push(...privateBindings.map(name => ({
    name, type: "secret_text", get text() {throw Error("A binding value was read.");},
  })));
  await checkSecretSlots(run);
  assert.deepEqual(bindings.slice(0, untouched.length), untouched);
});

test("a failed or ambiguous live lookup cannot authorize a handoff", async () => {
  assert.throws(() => assertSecretSlots(undefined), /Cannot inspect/);
  await assert.rejects(checkSecretSlots(async () => [{created_on: "2026-01-01",
    versions: [{version_id: "one", percentage: 50}, {version_id: "two", percentage: 50}]}]), /single active/);
  await assert.rejects(checkSecretSlots(async () => {throw Error("offline");}), /offline/);
});
