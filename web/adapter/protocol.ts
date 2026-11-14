export const VERSION = 3;
export const HEADER = 16;
export const MAX_FRAME = 4096;
export const U64_MAX = (1n << 64n) - 1n;
export const I64_MAX = (1n << 63n) - 1n;
export const I64_MIN = -(1n << 63n);

export enum Type {
  Hello = 1,
  HelloAck = 2,
  Health = 3,
  HealthAck = 4,
  Echo = 5,
  EchoReply = 6,
  Error = 7,
  Submit = 16,
  Cancel = 17,
  OrderAck = 18,
  Execution = 19,
  Subscribe = 20,
  Unsubscribe = 21,
  MarketAck = 22,
  MarketData = 23,
}
const sizes: Partial<Record<Type, number>> = {
  [Type.Hello]: 0,
  [Type.HelloAck]: 8,
  [Type.Health]: 0,
  [Type.HealthAck]: 0,
  [Type.Error]: 2,
  [Type.Submit]: 30,
  [Type.Cancel]: 8,
  [Type.OrderAck]: 17,
  [Type.Execution]: 69,
  [Type.Subscribe]: 4,
  [Type.Unsubscribe]: 4,
  [Type.MarketAck]: 5,
  [Type.MarketData]: 61,
};
export type Frame = { type: Type; requestId: bigint; payload: Buffer };

export function uint(text: unknown, max = U64_MAX): bigint {
  if (
    typeof text !== "string" ||
    !/^(0|[1-9][0-9]*)$/.test(text) ||
    text.length > 20
  )
    throw new Error("expected an unsigned decimal string");
  const value = BigInt(text);
  if (value > max) throw new Error("integer out of range");
  return value;
}
export function symbol(value: unknown): number {
  if (
    typeof value !== "number" ||
    !Number.isInteger(value) ||
    value < 0 ||
    value > 0xffffffff
  )
    throw new Error("invalid symbol");
  return value;
}
export function frame(
  type: Type,
  id: bigint,
  payload = Buffer.alloc(0),
): Buffer {
  if (
    id < 0n ||
    id > U64_MAX ||
    HEADER + payload.length > MAX_FRAME ||
    (!(type in sizes) && type !== Type.Echo && type !== Type.EchoReply) ||
    (sizes[type] !== undefined && sizes[type] !== payload.length)
  )
    throw new Error("invalid frame");
  const out = Buffer.alloc(HEADER + payload.length);
  out.writeUInt32BE(out.length, 0);
  out[4] = VERSION;
  out[5] = type;
  out.writeBigUInt64BE(id, 8);
  payload.copy(out, HEADER);
  return out;
}
export function subscription(
  type: Type.Subscribe | Type.Unsubscribe,
  id: bigint,
  sym: number,
): Buffer {
  const p = Buffer.alloc(4);
  p.writeUInt32BE(symbol(sym));
  return frame(type, id, p);
}
export function cancel(id: bigint, clientOrderId: bigint): Buffer {
  const p = Buffer.alloc(8);
  p.writeBigUInt64BE(clientOrderId);
  return frame(Type.Cancel, id, p);
}
export function submit(
  id: bigint,
  clientOrderId: bigint,
  sym: number,
  side: "buy" | "sell",
  kind: "limit" | "market",
  price: bigint,
  quantity: bigint,
): Buffer {
  if (
    (side !== "buy" && side !== "sell") ||
    (kind !== "limit" && kind !== "market") ||
    clientOrderId < 0n ||
    clientOrderId > U64_MAX ||
    quantity <= 0n ||
    quantity > U64_MAX ||
    price < I64_MIN ||
    price > I64_MAX ||
    (kind === "market" ? price !== 0n : price <= 0n)
  )
    throw new Error("invalid order fields");
  const p = Buffer.alloc(30);
  p.writeBigUInt64BE(clientOrderId, 0);
  p.writeUInt32BE(symbol(sym), 8);
  p[12] = side === "buy" ? 0 : 1;
  p[13] = kind === "limit" ? 0 : 1;
  p.writeBigInt64BE(price, 14);
  p.writeBigUInt64BE(quantity, 22);
  return frame(Type.Submit, id, p);
}

