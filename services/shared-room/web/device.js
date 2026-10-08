const PROTOCOL = "aspen-room.device.v1";
export const hex = bytes => Array.from(bytes, byte => byte.toString(16).padStart(2, "0")).join("");
export async function createDevice() {
  const keys = await crypto.subtle.generateKey({name: "Ed25519"}, false, ["sign", "verify"]);
  const publicKey = hex(new Uint8Array(await crypto.subtle.exportKey("raw", keys.publicKey)));
  return {version: 1, publicKey, privateKey: keys.privateKey, verificationKey: keys.publicKey};
}
export async function checkDevice(device) {
  if (!device || device.version !== 1 || !/^[a-f0-9]{64}$/.test(device.publicKey) ||
      !(device.privateKey instanceof CryptoKey) || device.privateKey.type !== "private" ||
      device.privateKey.algorithm.name !== "Ed25519" || device.privateKey.extractable ||
      !(device.verificationKey instanceof CryptoKey))
    throw new Error("This browser's saved device key is damaged. Restore its browser profile before signing in.");
  const publicKey = hex(new Uint8Array(await crypto.subtle.exportKey("raw", device.verificationKey)));
  const probe = crypto.getRandomValues(new Uint8Array(32));
  const signature = await crypto.subtle.sign("Ed25519", device.privateKey, probe);
  if (publicKey !== device.publicKey || !await crypto.subtle.verify("Ed25519", device.verificationKey, signature, probe))
    throw new Error("This browser's saved device keys do not match. Restore its browser profile before signing in.");
  return device;
}
export async function loadDevice() {
  const db = await new Promise((resolve, reject) => {
    const request = indexedDB.open("aspen.rooms.device", 1);
    request.onupgradeneeded = () => request.result.createObjectStore("keys");
    request.onsuccess = () => resolve(request.result);
    request.onerror = () => reject(new Error("Could not open private device-key storage"));
    request.onblocked = () => reject(new Error("Close other Aspen Rooms tabs to update device-key storage"));
  });
  try {
    const read = () => new Promise((resolve, reject) => {
      const request = db.transaction("keys", "readonly").objectStore("keys").get("desktop");
      request.onsuccess = () => resolve(request.result);
      request.onerror = () => reject(new Error("Could not read this browser's device key"));
    });
    let device = await read();
    if (!device) {
      const generated = await createDevice();
      await new Promise((resolve, reject) => {
        const transaction = db.transaction("keys", "readwrite", {durability: "strict"}), store = transaction.objectStore("keys");
        const request = store.get("desktop");
        request.onsuccess = () => {if (!request.result) store.add(generated, "desktop");};
        transaction.oncomplete = resolve;
        transaction.onabort = () => reject(new Error("Could not save this browser's device key; signing in was stopped"));
        transaction.onerror = () => reject(new Error("Could not save this browser's device key; signing in was stopped"));
      });
      device = await read();
    }
    return await checkDevice(device);
  } finally {db.close();}
}
export async function signChallenge(device, challenge, origin, alias, username) {
  if (!challenge || challenge.protocol !== PROTOCOL || !/^[a-f0-9]{64}$/.test(challenge.nonce) ||
      !Number.isSafeInteger(challenge.expires) || challenge.expires <= Math.floor(Date.now() / 1000))
    throw new Error("Device login challenge is invalid or expired; sign in again");
  const message = [PROTOCOL, origin, alias, username, device.publicKey, challenge.nonce].join("\n");
  if (challenge.message !== message || new TextEncoder().encode(message).length > 512)
    throw new Error("Device login challenge belongs to a different service, room or account");
  return hex(new Uint8Array(await crypto.subtle.sign("Ed25519", device.privateKey, new TextEncoder().encode(message))));
}
