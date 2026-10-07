import module from "./native-crypto.wasm";
// Synchronous bounded ABI: calls cannot interleave while key/scratch is live.
interface Exports {
  memory: WebAssembly.Memory;
  mc_arena(): number;
  mc_pub(): void;
  mc_seed(): void;
  mc_shared(): number;
  mc_sha(n: number): void;
  mc_transport(kind: number, n: number): number;
  mc_sign(n: number): void;
  mc_verify(n: number): number;
  mc_crypt(n: number, decrypt: number): number;
}
export const fromHex = (s: string): Uint8Array => {
  if (!/^(?:[a-f0-9]{2})+$/.test(s))
    throw new Error("Invalid native key encoding");
  return Uint8Array.from(s.match(/../g)!, (v) => parseInt(v, 16));
};
export const toHex = (b: Uint8Array): string =>
  Array.from(b, (v) => v.toString(16).padStart(2, "0")).join("");
export const base64 = (b: Uint8Array): string =>
  btoa(String.fromCharCode(...b));
export function unbase64(s: unknown, max = 255): Uint8Array {
  if (typeof s !== "string" || s.length > Math.ceil(max / 3) * 4)
    throw new Error("Invalid native packet encoding");
  const b = Uint8Array.from(atob(s), (v) => v.charCodeAt(0));
  if (b.length > max || base64(b) !== s)
    throw new Error("Invalid native packet encoding");
  return b;
}
export function join(...parts: Uint8Array[]): Uint8Array {
  const out = new Uint8Array(parts.reduce((n, p) => n + p.length, 0));
  let at = 0;
  for (const p of parts) {
    out.set(p, at);
    at += p.length;
  }
  return out;
}
export class NativeCrypto {
  private e = new WebAssembly.Instance(module, {})
    .exports as unknown as Exports;
  private call<T>(fn: (memory: Uint8Array) => T): T {
    const b = new Uint8Array(this.e.memory.buffer, this.e.mc_arena(), 2048);
    try {
      return fn(b);
    } finally {
      b.fill(0);
    }
  }
  publicKey(key: Uint8Array): Uint8Array {
    if (key.length !== 64 || key[0] & 7 || (key[31] & 0xc0) !== 0x40)
      throw new Error("Invalid expanded native key");
    return this.call((b) => {
      b.set(key);
      this.e.mc_pub();
      return b.slice(64, 96);
    });
  }
  // Deterministic public test fixtures only; production keys are provisioned.
  seed(seed: Uint8Array): { key: Uint8Array; publicKey: Uint8Array } {
    if (seed.length !== 32) throw new Error("Invalid seed length");
    return this.call((b) => {
      b.set(seed, 96);
      this.e.mc_seed();
      return { key: b.slice(0, 64), publicKey: b.slice(64, 96) };
    });
  }
  secret(key: Uint8Array, peer: Uint8Array): Uint8Array | undefined {
    if (key.length !== 64 || peer.length !== 32) return;
    return this.call((b) => {
      b.set(key);
      b.set(peer, 96);
      return this.e.mc_shared() ? b.slice(128, 160) : undefined;
    });
  }
  sha(data: Uint8Array): Uint8Array {
    if (data.length > 512) throw new Error("Native hash input too large");
    return this.call((b) => {
      b.set(data, 160);
      this.e.mc_sha(data.length);
      return b.slice(1024, 1056);
    });
  }
  crypt(
    secret: Uint8Array,
    data: Uint8Array,
    decrypt = false,
  ): Uint8Array | undefined {
    if (secret.length !== 32 || data.length > 256) return;
    return this.call((b) => {
      b.set(secret, 128);
      b.set(data, 160);
      const n = this.e.mc_crypt(data.length, decrypt ? 1 : 0);
      return n ? b.slice(1024, 1024 + n) : undefined;
    });
  }
  transportCode(key: Uint8Array, kind: number, payload: Uint8Array): number {
    if (key.length !== 16 || !Number.isInteger(kind) || kind < 0 || kind > 15 || payload.length > 184)
      throw new Error("Invalid native transport input");
    return this.call(b => {
      b.set(key, 128); b.set(payload, 160);
      return this.e.mc_transport(kind, payload.length);
    });
  }
  sign(key: Uint8Array, data: Uint8Array): Uint8Array {
    if (key.length !== 64 || data.length > 512)
      throw new Error("Invalid native signing input");
    return this.call((b) => {
      b.set(key);
      this.e.mc_pub();
      b.set(data, 160);
      this.e.mc_sign(data.length);
      return b.slice(1024, 1088);
    });
  }
  verify(
    publicKey: Uint8Array,
    signature: Uint8Array,
    data: Uint8Array,
  ): boolean {
    if (publicKey.length !== 32 || signature.length !== 64 || data.length > 512)
      return false;
    return this.call((b) => {
      b.set(publicKey, 64);
      b.set(signature, 1088);
      b.set(data, 160);
      return !!this.e.mc_verify(data.length);
    });
  }
}
