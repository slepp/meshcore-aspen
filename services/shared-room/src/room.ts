import {DurableObject} from "cloudflare:workers";
import {aliases, ApiError, authorize, credential, errorResponse, fail, wellFormed, type Connection, type Env} from "./config";
import type {Delivery, Member, Message, Operation, Result} from "./protocol";
import {RadioCodec, opaque, OPAQUE_PROTOCOL} from "./native";
import {base64, unbase64} from "./native-crypto";

export interface Transmit {
  type: "transmit"; alias: string; dispatchId: string; packet: string;
  delayMs: number; priority: number;
}

interface Session {
  alias: string; client: string; cursor: number; lastTimestamp: number;
  frontend: string; route: string;
}
interface Pending {
  alias: string; client: string; deliveryId: string; seq: number;
  frontend: string; proof: string | null; state: string; requirePathAck: number;
}
const MESSAGE_COLUMNS = "seq, timestamp, origin_alias AS originAlias, author, client_timestamp AS clientTimestamp, text";
const SESSION_COLUMNS = "alias, client, cursor, last_timestamp AS lastTimestamp, frontend, route";
const PENDING_COLUMNS = "alias, client, delivery_id AS deliveryId, seq, frontend, proof, state, require_path_ack AS requirePathAck";
const MAX_FRAME = 4096;
const WIRE_PROTOCOL = "aspen-room.v1.json";
const textEncoder = new TextEncoder();

function string(value: unknown, name: string, max: number): string {
  if (typeof value !== "string" || textEncoder.encode(value).length > max) fail(400, `Invalid ${name}`);
  return value;
}
function timestamp(value: unknown, name = "timestamp"): number {
  if (!Number.isInteger(value) || (value as number) < 0 || (value as number) > 0xffffffff) fail(400, `Invalid ${name}`);
  return value as number;
}
function hex(value: unknown, name: string, bytes: number): string {
  if (typeof value !== "string" || !new RegExp(`^[a-f0-9]{${bytes * 2}}$`).test(value)) fail(400, `Invalid ${name}`);
  return value as string;
}
function route(value: unknown): string {
  const encoded = string(value, "return route", 340);
  // Canonical base64 preserves opaque bytes and has a predictable wire budget.
  try {
    const decoded = atob(encoded);
    if (decoded.length > 255 || btoa(decoded) !== encoded) fail(400, "Invalid base64 return route");
  } catch {fail(400, "Invalid base64 return route");}
  return encoded;
}

/** One canonical history. Advertised identities have separate sessions and ACKs. */
export class Room extends DurableObject<Env> {
  private sql: SqlStorage;
  private codec: RadioCodec;
  constructor(ctx: DurableObjectState, env: Env) {
    super(ctx, env);
    this.sql = ctx.storage.sql;
    this.codec = new RadioCodec(env);
    this.sql.exec(`
      CREATE TABLE IF NOT EXISTS identities (alias TEXT PRIMARY KEY, public_key TEXT NOT NULL);
      CREATE TABLE IF NOT EXISTS messages (
        seq INTEGER PRIMARY KEY AUTOINCREMENT, timestamp INTEGER NOT NULL UNIQUE,
        origin_alias TEXT NOT NULL, author TEXT NOT NULL, client_timestamp INTEGER NOT NULL, text TEXT NOT NULL,
        UNIQUE(origin_alias, author, client_timestamp, text)
      );
      CREATE TABLE IF NOT EXISTS sessions (
        alias TEXT NOT NULL, client TEXT NOT NULL, cursor INTEGER NOT NULL DEFAULT 0,
        last_timestamp INTEGER NOT NULL, frontend TEXT NOT NULL, route TEXT NOT NULL,
        PRIMARY KEY(alias, client)
      );
      CREATE TABLE IF NOT EXISTS pending (
        alias TEXT NOT NULL, client TEXT NOT NULL, delivery_id TEXT NOT NULL,
        seq INTEGER NOT NULL, frontend TEXT NOT NULL, proof TEXT, state TEXT NOT NULL,
        require_path_ack INTEGER NOT NULL DEFAULT 0,
        PRIMARY KEY(alias, client)
      );
      CREATE TABLE IF NOT EXISTS attempts (
        alias TEXT NOT NULL, client TEXT NOT NULL, kind TEXT NOT NULL, attempt TEXT NOT NULL,
        frontend TEXT NOT NULL, PRIMARY KEY(alias, client, kind, attempt)
      );
      CREATE TABLE IF NOT EXISTS dispatches (
        id TEXT PRIMARY KEY, alias TEXT NOT NULL, frontend TEXT NOT NULL,
        client TEXT, delivery_id TEXT, state TEXT NOT NULL
      );
    `);
    // Additive migration preserves existing messages, sessions and dispatches.
    if (!this.rows<{name: string}>("PRAGMA table_info(pending)").some(c => c.name === "require_path_ack"))
      this.sql.exec("ALTER TABLE pending ADD COLUMN require_path_ack INTEGER NOT NULL DEFAULT 0");
  }

