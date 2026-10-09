import {DurableObject} from "cloudflare:workers";
import {generateAuthenticationOptions, generateRegistrationOptions, verifyAuthenticationResponse,
  verifyRegistrationResponse, type AuthenticationResponseJSON, type RegistrationResponseJSON,
  type WebAuthnCredential} from "@simplewebauthn/server";
import {aliases, credential, errorResponse, fail, type Env} from "./config";
import {username, webUsers} from "./accounts";
import {fromHex, toHex} from "./native-crypto";
import {sameOrigin, webBody, webResponse, WEB_SESSION_SECONDS} from "./web";

export const ACCOUNT_DEVICE_PROTOCOL = "aspen-account.device.v1";
const COOKIE = "aspen_account";
const LINK_SECONDS = 300;
interface Challenge {
  id: string; purpose: string; origin: string; publicKey: string; username?: string;
  grant?: string; challenge?: string; enrollment?: string; expires: number;
}
interface Device {username: string; publicKey: string; revoked: boolean; added: number}
interface AccountSession {username: string; publicKey: string; grant: string; expires: number}
interface Passkey {id: string; username: string; key: string; counter: number; transports?: WebAuthnCredential["transports"]}
interface Link {
  code: string; claim: string; publicKey: string; label: string; expires: number;
  username?: string; grant?: string; approver?: string;
}
interface Ticket extends AccountSession {alias: string}
interface Enrollment {username: string; grant: string; expires: number}

export function passkeyMode(env: Env): boolean {
  if (env.WEB_AUTH_MODE !== undefined && env.WEB_AUTH_MODE !== "passkey")
    fail(503, "Invalid WEB_AUTH_MODE; use passkey or leave it unset");
  return env.WEB_AUTH_MODE === "passkey";
}
function settings(env: Env): {origin: string; rpID: string} {
  if (!env.PASSKEY_ORIGIN || !env.PASSKEY_RP_ID) fail(503, "Configure PASSKEY_ORIGIN and PASSKEY_RP_ID");
  const origin = new URL(env.PASSKEY_ORIGIN), rpID = env.PASSKEY_RP_ID;
  if (origin.origin !== env.PASSKEY_ORIGIN ||
      (origin.protocol !== "https:" && !["localhost", "127.0.0.1"].includes(origin.hostname)) ||
      (origin.hostname !== rpID && !origin.hostname.endsWith("." + rpID)) ||
      !/^[a-z0-9.-]+$/.test(rpID))
    fail(503, "PASSKEY_ORIGIN must be an HTTPS origin within PASSKEY_RP_ID");
  return {origin: origin.origin, rpID};
}
function random(): string {return toHex(crypto.getRandomValues(new Uint8Array(32)));}
function key(value: unknown, label = "device public key"): string {
  if (typeof value !== "string" || !/^[a-f0-9]{64}$/.test(value)) fail(400, `Invalid ${label}`);
  return value;
}
function code(value: unknown): string {
  if (typeof value !== "string" || !/^[A-F0-9]{24}$/.test(value)) fail(400, "Use the 24-character device linking code");
  return value;
}
function cookie(token: string, seconds = WEB_SESSION_SECONDS): string {
  return `${COOKIE}=${token}; Path=/v1/auth/; Max-Age=${seconds}; HttpOnly; Secure; SameSite=Strict`;
}
function deviceMessage(c: Challenge): string {
  return [ACCOUNT_DEVICE_PROTOCOL, c.origin, c.purpose, c.publicKey, c.id].join("\n");
}
async function ownership(c: Challenge, signature: unknown): Promise<void> {
  if (typeof signature !== "string" || !/^[a-f0-9]{128}$/.test(signature))
    fail(400, "Invalid device ownership signature");
  const publicKey = await crypto.subtle.importKey("raw", fromHex(c.publicKey), "Ed25519", false, ["verify"]);
  if (!await crypto.subtle.verify("Ed25519", publicKey, fromHex(signature),
    new TextEncoder().encode(deviceMessage(c)))) fail(403, "Device ownership check failed; start again");
}
function object(value: unknown): value is Record<string, unknown> {
  return !!value && typeof value === "object" && !Array.isArray(value);
}
function passkeyResponse(value: unknown): value is RegistrationResponseJSON | AuthenticationResponseJSON {
  return object(value) && typeof value.id === "string" && value.id.length <= 1024 &&
    typeof value.rawId === "string" && value.type === "public-key" && object(value.clientExtensionResults) &&
    (value.authenticatorAttachment === undefined || value.authenticatorAttachment === "platform" ||
      value.authenticatorAttachment === "cross-platform") &&
    object(value.response) && typeof value.response.clientDataJSON === "string";
}
function registrationResponse(value: unknown): value is RegistrationResponseJSON {
  return passkeyResponse(value) && "attestationObject" in value.response &&
    typeof value.response.attestationObject === "string";
}
function authenticationResponse(value: unknown): value is AuthenticationResponseJSON {
  return passkeyResponse(value) && "authenticatorData" in value.response &&
    typeof value.response.authenticatorData === "string" && "signature" in value.response &&
    typeof value.response.signature === "string" &&
    (!("userHandle" in value.response) || value.response.userHandle === undefined ||
      typeof value.response.userHandle === "string");
}

