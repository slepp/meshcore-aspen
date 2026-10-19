// Registry and operator calls into a hosted Room Durable Object. Kept as an
// object so tests can inject a failing call and watch the Registry alarm retry.
import type {Env} from "./config";

export type HostedState = "active" | "disabled";
const stub = (env: Env, alias: string) => env.ROOMS.getByName("rt-" + alias);
export const roomOps = {
  async setState(env: Env, alias: string, state: HostedState): Promise<void> {await stub(env, alias).setState(alias, state);},
  async purge(env: Env, alias: string): Promise<void> {await stub(env, alias).purge(alias);},
  async revokeNode(env: Env, alias: string, node: string): Promise<void> {await stub(env, alias).revokeNode(alias, node);},
};
