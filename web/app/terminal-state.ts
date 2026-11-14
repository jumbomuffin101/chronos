import type { ServerMessage } from "../adapter/messages";

export type Market = Extract<
  ServerMessage,
  { type: "market.snapshot" | "market.book" | "market.trade" }
>;
export type Trade = {
  sequence: string;
  symbol: number;
  price: string;
  quantity: string;
  time: number;
};
export type Order = {
  id: string;
  browserId: string;
  symbol: number;
  side: string;
  orderType: string;
  price: string;
  quantity: string;
  remaining: string;
  status: string;
  reason?: string;
  cancelBrowserId?: string;
  beforeCancel?: string;
};
export type State = {
  browserStatus: "connecting" | "connected" | "unavailable" | "disconnected";
  sessionUncertain: boolean;
  privateStatus: string;
  publicStatus: string;
  feed: string;
  symbol: number;
  market: Market | null;
  sequence: string | null;
  trades: Trade[];
  orders: Order[];
  reports: string[];
  notice: string;
};
export const initialState: State = {
  browserStatus: "connecting",
  sessionUncertain: false,
  privateStatus: "connecting",
  publicStatus: "connecting",
  feed: "waiting",
  symbol: 1,
  market: null,
  sequence: null,
  trades: [],
  orders: [],
  reports: [],
  notice: "Local simulation · session history is not recoverable.",
};
export const reasons: Record<number, string> = {
  1: "Unknown session",
  2: "Duplicate request",
  3: "Duplicate order",
  4: "Not owner",
  5: "Order not found",
  6: "Invalid order",
  7: "Capacity reached",
  8: "Counter exhausted",
  9: "Kill switch active",
  10: "Symbol denied",
  11: "Quantity limit",
  12: "Notional limit",
  13: "Open-order limit",
  14: "Position limit",
  15: "Market price bound",
  16: "Arithmetic overflow",
};
export function format(value: string | undefined | null): string {
  if (value == null || !/^-?\d+$/.test(value)) return "—";
  return BigInt(value).toLocaleString("en-US");
}
export function spread(market: Market | null): string | null {
  if (
    !market ||
    BigInt(market.bidQuantity) === 0n ||
    BigInt(market.askQuantity) === 0n
  )
    return null;
  return (BigInt(market.ask) - BigInt(market.bid)).toString();
}
export function validateOrder(
  quantity: string,
  price: string,
  type: string,
): string | null {
  const valid = (v: string, max: bigint) =>
    /^(0|[1-9]\d*)$/.test(v) &&
    v.length <= 20 &&
    BigInt(v) > 0n &&
    BigInt(v) <= max;
  if (!valid(quantity, (1n << 64n) - 1n))
    return "Enter a positive whole quantity within uint64 range.";
  if (type === "limit" && !valid(price, (1n << 63n) - 1n))
    return "Enter a positive whole price in ticks within int64 range.";
  return null;
}
export type Action =
  | { type: "browser.open" }
  | { type: "new-session" }
  | { type: "message"; message: ServerMessage; at: number }
  | { type: "select"; symbol: number }
  | { type: "submit"; order: Order }
  | { type: "cancel"; id: string; browserId: string }
  | { type: "disconnect"; unavailable?: boolean }
  | { type: "notice"; text: string };
