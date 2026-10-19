import {DurableObject} from "cloudflare:workers";
import {aliases, ApiError, type Alias, authorize, credential, equalToken, errorResponse, fail, frontendRegion, wellFormed, type Connection, type Env} from "./config";
import type {Delivery, Member, Message, Operation, Result} from "./protocol";
import {RadioCodec, opaque, OPAQUE_PROTOCOL, type ParticipantProfile} from "./native";
import {base64, fromHex, toHex, unbase64} from "./native-crypto";
import {HOSTED_ALIAS, HOSTED_DDL, OWNER_DELAY, type HostedFrontendRow, type HostedGrant, type HostedMaterial, type HostedRow, type GrantReply} from "./hosted";
import {memoryLimit, intSetting} from "./limits";
import {NodeError, failureParts, nodeFail, codeHash, formatCode, generatedPassword, randomCode, validPassword, type FailureParts} from "./node-protocol";
import {newToken, openRoom, parseKek, sealRoom, tokenHash} from "./seal";
import {accountCredential, checkPassword, deviceChallenge, DEVICE_PROTOCOL, username, webUsers} from "./accounts";
import {passkeyMode} from "./authority";
import {cookieToken, displayName, isWebConnection, PAGE_SIZE, POST_BYTES, sameOrigin, sequence, sessionCookie,
  webBody, webCredential, webPath, webResponse, webText, WEB_PROTOCOL, WEB_SESSION_SECONDS,
  type WebConnection, type WebSession} from "./web";

interface Grant {aliases: string[]; token?: string; credential?: string}
type ControlReply = FailureParts;
const sec = (): number => Math.floor(Date.now() / 1000);
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
  proofs: string; retryAt: number; retryCount: number; retryBinding: string | null; dispatchId: string | null;
}
export interface RadioDelivery {
  recipients: number; queued: number; sent: number; acknowledged: number;
  uncertain: number; failed: number; paused: number; retrying: number; exhausted: number;
  attempts: number; nextRetryAt: number | null;
}
const MESSAGE_COLUMNS = "seq, timestamp, origin_alias AS originAlias, author, client_timestamp AS clientTimestamp, text, web_name AS webName";
const SESSION_COLUMNS = "alias, client, cursor, last_timestamp AS lastTimestamp, frontend, route";
const PENDING_COLUMNS = "alias, client, delivery_id AS deliveryId, seq, frontend, proof, state, require_path_ack AS requirePathAck, proofs, retry_at AS retryAt, retry_count AS retryCount, retry_binding AS retryBinding, dispatch_id AS dispatchId";
const MAX_FRAME = 4096;
const WIRE_PROTOCOL = "aspen-room.v1.json";
const HISTORY_TURNAROUND_MS = 1500;
export const HISTORY_RETRY_DELAYS = [30000, 60000, 120000] as const;
const PROFILE_COLUMNS = "public_key AS publicKey, name, advert_type AS advertType, timestamp, source";
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

function pendingProofs(pending: Pending): string[] {
  const proofs: unknown = JSON.parse(pending.proofs);
  if (!Array.isArray(proofs) || proofs.length > 4 || proofs.some(proof => typeof proof !== "string" || !/^[a-f0-9]{8}$/.test(proof)) ||
      new Set(proofs).size !== proofs.length || pending.proof && proofs.length === 4 && !proofs.includes(pending.proof))
    throw new Error("Invalid stored history ACK proofs");
  return pending.proof && !proofs.includes(pending.proof) ? [...proofs, pending.proof] : proofs;
}

/** One canonical history. Advertised identities have separate sessions and ACKs. */
export class Room extends DurableObject<Env> {
  private sql: SqlStorage;
  private codec: RadioCodec;
  constructor(ctx: DurableObjectState, env: Env) {
    super(ctx, env);
    this.sql = ctx.storage.sql;
    this.codec = new RadioCodec(env);
    this.hostedName = ctx.id.name?.startsWith("rt-") ?? false;
    // A hosted object creates no table until `provision`; only then is its state loaded.
    if (this.hostedName) ctx.blockConcurrencyWhile(() => this.loadHosted());
    else this.initStatic();
  }

