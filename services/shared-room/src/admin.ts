// Operator API: POST /v1/admin/*. Present only when ADMIN_TOKEN_HASH is set.
// Used by tools/admin.mjs to mint create codes, list and take down hosted
// rooms, ban nodes and read the audit ring.
import {equalToken, type Env} from "./config";
import {memoryLimit} from "./limits";
import {codeHash, formatCode, nodeError, nodeFail, nodeJson, randomCode, sha256Hex} from "./node-protocol";
import {readObject, registryOf} from "./redeem";
import {roomOps} from "./room-ops";
import {clientKey, parseKek} from "./seal";

const offsetOf = (body: Record<string, unknown>): number => {
  const v = body.offset === undefined ? 0 : body.offset;
  if (typeof v !== "number" || !Number.isInteger(v) || v < 0 || v > 1_000_000) nodeFail("bad_request");
  return v;
};
const aliasOf = (value: unknown): string => {
  if (typeof value !== "string" || !/^r[a-z2-7]{29}$/.test(value)) nodeFail("bad_request");
  return value;
};

export async function adminRoute(request: Request, env: Env, name: string): Promise<Response> {
  try {
    if (!env.ADMIN_TOKEN_HASH) return nodeJson({ok: 0, e: "not_found", m: "Not found."}, 404);
    const kek = parseKek(env);
    const ip = request.headers.get("CF-Connecting-IP");
    if (!ip || ip.length > 64) nodeFail("bad_request");
    const client = await clientKey(kek, ip, Math.floor(Date.now() / 1000));
    const wait = memoryLimit("adm:" + client, 10, 60);
    if (wait) nodeFail("rate", {retryAfter: wait});
    const body = await readObject(request, 1024);
    const registry = registryOf(env);
    const bearer = request.headers.get("Authorization")?.match(/^Bearer (.{1,256})$/)?.[1];
    if (!bearer || !equalToken(await sha256Hex(bearer), env.ADMIN_TOKEN_HASH.toLowerCase())) {
      const { limited } = await registry.adminFailure(client);
      nodeFail(limited ? "rate" : "auth", limited ? {retryAfter: 60} : {});
    }
    if (!kek) nodeFail("closed");
    const now = Math.floor(Date.now() / 1000);
    switch (name) {
      case "mint": {
        const count = body.count === undefined ? 1 : body.count, ttl = body.ttl === undefined ? 604800 : body.ttl;
        if (body.kind !== "create" || typeof count !== "number" || !Number.isInteger(count) || count < 1 || count > 20 ||
            typeof ttl !== "number" || !Number.isInteger(ttl) || ttl < 60 || ttl > 2592000) nodeFail("bad_request");
        const codes: string[] = [], hashes: string[] = [];
        for (let i = 0; i < count; i++) {
          const code = randomCode();
          codes.push(formatCode(code)); hashes.push(await codeHash(code));
        }
        await registry.mintCreate(hashes, now + ttl);
        return nodeJson({ok: 1, codes, expires: now + ttl});
      }
      case "rooms": {
        const page = await registry.adminRooms(offsetOf(body));
        return nodeJson({ok: 1, rooms: page.rooms, next: page.next});
      }
      case "room": {
        const alias = aliasOf(body.alias), action = body.action;
        if (action !== "disable" && action !== "enable" && action !== "delete") nodeFail("bad_request");
        if (!await registry.roomInfo(alias)) nodeFail("not_found");
        if (action === "delete") {
          try {await roomOps.purge(env, alias);} catch {/* the Registry alarm retries disabled rooms */}
          await registry.purgeRoom(alias, "admin", "delete");
        } else {
          const state = action === "disable" ? "disabled" : "active";
          let dirty = false;
          try {await roomOps.setState(env, alias, state);} catch {dirty = true;}
          await registry.setRoomState(alias, state, dirty);
        }
        await registry.adminNote(action, alias, "ok");
        return nodeJson({ok: 1});
      }
      case "codes": {
        const page = await registry.adminCodes(offsetOf(body));
        return nodeJson({ok: 1, codes: page.codes, next: page.next});
      }
      case "code": {
        const h = body.h;
        if (body.action !== "revoke" || typeof h !== "string" || !/^[a-f0-9]{8,64}$/.test(h)) nodeFail("bad_request");
        const result = await registry.adminRevokeCode(h);
        if (result !== "ok") nodeFail(result === "ambiguous" ? "bad_request" : "not_found");
        return nodeJson({ok: 1});
      }
      case "node": {
        const node = body.node, action = body.action;
        if (typeof node !== "string" || !/^[a-f0-9]{64}$/.test(node) || (action !== "ban" && action !== "unban")) nodeFail("bad_request");
        if (action === "unban") {await registry.unban(node); return nodeJson({ok: 1});}
        const until = body.until === undefined ? null : body.until;
        if (until !== null && (typeof until !== "number" || !Number.isInteger(until) || until <= now)) nodeFail("bad_request");
        for (const alias of await registry.ban(node, until)) {
          try {
            await roomOps.revokeNode(env, alias, node);
            await registry.memberRemove(node, alias);
          } catch {/* the Registry alarm retries banned members */}
        }
        return nodeJson({ok: 1});
      }
      case "invite": {
        const alias = aliasOf(body.alias);
        if (body.role !== "owner") nodeFail("bad_request");
        const code = randomCode();
        const result = await registry.mintJoin({alias, hash: await codeHash(code), role: 1, expires: now + 900, issuer: "admin"});
        if (result !== "ok") nodeFail(result === "cap" ? "cap" : "not_found");
        await registry.adminNote("invite-owner", alias, "ok");
        return nodeJson({ok: 1, code: formatCode(code), ttl: 900});
      }
      case "audit": {
        const page = await registry.adminAuditRows(offsetOf(body));
        return nodeJson({ok: 1, rows: page.rows, next: page.next});
      }
      default:
        return nodeFail("not_found");
    }
  } catch (error) {
    return nodeError(error, "admin");
  }
}
