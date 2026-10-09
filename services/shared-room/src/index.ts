import {aliases, authorize, errorResponse, fail, type Env} from "./config";
import {POST_BYTES, webPath, webResponse} from "./web";
import {DEVICE_PROTOCOL, webUsers} from "./accounts";
import {ACCOUNT_DEVICE_PROTOCOL, passkeyMode} from "./authority";
export {Room} from "./room";
export {Accounts} from "./authority";

export default {
  async fetch(request: Request, env: Env): Promise<Response> {
    try {
      const path = new URL(request.url).pathname;
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
      if (!browser) await authorize(request, env, alias);
      const configured = aliases(env)[alias];
      if (!configured) fail(404, "Unknown room alias");
      return env.ROOMS.getByName(configured.backend).fetch(request);
    } catch (error) {return errorResponse(error);}
  }
} satisfies ExportedHandler<Env>;
