// Names/types only; never return or log binding values.
export const privateBindings = ["ALIASES", "FRONTENDS", "ROOM_KEYS"];

export function assertSecretSlots(bindings) {
  if (!Array.isArray(bindings)) throw Error("Cannot inspect live binding types.");
  const conflicts = bindings.filter(binding => privateBindings.includes(binding.name) && binding.type !== "secret_text");
  if (conflicts.length) throw Error("Remove conflicting plaintext placeholder bindings through a code deployment before uploading secrets: " +
    conflicts.map(binding => binding.name).join(", ") + ".");
}

export async function checkSecretSlots(run) {
  const deployments = await run(["deployments", "list", "--json"]);
  const current = deployments.sort((a, b) => Date.parse(a.created_on) - Date.parse(b.created_on)).at(-1);
  if (!current || current.versions.length !== 1 || current.versions[0].percentage !== 100)
    throw Error("Cannot inspect a single active Worker version before uploading secrets.");
  const snapshot = await run(["versions", "view", current.versions[0].version_id, "--json"]);
  assertSecretSlots(snapshot.resources?.bindings);
}
