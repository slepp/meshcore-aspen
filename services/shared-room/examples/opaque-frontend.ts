/** Host/Birch example: one instance per alias, one shared physical radio queue. */
export interface OpaqueSocket {
  send(frame: string): void;
  onmessage: ((event: { data: string }) => void) | null;
  onclose: (() => void) | null;
  close(): void;
}
export type Submit = (
  packet: Uint8Array,
  delayMs: number,
  priority: number,
) => Promise<"sent" | "failed" | "unknown">;
export class OpaqueFrontend {
  private ready = false;
  private generation = 0;
  private id = 0;
  constructor(
    private socket: OpaqueSocket,
    private alias: { id: string; publicKey: string; name: string },
    private submit: Submit,
  ) {
    socket.onclose = () => {
      this.ready = false;
      ++this.generation;
    };
    socket.onmessage = (e) => {
      void this.frame(e.data).catch(() => socket.close());
    };
  }
  private operation(operation: unknown): void {
    if (this.ready)
      this.socket.send(JSON.stringify({ id: String(++this.id), operation }));
  }
  received(packet: Uint8Array, localReflection = false): void {
    if (!localReflection && packet.length && packet.length <= 255)
      this.operation({
        op: "rf",
        packet: btoa(String.fromCharCode(...packet)),
      });
  }
  advertise(): void {
    this.operation({ op: "advertise" });
  }
  private async frame(frame: string): Promise<void> {
    if (new TextEncoder().encode(frame).length > 4096)
      throw new Error("Oversized room frame");
    const event = JSON.parse(frame);
    if (event.type === "ready") {
      if (
        this.ready ||
        event.version !== 2 ||
        event.alias !== this.alias.id ||
        event.publicKey !== this.alias.publicKey ||
        event.name !== this.alias.name
      )
        throw new Error("Room identity mismatch");
      this.ready = true;
      return;
    }
    if (!this.ready) throw new Error("Room is not ready");
    if (event.type === "result" || event.type === "error") return;
    if (
      event.type !== "transmit" ||
      event.alias !== this.alias.id ||
      !/^[a-f0-9-]{36}$/.test(event.dispatchId) ||
      typeof event.packet !== "string" ||
      event.packet.length > 340 ||
      !Number.isInteger(event.delayMs) ||
      event.delayMs < 0 ||
      event.delayMs > 30000 ||
      !Number.isInteger(event.priority) ||
      event.priority < 0 ||
      event.priority > 255
    )
      throw new Error("Invalid room dispatch");
    const packet = Uint8Array.from(atob(event.packet), (c) => c.charCodeAt(0));
    if (
      !packet.length ||
      packet.length > 255 ||
      btoa(String.fromCharCode(...packet)) !== event.packet
    )
      throw new Error("Invalid room packet");
    const generation = this.generation;
    let outcome: "sent" | "failed" | "unknown";
    try {
      outcome = await this.submit(packet, event.delayMs, event.priority);
    } catch {
      outcome = "unknown";
    }
    if (this.ready && generation === this.generation)
      this.operation({
        op: "txReceipt",
        dispatchId: event.dispatchId,
        outcome,
      });
  }
}
