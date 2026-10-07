import {aliases, authorize, errorResponse, fail, type Env} from "./config";
export {Room} from "./room";

export default {
  async fetch(request: Request, env: Env): Promise<Response> {
    try {
      const match = new URL(request.url).pathname.match(/^\/v1\/aliases\/([a-zA-Z0-9_-]{1,64})\/(socket|operations)$/);
      if (!match) return Response.json({error: "Unknown endpoint"}, {status: 404});
      const alias = match[1];
      await authorize(request, env, alias);
      const configured = aliases(env)[alias];
      if (!configured) fail(404, "Unknown room alias");
      return env.ROOMS.getByName(configured.backend).fetch(request);
    } catch (error) {return errorResponse(error);}
  }
} satisfies ExportedHandler<Env>;
