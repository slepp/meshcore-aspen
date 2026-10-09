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
export function radioDeliveryLabel(rf) {
  if (rf === undefined) return "Radio delivery status unavailable";
  const counts = ["recipients", "queued", "sent", "acknowledged", "uncertain", "failed", "paused", "retrying", "exhausted", "attempts"];
  if (!rf || counts.some(key => !Number.isSafeInteger(rf[key]) || rf[key] < 0) ||
      rf.queued + rf.sent + rf.acknowledged + rf.uncertain + rf.failed !== rf.recipients ||
      rf.paused > rf.recipients || rf.retrying > rf.recipients || rf.exhausted > rf.recipients ||
      rf.nextRetryAt !== null && (!Number.isSafeInteger(rf.nextRetryAt) || rf.nextRetryAt < 0))
    throw new Error("The room returned invalid RF delivery status");
  if (!rf.recipients) return "No recorded radio delivery or waiting radio session";
  const parts = [`Radio ACK ${rf.acknowledged}/${rf.recipients} sessions`];
  for (const [key, label] of [["queued", "waiting for transmission"], ["sent", "transmitted, awaiting ACK"],
    ["uncertain", "transmission uncertain"], ["failed", "transmission failed"],
    ["paused", "paused"], ["retrying", "retried"], ["exhausted", "retry budget exhausted"]])
    if (rf[key]) parts.push(`${rf[key]} ${label}`);
  if (rf.attempts > 1) parts.push(`up to ${rf.attempts} dispatch attempts`);
  if (rf.nextRetryAt !== null) parts.push(`retry ${new Date(rf.nextRetryAt).toLocaleTimeString()}`);
  return parts.join(" · ");
}
export function radioDeliveryIndicator(rf) {
  radioDeliveryLabel(rf);
  const attention = !!(rf && (rf.uncertain || rf.failed || rf.exhausted));
  return {attention, count: rf?.acknowledged ?? 0,
    label: rf === undefined ? "Radio delivery status unavailable" :
      `${rf.acknowledged} radio acknowledgement${rf.acknowledged === 1 ? "" : "s"}${attention ? "; radio delivery needs attention" : ""}`};
}
export function mergeMessages(existing, incoming) {
  for (const message of incoming) {
    if (!Number.isSafeInteger(message.seq) || message.seq < 1 || typeof message.text !== "string" ||
        typeof message.author !== "string" || !/^[a-f0-9]{64}$/.test(message.author) ||
        !Number.isInteger(message.timestamp)) throw new Error("The room returned an invalid message");
    if (message.rf !== undefined) radioDeliveryLabel(message.rf);
    const previous = existing.get(message.seq);
    if (previous && (previous.text !== message.text || previous.author !== message.author ||
        previous.timestamp !== message.timestamp)) throw new Error("The room returned conflicting message history");
    existing.set(message.seq, message);
  }
  return [...existing.values()].sort((a, b) => a.seq - b.seq);
}
