export interface Message {
  seq: number;
  timestamp: number;
  originAlias: string;
  author: string;
  clientTimestamp: number;
  text: string;
}

export interface Delivery {
  type: "delivery";
  alias: string;
  client: string;
  deliveryId: string;
  route: string;
  message: Message;
}

export type Operation =
  | {op: "login"; client: string; timestamp: number; since: number; password: string; attempt: string; route: string}
  | {op: "refresh"; client: string; timestamp: number; attempt: string; route: string}
  | {op: "post"; client: string; timestamp: number; attempt: string; text: string; source: "client"}
  | {op: "prepare"; client: string; deliveryId: string; proof: string}
  | {op: "receipt"; client: string; deliveryId: string; outcome: "sent" | "failed" | "unknown"}
  | {op: "ack"; client: string; deliveryId: string; proof: string}
  | {op: "members"; prefix?: string};

export interface Member {
  client: string;
  cursor: number;
  // Only the selected frontend receives route/pending dispatch metadata.
  route?: string;
  pending?: {deliveryId: string; proof: string | null; state: string};
}

export interface Result {
  respond?: boolean;
  duplicate?: boolean;
  cursor?: number;
  message?: Message;
  transmit?: boolean;
  members?: Member[];
}

export type ServerEvent = Delivery
  | {type: "result"; id: string; result: Result}
  | {type: "error"; id?: string; error: string}
  | {type: "ready"; alias: string; publicKey: string; name: string};
