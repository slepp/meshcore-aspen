export const bytes = value => new TextEncoder().encode(value).length;
export const postBytes = (name, text) => bytes(`${name}: ${text}`);
export const validName = name => typeof name === "string" && name === name.trim() && bytes(name) > 0 &&
  bytes(name) <= 24 && !/[\u0000-\u001f\u007f-\u009f:]/u.test(name) && name.isWellFormed();
export function initials(name) {
  return name.trim().split(/\s+/u).slice(0, 2).map(word => Array.from(word)[0] ?? "").join("").toUpperCase();
}
export function messageBody(message) {
  const prefix = message.webName ? `${message.webName}: ` : "";
  return prefix && message.text.startsWith(prefix) ? message.text.slice(prefix.length) : message.text;
}
export function mergeProfiles(existing, incoming) {
  for (const profile of incoming) {
    if (!profile || !/^[a-f0-9]{64}$/.test(profile.publicKey) || typeof profile.name !== "string" ||
        !profile.name.trim() || !profile.name.isWellFormed() || bytes(profile.name) > 31 ||
        /[\u0000-\u001f\u007f-\u009f]/u.test(profile.name) || !["radio", "desktop"].includes(profile.source) ||
        !Number.isInteger(profile.timestamp) || profile.timestamp < 0 || profile.timestamp > 0xffffffff ||
        !Number.isInteger(profile.advertType) || profile.advertType < 1 || profile.advertType > 4)
      throw new Error("The room returned an invalid participant profile");
    const previous = existing.get(profile.publicKey);
    if (!previous || profile.timestamp > previous.timestamp) existing.set(profile.publicKey, profile);
  }
}
export function authorName(message, profiles) {
  return message.webName || profiles.get(message.author)?.name || `${message.author.slice(0, 8)}…`;
}
export function mergeMessages(existing, incoming) {
  for (const message of incoming) {
    if (!Number.isSafeInteger(message.seq) || message.seq < 1 || typeof message.text !== "string" ||
        typeof message.author !== "string" || !/^[a-f0-9]{64}$/.test(message.author) ||
        !Number.isInteger(message.timestamp)) throw new Error("The room returned an invalid message");
    const previous = existing.get(message.seq);
    if (previous && (previous.text !== message.text || previous.author !== message.author ||
        previous.timestamp !== message.timestamp)) throw new Error("The room returned conflicting message history");
    existing.set(message.seq, message);
  }
  return [...existing.values()].sort((a, b) => a.seq - b.seq);
}
