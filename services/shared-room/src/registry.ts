// Control plane for hosted rooms: one-time codes, room index, quotas, bans and
// audit. It is never on the frame path and never holds a room key or token.
// Every method here is an internal Durable Object RPC; the Worker validates
// requests and signatures before calling it.
import {DurableObject} from "cloudflare:workers";
import type {Env} from "./config";
import {roomOps} from "./room-ops";

const sec = (): number => Math.floor(Date.now() / 1000);
const LIMITS_CAP = 4096, LIMITS_STALE = 90000;
const CLAIM_RETRY = 60, CLAIM_STALE = 300;
const AUDIT_ROWS = 5000, AUDIT_DAYS = 30, ADMIN_AUDIT_ROWS = 1000;

export interface ClaimParams {
  op: "join" | "create" | "convert"; hash: string; open: boolean; node: string; ts: number; subject: string;
  client: string; signup: string; maxRooms: number; maxFrontends: number; avoid: number[];
  alias: string; pubHash?: string; pubFirst?: number; name?: string; writesPerDay: number;
}
export type ClaimResult =
  | {k: "claim"; alias: string; role: number; reclaim: boolean}
  | {k: "replay"; alias: string; role: number}
  | {k: "recover"; alias: string}
  | {k: "err"; e: string; retryAfter?: number};
export interface AdminRoom {alias: string; name: string | null; state: string; created: number; last_active: number; owner: string}
export interface AdminCode {h: string; kind: string; alias: string | null; issuer: string; expires: number; state: string}
export interface AuditRow {id: number; ts: number; actor: string; action: string; target: string; result: string}
export interface RoomRow {alias: string; name: string | null; state: string; created: number; last_active: number; owner_node: string}

export class Registry extends DurableObject<Env> {
  private sql: SqlStorage;
  private alarmChecked = false;
  constructor(ctx: DurableObjectState, env: Env) {
    super(ctx, env);
    this.sql = ctx.storage.sql;
    this.sql.exec(`
      CREATE TABLE IF NOT EXISTS codes(hash TEXT PRIMARY KEY, kind TEXT, alias TEXT, role INTEGER, issuer TEXT,
        expires INTEGER, attempts INTEGER DEFAULT 0, state TEXT DEFAULT 'new', claimed_by TEXT, claimed_at INTEGER,
        used_at INTEGER, op TEXT, subject TEXT, first_ts INTEGER);
      CREATE TABLE IF NOT EXISTS rooms(alias TEXT PRIMARY KEY, state TEXT, owner_node TEXT, created INTEGER,
        last_active INTEGER, pub_hash TEXT UNIQUE, name TEXT, pub_first INTEGER, state_since INTEGER, dirty INTEGER DEFAULT 0);
      CREATE TABLE IF NOT EXISTS members(node TEXT, alias TEXT, PRIMARY KEY(node, alias));
      CREATE TABLE IF NOT EXISTS bans(node TEXT PRIMARY KEY, until INTEGER);
      CREATE TABLE IF NOT EXISTS limits(key TEXT PRIMARY KEY, window_start INTEGER, count INTEGER);
      CREATE TABLE IF NOT EXISTS audit(id INTEGER PRIMARY KEY AUTOINCREMENT, ts INTEGER, actor TEXT, action TEXT, target TEXT, result TEXT);
      CREATE TABLE IF NOT EXISTS admin_audit(id INTEGER PRIMARY KEY AUTOINCREMENT, ts INTEGER, action TEXT, target TEXT, result TEXT);
      CREATE INDEX IF NOT EXISTS codes_alias ON codes(alias);
      CREATE INDEX IF NOT EXISTS members_alias ON members(alias);
      CREATE INDEX IF NOT EXISTS rooms_owner ON rooms(owner_node);
    `);
  }

  private rows<T>(query: string, ...params: SqlStorageValue[]): T[] {return this.sql.exec(query, ...params).toArray() as T[];}
  private one<T>(query: string, ...params: SqlStorageValue[]): T | undefined {return this.rows<T>(query, ...params)[0];}
  private num(query: string, ...params: SqlStorageValue[]): number {return this.one<{n: number}>(query, ...params)?.n ?? 0;}

  private async ensureAlarm(): Promise<void> {
    if (this.alarmChecked) return;
    this.alarmChecked = true;
    if (await this.ctx.storage.getAlarm() === null) await this.ctx.storage.setAlarm(Date.now() + 3600_000);
  }

