import { aliases, fail, type Env } from "./config";
import type { Delivery, Member, Operation, Result } from "./protocol";
import {
  NativeCrypto,
  fromHex,
  toHex,
  join,
  base64,
  unbase64,
} from "./native-crypto";
export const OPAQUE_PROTOCOL = "aspen-room.v2.json";
export function opaque(env: Env): boolean {
  if (env.MODE === undefined || env.MODE === "opaque") return true;
  if (env.MODE === "decoded") return false;
  return fail(503, "Invalid MODE configuration");
}
const encoder = new TextEncoder(),
  decoder = new TextDecoder("utf-8", { fatal: true, ignoreBOM: true });
export const le32 = (n: number): Uint8Array => {
  const b = new Uint8Array(4);
  new DataView(b.buffer).setUint32(0, n, true);
  return b;
};
const number = (b: Uint8Array): number =>
  new DataView(b.buffer, b.byteOffset, b.length).getUint32(0, true);
const proof = (n: number): string => n.toString(16).padStart(8, "0");
export interface Packet {
  kind: number;
  flood: boolean;
  length: number;
  path: Uint8Array;
  payload: Uint8Array;
}
export function pathSize(length: number): number {
  const n = (length & 63) * ((length >> 6) + 1);
  if (length >> 6 === 3 || n > 64) throw new Error("Invalid native path");
  return n;
}
export function packet(wire: Uint8Array): Packet | undefined {
  if (wire.length < 3 || wire.length > 255 || wire[0] >> 6) return;
  const route = wire[0] & 3;
  // Scoped transport authentication needs region-key configuration; never
  // silently treat its transport codes as authenticated/unscoped radio.
  if (route !== 1 && route !== 2) return;
  let n: number;
  try {
    n = pathSize(wire[1]);
  } catch {
    return;
  }
  if (2 + n >= wire.length || wire.length - 2 - n > 184) return;
  return {
    kind: (wire[0] >> 2) & 15,
    flood: route === 1,
    length: wire[1],
    path: wire.slice(2, 2 + n),
    payload: wire.slice(2 + n),
  };
}
export interface Identity {
  alias: string;
  key: Uint8Array;
  publicKey: Uint8Array;
  name: string;
}
export interface Decoded {
  identity: Identity;
  packet: Packet;
  operation: Operation;
  plain?: Uint8Array;
  route: string;
}
export type Lookup = (alias: string, prefix: string) => Promise<Member[]>;
function cstring(b: Uint8Array): Uint8Array {
  const n = b.indexOf(0);
  return n < 0 ? b : b.slice(0, n);
}
export function nativeRoute(raw: string): {
  known: boolean;
  length: number;
  path: Uint8Array;
} {
  const b = unbase64(raw);
  if (
    b.length < 3 ||
    b[0] !== 1 ||
    b[1] & ~1 ||
    b.length !== 3 + pathSize(b[2])
  )
    throw new Error("Invalid stored native route");
  return { known: !!b[1], length: b[2], path: b.slice(3) };
}
export class RadioCodec {
  readonly crypto = new NativeCrypto();
  private keys?: Record<string, string>;
  constructor(private env: Env) {}
  identity(alias: string): Identity {
    if (!this.keys) {
      try {
        this.keys = JSON.parse(this.env.ROOM_KEYS ?? "{}");
      } catch {
        return fail(503, "Invalid ROOM_KEYS configuration");
      }
    }
    const config = aliases(this.env)[alias],
      raw = this.keys?.[alias];
    if (!config || typeof raw !== "string" || !/^[a-f0-9]{128}$/.test(raw))
      return fail(503, "Native room key is not configured");
    const key = fromHex(raw);
    let pub: Uint8Array;
    try {
      pub = this.crypto.publicKey(key);
    } catch {
      return fail(503, "Invalid expanded native room key");
    }
    if (toHex(pub) !== config.publicKey)
      return fail(503, "Native room key does not match advertised identity");
    return { alias, key, publicKey: pub, name: config.name };
  }
  ack(plain: Uint8Array, peer: Uint8Array): number {
    return number(this.crypto.sha(join(plain, peer)));
  }
  async decode(
    wire: Uint8Array,
    identities: Identity[],
    lookup: Lookup,
  ): Promise<Decoded | undefined> {
    const p = packet(wire);
    if (!p) return;
    let ackPayload = p.payload;
    if (p.kind === 10) {
      if ((ackPayload[0] & 15) !== 3) return;
      ackPayload = ackPayload.slice(1);
    }
    if (p.kind === 3 || p.kind === 10) {
      if (ackPayload.length < 4) return;
      const value = proof(number(ackPayload));
      const found: Decoded[] = [];
      for (const identity of identities)
        for (const member of await lookup(identity.alias, ""))
          if (member.pending?.proof === value)
            found.push({
              identity,
              packet: p,
              route: "",
              operation: {
                op: "ack",
                client: member.client,
                deliveryId: member.pending.deliveryId,
                proof: value,
              },
            });
      return found.length === 1 ? found[0] : undefined;
    }
    if (![0, 2, 7, 8].includes(p.kind) || p.payload.length < 4) return;
    const attempt = toHex(
      this.crypto.sha(join(new Uint8Array([p.kind]), p.payload)),
    );
    const found: Decoded[] = [];
    for (const identity of identities) {
      if (p.payload[0] !== identity.publicKey[0]) continue;
      let candidates: Member[];
      if (p.kind === 7) {
        if (p.payload.length < 51) continue;
        const full = toHex(p.payload.slice(1, 33));
        candidates = await lookup(identity.alias, full);
        if (!candidates.length) candidates = [{ client: full, cursor: 0 }];
      } else
        candidates = await lookup(
          identity.alias,
          p.payload[1].toString(16).padStart(2, "0"),
        );
      for (const member of candidates) {
        const peer = fromHex(member.client),
          secret = this.crypto.secret(identity.key, peer);
        if (!secret) continue;
        const plain = this.crypto.crypt(
          secret,
          p.payload.slice(p.kind === 7 ? 33 : 2),
          true,
        );
        if (!plain) continue;
        let route = member.route ?? base64(new Uint8Array([1, 0, 0x80]));
        let op: Operation | undefined;
        let exact: Uint8Array | undefined;
        try {
          if (p.kind === 7 && plain.length >= 9) {
            if (p.flood) route = base64(new Uint8Array([1, 0, 0x80]));
            op = {
              op: "login",
              client: member.client,
              timestamp: number(plain),
              since: number(plain.slice(4)),
              password: decoder.decode(cstring(plain.slice(8))),
              attempt,
              route,
            };
          } else if (p.kind === 2 && plain.length >= 6 && plain[4] >> 2 === 0) {
            const text = decoder.decode(cstring(plain.slice(5)));
            if (!text) continue;
            // Match native UTF-8 truncation while ACKing exact received bytes.
            let bounded = text;
            while (encoder.encode(bounded).length > 150)
              bounded = Array.from(bounded).slice(0, -1).join("");
            exact = plain.slice(0, 5 + encoder.encode(text).length);
            op = {
              op: "post",
              client: member.client,
              timestamp: number(plain),
              text: bounded,
              source: "client",
              attempt,
            };
          } else if (
            p.kind === 0 &&
            !p.flood &&
            plain.length >= 9 &&
            plain[4] === 2
          ) {
            exact = plain.slice(0, 9);
            op = {
              op: "refresh",
              client: member.client,
              timestamp: number(plain),
              since: number(plain.slice(5)),
              route,
              attempt,
            };
          } else if (p.kind === 8) {
            const n = pathSize(plain[0]);
            if (plain.length < n + 2) continue;
            route = base64(
              join(new Uint8Array([1, 1, plain[0]]), plain.slice(1, 1 + n)),
            );
            op = { op: "path", client: member.client, route, attempt };
            if (
              (plain[1 + n] & 15) === 3 &&
              plain.length >= n + 6 &&
              member.pending?.proof === proof(number(plain.slice(n + 2)))
            ) {
              op.deliveryId = member.pending.deliveryId;
              op.proof = member.pending.proof;
            }
          }
          nativeRoute(route);
        } catch {
          continue;
        }
        if (op)
          found.push({
            identity,
            packet: p,
            operation: op,
            plain: exact,
            route,
          });
      }
    }
    return found.length === 1 ? found[0] : undefined;
  }
  routed(
    id: Identity,
    kind: number,
    payload: Uint8Array,
    raw: string,
  ): Uint8Array {
    const route = raw
      ? nativeRoute(raw)
      : { known: false, length: 0x80, path: new Uint8Array() };
    if (payload.length > 184) throw new Error("Native payload too large");
    return join(
      new Uint8Array([(kind << 2) | (route.known ? 2 : 1), route.length]),
      route.path,
      payload,
    );
  }
  datagram(
    id: Identity,
    client: string,
    kind: number,
    plain: Uint8Array,
    route: string,
  ): Uint8Array {
    const peer = fromHex(client),
      secret = this.crypto.secret(id.key, peer);
    if (!secret) throw new Error("Invalid native peer");
    const sealed = this.crypto.crypt(secret, plain);
    if (!sealed) throw new Error("Invalid native datagram");
    return this.routed(
      id,
      kind,
      join(new Uint8Array([peer[0], id.publicKey[0]]), sealed),
      route,
    );
  }
  response(d: Decoded, result: Result): Uint8Array | undefined {
    if (!result.respond) return;
    const op = d.operation;
    if (op.op === "login") {
      let body: Uint8Array = new Uint8Array(13);
      body.set(le32(Math.floor(Date.now() / 1000)));
      body[7] = 2;
      body.set(crypto.getRandomValues(new Uint8Array(4)), 8);
      body[12] = 1;
      let kind = 1;
      if (d.packet.flood) {
        body = join(
          new Uint8Array([d.packet.length]),
          d.packet.path,
          new Uint8Array([1]),
          body,
        );
        kind = 8;
      }
      return this.datagram(d.identity, op.client, kind, body, d.route);
    }
    if ((op.op === "post" || op.op === "refresh") && d.plain) {
      let body = le32(this.ack(d.plain, fromHex(op.client)));
      if (op.op === "refresh")
        body = join(body, new Uint8Array([result.remaining ?? 0]));
      return this.routed(d.identity, 3, body, d.route);
    }
  }
  delivery(
    id: Identity,
    d: Delivery,
    occupied: ReadonlySet<string> = new Set(),
  ): { wire: Uint8Array; proof: string } | undefined {
    const body = join(
      le32(d.message.timestamp),
      new Uint8Array([8]),
      fromHex(d.message.author).slice(0, 4),
      encoder.encode(d.message.text),
    );
    const start = crypto.getRandomValues(new Uint8Array(1))[0] & 3;
    // Bare ACKs carry no room/client ID. Native has four attempt-bit variants:
    // avoid all currently owned proofs, and wait if that space is exhausted.
    for (let n = 0; n < 4; n++) {
      body[4] = 8 | ((start + n) & 3);
      const value = proof(this.ack(body, fromHex(d.client)));
      if (value !== "00000000" && !occupied.has(value))
        return {
          wire: this.datagram(id, d.client, 2, body, d.route),
          proof: value,
        };
    }
  }
  advertisement(id: Identity): Uint8Array {
    const app = join(new Uint8Array([0x83]), encoder.encode(id.name));
    const stamp = le32(Math.floor(Date.now() / 1000));
    const signable = join(id.publicKey, stamp, app);
    return this.routed(
      id,
      4,
      join(id.publicKey, stamp, this.crypto.sign(id.key, signable), app),
      "",
    );
  }
}