export class Accounts extends DurableObject<Env> {
  private sql: SqlStorage;
  constructor(ctx: DurableObjectState, env: Env) {
    super(ctx, env);
    this.sql = ctx.storage.sql;
    this.sql.exec(`CREATE TABLE IF NOT EXISTS account_records (
      kind TEXT NOT NULL, id TEXT NOT NULL, data TEXT NOT NULL, expires INTEGER,
      PRIMARY KEY(kind,id));
      CREATE INDEX IF NOT EXISTS account_expiry ON account_records(expires);
      CREATE INDEX IF NOT EXISTS account_session_device ON account_records(json_extract(data,'$.publicKey')) WHERE kind='session';
      CREATE TABLE IF NOT EXISTS account_limits (address TEXT PRIMARY KEY, start INTEGER, attempts INTEGER);`);
  }
  private get<T>(kind: string, id: string): T | undefined {
    const row = this.sql.exec<{data: string}>("SELECT data FROM account_records WHERE kind=? AND id=?", kind, id).toArray()[0];
    return row ? JSON.parse(row.data) as T : undefined;
  }
  private put(kind: string, id: string, data: unknown, expires: number | null = null): void {
    this.sql.exec("INSERT OR REPLACE INTO account_records VALUES (?, ?, ?, ?)", kind, id, JSON.stringify(data), expires);
  }
  private remove(kind: string, id: string): void {
    this.sql.exec("DELETE FROM account_records WHERE kind=? AND id=?", kind, id);
  }
  private all<T>(kind: string): T[] {
    return this.sql.exec<{data: string}>("SELECT data FROM account_records WHERE kind=?", kind).toArray()
      .map(row => JSON.parse(row.data) as T);
  }
  private count(kind: string): number {
    return this.sql.exec<{n: number}>("SELECT COUNT(*) AS n FROM account_records WHERE kind=?", kind).one().n;
  }
  private account(user: string) {
    const account = webUsers(this.env)?.[user];
    if (!account) fail(403, "Account is unavailable; contact the room operator");
    return account;
  }
  private enrollments(): Record<string, Enrollment> {
    let value: unknown;
    try {value = JSON.parse(this.env.WEB_ENROLLMENTS ?? "{}");}
    catch {return fail(503, "Invalid WEB_ENROLLMENTS configuration");}
    if (!object(value)) fail(503, "Invalid WEB_ENROLLMENTS configuration");
    const result: Record<string, Enrollment> = Object.create(null);
    for (const [id, entry] of Object.entries(value)) {
      if (!/^[a-f0-9]{64}$/.test(id) || !object(entry) ||
          Object.keys(entry).sort().join(",") !== "expires,grant,username" ||
          typeof entry.username !== "string" || !/^[a-z0-9][a-z0-9_-]{2,63}$/.test(entry.username) ||
          typeof entry.grant !== "string" || !/^[a-f0-9]{64}$/.test(entry.grant) ||
          typeof entry.expires !== "number" || !Number.isSafeInteger(entry.expires) || entry.expires <= 0)
        fail(503, "Invalid WEB_ENROLLMENTS configuration");
      result[id] = {username: entry.username, grant: entry.grant, expires: entry.expires};
    }
    return result;
  }
  private async grant(user: string): Promise<string> {return credential(JSON.stringify([user, this.account(user)]));}
  private async current(session: AccountSession): Promise<boolean> {
    const account = webUsers(this.env)?.[session.username], device = this.get<Device>("device", session.publicKey);
    return session.expires > Math.floor(Date.now() / 1000) && !!account &&
      session.grant === await credential(JSON.stringify([session.username, account])) &&
      !!device && device.username === session.username && !device.revoked;
  }
  private async session(request: Request): Promise<AccountSession> {
    const cookies = (request.headers.get("Cookie") ?? "").split(";").map(v => v.trim())
      .filter(v => v.startsWith(COOKIE + "="));
    const token = cookies[0]?.slice(COOKIE.length + 1);
    if (cookies.length !== 1 || !token || !/^[a-f0-9]{64}$/.test(token)) fail(401, "Sign in with a passkey or approve this device");
    const session = this.get<AccountSession>("session", await credential(token));
    if (!session || !await this.current(session)) fail(401, "Account session expired or device access changed; sign in again");
    return session;
  }
  private async issue(user: string, publicKey: string): Promise<Response> {
    const account = this.account(user), now = Math.floor(Date.now() / 1000), token = random();
    const session: AccountSession = {username: user, publicKey, grant: await this.grant(user), expires: now + WEB_SESSION_SECONDS};
    if (!await this.current(session)) fail(403, "Device is not approved for this account");
    this.sql.exec("DELETE FROM account_records WHERE kind='session' AND json_extract(data,'$.publicKey')=?", publicKey);
    this.put("session", await credential(token), session, session.expires);
    await this.ctx.storage.sync();
    return webResponse({username: user, publicKey, name: account.name, expires: session.expires}, 200, cookie(token));
  }
  private async bind(user: string, publicKey: string, commit?: () => void): Promise<void> {
    // Existing room device keys remain assigned to their original account across the cutover.
    for (const backend of new Set(Object.values(aliases(this.env)).map(a => a.backend))) {
      const owner = await this.env.ROOMS.getByName(backend).deviceOwner(publicKey);
      if (owner && owner !== user) fail(409, "This device key belongs to another account; use a separate client profile");
    }
    this.ctx.storage.transactionSync(() => {
      const device = this.get<Device>("device", publicKey);
      if (device && device.username !== user) fail(409, "This device key belongs to another account; use a separate client profile");
      if (device?.revoked) fail(403, "This device key was revoked; use a new device profile");
      if (!device && this.all<Device>("device").filter(d => d.username === user).length >= 64)
        fail(409, "Account device limit reached; contact the room operator");
      if (!device) this.put("device", publicKey, {username: user, publicKey, revoked: false, added: Math.floor(Date.now() / 1000)});
      commit?.();
    });
  }
  private take(body: Record<string, unknown>, purpose: string, origin: string): Challenge {
    const id = key(body.id, "login challenge");
    const challenge = this.ctx.storage.transactionSync(() => {
      const found = this.get<Challenge>("challenge", id);
      this.remove("challenge", id);
      return found;
    });
    if (!challenge || challenge.expires <= Math.floor(Date.now() / 1000) ||
        challenge.purpose !== purpose || challenge.origin !== origin)
      fail(403, "Login challenge expired or already used; start again");
    return challenge;
  }
  private async checkGrant(c: Challenge): Promise<void> {
    if (!c.username || c.grant !== await this.grant(c.username)) fail(403, "Account access changed; start again");
  }
  private approved(link: Link): boolean {
    const approver = link.approver ? this.get<Device>("device", link.approver) : undefined;
    return !!approver && approver.username === link.username && !approver.revoked;
  }
  async deviceAllowed(user: string, publicKey: string): Promise<boolean> {
    const device = this.get<Device>("device", publicKey);
    return !device || device.username === user && !device.revoked;
  }
  async consumeTicket(token: string, alias: string): Promise<{username: string; name: string; publicKey: string} | null> {
    if (!/^[a-f0-9]{64}$/.test(token)) return null;
    const id = await credential(token);
    const ticket = this.ctx.storage.transactionSync(() => {
      const saved = this.get<Ticket>("ticket", id);
      this.remove("ticket", id);
      return saved;
    });
    await this.ctx.storage.sync();
    if (!ticket || ticket.alias !== alias || !await this.current(ticket) ||
        !this.account(ticket.username).aliases.includes(alias)) return null;
    return {username: ticket.username, name: this.account(ticket.username).name, publicKey: ticket.publicKey};
  }
  async fetch(request: Request): Promise<Response> {
    try {return await this.handle(request);} catch (error) {return errorResponse(error);}
  }
  private async handle(request: Request): Promise<Response> {
    const url = new URL(request.url), configured = settings(this.env), now = Math.floor(Date.now() / 1000);
    if (url.origin !== configured.origin) fail(403, "Use the configured passkey website: " + configured.origin);
    const endpoint = url.pathname.slice("/v1/auth/".length);
    if (request.method === "GET" && endpoint === "session") {
      const session = await this.session(request);
      return webResponse({...session, grant: undefined, name: this.account(session.username).name});
    }
    if (request.method !== "POST") fail(405, "POST required");
    sameOrigin(request);
    const body = await webBody(request, 16384);
    const address = await credential(request.headers.get("CF-Connecting-IP") ?? "local");
    this.ctx.storage.transactionSync(() => {
      this.sql.exec("DELETE FROM account_limits WHERE start<=?", now - 60);
      this.sql.exec("DELETE FROM account_records WHERE expires<=?", now);
      this.sql.exec(`INSERT INTO account_limits VALUES (?, ?, 1)
        ON CONFLICT(address) DO UPDATE SET attempts=attempts+1`, address, now);
      if (this.sql.exec<{attempts: number}>("SELECT attempts FROM account_limits WHERE address=?", address).one().attempts > 60)
        fail(429, "Too many account requests; wait one minute");
    });
    if (["device-challenge", "register/options", "authenticate/options"].includes(endpoint)) {
      const publicKey = key(body.publicKey), id = random();
      const purpose = endpoint === "device-challenge" ? body.purpose : endpoint.split("/")[0];
      if (endpoint === "device-challenge" && purpose !== "device-login" && purpose !== "link")
        fail(400, "Invalid device login purpose");
      if (typeof purpose !== "string") fail(400, "Invalid device login purpose");
      const challenge: Challenge = {id, purpose, publicKey, origin: url.origin, expires: now + 120};
      let options;
      if (purpose === "register" || purpose === "authenticate") {
        const user = username(body.username);
        challenge.username = user; challenge.grant = await this.grant(user);
        const existing = this.all<Passkey>("passkey").filter(p => p.username === user);
        if (purpose === "register") {
          let authorized = false;
          if (typeof body.enrollment === "string" && /^[a-f0-9]{64}$/.test(body.enrollment)) {
            const hash = await credential(body.enrollment);
            const entries = this.enrollments();
            const entry = entries[hash];
            if (entry?.username === user && entry.grant === challenge.grant && entry.expires > now &&
                !this.get("enrollment", hash) && !existing.length) {
              challenge.enrollment = hash; authorized = true;
            }
          }
          if (!authorized) {
            const session = await this.session(request);
            if (session.username !== user || session.publicKey !== publicKey) fail(403, "Register a passkey from your own signed-in device");
          }
          if (existing.length >= 20) fail(409, "Account passkey limit reached");
          options = await generateRegistrationOptions({rpName: "Aspen Rooms", rpID: configured.rpID,
            userID: new Uint8Array(new TextEncoder().encode(user)), userName: user, userDisplayName: this.account(user).name,
            attestationType: "none", excludeCredentials: existing.map(p => ({id: p.id, transports: p.transports})),
            authenticatorSelection: {residentKey: "required", userVerification: "required"},
            supportedAlgorithmIDs: [-7, -257], timeout: 120000});
        } else {
          if (!existing.length) fail(403, "No passkey is registered for this account; use the operator enrollment link");
          options = await generateAuthenticationOptions({rpID: configured.rpID,
            allowCredentials: existing.map(p => ({id: p.id, transports: p.transports})),
            userVerification: "required", timeout: 120000});
        }
        challenge.challenge = options.challenge;
      }
      if (this.count("challenge") >= 4096) fail(429, "Account login queue is full; wait two minutes");
      this.put("challenge", id, challenge, challenge.expires);
      await this.ctx.storage.sync();
      return webResponse({id, purpose, publicKey, origin: url.origin, message: deviceMessage(challenge),
        protocol: ACCOUNT_DEVICE_PROTOCOL, expires: challenge.expires, options});
    }
    if (endpoint === "register/verify" || endpoint === "authenticate/verify") {
      const registration = endpoint.startsWith("register");
      const challenge = this.take(body, registration ? "register" : "authenticate", url.origin);
      await this.ctx.storage.sync();
      await ownership(challenge, body.signature);
      await this.checkGrant(challenge);
      const user = challenge.username, expectedChallenge = challenge.challenge;
      if (!user || !expectedChallenge) fail(403, "Passkey challenge is incomplete; start again");
      if (registration) {
        if (!registrationResponse(body.response)) fail(400, "Invalid passkey registration response");
        let verification;
        try {verification = await verifyRegistrationResponse({response: body.response,
          expectedChallenge, expectedOrigin: configured.origin,
          expectedRPID: configured.rpID, requireUserVerification: true});}
        catch {return fail(403, "Passkey registration could not be verified; start again");}
        if (!verification.verified || !verification.registrationInfo) fail(403, "Passkey registration was not verified");
        const created = verification.registrationInfo.credential;
        await this.bind(user, challenge.publicKey, () => {
          if (challenge.enrollment) {
            const entries = this.enrollments();
            const entry = entries[challenge.enrollment];
            if (!entry || entry.username !== challenge.username || entry.grant !== challenge.grant || entry.expires <= Math.floor(Date.now() / 1000) ||
                this.get("enrollment", challenge.enrollment) ||
                this.all<Passkey>("passkey").some(p => p.username === challenge.username))
              fail(403, "Enrollment link expired or has already been used");
          }
          if (this.get("passkey", created.id)) fail(409, "Passkey is already registered");
          if (this.all<Passkey>("passkey").filter(p => p.username === challenge.username).length >= 20)
            fail(409, "Account passkey limit reached");
          this.put("passkey", created.id, {id: created.id, username: challenge.username,
            key: toHex(created.publicKey), counter: created.counter, transports: created.transports});
          if (challenge.enrollment) this.put("enrollment", challenge.enrollment, {used: now});
        });
      } else {
        if (!authenticationResponse(body.response)) fail(400, "Invalid passkey sign-in response");
        const saved = this.get<Passkey>("passkey", body.response.id);
        if (!saved || saved.username !== challenge.username) fail(403, "Passkey does not belong to this account");
        let verification;
        try {verification = await verifyAuthenticationResponse({response: body.response,
          expectedChallenge, expectedOrigin: configured.origin,
          expectedRPID: configured.rpID, requireUserVerification: true,
          credential: {id: saved.id, publicKey: new Uint8Array(fromHex(saved.key)), counter: saved.counter, transports: saved.transports}});}
        catch {return fail(403, "Passkey sign-in could not be verified; start again");}
        if (!verification.verified) fail(403, "Passkey sign-in was not verified");
        await this.bind(user, challenge.publicKey, () => {
          const current = this.get<Passkey>("passkey", saved.id);
          if (!current || current.counter !== saved.counter) fail(403, "Passkey changed during sign-in; start again");
          this.put("passkey", saved.id, {...saved, counter: verification.authenticationInfo.newCounter});
        });
      }
      return this.issue(user, challenge.publicKey);
    }
    if (endpoint === "device-login" || endpoint === "link/start") {
      const challenge = this.take(body, endpoint === "device-login" ? "device-login" : "link", url.origin);
      await this.ctx.storage.sync();
      await ownership(challenge, body.signature);
      if (endpoint === "device-login") {
        const device = this.get<Device>("device", challenge.publicKey);
        if (!device || device.revoked) fail(401, "Approve this device or sign in with a passkey");
        return this.issue(device.username, device.publicKey);
      }
      const label = typeof body.label === "string" ? body.label.trim() : "";
      if (!label || new TextEncoder().encode(label).length > 64 || /[\u0000-\u001f\u007f-\u009f]/.test(label))
        fail(400, "Use a device label of 1..64 UTF-8 bytes without control characters");
      if (this.count("link") >= 4096) fail(429, "Device linking queue is full; wait five minutes");
      const linkCode = random().slice(0, 24).toUpperCase(), claim = random();
      const link: Link = {code: linkCode, claim: await credential(claim), publicKey: challenge.publicKey, label, expires: now + LINK_SECONDS};
      this.put("link", linkCode, link, link.expires);
      await this.ctx.storage.sync();
      return webResponse({code: linkCode, claim, publicKey: link.publicKey, expires: link.expires,
        url: configured.origin + "/#link=" + linkCode});
    }
    if (endpoint === "link/status" || endpoint === "link/claim") {
      const linkCode = code(body.code), claim = key(body.claim, "device linking claim");
      const link = this.get<Link>("link", linkCode);
      if (!link || link.expires <= now || link.claim !== await credential(claim))
        fail(403, "Device link expired, already claimed or invalid; start again");
      if (endpoint === "link/status") return webResponse({approved: !!link.username, publicKey: link.publicKey, expires: link.expires});
      if (!link.username || !this.approved(link) || link.grant !== await this.grant(link.username))
        fail(403, "Device link is not approved or account/device access changed");
      this.ctx.storage.transactionSync(() => {
        if (!this.get("link", linkCode)) fail(403, "Device link has already been claimed");
        this.remove("link", linkCode);
      });
      await this.bind(link.username, link.publicKey, () => {
        if (link.expires <= Math.floor(Date.now() / 1000)) fail(403, "Device link expired; start a new five-minute request");
        if (!this.approved(link)) fail(403, "The approving device was revoked; start again from another signed-in device");
      });
      return this.issue(link.username, link.publicKey);
    }
    const session = await this.session(request);
    if (endpoint === "room-ticket") {
      const alias = typeof body.alias === "string" ? body.alias : "";
      if (!Object.hasOwn(aliases(this.env), alias) || !this.account(session.username).aliases.includes(alias))
        fail(403, "Account does not have access to this room");
      if (body.publicKey !== session.publicKey) fail(403, "Room login belongs to a different device key");
      const token = random();
      const expires = Math.min(session.expires, now + 60);
      this.put("ticket", await credential(token), {...session, alias, expires}, expires);
      await this.ctx.storage.sync();
      return webResponse({ticket: token, publicKey: session.publicKey, alias, expires});
    }
    if (endpoint === "link/inspect" || endpoint === "link/approve") {
      const linkCode = code(body.code), link = this.get<Link>("link", linkCode);
      if (!link || link.expires <= now) fail(403, "Device linking code expired; start again on the new device");
      if (link.publicKey === session.publicKey) fail(409, "This is already your device; link a different device key");
      if (link.username) fail(409, "Device link has already been approved");
      const owner = this.get<Device>("device", link.publicKey);
      if (owner && (owner.username !== session.username || owner.revoked)) fail(409, "Device key is unavailable for this account");
      if (endpoint === "link/inspect") return webResponse({code: link.code, publicKey: link.publicKey, label: link.label, expires: link.expires});
      if (body.publicKey !== link.publicKey) fail(409, "Device key changed; inspect the link before approving");
      // Approval does not itself grant device access; only the requesting key can claim it.
      this.ctx.storage.transactionSync(() => {
        const current = this.get<Link>("link", linkCode);
        if (!current || current.username || current.expires <= Math.floor(Date.now() / 1000))
          fail(409, "Device link expired or was already approved");
        const approver = this.get<Device>("device", session.publicKey);
        if (!approver || approver.revoked) fail(403, "This approving device was revoked; sign in on another device");
        this.put("link", linkCode, {...current, username: session.username, grant: session.grant, approver: session.publicKey}, current.expires);
      });
      await this.ctx.storage.sync();
      return webResponse({approved: true, publicKey: link.publicKey});
    }
    if (endpoint === "devices") return webResponse({devices: this.all<Device>("device").filter(d => d.username === session.username)});
    if (endpoint === "device/revoke") {
      const publicKey = key(body.publicKey), device = this.get<Device>("device", publicKey);
      if (!device || device.username !== session.username) fail(404, "Device is not linked to this account");
      this.put("device", publicKey, {...device, revoked: true});
      await this.ctx.storage.sync();
      return webResponse({revoked: true, publicKey});
    }
    if (endpoint === "logout") {
      const token = (request.headers.get("Cookie") ?? "").split(";").map(v => v.trim()).find(v => v.startsWith(COOKIE + "="))!.slice(COOKIE.length + 1);
      this.remove("session", await credential(token));
      await this.ctx.storage.sync();
      return webResponse({left: true}, 200, cookie("", 0));
    }
    fail(404, "Unknown account endpoint");
  }
}