  private rows<T>(query: string, ...params: SqlStorageValue[]): T[] {
    return this.sql.exec(query, ...params).toArray() as T[];
  }
  private session(alias: string, client: string): Session | undefined {
    return this.rows<Session>(`SELECT ${SESSION_COLUMNS} FROM sessions WHERE alias=? AND client=?`, alias, client)[0];
  }
  private pending(alias: string, client: string): Pending | undefined {
    return this.rows<Pending>(`SELECT ${PENDING_COLUMNS} FROM pending WHERE alias=? AND client=?`, alias, client)[0];
  }
  private needsPathAck(): boolean {
    // Separate frontend grants do not isolate RF: either modem can hear a
    // bare ACK from another room. Scope this choice to the whole service.
    return new Set(Object.values(aliases(this.env)).map(a => a.backend)).size > 1;
  }
  private send(ws: WebSocket, value: unknown): void {
    try {
      const frame = JSON.stringify(value);
      if (textEncoder.encode(frame).length > MAX_FRAME) throw new Error("Outgoing frame exceeds wire limit");
      ws.send(frame);
    } catch {ws.close(1011, "Frontend connection failed");}
  }
  private bindIdentity(alias: string): void {
    const configured = aliases(this.env)[alias];
    const existing = this.rows<{public_key: string}>("SELECT public_key FROM identities WHERE alias=?", alias)[0];
    if (existing && existing.public_key !== configured.publicKey) fail(409, "Use a new alias ID when replacing its advertised public key");
    if (!existing) this.sql.exec("INSERT INTO identities VALUES (?, ?)", alias, configured.publicKey);
  }

