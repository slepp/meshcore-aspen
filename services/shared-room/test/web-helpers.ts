import {SELF} from "cloudflare:test";
import {expect} from "vitest";
import {WEB_PROTOCOL} from "../src/web";
import type {Message} from "../src/protocol";

export const origin = "https://room.test";
export const device = (n: number) => n.toString(16).padStart(64, "0");
export async function webRequest(alias: string, path: string, cookie?: string, body?: unknown, extra: Record<string, string> = {}) {
  return SELF.fetch(`${origin}/v1/web/rooms/${alias}/${path}`, {
    ...(body === undefined ? {} : {method: "POST", body: JSON.stringify(body)}),
    headers: {Origin: origin, "Content-Type": "application/json", ...(cookie ? {Cookie: cookie} : {}), ...extra},
  });
}
export async function joinWeb(alias = "A", n = 1, name = "Alice", password = "room") {
  const response = await webRequest(alias, "login", undefined, {identity: device(n), name, password});
  expect(response.status, await response.clone().text()).toBe(200);
  const value = await response.json<{author: string; name: string}>();
  const cookie = response.headers.get("Set-Cookie")!.split(";")[0];
  return {...value, cookie};
}
export async function webPost(alias: string, cookie: string, text: string, id = crypto.randomUUID()) {
  const response = await webRequest(alias, "posts", cookie, {id, text});
  expect(response.status, await response.clone().text()).toBe(200);
  return response.json<{message: Message; duplicate: boolean}>();
}
export async function webSocket(alias: string, cookie: string, sockets: WebSocket[], since = 0) {
  const response = await webRequest(alias, `socket?since=${since}`, cookie, undefined,
    {Upgrade: "websocket", "Sec-WebSocket-Protocol": WEB_PROTOCOL});
  expect(response.status).toBe(101);
  const ws = response.webSocket!;
  sockets.push(ws);
  const inbox: Array<{type: string; message?: Message; cursor?: number}> = [];
  const waiters: Array<(value: typeof inbox[number]) => void> = [];
  ws.addEventListener("message", event => {
    const value = JSON.parse(event.data as string);
    if (waiters.length) waiters.shift()!(value);
    else inbox.push(value);
  });
  ws.accept();
  const next = async () => inbox.length ? inbox.shift()! : new Promise<typeof inbox[number]>((resolve, reject) => {
    const timer = setTimeout(() => reject(new Error("Missing web room event")), 2000);
    waiters.push(value => {clearTimeout(timer); resolve(value);});
  });
  expect(await next()).toMatchObject({type: "ready"});
  return {ws, inbox, next};
}
