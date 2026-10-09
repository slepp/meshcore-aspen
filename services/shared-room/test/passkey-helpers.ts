import {SELF} from "cloudflare:test";
import {encodeCBOR} from "@levischuck/tiny-cbor";
import {fromHex, toHex, base64, join} from "../src/native-crypto";
import {fixture, crypto as nativeCrypto} from "./native-helpers";
import {origin} from "./web-helpers";

export interface AuthChallenge {id: string; message: string; expires: number; options: {challenge: string}}
export const b64 = (bytes: Uint8Array) => base64(bytes).replace(/\+/g, "-").replace(/\//g, "_").replace(/=+$/, "");
export async function auth(path: string, body?: unknown, cookie?: string, from = origin) {
  const response = await SELF.fetch(`${from}/v1/auth/${path}`, {method: body === undefined ? "GET" : "POST",
    headers: {Origin: from, ...(body === undefined ? {} : {"Content-Type": "application/json"}),
      ...(cookie ? {Cookie: cookie} : {})}, ...(body === undefined ? {} : {body: JSON.stringify(body)})});
  return new Response(await response.arrayBuffer(), {status: response.status, headers: response.headers});
}
export function proof(challenge: AuthChallenge, device = fixture.author) {
  return {id: challenge.id, signature: toHex(nativeCrypto.sign(fromHex(device.key), new TextEncoder().encode(challenge.message)))};
}
async function digest(input: Uint8Array) {return new Uint8Array(await crypto.subtle.digest("SHA-256", input));}
const text = (value: unknown) => new TextEncoder().encode(JSON.stringify(value));
export async function authenticator() {
  const pair = await crypto.subtle.generateKey({name: "ECDSA", namedCurve: "P-256"}, true, ["sign", "verify"]);
  const publicKey = new Uint8Array(await crypto.subtle.exportKey("raw", pair.publicKey));
  const id = crypto.getRandomValues(new Uint8Array(32)), encoded = b64(id);
  const cose = encodeCBOR(new Map<number, number | Uint8Array>([[1, 2], [3, -7], [-1, 1],
    [-2, publicKey.slice(1, 33)], [-3, publicKey.slice(33)]]));
  const rpHash = await digest(new TextEncoder().encode("room.test"));
  return {
    id: encoded,
    async register(challenge: AuthChallenge, uv = true, clientOrigin = origin) {
      const data = join(rpHash, new Uint8Array([uv ? 0x45 : 0x41]), new Uint8Array(4), new Uint8Array(16),
        new Uint8Array([0, id.length]), id, cose);
      return {id: encoded, rawId: encoded, type: "public-key", clientExtensionResults: {},
        response: {clientDataJSON: b64(text({type: "webauthn.create", challenge: challenge.options.challenge, origin: clientOrigin})),
          attestationObject: b64(encodeCBOR(new Map<string, string | Uint8Array | Map<string, string>>(
            [["fmt", "none"], ["authData", data], ["attStmt", new Map()]]))), transports: ["internal"]}};
    },
    async authenticate(challenge: AuthChallenge, counter = 1, uv = true, clientOrigin = origin) {
      const count = new Uint8Array(4);
      new DataView(count.buffer).setUint32(0, counter);
      const data = join(rpHash, new Uint8Array([uv ? 5 : 1]), count);
      const client = text({type: "webauthn.get", challenge: challenge.options.challenge, origin: clientOrigin});
      const raw = new Uint8Array(await crypto.subtle.sign({name: "ECDSA", hash: "SHA-256"}, pair.privateKey,
        join(data, await digest(client))));
      const integer = (bytes: Uint8Array) => {
        while (bytes.length > 1 && bytes[0] === 0) bytes = bytes.slice(1);
        if (bytes[0] & 0x80) bytes = join(new Uint8Array([0]), bytes);
        return join(new Uint8Array([2, bytes.length]), bytes);
      };
      const r = integer(raw.slice(0, 32)), s = integer(raw.slice(32));
      const signature = join(new Uint8Array([0x30, r.length + s.length]), r, s);
      return {id: encoded, rawId: encoded, type: "public-key", clientExtensionResults: {},
        response: {clientDataJSON: b64(client), authenticatorData: b64(data), signature: b64(signature)}};
    },
  };
}