  async fetch(request: Request): Promise<Response> {
    try {
      const match = new URL(request.url).pathname.match(/^\/v1\/aliases\/([a-zA-Z0-9_-]{1,64})\/(socket|operations)$/);
      if (!match) fail(404, "Unknown endpoint");
      const connection = await authorize(request, this.env, match[1]);
      const alias = aliases(this.env)[connection.alias];
      if (!alias || !this.ctx.id.equals(this.env.ROOMS.idFromName(alias.backend))) fail(403, "Alias does not belong to this backend");
      this.bindIdentity(connection.alias);
      const isOpaque = opaque(this.env);
      if (isOpaque) this.codec.identity(connection.alias);
      if (match[2] === "socket") {
        if (request.method !== "GET" || request.headers.get("Upgrade")?.toLowerCase() !== "websocket") fail(426, "WebSocket upgrade required");
        const offered = request.headers.get("Sec-WebSocket-Protocol")?.split(",").map(p => p.trim());
        const protocol = isOpaque ? OPAQUE_PROTOCOL : WIRE_PROTOCOL;
        if (offered && !offered.includes(protocol)) fail(426, `Use ${protocol}`);
        // A frontend has one connection per alias. Reconnect replaces the old socket.
        for (const old of this.ctx.getWebSockets()) {
          const prior = old.deserializeAttachment() as Connection;
          if (prior.alias === connection.alias && prior.frontend === connection.frontend) old.close(1000, "Frontend reconnected");
        }
        const [client, server] = Object.values(new WebSocketPair());
        this.ctx.acceptWebSocket(server);
        server.serializeAttachment(connection);
        this.send(server, {type: "ready", version: isOpaque ? 2 : 1, alias: connection.alias, publicKey: alias.publicKey, name: alias.name});
        await this.pump();
        return new Response(null, {status: 101, webSocket: client,
          headers: offered ? {"Sec-WebSocket-Protocol": protocol} : undefined});
      }
      if (request.method !== "POST") fail(405, "POST required");
      const body = await request.text();
      if (textEncoder.encode(body).length > MAX_FRAME) fail(413, "Operation exceeds 4096 bytes");
      const result = await this.handle(connection, this.parse(body));
      await this.ctx.storage.sync();
      await this.pump();
      return Response.json(result);
    } catch (error) {return errorResponse(error);}
  }

  private parse(body: string): Operation {
    try {
      const value = JSON.parse(body);
      if (!value || typeof value !== "object" || Array.isArray(value)) fail(400, "Operation object required");
      return value as Operation;
    } catch (error) {
      if (error instanceof ApiError) throw error;
      return fail(400, "Invalid JSON");
    }
  }

  async webSocketMessage(ws: WebSocket, frame: string | ArrayBuffer): Promise<void> {
    let id: string | undefined;
    try {
      if (typeof frame !== "string" || textEncoder.encode(frame).length > MAX_FRAME) fail(400, "Use JSON text frames up to 4096 bytes");
      const envelope = this.parse(frame) as unknown as {id: string; operation: Operation};
      id = string(envelope.id, "request id", 32);
      if (!/^[a-zA-Z0-9_-]{1,32}$/.test(id)) fail(400, "Invalid request id");
      const connection = ws.deserializeAttachment() as Connection;
      // Recheck configuration after hibernation, including revoked tokens/aliases.
      const frontend = (JSON.parse(this.env.FRONTENDS) as Record<string, {aliases: string[]; token: string}>)[connection.frontend];
      const alias = aliases(this.env)[connection.alias];
      if (!frontend?.aliases.includes(connection.alias) || !frontend.token || await credential(frontend.token) !== connection.credential || !alias ||
          !this.ctx.id.equals(this.env.ROOMS.idFromName(alias.backend))) fail(403, "Frontend alias authorization changed");
      this.bindIdentity(connection.alias);
      const result = await this.handle(connection, envelope.operation);
      // Flush durable state before exposing success or permitting RF dispatch.
      await this.ctx.storage.sync();
      if ("transmission" in result && result.transmission) this.send(ws, result.transmission);
      this.send(ws, {type: "result", id, result: "transmission" in result ? {accepted: result.accepted} : result});
      await this.pump();
    } catch (error) {
      this.send(ws, {type: "error", id, error: error instanceof ApiError ? error.message : "Room operation failed"});
      if (!(error instanceof ApiError)) console.error("Room WebSocket operation failed", error);
    }
  }
  webSocketClose(ws: WebSocket, code: number, reason: string): void {
    ws.close(code === 1005 || code === 1006 || code === 1015 ? 1000 : code, reason);
  }
  webSocketError(ws: WebSocket): void {ws.close(1011, "Frontend connection failed");}