  private initStatic(): void {
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
        proofs TEXT NOT NULL DEFAULT '[]', retry_at INTEGER NOT NULL DEFAULT 0,
        retry_count INTEGER NOT NULL DEFAULT 0, retry_binding TEXT, dispatch_id TEXT,
        PRIMARY KEY(alias, client)
      );
      CREATE TABLE IF NOT EXISTS attempts (
        alias TEXT NOT NULL, client TEXT NOT NULL, kind TEXT NOT NULL, attempt TEXT NOT NULL,
        frontend TEXT NOT NULL, PRIMARY KEY(alias, client, kind, attempt)
      );
      CREATE TABLE IF NOT EXISTS dispatches (
        id TEXT PRIMARY KEY, alias TEXT NOT NULL, frontend TEXT NOT NULL,
        client TEXT, delivery_id TEXT, state TEXT NOT NULL, seq INTEGER
      );
      CREATE TABLE IF NOT EXISTS web_sessions (
        token TEXT PRIMARY KEY, alias TEXT NOT NULL, author TEXT NOT NULL,
        name TEXT NOT NULL, credential TEXT NOT NULL, expires INTEGER NOT NULL
      );
      CREATE INDEX IF NOT EXISTS web_session_expiry ON web_sessions(expires);
      CREATE TABLE IF NOT EXISTS web_posts (
        author TEXT NOT NULL, request_id TEXT NOT NULL, seq INTEGER NOT NULL,
        PRIMARY KEY(author, request_id)
      );
      CREATE TABLE IF NOT EXISTS web_login_limits (
        address TEXT PRIMARY KEY, start INTEGER NOT NULL, attempts INTEGER NOT NULL
      );
      CREATE TABLE IF NOT EXISTS profiles (
        public_key TEXT PRIMARY KEY, name TEXT NOT NULL, advert_type INTEGER NOT NULL,
        timestamp INTEGER NOT NULL, source TEXT NOT NULL
      );
      CREATE TABLE IF NOT EXISTS web_challenges (
        nonce TEXT PRIMARY KEY, alias TEXT NOT NULL, username TEXT NOT NULL,
        public_key TEXT NOT NULL, expires INTEGER NOT NULL
      );
      CREATE TABLE IF NOT EXISTS web_devices (
        public_key TEXT PRIMARY KEY, username TEXT NOT NULL
      );
    `);
    // Additive migration preserves existing messages, sessions and dispatches.
    if (!this.rows<{name: string}>("PRAGMA table_info(pending)").some(c => c.name === "require_path_ack"))
      this.sql.exec("ALTER TABLE pending ADD COLUMN require_path_ack INTEGER NOT NULL DEFAULT 0");
    const pendingColumns = new Set(this.rows<{name: string}>("PRAGMA table_info(pending)").map(c => c.name));
    for (const [name, definition] of [
      ["proofs", "TEXT NOT NULL DEFAULT '[]'"], ["retry_at", "INTEGER NOT NULL DEFAULT 0"],
      ["retry_count", "INTEGER NOT NULL DEFAULT 0"], ["retry_binding", "TEXT"], ["dispatch_id", "TEXT"],
    ]) {
      if (!pendingColumns.has(name)) this.sql.exec(`ALTER TABLE pending ADD COLUMN ${name} ${definition}`);
    }
    if (!this.rows<{name: string}>("PRAGMA table_info(messages)").some(c => c.name === "web_name"))
      this.sql.exec("ALTER TABLE messages ADD COLUMN web_name TEXT");
    if (!this.rows<{name: string}>("PRAGMA table_info(web_sessions)").some(c => c.name === "username"))
      this.sql.exec("ALTER TABLE web_sessions ADD COLUMN username TEXT");
    if (!this.rows<{name: string}>("PRAGMA table_info(dispatches)").some(c => c.name === "seq")) {
      this.sql.exec("ALTER TABLE dispatches ADD COLUMN seq INTEGER");
      this.sql.exec(`UPDATE dispatches SET seq=(SELECT seq FROM pending
        WHERE pending.alias=dispatches.alias AND pending.client=dispatches.client
          AND pending.delivery_id=dispatches.delivery_id) WHERE client IS NOT NULL`);
    }
    this.sql.exec("CREATE INDEX IF NOT EXISTS dispatches_message ON dispatches(seq)");
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
  /** Room configuration seam: the server-owned aliases, frontend grants, mode and history limit. */
  private aliasMap(): Record<string, Alias> {
    if (this.hostedName) return this.hostedActive() ? {[this.hostedRow!.alias]: this.hostedAlias!} : {};
    return aliases(this.env);
  }
  private grants(): Record<string, Grant> {
    if (this.hostedName) return this.hostedGrants();
    return JSON.parse(this.env.FRONTENDS ?? "{}") as Record<string, Grant>;
  }
  private async grantCredential(grant?: Grant): Promise<string | undefined> {
    if (!grant) return;
    return grant.credential ?? (grant.token ? credential(grant.token) : undefined);
  }
  private isOpaque(): boolean {return this.hostedName || opaque(this.env);}
  private region(frontend: string): string | undefined {return this.hostedName ? undefined : frontendRegion(this.env, frontend);}
  private historyLimitSetting(): string | undefined {return this.hostedName ? String(intSetting(this.env.HOSTED_HISTORY_LIMIT, 2000, 0)) : this.env.HISTORY_LIMIT;}
  private needsPathAck(): boolean {
    if (this.hostedName) return false; // one alias per hosted room
    // Separate frontend grants do not isolate RF: either modem can hear a
    // bare ACK from another room. Scope this choice to the whole service.
    return new Set(Object.values(this.aliasMap()).map(a => a.backend)).size > 1;
  }
  private send(ws: WebSocket, value: unknown): void {
    try {
      const frame = JSON.stringify(value);
      if (textEncoder.encode(frame).length > MAX_FRAME) throw new Error("Outgoing frame exceeds wire limit");
      ws.send(frame);
    } catch {ws.close(1011, "Frontend connection failed");}
  }
  private bindIdentity(alias: string): void {
    const configured = this.aliasMap()[alias];
    const existing = this.rows<{public_key: string}>("SELECT public_key FROM identities WHERE alias=?", alias)[0];
    if (existing && existing.public_key !== configured.publicKey) fail(409, "Use a new alias ID when replacing its advertised public key");
    if (!existing) this.sql.exec("INSERT INTO identities VALUES (?, ?)", alias, configured.publicKey);
  }

  async fetch(request: Request): Promise<Response> {
    try {
      const browser = webPath(new URL(request.url).pathname);
      if (this.hostedName) this.hostedGate(request, browser);
      if (browser) return await this.webFetch(request, browser[1], browser[2]);
      const match = new URL(request.url).pathname.match(/^\/v1\/aliases\/([a-zA-Z0-9_-]{1,64})\/(socket|operations)$/);
      if (!match) fail(404, "Unknown endpoint");
      const connection = this.hostedName ? await this.hostedAuthorize(request, match[1]) : await authorize(request, this.env, match[1]);
      if (this.hostedName && match[2] === "operations") {
        const admitted = await this.hostedAdmit(undefined, connection.credential, Number(request.headers.get("content-length")) || 0);
        if (admitted === "limit") fail(429, "Too many operations; slow down");
        if (admitted === "disabled") fail(403, "Room is disabled");
      }
      const alias = this.aliasMap()[connection.alias];
      if (!alias || !this.ctx.id.equals(this.env.ROOMS.idFromName(alias.backend))) fail(403, "Alias does not belong to this backend");
      this.bindIdentity(connection.alias);
      const isOpaque = this.isOpaque();
      const region = this.region(connection.frontend);
      if (isOpaque) this.codec.identity(connection.alias, region);
      if (match[2] === "socket") {
        if (request.method !== "GET" || request.headers.get("Upgrade")?.toLowerCase() !== "websocket") fail(426, "WebSocket upgrade required");
        const offered = request.headers.get("Sec-WebSocket-Protocol")?.split(",").map(p => p.trim());
        const protocol = isOpaque ? OPAQUE_PROTOCOL : WIRE_PROTOCOL;
        if (offered && !offered.includes(protocol)) fail(426, `Use ${protocol}`);
        // A frontend has one connection per alias. Reconnect replaces the old socket.
        for (const old of this.ctx.getWebSockets()) {
          const prior = old.deserializeAttachment() as Connection | WebConnection;
          if (isWebConnection(prior)) continue;
          if (prior.alias === connection.alias && prior.frontend === connection.frontend) old.close(1000, "Frontend reconnected");
        }
        const [client, server] = Object.values(new WebSocketPair());
        this.ctx.acceptWebSocket(server);
        server.serializeAttachment(connection);
        this.send(server, {type: "ready", version: isOpaque ? 2 : 1, alias: connection.alias, publicKey: alias.publicKey, name: alias.name,
          ...(region ? {region} : {})});
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
    const attachment = ws.deserializeAttachment() as Connection | WebConnection;
    if (isWebConnection(attachment)) {
      try {
        await this.validWebSession(attachment);
        if (typeof frame !== "string" || textEncoder.encode(frame).length > MAX_FRAME) fail(400, "Invalid browser frame");
        const value = this.parse(frame) as unknown as {op?: string; since?: number};
        if (value.op !== "sync" || !Number.isSafeInteger(value.since) || value.since! < 0) fail(400, "Use a sync operation with a sequence cursor");
        attachment.cursor = value.since!;
        ws.serializeAttachment(attachment);
        await this.pumpWebSocket(ws, attachment);
      } catch (error) {
        this.send(ws, {type: "error", error: error instanceof ApiError ? error.message : "Browser room connection failed"});
        if (!(error instanceof ApiError)) console.error("Browser room connection failed", error);
        ws.close(error instanceof ApiError && error.status === 401 ? 1008 : 1011, "Browser room connection failed");
      }
      return;
    }
    let id: string | undefined;
    try {
      if (this.hostedName && await this.hostedAdmit(ws, (ws.deserializeAttachment() as Connection).credential,
          typeof frame === "string" ? textEncoder.encode(frame).length : frame.byteLength) !== "ok") return;
      if (typeof frame !== "string" || textEncoder.encode(frame).length > MAX_FRAME) fail(400, "Use JSON text frames up to 4096 bytes");
      const envelope = this.parse(frame) as unknown as {id: string; operation: Operation};
      id = string(envelope.id, "request id", 32);
      if (!/^[a-zA-Z0-9_-]{1,32}$/.test(id)) fail(400, "Invalid request id");
      const connection = ws.deserializeAttachment() as Connection;
      // Recheck configuration after hibernation, including revoked tokens/aliases.
      const frontend = this.grants()[connection.frontend];
      const alias = this.aliasMap()[connection.alias];
      if (!frontend?.aliases.includes(connection.alias) || await this.grantCredential(frontend) !== connection.credential || !alias ||
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
    const config = this.aliasMap()[alias];
    const grants = this.grants();
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
        proofs: pendingProofs(p), requirePathAck: !!p.requirePathAck || this.needsPathAck()};
      return member;
    });
  }

  private dispatch(c: Connection, wire: Uint8Array, delayMs: number, client?: string, deliveryId?: string, seq?: number): Transmit {
    const dispatchId = crypto.randomUUID();
    this.sql.exec("INSERT INTO dispatches(id, alias, frontend, client, delivery_id, state, seq) VALUES (?, ?, ?, ?, ?, 'prepared', ?)",
      dispatchId, c.alias, c.frontend, client ?? null, deliveryId ?? null, seq ?? null);
    return {type: "transmit", alias: c.alias, dispatchId, packet: base64(wire), delayMs, priority: 0};
  }

  private async handle(c: Connection, operation: Operation): Promise<Result | {accepted: boolean; transmission?: Transmit}> {
    if (!this.isOpaque()) return this.run(c, operation);
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
        if (d.client && d.delivery_id) {
          const pending = this.pending(c.alias, d.client);
          if (!pending?.dispatchId || pending.dispatchId === id)
            this.run(c, {op: "receipt", client: d.client, deliveryId: d.delivery_id, outcome: op.outcome as "sent" | "failed" | "unknown"});
        }
      }
      return {accepted: true};
    }
    if (op.op === "advertise") return {accepted: true, transmission: this.dispatch(c, this.codec.advertisement(this.codec.identity(c.alias, this.region(c.frontend))), 0)};
    if (op.op !== "rf") fail(403, "Opaque mode accepts only rf, txReceipt and explicit advertise operations");
    let wire: Uint8Array;
    try {wire = unbase64(string(op.packet, "native packet", 340));} catch {return fail(400, "Invalid native packet base64");}
    const configured = this.aliasMap();
    const grant = (this.grants())[c.frontend];
    const identities = grant.aliases.map(alias => this.codec.identity(alias, this.region(c.frontend)));
    const profile = this.codec.profile(wire, this.codec.identity(c.alias, this.region(c.frontend)));
    if (profile) {
      const changed = this.ctx.storage.transactionSync(() => {
        const previous = this.rows<ParticipantProfile>(`SELECT ${PROFILE_COLUMNS} FROM profiles WHERE public_key=?`, profile.publicKey)[0];
        if (previous && previous.timestamp >= profile.timestamp) return false;
        this.sql.exec(`INSERT INTO profiles VALUES (?, ?, ?, ?, ?)
          ON CONFLICT(public_key) DO UPDATE SET name=excluded.name, advert_type=excluded.advert_type,
          timestamp=excluded.timestamp, source=excluded.source`,
          profile.publicKey, profile.name, profile.advertType, profile.timestamp, profile.source);
        this.sql.exec(`DELETE FROM profiles WHERE public_key IN
          (SELECT public_key FROM profiles ORDER BY timestamp DESC, public_key LIMIT -1 OFFSET 4096)`);
        return true;
      });
      if (changed) {
        await this.ctx.storage.sync();
        await this.publishProfile(profile);
      }
      return {accepted: true};
    }
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
        if (password !== this.aliasMap()[c.alias].password && !(password === "" && session)) fail(403, "Incorrect room password");
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
              !pendingProofs(p).includes(hex(operation.proof, "ACK proof", 4))) fail(403, "PATH ACK does not match this delivery");
          this.sql.exec("UPDATE sessions SET cursor=MAX(cursor,?) WHERE alias=? AND client=?", p.seq, c.alias, client);
          this.confirmRadioDelivery(p);
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
          message = this.commitMessage(c.alias, client, text, stamp);
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
      if (!p.proof || !pendingProofs(p).includes(hex(operation.proof, "ACK proof", 4))) fail(403, "ACK proof does not match this delivery");
      if (p.requirePathAck || (this.isOpaque() && this.needsPathAck())) fail(403, "This delivery requires an authenticated PATH ACK");
      this.sql.exec("UPDATE sessions SET cursor=MAX(cursor,?) WHERE alias=? AND client=?", p.seq, c.alias, client);
      this.confirmRadioDelivery(p);
      this.sql.exec("DELETE FROM pending WHERE alias=? AND client=?", c.alias, client);
      return {cursor: p.seq};
    });
  }

  async alarm(): Promise<void> {
    if (this.hostedName) await this.hostedHousekeeping();
    await this.pump(true);
  }

  private historyBinding(connection: Connection): string {
    return JSON.stringify([connection.credential, this.region(connection.frontend) ?? null]);
  }

  private async scheduleHistoryRetry(sockets: Map<string, WebSocket>): Promise<void> {
    const current = await this.ctx.storage.getAlarm();
    if (!this.isOpaque()) {
      if (current !== null) await this.ctx.storage.deleteAlarm();
      return;
    }
    let next: number | undefined;
    for (const pending of this.rows<Pending>(`SELECT ${PENDING_COLUMNS} FROM pending WHERE retry_at>0`)) {
      const ws = sockets.get(`${pending.alias}:${pending.frontend}`);
      if (!ws || pending.retryBinding !== this.historyBinding(ws.deserializeAttachment() as Connection)) continue;
      if (next === undefined || pending.retryAt < next) next = pending.retryAt;
    }
    if (this.hostedName && this.hostedRow && this.hostedRow.state !== "deleted") {
      const daily = this.dailyAt() * 1000;
      if (next === undefined || daily < next) next = daily;
    }
    if (next === undefined) {
      if (current !== null) await this.ctx.storage.deleteAlarm();
    } else {
      const at = Math.max(next, Date.now() + 1000);
      if (current === null || current > at) await this.ctx.storage.setAlarm(at);
    }
  }

  private async pump(retryDue = false): Promise<void> {
    const sockets = new Map<string, WebSocket>();
    const configuredAliases = this.aliasMap();
    const configuredFrontends = this.grants();
    for (const ws of this.ctx.getWebSockets()) {
      if (ws.readyState !== WebSocket.OPEN) continue;
      const c = ws.deserializeAttachment() as Connection | WebConnection;
      if (isWebConnection(c)) continue;
      const alias = configuredAliases[c.alias], frontend = configuredFrontends[c.frontend];
      const boundKey = this.rows<{public_key: string}>("SELECT public_key FROM identities WHERE alias=?", c.alias)[0]?.public_key;
      if (!alias || !this.ctx.id.equals(this.env.ROOMS.idFromName(alias.backend)) ||
          boundKey !== alias.publicKey || !frontend?.aliases.includes(c.alias) ||
          await this.grantCredential(frontend) !== c.credential) {
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
          for (const proof of pendingProofs(p)) occupied.add(proof);
      }
      const floor = this.historyFloor();
      for (const s of this.rows<Session>(`SELECT ${SESSION_COLUMNS} FROM sessions`)) {
        const ws = sockets.get(`${s.alias}:${s.frontend}`);
        if (!ws) continue;
        const connection = ws.deserializeAttachment() as Connection;
        const pending = this.pending(s.alias, s.client);
        if (pending) {
          if (!retryDue || !this.isOpaque() || pending.frontend !== s.frontend || pending.retryAt <= 0 ||
              pending.retryAt > Date.now() || pending.retryBinding !== this.historyBinding(connection)) continue;
          if (pending.retryCount >= HISTORY_RETRY_DELAYS.length) {
            this.sql.exec("UPDATE pending SET retry_at=0 WHERE alias=? AND client=?", s.alias, s.client);
            continue;
          }
          const message = this.rows<Message>(`SELECT ${MESSAGE_COLUMNS} FROM messages WHERE seq=?`, pending.seq)[0];
          if (!message) throw new Error("Pending radio history message is missing");
          const pathAck = !!pending.requirePathAck || requirePathAck;
          const proofs = new Set(pathAck ? pendingProofs(pending) : occupied);
          const delivery: Delivery = {type: "delivery", alias: s.alias, client: s.client, deliveryId: pending.deliveryId,
            route: pathAck ? base64(new Uint8Array([1, 0, 0x80])) : s.route, message};
          const encoded = this.codec.delivery(this.codec.identity(s.alias, this.region(s.frontend)), delivery, proofs);
          if (!encoded) {
            this.sql.exec("UPDATE pending SET retry_at=0 WHERE alias=? AND client=?", s.alias, s.client);
            console.warn("Radio history retry paused: native ACK attempt space is occupied", s.alias, s.client.slice(0, 8), pending.seq);
            continue;
          }
          occupied.add(encoded.proof);
          const retryCount = pending.retryCount + 1;
          const retryAt = retryCount < HISTORY_RETRY_DELAYS.length ? Date.now() + HISTORY_RETRY_DELAYS[retryCount] : 0;
          const transmit = this.dispatch(connection, encoded.wire, HISTORY_TURNAROUND_MS, s.client, pending.deliveryId, pending.seq);
          this.sql.exec(`UPDATE pending SET proof=?, proofs=?, state='prepared', require_path_ack=?,
            retry_at=?, retry_count=?, dispatch_id=? WHERE alias=? AND client=?`,
            encoded.proof, JSON.stringify([...pendingProofs(pending), encoded.proof]), pathAck ? 1 : 0,
            retryAt, retryCount, transmit.dispatchId, s.alias, s.client);
          dispatches.push({ws, delivery: transmit});
          continue;
        }
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
        if (this.isOpaque()) {
          // Native clients return a MAC-authenticated PATH+ACK for flooded
          // signed history even when they already know a direct route.
          if (requirePathAck) delivery.route = base64(new Uint8Array([1, 0, 0x80]));
          const proofs = requirePathAck ? new Set<string>() : occupied;
          const encoded = this.codec.delivery(this.codec.identity(s.alias, this.region(s.frontend)), delivery, proofs);
          if (!encoded) continue;
          proofs.add(encoded.proof);
          const transmit = this.dispatch(connection, encoded.wire, HISTORY_TURNAROUND_MS, s.client, deliveryId, message.seq);
          this.sql.exec(`INSERT INTO pending(alias, client, delivery_id, seq, frontend, proof, state, require_path_ack,
            proofs, retry_at, retry_binding, dispatch_id) VALUES (?, ?, ?, ?, ?, ?, 'prepared', ?, ?, ?, ?, ?)`,
            s.alias, s.client, deliveryId, message.seq, s.frontend, encoded.proof, requirePathAck ? 1 : 0,
            JSON.stringify([encoded.proof]), Date.now() + HISTORY_RETRY_DELAYS[0], this.historyBinding(connection), transmit.dispatchId);
          dispatches.push({ws, delivery: transmit});
        } else {
          this.sql.exec("INSERT INTO pending(alias, client, delivery_id, seq, frontend, proof, state) VALUES (?, ?, ?, ?, ?, NULL, 'queued')", s.alias, s.client, deliveryId, message.seq, s.frontend);
          dispatches.push({ws, delivery});
        }
      }
    });
    await this.scheduleHistoryRetry(sockets);
    await this.ctx.storage.sync();
    for (const {ws, delivery} of dispatches) this.send(ws, delivery);
    for (const ws of this.ctx.getWebSockets()) {
      if (ws.readyState !== WebSocket.OPEN) continue;
      const connection = ws.deserializeAttachment() as Connection | WebConnection;
      if (!isWebConnection(connection)) continue;
      try {
        await this.validWebSession(connection);
        await this.pumpWebSocket(ws, connection);
      } catch (error) {
        if (!(error instanceof ApiError)) console.error("Browser room delivery failed", error);
        ws.close(error instanceof ApiError ? 1008 : 1011, error instanceof ApiError ? error.message : "Browser room delivery failed");
      }
    }
  }

  private commitMessage(alias: string, author: string, text: string, clientTimestamp?: number, webName?: string): Message {
    const last = this.rows<{timestamp: number}>("SELECT COALESCE(MAX(timestamp),0) AS timestamp FROM messages")[0].timestamp;
    const wireTime = Math.max(Math.floor(Date.now() / 1000), last + 1);
    if (wireTime > 0xffffffff) fail(503, "Room timestamp range exhausted");
    this.sql.exec("INSERT INTO messages(timestamp, origin_alias, author, client_timestamp, text, web_name) VALUES (?, ?, ?, ?, ?, ?)",
      wireTime, alias, author, clientTimestamp ?? wireTime, text, webName ?? null);
    return this.rows<Message>(`SELECT ${MESSAGE_COLUMNS} FROM messages ORDER BY seq DESC LIMIT 1`)[0];
  }

  private historyFloor(): number {
    const limit = Number(this.historyLimitSetting() ?? "0");
    if (!Number.isSafeInteger(limit) || limit < 0) fail(503, "Invalid HISTORY_LIMIT");
    const latest = this.rows<{seq: number}>("SELECT COALESCE(MAX(seq),0) AS seq FROM messages")[0].seq;
    return limit ? Math.max(0, latest - limit) : 0;
  }

  private profilesFor(messages: Message[]): ParticipantProfile[] {
    return [...new Set(messages.map(m => m.author))].flatMap(author =>
      this.rows<ParticipantProfile>(`SELECT ${PROFILE_COLUMNS} FROM profiles WHERE public_key=?`, author));
  }

  private confirmRadioDelivery(pending: Pending): void {
    this.sql.exec("UPDATE dispatches SET state='acknowledged' WHERE alias=? AND client=? AND seq=?",
      pending.alias, pending.client, pending.seq);
    // Decoded frontends and migrated pending deliveries may have no dispatch row.
    if (!this.rows("SELECT 1 FROM dispatches WHERE alias=? AND client=? AND seq=?",
      pending.alias, pending.client, pending.seq).length)
      this.sql.exec(`INSERT INTO dispatches(id, alias, frontend, client, delivery_id, state, seq)
        VALUES (?, ?, ?, ?, ?, 'acknowledged', ?)`,
      crypto.randomUUID(), pending.alias, pending.frontend, pending.client, pending.deliveryId, pending.seq);
  }

  private async webMessages(messages: Message[]): Promise<Array<Message & {rf: RadioDelivery}>> {
    const connected = new Map<string, Connection>();
    const configuredAliases = this.aliasMap();
    const frontends = this.grants();
    for (const ws of this.ctx.getWebSockets()) {
      if (ws.readyState !== WebSocket.OPEN) continue;
      const connection = ws.deserializeAttachment() as Connection | WebConnection;
      if (isWebConnection(connection)) continue;
      const config = configuredAliases[connection.alias], frontend = frontends[connection.frontend];
      const bound = this.rows<{public_key: string}>("SELECT public_key FROM identities WHERE alias=?", connection.alias)[0];
      if (config && this.ctx.id.equals(this.env.ROOMS.idFromName(config.backend)) &&
          bound?.public_key === config.publicKey && frontend?.aliases.includes(connection.alias) &&
          await this.grantCredential(frontend) === connection.credential)
        connected.set(`${connection.alias}:${connection.frontend}`, connection);
    }
    const sessions = this.rows<Session>(`SELECT ${SESSION_COLUMNS} FROM sessions`);
    const pendingByClient = new Map(this.rows<Pending>(`SELECT ${PENDING_COLUMNS} FROM pending`)
      .map(p => [`${p.alias}:${p.client}`, p]));
    return messages.map(message => {
      const status: RadioDelivery = {recipients: 0, queued: 0, sent: 0, acknowledged: 0, uncertain: 0,
        failed: 0, paused: 0, retrying: 0, exhausted: 0, attempts: 0, nextRetryAt: null};
      const attempts = this.rows<{alias: string; client: string; frontend: string; state: string}>(
        "SELECT alias, client, frontend, state FROM dispatches WHERE seq=? ORDER BY rowid", message.seq);
      const recipients = new Map<string, {session?: Session; attempts: typeof attempts}>();
      for (const session of sessions)
        if (session.cursor < message.seq && !(session.alias === message.originAlias && session.client === message.author))
          recipients.set(`${session.alias}:${session.client}`, {session, attempts: []});
      for (const attempt of attempts) {
        const key = `${attempt.alias}:${attempt.client}`;
        if (!recipients.has(key)) recipients.set(key, {attempts: []});
        recipients.get(key)!.attempts.push(attempt);
      }
      for (const recipient of recipients.values()) {
        ++status.recipients;
        const latest = recipient.attempts.at(-1);
        const session = recipient.session;
        const pending = session && pendingByClient.get(`${session.alias}:${session.client}`);
        const selected = pending?.seq === message.seq ? pending : undefined;
        const count = Math.max(recipient.attempts.length, selected ? selected.retryCount + 1 : 0);
        status.attempts = Math.max(status.attempts, count);
        if (recipient.attempts.some(attempt => attempt.state === "acknowledged")) {++status.acknowledged; continue;}
        const connection = connected.get(`${session?.alias ?? latest?.alias}:${selected?.frontend ?? session?.frontend ?? latest?.frontend}`);
        if (!connection || selected && this.isOpaque() && selected.retryBinding !== this.historyBinding(connection))
          ++status.paused;
        if (count > 1) ++status.retrying;
        if (selected && selected.retryCount >= HISTORY_RETRY_DELAYS.length && !selected.retryAt)
          ++status.exhausted;
        if (connection && selected?.retryAt && selected.retryBinding === this.historyBinding(connection) &&
            (status.nextRetryAt === null || selected.retryAt < status.nextRetryAt))
          status.nextRetryAt = selected.retryAt;
        const state = latest?.state ?? selected?.state;
        if (state === "sent") ++status.sent;
        else if (state === "unknown") ++status.uncertain;
        else if (state === "failed") ++status.failed;
        else ++status.queued;
      }
      return {...message, rf: status};
    });
  }

  private async publishProfile(profile: ParticipantProfile): Promise<void> {
    for (const ws of this.ctx.getWebSockets()) {
      if (ws.readyState !== WebSocket.OPEN) continue;
      const connection = ws.deserializeAttachment() as Connection | WebConnection;
      if (!isWebConnection(connection)) continue;
      try {
        await this.validWebSession(connection);
        this.send(ws, {type: "profile", profile});
      } catch (error) {
        if (!(error instanceof ApiError)) console.error("Room profile delivery failed", error);
        ws.close(error instanceof ApiError ? 1008 : 1011, "Room profile connection failed");
      }
    }
  }

  private webAlias(alias: string) {
    const config = this.aliasMap()[alias];
    if (!config || !this.ctx.id.equals(this.env.ROOMS.idFromName(config.backend))) fail(403, "Room does not belong to this backend");
    this.bindIdentity(alias);
    return config;
  }

  private async validWebSession(session: WebSession): Promise<WebSession> {
    const config = this.webAlias(session.alias);
    const found = this.rows<WebSession>("SELECT * FROM web_sessions WHERE token=? AND alias=?", session.token, session.alias)[0];
    const users = webUsers(this.env);
    if (!found || found.expires <= Math.floor(Date.now() / 1000) || found.author !== session.author ||
        (users === undefined ? !!found.username || found.credential !== await webCredential(config) :
          !found.username || found.credential !== await accountCredential(this.env, session.alias, found.username)))
      fail(401, "Room access expired; sign in again");
    if (passkeyMode(this.env) && found.username &&
        !await this.env.ACCOUNTS.getByName("accounts").deviceAllowed(found.username, found.author))
      fail(401, "This device's account access was revoked; use another device");
    return found;
  }

  async deviceOwner(publicKey: string): Promise<string | null> {
    return this.rows<{username: string}>("SELECT username FROM web_devices WHERE public_key=?", publicKey)[0]?.username ?? null;
  }

  private async browserSession(request: Request, alias: string): Promise<WebSession> {
    const hash = await credential(cookieToken(request, alias));
    const found = this.rows<WebSession>("SELECT * FROM web_sessions WHERE token=? AND alias=?", hash, alias)[0];
    if (!found) fail(401, "Join this room to read or send messages");
    return this.validWebSession(found);
  }

  private async webFetch(request: Request, alias: string, endpoint: string): Promise<Response> {
    const config = this.webAlias(alias);
    const url = new URL(request.url);
    const now = Math.floor(Date.now() / 1000);
    if (endpoint === "login" || endpoint === "challenge") {
      if (request.method !== "POST") fail(405, "POST required");
      sameOrigin(request);
      const body = await webBody(request);
      const address = await credential(`${endpoint}:${request.headers.get("CF-Connecting-IP") ?? "local"}`);
      const permitted = this.ctx.storage.transactionSync(() => {
        this.sql.exec("DELETE FROM web_login_limits WHERE start<?", now - 60);
        this.sql.exec(`INSERT INTO web_login_limits VALUES (?, ?, 1)
          ON CONFLICT(address) DO UPDATE SET attempts=attempts+1`, address, now);
        return this.rows<{attempts: number}>("SELECT attempts FROM web_login_limits WHERE address=?", address)[0].attempts <= 10;
      });
      if (!permitted) fail(429, "Too many room login attempts; wait one minute");
      const users = webUsers(this.env);
      if (endpoint === "challenge") {
        if (passkeyMode(this.env)) fail(403, "Use passkey sign-in or device linking; password sign-in is disabled");
        if (users === undefined) fail(503, "Operator accounts are not configured on this room service");
        const user = username(body.username), publicKey = hex(body.publicKey, "device public key", 32);
        const nonce = Array.from(crypto.getRandomValues(new Uint8Array(32)), b => b.toString(16).padStart(2, "0")).join("");
        const message = deviceChallenge(url.origin, alias, user, publicKey, nonce);
        this.ctx.storage.transactionSync(() => {
          this.sql.exec("DELETE FROM web_challenges WHERE expires<=?", now);
          if (this.rows<{n: number}>("SELECT COUNT(*) AS n FROM web_challenges")[0].n >= 4096)
            fail(429, "Room login queue is full; wait two minutes");
          this.sql.exec("INSERT INTO web_challenges VALUES (?, ?, ?, ?, ?)", nonce, alias, user, publicKey, now + 120);
        });
        await this.ctx.storage.sync();
        return webResponse({nonce, message, expires: now + 120, protocol: DEVICE_PROTOCOL});
      }
      let author: string, name: string, user: string | undefined, sessionCredential: string;
      if (passkeyMode(this.env)) {
        const ticket = hex(body.ticket, "account room ticket", 32);
        const account = await this.env.ACCOUNTS.getByName("accounts").consumeTicket(ticket, alias);
        if (!account) fail(401, "Account room ticket expired or invalid; sign in again");
        const owner = await this.deviceOwner(account.publicKey);
        if (owner && owner !== account.username) fail(409, "This device key belongs to another account");
        user = account.username; author = account.publicKey; name = account.name;
        this.sql.exec("INSERT OR IGNORE INTO web_devices VALUES (?, ?)", author, user);
        sessionCredential = await accountCredential(this.env, alias, user);
      } else if (users !== undefined) {
        user = username(body.username);
        const account = users[user], publicKey = hex(body.publicKey, "device public key", 32);
        const nonce = hex(body.nonce, "device login challenge", 32), signature = hex(body.signature, "device signature", 64);
        const challenge = this.ctx.storage.transactionSync(() => {
          const saved = this.rows<{alias: string; username: string; public_key: string; expires: number}>(
            "SELECT * FROM web_challenges WHERE nonce=?", nonce)[0];
          this.sql.exec("DELETE FROM web_challenges WHERE nonce=?", nonce);
          return saved;
        });
        await this.ctx.storage.sync();
        if (!challenge || challenge.expires <= now || challenge.alias !== alias || challenge.username !== user ||
            challenge.public_key !== publicKey || !this.codec.crypto.verify(fromHex(publicKey), fromHex(signature),
              textEncoder.encode(deviceChallenge(url.origin, alias, user, publicKey, nonce))))
          fail(403, "Device ownership check failed; request a new login challenge");
        if (!await checkPassword(body.password, account) || !account.aliases.includes(alias))
          fail(403, "Incorrect username, password or room access");
        const owner = this.rows<{username: string}>("SELECT username FROM web_devices WHERE public_key=?", publicKey)[0];
        if (owner && owner.username !== user) fail(409, "This desktop key belongs to another account; use a separate browser profile or client state directory");
        this.sql.exec("INSERT OR IGNORE INTO web_devices VALUES (?, ?)", publicKey, user);
        author = publicKey; name = account.name;
        sessionCredential = await accountCredential(this.env, alias, user);
      } else {
        const password = typeof body.password === "string" && wellFormed(body.password) && textEncoder.encode(body.password).length <= 256 ? body.password : undefined;
        if (password === undefined || !equalToken(password, config.password)) fail(403, "Incorrect room password");
        const identity = hex(body.identity, "browser identity", 32);
        name = displayName(body.name);
        author = await credential(`aspen-web-author:${identity}`);
        sessionCredential = await webCredential(config);
      }
      // A hex token avoids URL/cookie encoding and is never put in a socket URL.
      const cookie = Array.from(crypto.getRandomValues(new Uint8Array(32)), b => b.toString(16).padStart(2, "0")).join("");
      const session: WebSession = {token: await credential(cookie), alias, author, name,
        credential: sessionCredential, expires: now + WEB_SESSION_SECONDS, username: user ?? null};
      let profile: ParticipantProfile | undefined;
      this.ctx.storage.transactionSync(() => {
        this.sql.exec("DELETE FROM web_sessions WHERE expires<=? OR (alias=? AND author=?)", now, alias, author);
        this.sql.exec("INSERT INTO web_sessions(token,alias,author,name,credential,expires,username) VALUES (?, ?, ?, ?, ?, ?, ?)",
          session.token, alias, author, name, session.credential, session.expires, session.username ?? null);
        if (user) {
          const previous = this.rows<ParticipantProfile>(`SELECT ${PROFILE_COLUMNS} FROM profiles WHERE public_key=?`, author)[0];
          profile = {publicKey: author, name, advertType: 1, timestamp: Math.max(now, (previous?.timestamp ?? 0) + 1), source: "desktop"};
          this.sql.exec(`INSERT INTO profiles VALUES (?, ?, ?, ?, ?)
            ON CONFLICT(public_key) DO UPDATE SET name=excluded.name, advert_type=excluded.advert_type,
            timestamp=excluded.timestamp, source=excluded.source`, author, name, 1, profile.timestamp, profile.source);
        }
      });
      await this.ctx.storage.sync();
      if (profile) await this.publishProfile(profile);
      return webResponse({author, name, username: session.username, expires: session.expires, maxPostBytes: POST_BYTES}, 200, sessionCookie(alias, cookie));
    }
    const session = await this.browserSession(request, alias);
    if (endpoint === "session") {
      if (request.method !== "GET") fail(405, "GET required");
      return webResponse({author: session.author, name: session.name, username: session.username ?? null, expires: session.expires, maxPostBytes: POST_BYTES});
    }
    if (endpoint === "logout") {
      if (request.method !== "POST") fail(405, "POST required");
      sameOrigin(request);
      this.sql.exec("DELETE FROM web_sessions WHERE token=?", session.token);
      await this.ctx.storage.sync();
      for (const ws of this.ctx.getWebSockets()) {
        const c = ws.deserializeAttachment() as Connection | WebConnection;
        if (isWebConnection(c) && c.token === session.token) ws.close(1008, "Left room");
      }
      return webResponse({left: true}, 200, sessionCookie(alias, "", 0));
    }
    if (endpoint === "history") {
      if (request.method !== "GET") fail(405, "GET required");
      const after = url.searchParams.get("after"), before = url.searchParams.get("before");
      if (after !== null && before !== null) fail(400, "Use either after or before");
      const floor = this.historyFloor();
      let messages: Message[], more: boolean;
      if (after !== null) {
        const cursor = Math.max(floor, sequence(after, "history cursor"));
        const rows = this.rows<Message>(`SELECT ${MESSAGE_COLUMNS} FROM messages WHERE seq>? ORDER BY seq LIMIT ?`, cursor, PAGE_SIZE + 1);
        more = rows.length > PAGE_SIZE;
        messages = rows.slice(0, PAGE_SIZE);
      } else {
        const ceiling = before === null ? Number.MAX_SAFE_INTEGER : sequence(before, "history cursor");
        const rows = this.rows<Message>(`SELECT ${MESSAGE_COLUMNS} FROM messages WHERE seq>? AND seq<? ORDER BY seq DESC LIMIT ?`, floor, ceiling, PAGE_SIZE + 1);
        more = rows.length > PAGE_SIZE;
        messages = rows.slice(0, PAGE_SIZE).reverse();
      }
      return webResponse({messages: await this.webMessages(messages), more, floor, profiles: this.profilesFor(messages)});
    }
    if (endpoint === "posts") {
      if (request.method !== "POST") fail(405, "POST required");
      sameOrigin(request);
      const body = await webBody(request);
      const id = string(body.id, "post ID", 36);
      if (!/^[a-f0-9]{8}-[a-f0-9]{4}-4[a-f0-9]{3}-[89ab][a-f0-9]{3}-[a-f0-9]{12}$/.test(id)) fail(400, "Use a UUID v4 post ID");
      const bodyText = webText(body.text, "message text", POST_BYTES);
      const result = this.ctx.storage.transactionSync(() => {
        const existing = this.rows<Message>(`SELECT ${MESSAGE_COLUMNS} FROM messages WHERE seq=(SELECT seq FROM web_posts WHERE author=? AND request_id=?)`,
          session.author, id)[0];
        if (existing) {
          if (existing.text !== `${existing.webName}: ${bodyText}`) fail(409, "This post ID already belongs to a different message");
          return {message: existing, duplicate: true};
        }
        const text = `${session.name}: ${bodyText}`;
        webText(text, "message including display name", POST_BYTES);
        const message = this.commitMessage(alias, session.author, text, undefined, session.name);
        this.sql.exec("INSERT INTO web_posts VALUES (?, ?, ?)", session.author, id, message.seq);
        return {message, duplicate: false};
      });
      await this.ctx.storage.sync();
      await this.pump();
      return webResponse({...result, message: (await this.webMessages([result.message]))[0]});
    }
    if (request.method !== "GET" || request.headers.get("Upgrade")?.toLowerCase() !== "websocket") fail(426, "WebSocket upgrade required");
    sameOrigin(request);
    if (!request.headers.get("Sec-WebSocket-Protocol")?.split(",").map(p => p.trim()).includes(WEB_PROTOCOL))
      fail(426, `Use ${WEB_PROTOCOL}`);
    const cursor = sequence(url.searchParams.get("since") ?? "0", "history cursor");
    const [client, server] = Object.values(new WebSocketPair());
    this.ctx.acceptWebSocket(server);
    const connection: WebConnection = {...session, kind: "web", cursor};
    server.serializeAttachment(connection);
    this.send(server, {type: "ready", version: 1, alias, publicKey: config.publicKey, name: config.name});
    await this.pumpWebSocket(server, connection);
    return new Response(null, {status: 101, webSocket: client, headers: {"Sec-WebSocket-Protocol": WEB_PROTOCOL}});
  }

  // ---- Hosted rooms: `rt-<alias>` objects ------------------------------------------------------------
  // Created only by `provision`. A hosted object that was never provisioned answers 404 and writes nothing.
  private hostedName: boolean;
  private hostedRow?: HostedRow;
  private hosted?: HostedMaterial;
  private hostedAlias?: Alias;
  private hostedFailed = false;
  private budgetDay = 0;
  private budgetCount = 0;
  private budgetSaved = 0;

  private registry() {return this.env.REGISTRY.getByName("registry");}
  private meta(key: string): number | undefined {
    return this.rows<{v: number}>("SELECT v FROM hosted_meta WHERE k=?", key)[0]?.v;
  }
  private setMeta(key: string, value: number): void {
    this.sql.exec("INSERT INTO hosted_meta(k, v) VALUES (?, ?) ON CONFLICT(k) DO UPDATE SET v=excluded.v", key, value);
  }
  private async loadHosted(): Promise<void> {
    if (!this.rows("SELECT 1 FROM sqlite_master WHERE type='table' AND name='hosted_room'").length) return;
    const row = this.rows<HostedRow>("SELECT * FROM hosted_room WHERE id=1")[0];
    if (!row) return;
    this.hostedRow = row;
    if (row.state === "deleted") return;
    await this.openHosted(row);
    this.budgetDay = this.meta("wday") ?? 0;
    this.budgetCount = this.meta("wcount") ?? 0;
  }
  private async openHosted(row: HostedRow): Promise<void> {
    const kek = parseKek(this.env);
    try {
      if (!kek || !row.sealed) throw new Error("sealed");
      this.hosted = await openRoom(kek, row.alias, row.public_key, row.kid, new Uint8Array(row.sealed));
      this.hostedAlias = {backend: "rt-" + row.alias, publicKey: row.public_key, name: row.name, password: this.hosted.password};
      this.codec.setOverlay({aliases: {[row.alias]: this.hostedAlias}, keys: {[row.alias]: toHex(this.hosted.key)}});
      this.hostedFailed = false;
    } catch {
      this.hostedFailed = true;
      this.hosted = undefined;
    }
  }
  private hostedActive(): boolean {return !!this.hosted && !!this.hostedAlias && this.hostedRow?.state === "active";}
  private hostedGrants(): Record<string, Grant> {
    if (!this.hostedActive()) return {};
    const grants: Record<string, Grant> = {};
    for (const f of this.rows<{node: string; token_hash: string}>("SELECT node, token_hash FROM hosted_frontend WHERE revoked=0"))
      grants[f.node.slice(0, 16)] = {aliases: [this.hostedRow!.alias], credential: f.token_hash};
    return grants;
  }

  /** Request gate for a hosted object: only the Worker's validated alias header reaches storage. */
  private hostedGate(request: Request, browser: unknown): void {
    const match = new URL(request.url).pathname.match(/^\/v1\/aliases\/([a-z2-7]{30})\/(socket|operations)$/);
    if (browser || !match || !HOSTED_ALIAS.test(match[1]) || request.headers.get("x-aspen-hosted-alias") !== match[1] ||
        this.ctx.id.name !== "rt-" + match[1] || !this.hostedRow || this.hostedRow.state === "deleted")
      fail(404, "Unknown room alias");
    if (this.hostedFailed) fail(503, "Hosted room is not available");
  }
  private async hostedAuthorize(request: Request, alias: string): Promise<Connection> {
    const client = request.headers.get("x-aspen-client") ?? "none";
    const token = request.headers.get("Authorization")?.match(/^Bearer (ar1_[a-f0-9]{64})$/)?.[1];
    const credentialHash = token ? await tokenHash(token) : undefined;
    const row = credentialHash ?
      this.rows<HostedFrontendRow>("SELECT * FROM hosted_frontend WHERE token_hash=? AND revoked=0", credentialHash)[0] : undefined;
    if (!row) {
      if (memoryLimit("bad:" + client, 20, 60)) fail(429, "Too many failed frontend attempts; wait one minute");
      fail(401, "Unknown frontend token");
    }
    if (this.hostedRow!.state === "disabled") fail(403, "Room is disabled");
    if (memoryLimit("conn:" + credentialHash, 12, 60)) fail(429, "Too many frontend connections; wait one minute");
    const now = sec();
    if (now - (row.last_seen ?? 0) > 3600) {
      this.sql.exec("UPDATE hosted_frontend SET last_seen=? WHERE node=?", now, row.node);
      this.sql.exec("UPDATE hosted_room SET last_seen=? WHERE id=1", now);
      this.hostedRow!.last_seen = now;
      try {await this.registry().touchRoom(alias);} catch {/* the index is advisory */}
    }
    return {alias, frontend: row.node.slice(0, 16), credential: credentialHash!};
  }

  /** Counts one inbound operation against the per-token and daily room limits. False closes the socket. */
  private async hostedAdmit(ws: WebSocket | undefined, credentialHash: string, bytes: number): Promise<"ok" | "limit" | "disabled"> {
    if (memoryLimit("fr:" + credentialHash, 20, 1) || memoryLimit("by:" + credentialHash, 65536, 60, Date.now() / 1000, bytes)) {
      ws?.close(1008, "Frame limit exceeded");
      return "limit";
    }
    return await this.hostedBudget(ws) ? "ok" : "disabled";
  }
  private async hostedBudget(ws?: WebSocket): Promise<boolean> {
    const now = sec(), day = Math.floor(now / 86400), limit = intSetting(this.env.ROOM_WRITES_PER_DAY, 20000);
    if (this.budgetDay !== day) {this.budgetDay = day; this.budgetCount = 0;}
    if (++this.budgetCount > limit) {
      await this.hostedSetState("disabled", "write-budget");
      ws?.close(1008, "Room write limit reached");
      return false;
    }
    if (now - this.budgetSaved >= 60) {
      this.budgetSaved = now;
      this.setMeta("wday", this.budgetDay); this.setMeta("wcount", this.budgetCount);
    }
    return true;
  }
  private async hostedSetState(state: "active" | "disabled", reason?: string): Promise<void> {
    if (!this.hostedRow || this.hostedRow.state === "deleted" || this.hostedRow.state === state) return;
    this.sql.exec("UPDATE hosted_room SET state=? WHERE id=1", state);
    this.hostedRow.state = state;
    if (reason) {
      try {
        await this.registry().recordAudit("system", reason, this.hostedRow.alias, state);
        await this.registry().setRoomState(this.hostedRow.alias, state);
      } catch {await this.registry().adminSetDirty(this.hostedRow.alias).catch(() => undefined);}
    }
    await this.pump();
  }
  private dailyAt(): number {return this.meta("daily_at") ?? 0;}

  private async hostedHousekeeping(): Promise<void> {
    if (!this.hostedRow || this.hostedRow.state === "deleted") return;
    const row = this.hostedRow = this.rows<HostedRow>("SELECT * FROM hosted_room WHERE id=1")[0] ?? this.hostedRow;
    const now = sec();
    if (now < this.dailyAt()) return;
    const day = Math.floor(now / 86400), limit = intSetting(this.env.HOSTED_HISTORY_LIMIT, 2000);
    this.ctx.storage.transactionSync(() => {
      // Rows have no timestamps, so a daily row-id mark ages them: anything older than the 14-day mark goes.
      if (this.meta("mark:" + (day - 14)) !== undefined) {
        const a = this.meta("amax:" + (day - 14)) ?? 0, d = this.meta("dmax:" + (day - 14)) ?? 0;
        this.sql.exec("DELETE FROM attempts WHERE rowid<=?", a);
        this.sql.exec("DELETE FROM dispatches WHERE rowid<=?", d);
      }
      for (const k of this.rows<{k: string}>("SELECT k FROM hosted_meta WHERE k LIKE 'mark:%' OR k LIKE 'amax:%' OR k LIKE 'dmax:%'")) {
        const n = Number(k.k.split(":")[1]);
        if (n < day - 15) this.sql.exec("DELETE FROM hosted_meta WHERE k=?", k.k);
      }
      this.setMeta("mark:" + day, 1);
      this.setMeta("amax:" + day, this.rows<{n: number}>("SELECT COALESCE(MAX(rowid),0) AS n FROM attempts")[0].n);
      this.setMeta("dmax:" + day, this.rows<{n: number}>("SELECT COALESCE(MAX(rowid),0) AS n FROM dispatches")[0].n);
      // History beyond the limit, and any delivery still pointing at it, goes.
      const latest = this.rows<{n: number}>("SELECT COALESCE(MAX(seq),0) AS n FROM messages")[0].n;
      if (latest > limit) {
        this.sql.exec("DELETE FROM pending WHERE seq<=?", latest - limit);
        this.sql.exec("DELETE FROM messages WHERE seq<=?", latest - limit);
      }
      // Session table stays bounded; the least recently active clients go first.
      this.sql.exec(`DELETE FROM sessions WHERE rowid IN (SELECT rowid FROM sessions ORDER BY last_timestamp DESC LIMIT -1 OFFSET 1000)`);
      this.setMeta("daily_at", now + 86400);
    });
    if (row.pending_delete !== null && now >= row.pending_delete) {
      await this.hostedPurge("delete");
      return;
    }
    if (now - row.last_seen > 365 * 86400) {await this.hostedPurge("idle-365"); return;}
    if (row.state === "active" && now - row.last_seen > 180 * 86400) await this.hostedSetState("disabled", "idle-180");
  }

  private async hostedPurge(reason: string, actor = "system"): Promise<void> {
    const row = this.hostedRow;
    if (!row || row.state === "deleted") return;
    for (const ws of this.ctx.getWebSockets()) ws.close(1008, "Room deleted");
    this.ctx.storage.transactionSync(() => {
      for (const table of ["identities", "messages", "sessions", "pending", "attempts", "dispatches", "web_sessions", "web_posts",
        "web_login_limits", "profiles", "web_challenges", "web_devices", "hosted_frontend", "hosted_meta"])
        this.sql.exec(`DELETE FROM ${table}`);
      this.sql.exec("UPDATE hosted_room SET state='deleted', sealed=NULL, name='', public_key='', pending_delete=NULL WHERE id=1");
    });
    row.state = "deleted";
    this.hosted = undefined; this.hostedAlias = undefined;
    this.codec.setOverlay({aliases: {}, keys: {}});
    await this.ctx.storage.deleteAlarm();
    try {await this.registry().purgeRoom(row.alias, actor, reason);} catch {/* the Registry alarm drops stale rooms */}
  }

  // Internal RPC. Results are plain values: RPC does not carry error classes.
  private requireHosted(alias: string): void {
    if (!this.hostedName || this.ctx.id.name !== "rt-" + alias || !HOSTED_ALIAS.test(alias)) throw new Error("not a hosted room");
  }
  private async issue(node: string, role: number, creator = false): Promise<HostedGrant> {
    const row = this.hostedRow!, now = sec();
    const token = newToken(), hash = await tokenHash(token);
    const existing = this.rows<HostedFrontendRow>("SELECT * FROM hosted_frontend WHERE node=?", node)[0];
    const live = existing && !existing.revoked;
    const finalRole = Math.max(live ? existing.role : 0, role);
    const ownerSince = creator ? null : finalRole === 1 && !(live && existing.role === 1) ? now : live ? existing.owner_since : null;
    this.sql.exec(`INSERT INTO hosted_frontend(node, token_hash, role, created, last_seen, revoked, owner_since) VALUES (?, ?, ?, ?, ?, 0, ?)
      ON CONFLICT(node) DO UPDATE SET token_hash=excluded.token_hash, role=excluded.role, revoked=0, owner_since=excluded.owner_since,
      created=CASE WHEN revoked=1 THEN excluded.created ELSE created END`, node, hash, finalRole, now, now, ownerSince);
    if (existing) await this.pump(); // the replaced token's sockets close
    return {alias: row.alias, name: row.name, pub: row.public_key, token, role: finalRole};
  }
  async provision(p: {alias: string; name: string; publicKey: string; key: string; password: string | null; node: string}):
      Promise<GrantReply> {
    this.requireHosted(p.alias);
    return this.ctx.blockConcurrencyWhile(async () => {
      if (this.hostedRow) {
        if (this.hostedRow.state === "deleted") return {ok: false, e: "not_found"};
        return {ok: true, grant: await this.issue(p.node, 1, true)};
      }
      const kek = parseKek(this.env);
      if (!kek) return {ok: false, e: "closed"};
      const key = fromHex(p.key);
      let publicKey: string;
      try {publicKey = toHex(this.codec.crypto.publicKey(key));} catch {return {ok: false, e: "key"};}
      if (publicKey !== p.publicKey) return {ok: false, e: "key"};
      const password = p.password ?? generatedPassword();
      const {kid, sealed} = await sealRoom(kek, p.alias, publicKey, key, password);
      key.fill(0);
      this.initStatic();
      this.sql.exec(HOSTED_DDL);
      const now = sec();
      this.sql.exec("INSERT INTO hosted_room(id, alias, name, public_key, sealed, kid, state, created, last_seen, pending_delete) VALUES (1, ?, ?, ?, ?, ?, 'active', ?, ?, NULL)",
        p.alias, p.name, publicKey, sealed.buffer as ArrayBuffer, kid, now, now);
      this.hostedRow = this.rows<HostedRow>("SELECT * FROM hosted_room WHERE id=1")[0];
      await this.openHosted(this.hostedRow);
      this.setMeta("daily_at", now + 86400);
      await this.ctx.storage.setAlarm(Date.now() + 86400_000);
      return {ok: true, grant: await this.issue(p.node, 1, true)};
    });
  }
  async addFrontend(p: {alias: string; node: string; role: number}): Promise<GrantReply> {
    this.requireHosted(p.alias);
    if (!this.hostedRow || this.hostedRow.state !== "active") return {ok: false, e: "not_found"};
    if (this.hostedFailed) return {ok: false, e: "closed"};
    return {ok: true, grant: await this.issue(p.node, p.role)};
  }
  async setState(alias: string, state: "active" | "disabled"): Promise<void> {
    this.requireHosted(alias);
    await this.hostedSetState(state);
  }
  async purge(alias: string): Promise<void> {
    this.requireHosted(alias);
    await this.hostedPurge("purge", "admin");
  }
  async revokeNode(alias: string, node: string): Promise<void> {
    this.requireHosted(alias);
    if (!this.hostedRow || this.hostedRow.state === "deleted") return;
    const r = this.sql.exec("UPDATE hosted_frontend SET revoked=1 WHERE node=? AND revoked=0", node);
    if (r.rowsWritten) await this.pump();
  }

  /** Owner and member calls on a hosted room, authenticated by the room token. */
  async control(alias: string, bearer: string, body: Record<string, unknown>, meta: {host: string; client: string}): Promise<ControlReply> {
    try {
      return {status: 200, body: await this.controlNow(alias, bearer, body, meta)};
    } catch (error) {
      if (error instanceof NodeError) return failureParts(error);
      console.error("control failed", error instanceof Error ? error.constructor.name : typeof error);
      return {status: 500, body: {ok: 0, e: "internal", m: "Internal error."}};
    }
  }
  private ownerCount(excluding: string): number {
    return this.rows<{n: number}>("SELECT COUNT(*) AS n FROM hosted_frontend WHERE role=1 AND revoked=0 AND node<>?", excluding)[0].n;
  }
  private otherCount(excluding: string): number {
    return this.rows<{n: number}>("SELECT COUNT(*) AS n FROM hosted_frontend WHERE revoked=0 AND node<>?", excluding)[0].n;
  }
  private async controlNow(alias: string, bearer: string, body: Record<string, unknown>, meta: {host: string; client: string}):
      Promise<Record<string, unknown>> {
    if (!this.hostedName || this.ctx.id.name !== "rt-" + alias || !this.hostedRow) nodeFail("not_found");
    const room = this.hostedRow;
    if (room.state === "deleted") nodeFail("gone");
    if (this.hostedFailed) nodeFail("closed");
    const hash = await tokenHash(bearer);
    const me = this.rows<HostedFrontendRow>("SELECT * FROM hosted_frontend WHERE token_hash=? AND revoked=0", hash)[0];
    if (!me) {
      const wait = memoryLimit("bad:" + meta.client, 20, 60);
      if (wait) nodeFail("rate", {retryAfter: wait});
      nodeFail("auth");
    }
    const wait = memoryLimit("ctl:" + hash, 30, 60);
    if (wait) nodeFail("rate", {retryAfter: wait});
    const op = body.op, now = sec(), owner = me.role === 1;
    if (room.state === "disabled" && op !== "leave") nodeFail("disabled");
    const needOwner = () => {if (!owner) nodeFail("forbidden");};
    const recent = (row: HostedFrontendRow) => row.owner_since !== null && now - row.owner_since < OWNER_DELAY;
    switch (op) {
      case "invite": {
        needOwner();
        const role = body.role === undefined ? "frontend" : body.role;
        if (role !== "frontend" && role !== "owner") nodeFail("bad_request");
        const max = role === "owner" ? 900 : 86400, def = role === "owner" ? 120 : 600;
        const ttl = body.ttl === undefined ? def : body.ttl;
        if (typeof ttl !== "number" || !Number.isInteger(ttl) || ttl < 60 || ttl > max) nodeFail("bad_request");
        const code = randomCode();
        const result = await this.registry().mintJoin({alias, hash: await codeHash(code), role: role === "owner" ? 1 : 0,
          expires: now + ttl, issuer: me.node.slice(0, 8)});
        if (result === "cap") nodeFail("cap");
        if (result !== "ok") nodeFail("not_found");
        return {ok: 1, code: formatCode(code), host: meta.host, ttl};
      }
      case "nodes": {
        needOwner();
        const offset = body.offset === undefined ? 0 : body.offset;
        if (typeof offset !== "number" || !Number.isInteger(offset) || offset < 0 || offset > 100000) nodeFail("bad_request");
        const rows = this.rows<HostedFrontendRow>("SELECT * FROM hosted_frontend WHERE revoked=0 ORDER BY role DESC, created, node LIMIT 6 OFFSET ?", offset);
        const total = this.rows<{n: number}>("SELECT COUNT(*) AS n FROM hosted_frontend WHERE revoked=0")[0].n;
        return {ok: 1, nodes: rows.slice(0, 5).map(r => ({id: r.node.slice(0, 8), r: r.role === 1 ? "o" : "f",
          seen: Math.max(0, now - (r.last_seen ?? r.created))})), total, next: rows.length > 5 ? offset + 5 : null};
      }
      case "revoke": {
        needOwner();
        const id = body.id;
        if (typeof id !== "string" || !/^[a-f0-9]{8,16}$/.test(id)) nodeFail("bad_request");
        const match = this.rows<HostedFrontendRow>("SELECT * FROM hosted_frontend WHERE revoked=0 AND substr(node, 1, ?)=? LIMIT 2", id.length, id);
        if (match.length !== 1) nodeFail("not_found");
        const target = match[0];
        if (target.node === me.node) nodeFail("self");
        if (target.role === 1) {
          if (this.ownerCount(target.node) < 1) nodeFail("last_owner");
          if (recent(me)) nodeFail("too_new");
        }
        this.sql.exec("UPDATE hosted_frontend SET revoked=1 WHERE node=?", target.node);
        await this.pump();
        try {await this.registry().memberRemove(target.node, alias);} catch {/* the Registry alarm re-checks bans */}
        return {ok: 1};
      }
      case "password": {
        needOwner();
        const mode = body.mode;
        if (mode === "get") return {ok: 1, pw: this.hosted!.password};
        let next: string;
        if (mode === "set") {
          if (!validPassword(body.pw)) nodeFail("bad_request");
          next = body.pw;
        } else if (mode === "open") next = "";
        else if (mode === "new") next = generatedPassword();
        else return nodeFail("bad_request");
        const kek = parseKek(this.env);
        if (!kek) nodeFail("closed");
        const key = this.hosted!.key.slice();
        const {kid, sealed} = await sealRoom(kek, alias, room.public_key, key, next);
        key.fill(0);
        this.sql.exec("UPDATE hosted_room SET sealed=?, kid=? WHERE id=1", sealed.buffer as ArrayBuffer, kid);
        room.kid = kid;
        this.hosted!.password = next; this.hostedAlias!.password = next;
        return {ok: 1, pw: next};
      }
      case "leave": {
        if (owner && this.ownerCount(me.node) < 1 && this.otherCount(me.node) > 0) nodeFail("last_owner");
        const pw = this.hosted?.password ?? "";
        if (this.otherCount(me.node) === 0) {
          await this.hostedPurge("leave", me.node.slice(0, 8));
          return {ok: 1, deleted: 1, pw};
        }
        this.sql.exec("UPDATE hosted_frontend SET revoked=1 WHERE node=?", me.node);
        await this.pump();
        try {await this.registry().memberRemove(me.node, alias);} catch {/* see revoke */}
        return {ok: 1, deleted: 0};
      }
      case "delete": {
        needOwner();
        if (body.confirm !== alias) nodeFail("bad_request");
        if (recent(me)) nodeFail("too_new");
        if (this.ownerCount(me.node) < 1) {
          await this.hostedPurge("delete", me.node.slice(0, 8));
          return {ok: 1};
        }
        const at = room.pending_delete ?? now + OWNER_DELAY;
        this.sql.exec("UPDATE hosted_room SET pending_delete=? WHERE id=1", at);
        room.pending_delete = at;
        return {ok: 1, pending: Math.max(0, at - now)};
      }
      case "cancel_delete": {
        needOwner();
        this.sql.exec("UPDATE hosted_room SET pending_delete=NULL WHERE id=1");
        room.pending_delete = null;
        return {ok: 1};
      }
      default:
        return nodeFail("bad_request");
    }
  }

  private async pumpWebSocket(ws: WebSocket, connection: WebConnection): Promise<void> {
    const cursor = Math.max(connection.cursor, this.historyFloor());
    const messages = this.rows<Message>(`SELECT ${MESSAGE_COLUMNS} FROM messages WHERE seq>? ORDER BY seq LIMIT ?`, cursor, PAGE_SIZE + 1);
    for (const message of await this.webMessages(messages.slice(0, PAGE_SIZE))) {
      for (const profile of this.profilesFor([message])) this.send(ws, {type: "profile", profile});
      this.send(ws, {type: "message", message});
      connection.cursor = message.seq;
    }
    if (connection.cursor < cursor) connection.cursor = cursor;
    ws.serializeAttachment(connection);
    if (messages.length > PAGE_SIZE) this.send(ws, {type: "catchup", cursor: connection.cursor});
  }
}