export function reducer(s: State, a: Action): State {
  if (a.type === "browser.open") return { ...s, browserStatus: "connected" };
  if (a.type === "new-session")
    return {
      ...s,
      browserStatus: "connecting",
      privateStatus: "connecting",
      publicStatus: "connecting",
      feed: "waiting",
      market: null,
      sequence: null,
      trades: [],
    };
  if (a.type === "notice") return { ...s, notice: a.text };
  if (a.type === "select")
    return {
      ...s,
      symbol: a.symbol,
      feed: "waiting",
      market: null,
      sequence: null,
      trades: [],
    };
  if (a.type === "submit")
    return {
      ...s,
      orders: [a.order, ...s.orders].slice(0, 1024),
      notice: `Order ${a.order.id} submitted; awaiting exchange response.`,
    };
  if (a.type === "cancel")
    return {
      ...s,
      orders: s.orders.map((o) =>
        o.id === a.id
          ? {
              ...o,
              beforeCancel: o.status,
              cancelBrowserId: a.browserId,
              status: "cancel pending",
            }
          : o,
      ),
    };
  if (a.type === "disconnect")
    return {
      ...s,
      browserStatus: a.unavailable ? "unavailable" : "disconnected",
      sessionUncertain: s.sessionUncertain || !a.unavailable,
      privateStatus: "disconnected",
      publicStatus: "disconnected",
      feed: "disconnected",
      market: null,
      sequence: null,
      trades: [],
      orders: s.orders.map((o) =>
        [
          "pending",
          "accepted",
          "resting",
          "partial fill",
          "cancel pending",
        ].includes(o.status)
          ? { ...o, status: "uncertain" }
          : o,
      ),
      notice: a.unavailable
        ? "Adapter unavailable. Start the gateway and adapter, then start a new session."
        : "Browser disconnected. Pending outcomes are uncertain and resting orders may remain; this session cannot be recovered.",
    };
  const m = a.message;
  if (m.type === "connection.status") {
    if (m.flow === "private")
      return m.state === "disconnected"
        ? {
            ...s,
            privateStatus: m.state,
            sessionUncertain: true,
            orders: s.orders.map((o) =>
              [
                "pending",
                "accepted",
                "resting",
                "partial fill",
                "cancel pending",
              ].includes(o.status)
                ? { ...o, status: "uncertain" }
                : o,
            ),
          }
        : { ...s, privateStatus: m.state };
    return m.state === "disconnected"
      ? {
          ...s,
          publicStatus: m.state,
          feed: "disconnected",
          market: null,
          sequence: null,
          trades: [],
        }
      : { ...s, publicStatus: m.state };
  }
  if (m.type === "market.status" || m.type === "market.gap")
    return m.symbol !== s.symbol || s.publicStatus === "disconnected"
      ? s
      : {
          ...s,
          feed: m.type === "market.gap" ? "resynchronizing" : m.state,
          market: null,
          sequence: null,
          trades: m.type === "market.gap" ? [] : s.trades,
        };
  if (
    m.type === "market.snapshot" ||
    m.type === "market.book" ||
    m.type === "market.trade"
  ) {
    if (m.symbol !== s.symbol || s.publicStatus === "disconnected") return s;
    if (
      m.type !== "market.snapshot" &&
      (s.feed !== "live" ||
        s.sequence === null ||
        BigInt(m.sequence) !== BigInt(s.sequence) + 1n)
    )
      return { ...s, feed: "stale", market: null, sequence: null };
    if (m.type === "market.snapshot" && s.feed === "live") return s;
    if (m.type === "market.snapshot")
      return { ...s, feed: "live", market: m, sequence: m.sequence };
    if (m.type === "market.book")
      return { ...s, market: m, sequence: m.sequence };
    const trade = {
      sequence: m.sequence,
      symbol: m.symbol,
      price: m.tradePrice,
      quantity: m.tradeQuantity,
      time: a.at,
    };
    return {
      ...s,
      sequence: m.sequence,
      trades: [trade, ...s.trades].slice(0, 200),
    };
  }
  if (m.type === "order.report") {
    const names = [
      "accepted",
      "partial fill",
      "filled",
      "resting",
      "cancelled",
      "remainder cancelled",
    ];
    const status = names[m.kind] ?? "unknown";
    return {
      ...s,
      orders: s.orders.map((o) =>
        o.id !== m.clientOrderId
          ? o
          : o.status === "cancel pending" && m.kind === 1
            ? { ...o, beforeCancel: status, remaining: m.remaining }
            : { ...o, status, remaining: m.remaining },
      ),
      reports: [
        `${status} · order ${m.clientOrderId} · ${format(m.quantity)} @ ${format(m.price)}`,
        ...s.reports,
      ].slice(0, 100),
      notice: `Order ${m.clientOrderId}: ${status}.`,
    };
  }
  if (m.type === "order.rejected") {
    const reason = reasons[m.code] ?? `Exchange rejection ${m.code}`;
    return {
      ...s,
      orders: s.orders.map((o) =>
        o.id !== m.clientOrderId
          ? o
          : o.cancelBrowserId && o.cancelBrowserId === m.browserId
            ? {
                ...o,
                status:
                  o.status === "cancel pending"
                    ? (o.beforeCancel ?? "uncertain")
                    : o.status,
                cancelBrowserId: undefined,
                beforeCancel: undefined,
                reason,
              }
            : { ...o, status: "rejected", reason },
      ),
      notice: `Request for order ${m.clientOrderId} rejected: ${reason}.`,
    };
  }
  if (m.type === "order.unknown")
    return {
      ...s,
      orders: s.orders.map((o) =>
        o.id === m.clientOrderId &&
        [
          "pending",
          "accepted",
          "resting",
          "partial fill",
          "cancel pending",
        ].includes(o.status)
          ? { ...o, status: "uncertain" }
          : o,
      ),
      notice: `Order ${m.clientOrderId}: outcome uncertain.`,
    };
  if (m.type === "command.error" || m.type === "protocol.error") {
    const text =
      m.type === "command.error" ? m.message : `Protocol error ${m.code}`;
    return {
      ...s,
      orders: s.orders.map((o) =>
        o.cancelBrowserId && o.cancelBrowserId === m.browserId
          ? {
              ...o,
              status:
                o.status === "cancel pending"
                  ? (o.beforeCancel ?? "uncertain")
                  : o.status,
              cancelBrowserId: undefined,
              beforeCancel: undefined,
              reason: text,
            }
          : o.browserId === m.browserId
            ? { ...o, status: "rejected", reason: text }
            : o,
      ),
      notice: text,
    };
  }
  if (m.type === "connection.error") return { ...s, notice: m.message };
  if (m.type === "session.uncertain")
    return { ...s, sessionUncertain: true, notice: m.reason };
  return s;
}