  /** Internal DO RPC: prefix resolution stays behind the Worker binding. */
  nativeMembers(alias: string, prefix: string, frontend: string): Member[] {
    const config = aliases(this.env)[alias];
    const grants = JSON.parse(this.env.FRONTENDS) as Record<string, {aliases: string[]}>;
    if (!config || !this.ctx.id.equals(this.env.ROOMS.idFromName(config.backend)) ||
        !grants[frontend]?.aliases.includes(alias) || !/^[a-f0-9]{0,64}$/.test(prefix)) fail(403, "Invalid native lookup");
    const bound = this.rows<{public_key: string}>("SELECT public_key FROM identities WHERE alias=?", alias)[0];
    if (bound && bound.public_key !== config.publicKey) return [];
    // Empty prefix is only used for bare ACKs: examine selected pending
    // deliveries, without enumerating the whole membership over a socket.
    const query = prefix ? `SELECT ${SESSION_COLUMNS} FROM sessions WHERE alias=? AND client>=? AND client<?` :
      `SELECT ${SESSION_COLUMNS} FROM sessions WHERE alias=? AND frontend=? AND client IN (SELECT client FROM pending WHERE alias=? AND frontend=?)`;
    const params = prefix ? [alias, prefix, `${prefix}g`] : [alias, frontend, alias, frontend];
    return this.rows<Session>(query, ...params).map(s => {
      const member: Member = {client: s.client, cursor: s.cursor};
      if (s.frontend === frontend) member.route = s.route;
      const p = this.pending(alias, s.client);
      if (p?.frontend === frontend) member.pending = {deliveryId: p.deliveryId, proof: p.proof, state: p.state,
        requirePathAck: !!p.requirePathAck || this.needsPathAck()};
      return member;
    });
  }

  private dispatch(c: Connection, wire: Uint8Array, delayMs: number, client?: string, deliveryId?: string): Transmit {
    const dispatchId = crypto.randomUUID();
    this.sql.exec("INSERT INTO dispatches VALUES (?, ?, ?, ?, ?, 'prepared')", dispatchId, c.alias, c.frontend, client ?? null, deliveryId ?? null);
    return {type: "transmit", alias: c.alias, dispatchId, packet: base64(wire), delayMs, priority: 0};
  }

  private async handle(c: Connection, operation: Operation): Promise<Result | {accepted: boolean; transmission?: Transmit}> {
    if (!opaque(this.env)) return this.run(c, operation);
    const op = operation as unknown as {op?: string; packet?: string; dispatchId?: string; outcome?: string};
    if (!op || typeof op !== "object") fail(400, "Operation required");
    if (op.op === "txReceipt") {
      const id = string(op.dispatchId, "dispatch ID", 36);
      if (!["sent", "failed", "unknown"].includes(op.outcome ?? "")) fail(400, "Invalid RF outcome");
      const d = this.rows<{client: string | null; delivery_id: string | null; state: string}>(
        "SELECT client, delivery_id, state FROM dispatches WHERE id=? AND alias=? AND frontend=?", id, c.alias, c.frontend)[0];
      if (!d) fail(403, "Dispatch does not belong to this frontend");
      // First terminal receipt wins. It never proves client reception.
      if (d.state === "prepared") {
        this.sql.exec("UPDATE dispatches SET state=? WHERE id=?", op.outcome!, id);
        if (d.client && d.delivery_id) this.run(c, {op: "receipt", client: d.client, deliveryId: d.delivery_id, outcome: op.outcome as "sent" | "failed" | "unknown"});
      }
      return {accepted: true};
    }
    if (op.op === "advertise") return {accepted: true, transmission: this.dispatch(c, this.codec.advertisement(this.codec.identity(c.alias)), 0)};
    if (op.op !== "rf") fail(403, "Opaque mode accepts only rf, txReceipt and explicit advertise operations");
    let wire: Uint8Array;
    try {wire = unbase64(string(op.packet, "native packet", 340));} catch {return fail(400, "Invalid native packet base64");}
    const configured = aliases(this.env);
    const grant = (JSON.parse(this.env.FRONTENDS) as Record<string, {aliases: string[]}>)[c.frontend];
    const identities = grant.aliases.map(alias => this.codec.identity(alias));
    const decoded = await this.codec.decode(wire, identities, async (alias, prefix) => {
      const backend = configured[alias].backend;
      if (this.ctx.id.equals(this.env.ROOMS.idFromName(backend))) return this.nativeMembers(alias, prefix, c.frontend);
      return this.env.ROOMS.getByName(backend).nativeMembers(alias, prefix, c.frontend);
    });
    if (!decoded || decoded.identity.alias !== c.alias) return {accepted: false};
    const result = this.run(c, decoded.operation);
    const response = this.codec.response(decoded, result);
    return {accepted: true, transmission: response ? this.dispatch(c, response, decoded.operation.op === "login" ? 300 : 0) : undefined};
  }

