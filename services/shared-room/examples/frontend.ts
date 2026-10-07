import type {Delivery, Member, Operation, Result, ServerEvent} from "../src/protocol";

export interface Identity {alias: string; publicKey: string; name: string}
export interface Socket {
  send(frame: string): void;
  close(): void;
  onMessage(handler: (frame: string) => void): void;
  onClose(handler: () => void): void;
}
export type OpenSocket = (url: string, bearerToken: string) => Promise<Socket>;
export interface Radio {
  // Connect this to Birch Link.SubmitWithReceipt and its final TXResult.
  // Queue acceptance alone is not "sent"; transport errors after allocation are "unknown".
  submitWithReceipt(packet: Uint8Array): Promise<"sent" | "failed" | "unknown">;
}
export interface RadioCodec {
  // Try all matching advertised/member prefixes, then verify MAC/decryption.
  // Return exactly one authenticated original client operation, or null.
  // Ignore outbound signed room posts (TXT type 2) and local reflections.
  // Resolve bare ACKs against *all* pending proofs; reject ambiguous matches.
  // For a directed source prefix, query resolveMembers even if a cached candidate exists:
  // another frontend may have just enrolled a client with the same prefix.
  decode(packet: Uint8Array, identities: readonly Identity[], members: ReadonlyMap<string, Member[]>,
    resolveMembers: (alias: string, prefix: string) => Promise<Member[]>):
    Promise<{alias: string; operation: Operation} | null>;
  response(identity: Identity, operation: Operation, result: Result): Promise<Uint8Array | null>;
  delivery(identity: Identity, delivery: Delivery): Promise<{packet: Uint8Array; proof: string}>;
  advertisement(identity: Identity): Promise<Uint8Array>;
}

/** One host/radio, several room identities, per-identity API sockets, one TX queue. */
export class Frontend {
  private sockets = new Map<string, Socket>();
  private members = new Map<string, Member[]>();
  private requests = new Map<string, {resolve: (r: Result) => void; reject: (e: Error) => void}>();
  private queues = new Map<string, Array<() => Promise<void>>>();
  private running = false;
  private nextQueue = 0;
  private nextAdvert = 0;
  private requestId = 0;

  constructor(
    private baseURL: string, private token: string, readonly identities: readonly Identity[],
    private radio: Radio, private codec: RadioCodec, private open: OpenSocket,
    private report: (error: unknown) => void = console.error,
  ) {
    if (!identities.length || new Set(identities.map(i => i.alias)).size !== identities.length ||
        new Set(identities.map(i => i.publicKey)).size !== identities.length) throw new Error("Configure distinct advertised identities");
    for (const identity of identities) this.queues.set(identity.alias, []);
  }

  async connect(): Promise<void> {
    for (const identity of this.identities) await this.connectAlias(identity.alias);
  }
  async connectAlias(alias: string): Promise<void> {
    const identity = this.identity(alias);
    const previous = this.sockets.get(alias);
    this.sockets.delete(alias);
    this.members.delete(alias);
    this.rejectRequests(alias);
    previous?.close();
    const url = `${this.baseURL.replace(/\/$/, "").replace(/^http/, "ws")}/v1/aliases/${alias}/socket`;
    const socket = await this.open(url, this.token);
    this.sockets.set(alias, socket);
    socket.onMessage(frame => {
      try {
        const event = JSON.parse(frame) as ServerEvent;
        if (event.type === "ready" && (event.publicKey !== identity.publicKey || event.name !== identity.name)) {
          socket.close();
          throw new Error(`Configured public key differs for ${alias}`);
        }
        if (event.type === "result" || event.type === "error") {
          const key = `${alias}:${event.id}`;
          const request = this.requests.get(key);
          this.requests.delete(key);
          if (event.type === "result") request?.resolve(event.result);
          else request?.reject(new Error(event.error));
        }
        if (event.type === "delivery") this.enqueue(alias, () => this.deliver(identity, event));
      } catch (error) {this.report(error);}
    });
    socket.onClose(() => {
      if (this.sockets.get(alias) !== socket) return;
      this.sockets.delete(alias);
      this.members.delete(alias);
      this.rejectRequests(alias);
    });
    // Restores full-key candidates, owned routes, and pending ACK bindings.
    // Does not retransmit a prepared delivery after reconnect.
    await this.reloadMembers(alias);
  }

