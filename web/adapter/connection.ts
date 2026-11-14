import net from "node:net";
import {
  Decoder,
  Type,
  cancel,
  decode,
  frame,
  submit,
  subscription,
  U64_MAX,
} from "./protocol";

type Message = Record<string, string | number | boolean>;
type Pending = {
  kind: "submit" | "cancel" | "subscribe" | "unsubscribe" | "health";
  browserId?: string;
  clientOrderId?: string;
  symbol?: number;
  generation?: bigint;
  sentAt?: number;
};
type Feed = {
  phase: "waiting" | "live";
  acknowledged: boolean;
  generation: bigint;
  sequence?: bigint;
};
const MAX_PENDING = 256;
const MAX_WRITE = 65536;

export class ExchangeConnection {
  private readonly decoder = new Decoder();
  private readonly socket = new net.Socket();
  private readonly pending = new Map<bigint, Pending>();
  private readonly acceptedOrders = new Map<bigint, string>();
  private readonly feeds = new Map<number, Feed>();
  private nextId = 0n;
  private nextFeedGeneration = 0n;
  private ready = false;
  private closed = false;
  private health?: NodeJS.Timeout;
  constructor(
    private readonly port: number,
    private readonly flow: "private" | "public",
    private readonly emit: (message: Message) => void,
  ) {}
  start(): void {
    this.socket.setNoDelay(true);
    this.socket.setTimeout(5000);
    this.socket.on("connect", () => {
      try {
        this.write(frame(Type.Hello, 0n));
      } catch {
        this.socket.destroy();
      }
    });
    this.socket.on("timeout", () =>
      this.socket.destroy(new Error("connection timeout")),
    );
    this.socket.on("data", (data) => {
      try {
        this.decoder.push(data, (f) => this.handle(f.type, f.requestId, f));
      } catch {
        this.socket.destroy(new Error("invalid exchange frame"));
      }
    });
    this.socket.on("error", (error) => {
      this.emit({
        type: "connection.error",
        flow: this.flow,
        message: error.message,
      });
    });
    this.socket.on("close", () => this.onClose());
    this.socket.connect(this.port, "127.0.0.1");
  }
  private write(data: Buffer): void {
    if (
      this.closed ||
      this.socket.destroyed ||
      this.socket.writableLength + data.length > MAX_WRITE
    )
      throw new Error("exchange output capacity exceeded or disconnected");
    // Node owns the buffer after write, including when it returns false; never resend it.
    this.socket.write(data);
  }
  private request(pending: Pending, encode: (id: bigint) => Buffer): void {
    if (!this.ready || this.closed) throw new Error("exchange not connected");
    if (this.pending.size >= MAX_PENDING || this.nextId === U64_MAX)
      throw new Error("request capacity exhausted");
    const id = ++this.nextId;
    const payload = encode(id);
    this.pending.set(id, { ...pending, sentAt: Date.now() });
    try {
      this.write(payload);
    } catch (error) {
      this.pending.delete(id);
      throw error;
    }
  }
  submit(
    browserId: string,
    orderId: bigint,
    sym: number,
    side: "buy" | "sell",
    kind: "market" | "limit",
    price: bigint,
    qty: bigint,
  ): void {
    if (this.flow !== "private") throw new Error("wrong flow");
    if (this.acceptedOrders.size >= 1024)
      throw new Error("session order capacity exhausted");
    this.request(
      { kind: "submit", browserId, clientOrderId: orderId.toString() },
      (id) => submit(id, orderId, sym, side, kind, price, qty),
    );
  }
  cancel(browserId: string, orderId: bigint): void {
    if (this.flow !== "private") throw new Error("wrong flow");
    this.request(
      { kind: "cancel", browserId, clientOrderId: orderId.toString() },
      (id) => cancel(id, orderId),
    );
  }
  subscribe(sym: number): void {
    if (this.flow !== "public") throw new Error("wrong flow");
    if (this.feeds.has(sym)) throw new Error("symbol already subscribed");
    if (!this.feeds.has(sym) && this.feeds.size >= 32)
      throw new Error("subscription limit");
    if (this.nextFeedGeneration === U64_MAX)
      throw new Error("subscription generation exhausted");
    const generation = ++this.nextFeedGeneration;
    this.feeds.set(sym, { phase: "waiting", acknowledged: false, generation });
    try {
      this.request({ kind: "subscribe", symbol: sym, generation }, (id) =>
        subscription(Type.Subscribe, id, sym),
      );
    } catch (error) {
      this.feeds.delete(sym);
      throw error;
    }
    this.emit({ type: "market.status", symbol: sym, state: "resynchronizing" });
  }
  unsubscribe(sym: number): void {
    if (this.flow !== "public") throw new Error("wrong flow");
    this.request({ kind: "unsubscribe", symbol: sym }, (id) =>
      subscription(Type.Unsubscribe, id, sym),
    );
    this.feeds.delete(sym);
  }
  private handle(
    type: Type,
    requestId: bigint,
    f: Parameters<typeof decode>[0],
  ): void {
    const message = decode(f);
    if (type === Type.HelloAck) {
      if (requestId !== 0n || this.ready) throw new Error("invalid handshake");
      this.ready = true;
      this.socket.setTimeout(0);
      this.emit({
        type: "connection.status",
        flow: this.flow,
        state: "connected",
      });
      this.health = setInterval(() => {
        if (
          [...this.pending.values()].some(
            (p) => p.sentAt !== undefined && Date.now() - p.sentAt > 10000,
          )
        ) {
          this.socket.destroy(new Error("exchange response timeout"));
          return;
        }
        if ([...this.pending.values()].some((p) => p.kind === "health")) return;
        try {
          this.request({ kind: "health" }, (id) => frame(Type.Health, id));
        } catch {
          this.socket.destroy();
        }
      }, 2000);
      return;
    }
    if (!this.ready) throw new Error("handshake required");
    if (type === Type.HealthAck) {
      if (this.pending.get(requestId)?.kind !== "health")
        throw new Error("unexpected health reply");
      this.pending.delete(requestId);
      return;
    }
    if (type === Type.MarketAck) {
      const pending = this.pending.get(requestId);
      if (
        !pending ||
        (pending.kind !== "subscribe" && pending.kind !== "unsubscribe") ||
        pending.symbol !== message.symbol
      )
        throw new Error("unexpected market acknowledgment");
      this.pending.delete(requestId);
      const current = this.feeds.get(pending.symbol!);
      const currentSubscription =
        pending.kind === "subscribe" &&
        current?.generation === pending.generation;
      if (message.status !== 0) {
        if (currentSubscription) this.feeds.delete(pending.symbol!);
        if (pending.kind === "subscribe" && !currentSubscription) return;
        this.emit({
          type: "market.status",
          symbol: pending.symbol!,
          state: "error",
        });
      } else if (pending.kind === "subscribe") {
        if (currentSubscription && current) current.acknowledged = true;
      }
      return;
    }
    if (type === Type.MarketData) {
      if (this.flow !== "public" || requestId !== 0n)
        throw new Error("invalid market data flow");
      const sym = message.symbol as number,
        kind = message.kind as number,
        feed = this.feeds.get(sym);
      if (!feed) return;
      const seq = BigInt(message.sequence as string);
      if (kind === 0) {
        if (!feed.acknowledged || feed.phase !== "waiting") return;
        feed.phase = "live";
        feed.sequence = seq;
        this.emit({ ...message, type: "market.snapshot" });
        return;
      }
      if (feed.phase !== "live" || feed.sequence === undefined) return;
      if (kind === 3 || seq !== feed.sequence + 1n) {
        this.feeds.delete(sym);
        this.emit({
          type: "market.gap",
          symbol: sym,
          expected: (feed.sequence + 1n).toString(),
          received: seq.toString(),
        });
        this.subscribe(sym);
        return;
      }
      feed.sequence = seq;
      this.emit({
        ...message,
        type: kind === 1 ? "market.book" : "market.trade",
      });
      return;
    }
    if (type === Type.Execution || type === Type.OrderAck) {
      if (this.flow !== "private")
        throw new Error("private report on public flow");
      const pending = this.pending.get(requestId);
      if (
        type === Type.OrderAck &&
        (!pending || (pending.kind !== "submit" && pending.kind !== "cancel"))
      )
        throw new Error("uncorrelated rejection");
      if (
        type === Type.Execution &&
        pending &&
        !(
          (pending.kind === "submit" && message.kind === 0) ||
          (pending.kind === "cancel" && message.kind === 4)
        )
      )
        throw new Error("unexpected execution correlation");
      if (
        type === Type.Execution &&
        !pending &&
        !this.acceptedOrders.has(requestId)
      )
        throw new Error("uncorrelated execution report");
      if (
        type === Type.Execution &&
        pending?.kind === "submit" &&
        message.kind === 0
      )
        this.acceptedOrders.set(requestId, pending.browserId ?? "");
      if (
        pending &&
        (type === Type.OrderAck || message.kind === 0 || message.kind === 4)
      )
        this.pending.delete(requestId);
      const browserId =
        pending?.browserId ?? this.acceptedOrders.get(requestId);
      this.emit({ ...message, ...(browserId ? { browserId } : {}) });
      return;
    }
    if (type === Type.Error) {
      const pending = this.pending.get(requestId);
      if (pending) this.pending.delete(requestId);
      if (
        pending?.kind === "subscribe" &&
        pending.symbol !== undefined &&
        this.feeds.get(pending.symbol)?.generation === pending.generation
      ) {
        this.feeds.delete(pending.symbol);
        this.emit({
          type: "market.status",
          symbol: pending.symbol,
          state: "error",
        });
      }
      this.emit({
        ...message,
        ...(pending?.browserId ? { browserId: pending.browserId } : {}),
      });
      return;
    }
    throw new Error("unexpected exchange response");
  }
  private onClose(): void {
    if (this.closed) return;
    this.closed = true;
    this.ready = false;
    try {
      this.decoder.finish();
    } catch {
      this.emit({
        type: "connection.error",
        flow: this.flow,
        message: "Truncated exchange frame",
      });
    }
    if (this.health) clearInterval(this.health);
    for (const pending of this.pending.values())
      if (pending.kind === "submit" || pending.kind === "cancel")
        this.emit({
          type: "order.unknown",
          browserId: pending.browserId ?? "",
          clientOrderId: pending.clientOrderId ?? "",
          reason: "exchange disconnected",
        });
    this.pending.clear();
    this.acceptedOrders.clear();
    this.feeds.clear();
    if (this.flow === "private")
      this.emit({
        type: "session.uncertain",
        reason: "Existing orders cannot be recovered after disconnect.",
      });
    this.emit({
      type: "connection.status",
      flow: this.flow,
      state: "disconnected",
    });
  }
  stop(): void {
    this.socket.destroy();
    this.onClose();
  }
}
