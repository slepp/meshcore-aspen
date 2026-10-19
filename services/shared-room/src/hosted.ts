// Hosted-room tables and shared types for the Room Durable Object. The tables
// exist only after `provision`; a Room object that was never provisioned never
// writes, and static rooms never get these tables.
export const HOSTED_DDL = `
  CREATE TABLE IF NOT EXISTS hosted_room(id INTEGER PRIMARY KEY CHECK(id=1), alias TEXT, name TEXT, public_key TEXT,
    sealed BLOB, kid TEXT, state TEXT, created INTEGER, last_seen INTEGER, pending_delete INTEGER);
  CREATE TABLE IF NOT EXISTS hosted_frontend(node TEXT PRIMARY KEY, token_hash TEXT UNIQUE, role INTEGER,
    created INTEGER, last_seen INTEGER, revoked INTEGER DEFAULT 0, owner_since INTEGER);
  CREATE TABLE IF NOT EXISTS hosted_meta(k TEXT PRIMARY KEY, v INTEGER);
`;
export interface HostedRow {
  alias: string; name: string; public_key: string; sealed: ArrayBuffer | null; kid: string;
  state: "active" | "disabled" | "deleted"; created: number; last_seen: number; pending_delete: number | null;
}
export interface HostedFrontendRow {
  node: string; token_hash: string; role: number; created: number; last_seen: number | null;
  revoked: number; owner_since: number | null;
}
/** Memory copy of one hosted room, opened once per activation. */
export interface HostedMaterial {key: Uint8Array; password: string}
export interface HostedGrant {alias: string; name: string; pub: string; token: string; role: number}
export interface GrantReply {ok: boolean; e?: string; grant?: HostedGrant}
export const HOSTED_ALIAS = /^r[a-z2-7]{29}$/;
export const OWNER_DELAY = 86400;