  private identity(alias: string): Identity {
    const identity = this.identities.find(i => i.alias === alias);
    if (!identity) throw new Error(`Frontend does not advertise ${alias}`);
    return identity;
  }
  private call(alias: string, operation: Operation): Promise<Result> {
    const socket = this.sockets.get(alias);
    if (!socket) return Promise.reject(new Error(`${alias} API is disconnected`));
    const id = String(++this.requestId);
    return new Promise((resolve, reject) => {
      const key = `${alias}:${id}`;
      const timer = setTimeout(() => {
        this.requests.delete(key);
        reject(new Error(`${alias} API response timed out; operation outcome may be unknown`));
      }, 15000);
      this.requests.set(key, {resolve: result => {clearTimeout(timer); resolve(result);},
        reject: error => {clearTimeout(timer); reject(error);}});
      try {socket.send(JSON.stringify({id, operation}));}
      catch (error) {clearTimeout(timer); this.requests.delete(key); reject(error);}
    });
  }
  private rejectRequests(alias: string): void {
    for (const [key, request] of this.requests) if (key.startsWith(`${alias}:`)) {
      this.requests.delete(key);
      request.reject(new Error(`${alias} connection closed; operation outcome may be unknown`));
    }
  }
  private async fetchMembers(alias: string, prefix = ""): Promise<Member[]> {
    try {return (await this.call(alias, {op: "members", prefix})).members ?? [];}
    catch (error) {
      if (!(error instanceof Error) || error.message !== "Use a longer member key prefix" || prefix.length >= 64) throw error;
      const members: Member[] = [];
      for (const nibble of "0123456789abcdef") members.push(...await this.fetchMembers(alias, prefix + nibble));
      return members;
    }
  }
  private async reloadMembers(alias: string): Promise<void> {
    this.members.set(alias, await this.fetchMembers(alias));
  }

  async receive(packet: Uint8Array): Promise<void> {
    const decoded = await this.codec.decode(packet, this.identities, this.members,
      (alias, prefix) => {this.identity(alias); return this.fetchMembers(alias, prefix);});
    if (!decoded) return;
    const identity = this.identity(decoded.alias);
    const result = await this.call(decoded.alias, decoded.operation);
    if (result.respond) {
      this.enqueue(decoded.alias, async () => {
        const response = await this.codec.response(identity, decoded.operation, result);
        if (response) await this.radio.submitWithReceipt(response);
      });
    }
    await this.reloadMembers(decoded.alias);
  }

  private async deliver(identity: Identity, delivery: Delivery): Promise<void> {
    if (delivery.alias !== identity.alias) throw new Error("Delivery identity mismatch");
    const encoded = await this.codec.delivery(identity, delivery);
    const prepared = await this.call(identity.alias, {op: "prepare", client: delivery.client,
      deliveryId: delivery.deliveryId, proof: encoded.proof});
    if (!prepared.transmit) return;
    // Keep the proof available before RF submission: an ACK can beat the TX receipt.
    const member = this.members.get(identity.alias)?.find(m => m.client === delivery.client);
    if (member) member.pending = {deliveryId: delivery.deliveryId, proof: encoded.proof, state: "prepared"};
    let outcome: "sent" | "failed" | "unknown";
    try {outcome = await this.radio.submitWithReceipt(encoded.packet);} catch {outcome = "unknown";}
    await this.call(identity.alias, {op: "receipt", client: delivery.client, deliveryId: delivery.deliveryId, outcome});
  }

  // Call once per global advertisement interval (e.g. 60 s + jitter), not once per identity.
  // This rotates A, B, C across successive slots instead of flooding them together.
  advertNext(): void {
    const identity = this.identities[this.nextAdvert++ % this.identities.length];
    this.enqueue(identity.alias, async () => {
      const packet = await this.codec.advertisement(identity);
      await this.radio.submitWithReceipt(packet);
    });
  }
  private enqueue(alias: string, job: () => Promise<void>): void {
    const queue = this.queues.get(alias);
    if (!queue) throw new Error(`Unknown TX identity ${alias}`);
    // No unbounded history buffer at a disconnected/slow frontend.
    if (queue.length >= 64) {this.report(new Error(`${alias} TX queue full; refresh on next client request`)); return;}
    queue.push(job);
    void this.drain();
  }
  private async drain(): Promise<void> {
    if (this.running) return;
    this.running = true;
    try {
      const aliases = this.identities.map(i => i.alias);
      while (aliases.some(alias => this.queues.get(alias)!.length)) {
        const alias = aliases[this.nextQueue++ % aliases.length];
        const job = this.queues.get(alias)!.shift();
        if (job) try {await job();} catch (error) {this.report(error);}
      }
    } finally {this.running = false;}
  }
}