  // Counters --------------------------------------------------------------
  private count(key: string, window: number, now: number): number {
    const r = this.one<{window_start: number; count: number}>("SELECT window_start, count FROM limits WHERE key=?", key);
    return r && now < r.window_start + window ? r.count : 0;
  }
  private bump(key: string, window: number, now: number, by = 1): void {
    const r = this.one<{window_start: number; count: number}>("SELECT window_start, count FROM limits WHERE key=?", key);
    if (r && now < r.window_start + window) {
      this.sql.exec("UPDATE limits SET count=count+? WHERE key=?", by, key);
      return;
    }
    if (!r) {
      // A new key prunes stale rows, then evicts the oldest when the table is full.
      this.sql.exec("DELETE FROM limits WHERE window_start<?", now - LIMITS_STALE);
      if (this.num("SELECT COUNT(*) AS n FROM limits") >= LIMITS_CAP)
        // Only per-client counters may be evicted; quota rows (cc, oc, os, mj, global) stay.
        this.sql.exec(`DELETE FROM limits WHERE key=(SELECT key FROM limits
          WHERE key LIKE 'rq%' OR key LIKE 'ci%' OR key LIKE 'adm:%' ORDER BY window_start LIMIT 1)`);
    }
    this.sql.exec("INSERT INTO limits(key, window_start, count) VALUES (?, ?, ?) ON CONFLICT(key) DO UPDATE SET window_start=excluded.window_start, count=excluded.count",
      key, now, by);
  }
  /** Counts a request when under `max`; over the limit nothing is written and the wait is returned. */
  private hit(key: string, window: number, max: number, now: number): number {
    if (this.count(key, window, now) >= max) {
      const r = this.one<{window_start: number}>("SELECT window_start FROM limits WHERE key=?", key)!;
      return Math.max(1, r.window_start + window - now);
    }
    this.bump(key, window, now);
    return 0;
  }
  private unbump(key: string): void {this.sql.exec("UPDATE limits SET count=MAX(0,count-1) WHERE key=?", key);}

  /** Charges one admitted claim to the daily Registry write budget; false once the budget is spent. */
  private chargeWrite(p: ClaimParams, now: number): boolean {
    const key = `global:day:${Math.floor(now / 86400)}`;
    if (this.count(key, 86400, now) >= p.writesPerDay) return false;
    this.bump(key, 86400, now);
    return true;
  }
  /** Anonymous-failure counter for the operator API; written only after a bounded in-memory gate. */
  adminFailure(client: string): {limited: boolean} {
    const now = sec();
    return {limited: this.hit(`adm:${client}`, 60, 10, now) > 0};
  }

  // Audit -----------------------------------------------------------------
  private audit(actor: string, action: string, target: string, result: string): void {
    const now = sec();
    this.sql.exec("INSERT INTO audit(ts, actor, action, target, result) VALUES (?, ?, ?, ?, ?)", now, actor, action, target, result);
    if (this.num("SELECT COUNT(*) AS n FROM audit") > AUDIT_ROWS)
      this.sql.exec("DELETE FROM audit WHERE id IN (SELECT id FROM audit ORDER BY id LIMIT ?)", this.num("SELECT COUNT(*) AS n FROM audit") - AUDIT_ROWS);
  }
  private adminAudit(action: string, target: string, result: string): void {
    this.sql.exec("INSERT INTO admin_audit(ts, action, target, result) VALUES (?, ?, ?, ?)", sec(), action, target, result);
    const n = this.num("SELECT COUNT(*) AS n FROM admin_audit");
    if (n > ADMIN_AUDIT_ROWS) this.sql.exec("DELETE FROM admin_audit WHERE id IN (SELECT id FROM admin_audit ORDER BY id LIMIT ?)", n - ADMIN_AUDIT_ROWS);
  }
  recordAudit(actor: string, action: string, target: string, result: string): void {this.audit(actor, action, target, result);}

