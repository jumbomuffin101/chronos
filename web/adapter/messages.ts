// The browser supplies correlation IDs, never an account or exchange session ID.
export type BrowserCommand =
  | { type: "subscribe" | "unsubscribe"; symbol: number }
  | {
      type: "order.submit";
      browserId: string;
      clientOrderId: string;
      symbol: number;
      side: "buy" | "sell";
      orderType: "limit" | "market";
      price: string;
      quantity: string;
    }
  | { type: "order.cancel"; browserId: string; clientOrderId: string };

export type ServerMessage =
  | {
      type: "connection.status";
      flow: "private" | "public";
      state: "connected" | "disconnected";
    }
  | { type: "session.uncertain"; reason: string }
  | { type: "command.error"; message: string; browserId?: string }
  | { type: "connection.error"; flow: "private" | "public"; message: string }
  | { type: "market.status"; symbol: number; state: string }
  | { type: "market.gap"; symbol: number; expected: string; received: string }
  | {
      type: "market.snapshot" | "market.book" | "market.trade";
      symbol: number;
      sequence: string;
      kind: number;
      bid: string;
      bidQuantity: string;
      ask: string;
      askQuantity: string;
      tradePrice: string;
      tradeQuantity: string;
    }
  | {
      type: "order.report";
      requestId: string;
      browserId?: string;
      sequence: string;
      commandSequence: string;
      clientOrderId: string;
      internalOrderId: string;
      tradeId: string;
      symbol: number;
      kind: number;
      price: string;
      quantity: string;
      remaining: string;
    }
  | {
      type: "order.rejected";
      requestId: string;
      browserId?: string;
      clientOrderId: string;
      code: number;
    }
  | {
      type: "order.unknown";
      browserId: string;
      clientOrderId: string;
      reason: string;
    }
  | {
      type: "protocol.error";
      requestId: string;
      browserId?: string;
      code: number;
    };
