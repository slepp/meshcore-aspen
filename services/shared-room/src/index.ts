import {aliases, authorize, errorResponse, fail, type Env} from "./config";
import {POST_BYTES, webPath, webResponse} from "./web";
import {DEVICE_PROTOCOL, webUsers} from "./accounts";
import {ACCOUNT_DEVICE_PROTOCOL, passkeyMode} from "./authority";
import {adminRoute} from "./admin";
import {HOSTED_ALIAS} from "./hosted";
import {nodeError, nodeFail} from "./node-protocol";
import {control, info, redeem} from "./redeem";
import {clientKey, parseKek, validAliasId} from "./seal";
export {Room} from "./room";
export {Accounts} from "./authority";
export {Registry} from "./registry";

function staticAlias(env: Env, alias: string): boolean {
  try {return Object.hasOwn(aliases(env), alias);} catch {return false;}
}

/**
 * Hosted aliases are self-authenticating, so unauthenticated traffic never
 * reaches a Durable Object. Static aliases and any other id keep today's path.
 */
async function hostedSocket(request: Request, env: Env, alias: string): Promise<Response | undefined> {
  if (!HOSTED_ALIAS.test(alias) || staticAlias(env, alias)) return;
  const kek = parseKek(env);
  if (!kek) fail(503, "Hosted rooms are not available on this service");
  if (!await validAliasId(kek, alias)) fail(404, "Unknown room alias");
  const headers = new Headers(request.headers);
  headers.delete("x-aspen-hosted-alias"); headers.delete("x-aspen-client");
  headers.set("x-aspen-hosted-alias", alias);
  const ip = request.headers.get("CF-Connecting-IP");
  headers.set("x-aspen-client", ip && ip.length <= 64 ? await clientKey(kek, ip, Math.floor(Date.now() / 1000)) : "none");
  return env.ROOMS.getByName("rt-" + alias).fetch(new Request(request, {headers}));
}

async function controlRoute(request: Request, env: Env, alias: string): Promise<Response> {
  try {
    if (staticAlias(env, alias)) nodeFail("not_found");
    const kek = parseKek(env);
    if (!kek) nodeFail("closed");
    if (!HOSTED_ALIAS.test(alias) || !await validAliasId(kek, alias)) nodeFail("not_found");
    return await control(request, env, alias);
  } catch (error) {return nodeError(error, "control");}
}

export default {
  async fetch(request: Request, env: Env): Promise<Response> {
    try {
      const path = new URL(request.url).pathname;
      if (path === "/v1/info") {
        if (request.method !== "GET") fail(405, "GET required");
        return info(env);
      }
      if (path === "/v1/node/redeem") {
        try {return await redeem(request, env);} catch (error) {return nodeError(error, "redeem");}
      }
      const controlMatch = path.match(/^\/v1\/aliases\/([a-zA-Z0-9_-]{1,64})\/control$/);
      if (controlMatch) return await controlRoute(request, env, controlMatch[1]);
      const adminMatch = path.match(/^\/v1\/admin\/([a-z]{1,12})$/);
      if (adminMatch) return await adminRoute(request, env, adminMatch[1]);
      if (path.startsWith("/v1/auth/")) {
        if (!passkeyMode(env)) fail(503, "Passkey account sign-in is not enabled");
        return env.ACCOUNTS.getByName("accounts").fetch(request);
      }
      if (path === "/v1/web/rooms" && request.method === "GET") {
        return webResponse({rooms: Object.entries(aliases(env)).map(([id, a]) =>
          ({id, name: a.name, publicKey: a.publicKey})), maxPostBytes: POST_BYTES,
          loginMode: passkeyMode(env) ? "passkey" : webUsers(env) === undefined ? "room-password" : "account",
          deviceProtocol: DEVICE_PROTOCOL, accountDeviceProtocol: ACCOUNT_DEVICE_PROTOCOL,
          ...(passkeyMode(env) ? {authOrigin: env.PASSKEY_ORIGIN} : {})});
      }
      const browser = webPath(path);
      const match = browser ?? path.match(/^\/v1\/aliases\/([a-zA-Z0-9_-]{1,64})\/(socket|operations)$/);
      if (!match) {
        if (!path.startsWith("/v1/") && env.ASSETS) return env.ASSETS.fetch(request);
        return webResponse({error: "Unknown endpoint"}, 404);
      }
      const alias = match[1];
      if (!browser) {
        const hosted = await hostedSocket(request, env, alias);
        if (hosted) return hosted;
        await authorize(request, env, alias);
      }
      const configured = aliases(env)[alias];
      if (!configured) fail(404, "Unknown room alias");
      return env.ROOMS.getByName(configured.backend).fetch(request);
    } catch (error) {return errorResponse(error);}
  }
} satisfies ExportedHandler<Env>;