  // Redeem ----------------------------------------------------------------
  async claim(p: ClaimParams): Promise<ClaimResult> {
    await this.ensureAlarm();
    return this.ctx.storage.transactionSync(() => this.claimNow(p, sec()));
  }
  private claimNow(p: ClaimParams, now: number): ClaimResult {
    const ban = this.one<{until: number | null}>("SELECT until FROM bans WHERE node=?", p.node);
    if (ban && (ban.until === null || ban.until > now)) return {k: "err", e: "banned"};
    // Requests that passed their signature, per client key.
    let wait = this.hit(`rq60:${p.client}`, 60, 10, now);
    if (!wait) wait = this.hit(`rq1h:${p.client}`, 3600, 40, now);
    if (wait) return {k: "err", e: "rate", retryAfter: wait};
    if (this.count(`ci60:${p.client}`, 60, now) >= 5 || this.count(`ci1h:${p.client}`, 3600, now) >= 30) {
      const w = this.one<{window_start: number}>("SELECT window_start FROM limits WHERE key=?", `ci60:${p.client}`);
      return {k: "err", e: "rate", retryAfter: w ? Math.max(1, w.window_start + 60 - now) : 60};
    }
    const invalid = (): ClaimResult => {
      this.bump(`ci60:${p.client}`, 60, now); this.bump(`ci1h:${p.client}`, 3600, now);
      return {k: "err", e: "code_invalid"};
    };
    const wrongState = (hash: string): void => {
      this.sql.exec("UPDATE codes SET attempts=attempts+1 WHERE hash=?", hash);
      this.sql.exec("DELETE FROM codes WHERE hash=? AND attempts>=5", hash);
    };
    let row = this.one<{hash: string; kind: string; alias: string | null; role: number; expires: number; state: string;
      claimed_by: string | null; claimed_at: number | null; op: string | null; subject: string | null; first_ts: number | null}>(
      "SELECT * FROM codes WHERE hash=?", p.hash);
    if (!row) {
      if (!p.open || p.signup !== "open" || p.op !== "create") return invalid();
      // Open signup: a synthetic code keyed by node and subject makes a lost reply recoverable.
      if (this.hit(`oc:${p.client}`, 86400, 3, now)) return {k: "err", e: "rate", retryAfter: 3600};
      if (this.hit("os:service", 3600, 20, now)) return {k: "err", e: "rate", retryAfter: 600};
      this.sql.exec("INSERT INTO codes(hash, kind, role, issuer, expires) VALUES (?, 'create', 1, 'open', ?)", p.hash, now + 3600);
      row = this.one("SELECT * FROM codes WHERE hash=?", p.hash)!;
    }
    const kindOk = p.op === "join" ? row.kind === "join" : row.kind === "create";
    if (!kindOk) {wrongState(row.hash); return {k: "err", e: "code_invalid"};}
    if (now > row.expires) {wrongState(row.hash); return {k: "err", e: "code_expired"};}
    let reclaim = false;
    if (row.state !== "new") {
      const same = row.claimed_by === p.node && row.op === p.op && row.subject === p.subject && p.ts > (row.first_ts ?? 0);
      if (!same) {wrongState(row.hash); return {k: "err", e: "code_used"};}
      if (row.state === "used") return {k: "replay", alias: row.alias!, role: row.role};
      if (now - (row.claimed_at ?? now) < CLAIM_RETRY) return {k: "err", e: "code_used"};
      reclaim = true;
    }
    if (p.op === "join") {
      const room = this.one<{state: string; pub_first: number}>("SELECT state, pub_first FROM rooms WHERE alias=?", row.alias);
      if (!room || room.state !== "active") return {k: "err", e: "code_invalid"};
      const member = this.num("SELECT COUNT(*) AS n FROM members WHERE alias=? AND node=?", row.alias, p.node);
      if (!member && this.num("SELECT COUNT(*) AS n FROM members WHERE alias=?", row.alias) >= p.maxFrontends) return {k: "err", e: "cap"};
      if (p.avoid.includes(room.pub_first)) return {k: "err", e: "collision"};
      if (!this.chargeWrite(p, now)) return {k: "err", e: "closed"};
      this.sql.exec("UPDATE codes SET state='claimed', claimed_by=?, claimed_at=?, op=?, subject=?, first_ts=COALESCE(first_ts, ?) WHERE hash=?",
        p.node, now, p.op, p.subject, p.ts, row.hash);
      return {k: "claim", alias: row.alias!, role: row.role, reclaim};
    }
    const alias = reclaim && row.alias ? row.alias : p.alias;
    const existing = this.one<{alias: string; state: string; owner_node: string}>("SELECT alias, state, owner_node FROM rooms WHERE pub_hash=?", p.pubHash!);
    if (existing && existing.alias !== alias) {
      if (p.op === "convert" && existing.owner_node === p.node && existing.state === "active") return {k: "recover", alias: existing.alias};
      return {k: "err", e: "exists"};
    }
    if (!reclaim) {
      if (this.count(`cc:${p.node}`, 86400, now) >= 2) return {k: "err", e: "cap"};
      if (this.num("SELECT COUNT(*) AS n FROM rooms WHERE owner_node=? AND alias<>?", p.node, alias) >= 4) return {k: "err", e: "cap"};
    }
    if (this.num("SELECT COUNT(*) AS n FROM rooms WHERE alias<>?", alias) >= p.maxRooms) return {k: "err", e: "cap"};
    if (!this.chargeWrite(p, now)) return {k: "err", e: "closed"};
    if (!reclaim) this.bump(`cc:${p.node}`, 86400, now);
    this.sql.exec(`INSERT INTO rooms(alias, state, owner_node, created, last_active, pub_hash, name, pub_first, state_since)
      VALUES (?, 'pending', ?, ?, ?, ?, ?, ?, ?)
      ON CONFLICT(alias) DO UPDATE SET pub_hash=excluded.pub_hash, name=excluded.name, pub_first=excluded.pub_first`,
    alias, p.node, now, now, p.pubHash!, p.name ?? null, p.pubFirst ?? null, now);
    this.sql.exec("UPDATE codes SET state='claimed', claimed_by=?, claimed_at=?, op=?, subject=?, first_ts=COALESCE(first_ts, ?), alias=? WHERE hash=?",
      p.node, now, p.op, p.subject, p.ts, alias, row.hash);
    return {k: "claim", alias, role: 1, reclaim};
  }