  private claim(c: Connection, client: string, kind: string, attempt: unknown): boolean {
    const key = hex(attempt, "RF attempt", 32);
    if (this.rows("SELECT 1 FROM attempts WHERE alias=? AND client=? AND kind=? AND attempt=?", c.alias, client, kind, key).length) return false;
    this.sql.exec("INSERT INTO attempts VALUES (?, ?, ?, ?, ?)", c.alias, client, kind, key, c.frontend);
    return true;
  }

  private run(c: Connection, operation: Operation): Result {
    if (!operation || typeof operation !== "object") fail(400, "Operation required");
    if ("backend" in operation || "alias" in operation) fail(400, "Room mapping is configured by the server");
    return this.ctx.storage.transactionSync(() => {
      if (operation.op === "members") {
        const prefix = operation.prefix === undefined ? "" : string(operation.prefix, "key prefix", 64);
        if (!/^[a-f0-9]*$/.test(prefix)) fail(400, "Invalid key prefix");
        // Hex keys permit an indexed prefix range. LIKE rejects long native
        // full-key prefixes under workerd's bounded SQLite pattern limits.
        const members: Member[] = this.rows<Session>(`SELECT ${SESSION_COLUMNS} FROM sessions WHERE alias=? AND client>=? AND client<? ORDER BY client LIMIT 1001`, c.alias, prefix, `${prefix}g`).map(s => {
          const member: Member = {client: s.client, cursor: s.cursor};
          if (s.frontend === c.frontend) {
            member.route = s.route;
            const p = this.pending(c.alias, s.client);
            if (p) member.pending = {deliveryId: p.deliveryId, proof: p.proof, state: p.state};
          }
          return member;
        });
        // Reserve enough for the longest correlation ID and result envelope.
        if (members.length > 1000 || textEncoder.encode(JSON.stringify({members})).length > MAX_FRAME - 96)
          fail(413, "Use a longer member key prefix");
        return {members};
      }
      const client = hex(operation.client, "full client key", 32);
      let session = this.session(c.alias, client);
      if (operation.op === "login") {
        const password = string(operation.password, "room password", 256);
        if (password !== aliases(this.env)[c.alias].password && !(password === "" && session)) fail(403, "Incorrect room password");
        const stamp = timestamp(operation.timestamp);
        const since = timestamp(operation.since, "since");
        const returnRoute = route(operation.route);
        if (!this.claim(c, client, "login", operation.attempt)) return {respond: false, duplicate: true};
        // Native empty-password ACL login is an activation, not a new history
        // request. It accepts zero/repeated timestamps and preserves delivery.
        if (password === "" && session) {
          const pending = this.pending(c.alias, client);
          if (!pending || pending.frontend === c.frontend) {
            this.sql.exec("UPDATE sessions SET frontend=?, route=? WHERE alias=? AND client=?", c.frontend, returnRoute, c.alias, client);
          }
          return {respond: true, cursor: session.cursor};
        }
        if (stamp === 0 || (session && stamp <= session.lastTimestamp)) fail(409, "Login timestamp must increase");
        // A client can request replay, but cannot advance our ACK-confirmed cursor.
        const sinceSeq = this.rows<{seq: number}>("SELECT COALESCE(MAX(seq),0) AS seq FROM messages WHERE timestamp<=?", since)[0].seq;
        const cursor = session ? Math.min(session.cursor, sinceSeq) : 0;
        this.sql.exec(`INSERT INTO sessions VALUES (?, ?, ?, ?, ?, ?)
          ON CONFLICT(alias, client) DO UPDATE SET cursor=excluded.cursor, last_timestamp=excluded.last_timestamp,
          frontend=excluded.frontend, route=excluded.route`, c.alias, client, cursor, stamp, c.frontend, returnRoute);
        this.sql.exec("DELETE FROM pending WHERE alias=? AND client=?", c.alias, client);
        return {respond: true, cursor};
      }
      if (!session) fail(403, "Client must log in to this advertised identity");
      if (operation.op === "path") {
        const returnRoute = route(operation.route);
        if (session.frontend !== c.frontend) return {respond: false};
        // A replayed message can produce the same native PATH+ACK bytes.
        // Its current pending proof still confirms that delivery; only pure
        // route updates are suppressed by permanent RF-attempt dedup.
        if (!this.claim(c, client, "path", operation.attempt) && operation.proof === undefined) return {respond: false};
        this.sql.exec("UPDATE sessions SET route=? WHERE alias=? AND client=?", returnRoute, c.alias, client);
        if (operation.proof !== undefined) {
          const p = this.pending(c.alias, client);
          if (!p || p.frontend !== c.frontend || p.deliveryId !== operation.deliveryId ||
              p.proof !== hex(operation.proof, "ACK proof", 4)) fail(403, "PATH ACK does not match this delivery");
          this.sql.exec("UPDATE sessions SET cursor=MAX(cursor,?) WHERE alias=? AND client=?", p.seq, c.alias, client);
          this.sql.exec("DELETE FROM pending WHERE alias=? AND client=?", c.alias, client);
        }
        return {};
      }
      if (operation.op === "post") {
        const stamp = timestamp(operation.timestamp);
        const text = string(operation.text, "post text", 151);
        if (!text.length || text.includes("\0") || !wellFormed(text) || operation.source !== "client") fail(400, "Only original client UTF-8 text posts are accepted");
        let message = this.rows<Message>(`SELECT ${MESSAGE_COLUMNS} FROM messages WHERE origin_alias=? AND author=? AND client_timestamp=? AND text=?`, c.alias, client, stamp, text)[0];
        if (!message && stamp <= session.lastTimestamp) fail(409, "Client timestamp must increase");
        const duplicate = !!message;
        if (!message) {
          const last = this.rows<{timestamp: number}>("SELECT COALESCE(MAX(timestamp),0) AS timestamp FROM messages")[0].timestamp;
          const wireTime = Math.max(Math.floor(Date.now() / 1000), last + 1);
          if (wireTime > 0xffffffff) fail(503, "Room timestamp range exhausted");
          this.sql.exec("INSERT INTO messages(timestamp, origin_alias, author, client_timestamp, text) VALUES (?, ?, ?, ?, ?)", wireTime, c.alias, client, stamp, text);
          message = this.rows<Message>(`SELECT ${MESSAGE_COLUMNS} FROM messages ORDER BY seq DESC LIMIT 1`)[0];
          this.sql.exec("UPDATE sessions SET last_timestamp=? WHERE alias=? AND client=?", stamp, c.alias, client);
        }
        // Logical dedup is independent of the RF attempt; each new attempt may get an ACK.
        return {respond: this.claim(c, client, "post", operation.attempt), duplicate, message};
      }
      if (operation.op === "refresh") {
        const stamp = timestamp(operation.timestamp);
        const returnRoute = route(operation.route);
        if (!this.claim(c, client, "refresh", operation.attempt)) return {respond: false, duplicate: true};
        if (stamp <= session.lastTimestamp) fail(409, "Client timestamp must increase");
        let cursor = session.cursor;
        if (operation.since !== undefined) {
          const since = timestamp(operation.since, "since");
          cursor = Math.min(cursor, this.rows<{seq: number}>("SELECT COALESCE(MAX(seq),0) AS seq FROM messages WHERE timestamp<=?", since)[0].seq);
        }
        this.sql.exec("UPDATE sessions SET last_timestamp=?, frontend=?, route=?, cursor=? WHERE alias=? AND client=?", stamp, c.frontend, returnRoute, cursor, c.alias, client);
        this.sql.exec("DELETE FROM pending WHERE alias=? AND client=?", c.alias, client);
        const remaining = this.rows<{n: number}>("SELECT COUNT(*) AS n FROM messages WHERE seq>? AND NOT(origin_alias=? AND author=?)", cursor, c.alias, client)[0].n;
        return {respond: true, cursor, remaining: Math.min(255, remaining)};
      }
      if (!["prepare", "receipt", "ack"].includes(operation.op)) fail(400, "Unknown operation");
      const p = this.pending(c.alias, client);
      // A fast RF ACK can clear/replace pending state before the TX receipt arrives.
      if (operation.op === "receipt" && (!p || p.deliveryId !== operation.deliveryId)) return {duplicate: true};
      if (!p || p.deliveryId !== operation.deliveryId) fail(409, "Stale delivery");
      if (p.frontend !== c.frontend) fail(403, "Delivery belongs to another frontend");
      if (operation.op === "prepare") {
        const proof = hex(operation.proof, "ACK proof", 4);
        if (proof === "00000000") fail(400, "Zero ACK proof is invalid");
        if (p.state !== "queued") return {transmit: false};
        this.sql.exec("UPDATE pending SET proof=?, state='prepared' WHERE alias=? AND client=?", proof, c.alias, client);
        return {transmit: true};
      }
      if (operation.op === "receipt") {
        if (!p.proof) fail(409, "Prepare delivery before RF submission");
        if (!["sent", "failed", "unknown"].includes(operation.outcome)) fail(400, "Invalid RF outcome");
        this.sql.exec("UPDATE pending SET state=? WHERE alias=? AND client=?", operation.outcome, c.alias, client);
        return {};
      }
      if (!p.proof || hex(operation.proof, "ACK proof", 4) !== p.proof) fail(403, "ACK proof does not match this delivery");
      if (p.requirePathAck || (opaque(this.env) && this.needsPathAck())) fail(403, "This delivery requires an authenticated PATH ACK");
      this.sql.exec("UPDATE sessions SET cursor=MAX(cursor,?) WHERE alias=? AND client=?", p.seq, c.alias, client);
      this.sql.exec("DELETE FROM pending WHERE alias=? AND client=?", c.alias, client);
      return {cursor: p.seq};
    });
  }