export class Decoder {
  private readonly buffer = Buffer.alloc(MAX_FRAME);
  private used = 0;
  private expected = HEADER;
  private failed = false;
  push(input: Uint8Array, onFrame: (f: Frame) => void): void {
    if (this.failed) throw new Error("decoder already failed");
    try {
      let pos = 0;
      while (pos < input.length) {
        const take = Math.min(input.length - pos, this.expected - this.used);
        this.buffer.set(input.subarray(pos, pos + take), this.used);
        this.used += take;
        pos += take;
        if (this.used !== this.expected) continue;
        if (this.expected === HEADER) {
          const length = this.buffer.readUInt32BE(0),
            version = this.buffer[4],
            type = this.buffer[5] as Type;
          if (length < HEADER || length > MAX_FRAME)
            throw new Error("invalid frame length");
          if (version !== VERSION || this.buffer.readUInt16BE(6) !== 0)
            throw new Error("invalid version or flags");
          if (!(type in sizes) && type !== Type.Echo && type !== Type.EchoReply)
            throw new Error("unknown type");
          if (sizes[type] !== undefined && sizes[type] !== length - HEADER)
            throw new Error("invalid payload length");
          this.expected = length;
          if (this.used !== this.expected) continue;
        }
        const f = {
          type: this.buffer[5] as Type,
          requestId: this.buffer.readBigUInt64BE(8),
          payload: Buffer.from(this.buffer.subarray(HEADER, this.expected)),
        };
        this.used = 0;
        this.expected = HEADER;
        onFrame(f);
      }
    } catch (error) {
      this.failed = true;
      throw error;
    }
  }
  finish(): void {
    if (this.failed || this.used !== 0)
      throw new Error("truncated or invalid frame");
  }
}

export function decode(f: Frame): Record<string, string | number> {
  const p = f.payload,
    id = f.requestId.toString();
  switch (f.type) {
    case Type.HelloAck:
      return {
        type: "hello",
        requestId: id,
        session: p.readBigUInt64BE().toString(),
      };
    case Type.OrderAck:
      return {
        type: "order.rejected",
        requestId: id,
        clientOrderId: p.readBigUInt64BE(0).toString(),
        code: p[8],
      };
    case Type.Execution:
      if (p[44] > 5) throw new Error("invalid execution kind");
      return {
        type: "order.report",
        requestId: id,
        sequence: p.readBigUInt64BE(0).toString(),
        commandSequence: p.readBigUInt64BE(8).toString(),
        clientOrderId: p.readBigUInt64BE(16).toString(),
        internalOrderId: p.readBigUInt64BE(24).toString(),
        tradeId: p.readBigUInt64BE(32).toString(),
        symbol: p.readUInt32BE(40),
        kind: p[44],
        price: p.readBigInt64BE(45).toString(),
        quantity: p.readBigUInt64BE(53).toString(),
        remaining: p.readBigUInt64BE(61).toString(),
      };
    case Type.MarketAck:
      if (p[4] > 1) throw new Error("invalid market acknowledgment");
      return {
        type: "market.ack",
        requestId: id,
        symbol: p.readUInt32BE(0),
        status: p[4],
      };
    case Type.MarketData:
      if (p[12] > 3) throw new Error("invalid market kind");
      return {
        type: "market.data",
        symbol: p.readUInt32BE(0),
        sequence: p.readBigUInt64BE(4).toString(),
        kind: p[12],
        bid: p.readBigInt64BE(13).toString(),
        bidQuantity: p.readBigUInt64BE(21).toString(),
        ask: p.readBigInt64BE(29).toString(),
        askQuantity: p.readBigUInt64BE(37).toString(),
        tradePrice: p.readBigInt64BE(45).toString(),
        tradeQuantity: p.readBigUInt64BE(53).toString(),
      };
    case Type.Error:
      return { type: "protocol.error", requestId: id, code: p.readUInt16BE(0) };
    case Type.HealthAck:
      return { type: "health", requestId: id };
    default:
      throw new Error("unexpected server message");
  }
}