  /** Second transaction: the Room Durable Object accepted the node. */
  finish(p: {hash: string; node: string; alias: string; role: number; pubHash?: string; pubFirst?: number; name?: string}): void {
    const now = sec();
    this.ctx.storage.transactionSync(() => {
      this.sql.exec("UPDATE codes SET state='used', used_at=? WHERE hash=?", now, p.hash);
      const room = this.one<{state: string}>("SELECT state FROM rooms WHERE alias=?", p.alias);
      if (room?.state === "pending") {
        this.sql.exec("UPDATE rooms SET state='active', state_since=?, last_active=?, pub_hash=COALESCE(?, pub_hash), pub_first=COALESCE(?, pub_first), name=COALESCE(?, name) WHERE alias=?",
          now, now, p.pubHash ?? null, p.pubFirst ?? null, p.name ?? null, p.alias);
      }
      this.sql.exec("INSERT OR IGNORE INTO members(node, alias) VALUES (?, ?)", p.node, p.alias);
      this.audit(p.node.slice(0, 8), p.role ? "redeem-owner" : "redeem", p.alias, "ok");
    });
  }
  /** The Room Durable Object call failed: release the claim and its pending room. */
  release(p: {hash: string; node: string; alias: string}): void {
    this.ctx.storage.transactionSync(() => {
      this.sql.exec("UPDATE codes SET state='new', claimed_by=NULL, claimed_at=NULL, op=NULL, subject=NULL, first_ts=NULL WHERE hash=? AND state='claimed' AND claimed_by=?",
        p.hash, p.node);
      this.dropPending(p.alias);
    });
  }
  private dropPending(alias: string): void {
    const room = this.one<{owner_node: string; created: number}>("SELECT owner_node, created FROM rooms WHERE alias=? AND state='pending'", alias);
    if (!room) return;
    this.sql.exec("DELETE FROM rooms WHERE alias=?", alias);
    this.unbump(`cc:${room.owner_node}`);
  }