  /** Only new deliveries are pushed. Prepared/uncertain deliveries await client activity. */
  private async pump(): Promise<void> {
    const sockets = new Map<string, WebSocket>();
    const configuredAliases = aliases(this.env);
    const configuredFrontends = JSON.parse(this.env.FRONTENDS) as Record<string, {aliases: string[]; token: string}>;
    for (const ws of this.ctx.getWebSockets()) {
      if (ws.readyState !== WebSocket.OPEN) continue;
      const c = ws.deserializeAttachment() as Connection;
      const alias = configuredAliases[c.alias], frontend = configuredFrontends[c.frontend];
      const boundKey = this.rows<{public_key: string}>("SELECT public_key FROM identities WHERE alias=?", c.alias)[0]?.public_key;
      if (!alias || !this.ctx.id.equals(this.env.ROOMS.idFromName(alias.backend)) ||
          boundKey !== alias.publicKey || !frontend?.aliases.includes(c.alias) || !frontend.token ||
          await credential(frontend.token) !== c.credential) {
        ws.close(1008, "Frontend alias authorization changed"); continue;
      }
      sockets.set(`${c.alias}:${c.frontend}`, ws);
    }
    const dispatches: {ws: WebSocket; delivery: Delivery | Transmit}[] = [];
    const occupied = new Set<string>();
    const requirePathAck = this.needsPathAck();
    this.ctx.storage.transactionSync(() => {
      // Bare ACK selection is local and atomic across all eligible frontends,
      // since each modem can hear another's ACK. Independent backends use PATH.
      for (const p of this.rows<Pending>(`SELECT ${PENDING_COLUMNS} FROM pending WHERE proof IS NOT NULL AND require_path_ack=0`)) {
        const alias = configuredAliases[p.alias];
        const bound = this.rows<{public_key: string}>("SELECT public_key FROM identities WHERE alias=?", p.alias)[0];
        if (alias && configuredFrontends[p.frontend]?.aliases.includes(p.alias) &&
            this.ctx.id.equals(this.env.ROOMS.idFromName(alias.backend)) && bound?.public_key === alias.publicKey)
          occupied.add(p.proof!);
      }
      const configuredLimit = Number(this.env.HISTORY_LIMIT ?? "0");
      if (!Number.isSafeInteger(configuredLimit) || configuredLimit < 0) fail(503, "Invalid HISTORY_LIMIT");
      const latest = this.rows<{seq: number}>("SELECT COALESCE(MAX(seq),0) AS seq FROM messages")[0].seq;
      const floor = configuredLimit ? Math.max(0, latest - configuredLimit) : 0;
      for (const s of this.rows<Session>(`SELECT ${SESSION_COLUMNS} FROM sessions`)) {
        const ws = sockets.get(`${s.alias}:${s.frontend}`);
        if (!ws || this.pending(s.alias, s.client)) continue;
        let cursor = Math.max(s.cursor, floor);
        let message: Message | undefined;
        while ((message = this.rows<Message>(`SELECT ${MESSAGE_COLUMNS} FROM messages WHERE seq>? ORDER BY seq LIMIT 1`, cursor)[0])) {
          if (message.author !== s.client || message.originAlias !== s.alias) break;
          cursor = message.seq; // The author's local client already has its own committed post.
        }
        if (cursor !== s.cursor) this.sql.exec("UPDATE sessions SET cursor=? WHERE alias=? AND client=?", cursor, s.alias, s.client);
        if (!message) continue;
        const deliveryId = crypto.randomUUID();
        const delivery: Delivery = {type: "delivery", alias: s.alias, client: s.client, deliveryId, route: s.route, message};
        if (opaque(this.env)) {
          // Native clients return a MAC-authenticated PATH+ACK for flooded
          // signed history even when they already know a direct route.
          if (requirePathAck) delivery.route = base64(new Uint8Array([1, 0, 0x80]));
          const proofs = requirePathAck ? new Set<string>() : occupied;
          const encoded = this.codec.delivery(this.codec.identity(s.alias), delivery, proofs);
          if (!encoded) continue;
          proofs.add(encoded.proof);
          this.sql.exec("INSERT INTO pending(alias, client, delivery_id, seq, frontend, proof, state, require_path_ack) VALUES (?, ?, ?, ?, ?, ?, 'prepared', ?)",
            s.alias, s.client, deliveryId, message.seq, s.frontend, encoded.proof, requirePathAck ? 1 : 0);
          dispatches.push({ws, delivery: this.dispatch(ws.deserializeAttachment() as Connection, encoded.wire, 0, s.client, deliveryId)});
        } else {
          this.sql.exec("INSERT INTO pending(alias, client, delivery_id, seq, frontend, proof, state) VALUES (?, ?, ?, ?, ?, NULL, 'queued')", s.alias, s.client, deliveryId, message.seq, s.frontend);
          dispatches.push({ws, delivery});
        }
      }
    });
    await this.ctx.storage.sync();
    for (const {ws, delivery} of dispatches) this.send(ws, delivery);
  }
}