  // Codes -----------------------------------------------------------------
  async mintCreate(hashes: string[], expires: number): Promise<void> {
    await this.ensureAlarm();
    this.ctx.storage.transactionSync(() => {
      for (const hash of hashes)
        this.sql.exec("INSERT INTO codes(hash, kind, role, issuer, expires) VALUES (?, 'create', 1, 'admin', ?)", hash, expires);
      this.adminAudit("mint", String(hashes.length), "ok");
    });
  }
  /** Invite code for a room. Returns an error name or "ok". */
  async mintJoin(p: {alias: string; hash: string; role: number; expires: number; issuer: string}): Promise<string> {
    const now = sec();
    await this.ensureAlarm();
    return this.ctx.storage.transactionSync(() => {
      const room = this.one<{state: string}>("SELECT state FROM rooms WHERE alias=?", p.alias);
      if (!room || room.state !== "active") return "not_found";
      this.sql.exec("DELETE FROM codes WHERE kind='join' AND state='new' AND expires<?", now);
      if (this.num("SELECT COUNT(*) AS n FROM codes WHERE kind='join' AND alias=? AND state='new'", p.alias) >= 4) return "cap";
      if (p.issuer !== "admin" && this.num("SELECT COUNT(*) AS n FROM codes WHERE kind='join' AND issuer=? AND state='new'", p.issuer) >= 8) return "cap";
      if (this.hit(`mj:${p.alias}`, 86400, 20, now)) return "cap";
      this.sql.exec("INSERT INTO codes(hash, kind, alias, role, issuer, expires) VALUES (?, 'join', ?, ?, ?, ?)", p.hash, p.alias, p.role, p.issuer, p.expires);
      this.audit(p.issuer === "admin" ? "admin" : p.issuer, p.role ? "invite-owner" : "invite", p.alias, "ok");
      return "ok";
    });
  }

  // Rooms and members -----------------------------------------------------
  setRoomState(alias: string, state: string, dirty = false): boolean {
    const now = sec();
    const r = this.sql.exec("UPDATE rooms SET state=?, state_since=?, dirty=? WHERE alias=?", state, now, dirty ? 1 : 0, alias);
    return r.rowsWritten > 0;
  }
  touchRoom(alias: string): void {this.sql.exec("UPDATE rooms SET last_active=? WHERE alias=?", sec(), alias);}
  memberRemove(node: string, alias: string): void {this.sql.exec("DELETE FROM members WHERE node=? AND alias=?", node, alias);}
  /** Removes the room, its members and its codes, and frees the pub_hash. */
  purgeRoom(alias: string, actor = "system", reason = "purge"): void {
    this.ctx.storage.transactionSync(() => {
      this.dropPending(alias);
      this.sql.exec("DELETE FROM rooms WHERE alias=?", alias);
      this.sql.exec("DELETE FROM members WHERE alias=?", alias);
      this.sql.exec("DELETE FROM codes WHERE alias=?", alias);
      this.audit(actor, reason, alias, "ok");
    });
  }
  roomInfo(alias: string): RoomRow | undefined {
    return this.one<RoomRow>("SELECT alias, name, state, created, last_active, owner_node FROM rooms WHERE alias=?", alias);
  }

  // Operator API ----------------------------------------------------------
  adminRooms(offset: number): {rooms: AdminRoom[]; next: number | null} {
    const rows = this.rows<RoomRow>("SELECT alias, name, state, created, last_active, owner_node FROM rooms ORDER BY created, alias LIMIT 21 OFFSET ?", offset);
    return {rooms: rows.slice(0, 20).map(r => ({alias: r.alias, name: r.name, state: r.state, created: r.created,
      last_active: r.last_active, owner: r.owner_node.slice(0, 8)})), next: rows.length > 20 ? offset + 20 : null};
  }
  adminCodes(offset: number): {codes: AdminCode[]; next: number | null} {
    const rows = this.rows<{hash: string; kind: string; alias: string | null; issuer: string; expires: number; state: string}>(
      "SELECT hash, kind, alias, issuer, expires, state FROM codes ORDER BY expires, hash LIMIT 21 OFFSET ?", offset);
    return {codes: rows.slice(0, 20).map(r => ({h: r.hash.slice(0, 8), kind: r.kind, alias: r.alias, issuer: r.issuer,
      expires: r.expires, state: r.state})), next: rows.length > 20 ? offset + 20 : null};
  }
  adminRevokeCode(prefix: string): "ok" | "not_found" | "ambiguous" {
    return this.ctx.storage.transactionSync(() => {
      const rows = this.rows<{hash: string}>("SELECT hash FROM codes WHERE hash>=? AND hash<? LIMIT 2", prefix, prefix + "g");
      if (rows.length !== 1) return rows.length ? "ambiguous" : "not_found";
      this.sql.exec("DELETE FROM codes WHERE hash=?", rows[0].hash);
      this.adminAudit("code-revoke", prefix.slice(0, 8), "ok");
      return "ok";
    });
  }
  adminAuditRows(offset: number): {rows: AuditRow[]; next: number | null} {
    const rows = this.rows<AuditRow>("SELECT id, ts, actor, action, target, result FROM audit ORDER BY id DESC LIMIT 51 OFFSET ?", offset);
    return {rows: rows.slice(0, 50), next: rows.length > 50 ? offset + 50 : null};
  }
  adminNote(action: string, target: string, result: string): void {this.adminAudit(action, target, result);}
  /** Records the ban and lists the rooms whose tokens must be revoked. */
  ban(node: string, until: number | null): string[] {
    return this.ctx.storage.transactionSync(() => {
      this.sql.exec("INSERT INTO bans(node, until) VALUES (?, ?) ON CONFLICT(node) DO UPDATE SET until=excluded.until", node, until);
      this.adminAudit("ban", node.slice(0, 8), "ok");
      return this.rows<{alias: string}>("SELECT alias FROM members WHERE node=?", node).map(r => r.alias);
    });
  }
  unban(node: string): void {
    this.sql.exec("DELETE FROM bans WHERE node=?", node);
    this.adminAudit("unban", node.slice(0, 8), "ok");
  }
  adminSetDirty(alias: string): void {this.sql.exec("UPDATE rooms SET dirty=1 WHERE alias=?", alias);}

  // Housekeeping ----------------------------------------------------------
  async alarm(): Promise<void> {
    const now = sec();
    // Stale claims: release when the room never became active.
    for (const c of this.rows<{hash: string; alias: string | null; claimed_by: string | null}>(
        "SELECT hash, alias, claimed_by FROM codes WHERE state='claimed' AND claimed_at<?", now - CLAIM_STALE)) {
      const room = c.alias ? this.one<{state: string}>("SELECT state FROM rooms WHERE alias=?", c.alias) : undefined;
      if (room?.state === "active") {this.sql.exec("UPDATE codes SET state='used', used_at=? WHERE hash=?", now, c.hash); continue;}
      this.ctx.storage.transactionSync(() => {
        this.sql.exec("UPDATE codes SET state='new', claimed_by=NULL, claimed_at=NULL, op=NULL, subject=NULL, first_ts=NULL WHERE hash=?", c.hash);
        if (c.alias) this.dropPending(c.alias);
      });
      if (c.alias) try {await roomOps.purge(this.env, c.alias);} catch {/* the Room object keeps no data until provisioned */}
    }
    this.sql.exec("DELETE FROM codes WHERE expires<?", now - 3600);
    this.sql.exec("DELETE FROM audit WHERE ts<?", now - AUDIT_DAYS * 86400);
    this.sql.exec("DELETE FROM limits WHERE window_start<?", now - LIMITS_STALE);
    this.sql.exec("DELETE FROM bans WHERE until IS NOT NULL AND until<?", now);
    // Re-drive rooms where the Registry and the Room object may disagree.
    for (const r of this.rows<{alias: string; state: string}>("SELECT alias, state FROM rooms WHERE dirty=1")) {
      try {
        await roomOps.setState(this.env, r.alias, r.state === "disabled" ? "disabled" : "active");
        this.sql.exec("UPDATE rooms SET dirty=0 WHERE alias=?", r.alias);
      } catch {/* retried next hour */}
    }
    // Disabled rooms keep their data for 30 days.
    for (const r of this.rows<{alias: string}>("SELECT alias FROM rooms WHERE state='disabled' AND state_since<?", now - 30 * 86400)) {
      try {await roomOps.purge(this.env, r.alias); this.purgeRoom(r.alias, "system", "purge-disabled");} catch {/* retried next hour */}
    }
    // Banned nodes lose any token they still hold.
    for (const b of this.rows<{node: string}>("SELECT node FROM bans WHERE until IS NULL OR until>=?", now)) {
      for (const m of this.rows<{alias: string}>("SELECT alias FROM members WHERE node=?", b.node)) {
        try {await roomOps.revokeNode(this.env, m.alias, b.node); this.memberRemove(b.node, m.alias);} catch {/* retried next hour */}
      }
    }
    await this.ctx.storage.setAlarm(Date.now() + 3600_000);
  }
}
