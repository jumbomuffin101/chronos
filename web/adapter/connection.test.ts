import assert from "node:assert/strict";
import net from "node:net";
import test from "node:test";
import { ExchangeConnection } from "./connection";
import { Decoder, frame, Type } from "./protocol";

async function listener(
  handler: (socket: net.Socket) => void,
): Promise<{ server: net.Server; port: number }> {
  const server = net.createServer(handler);
  await new Promise<void>((resolve) => server.listen(0, "127.0.0.1", resolve));
  const address = server.address();
  if (!address || typeof address === "string")
    throw new Error("invalid test address");
  return { server, port: address.port };
}
function helloAck(): Buffer {
  const p = Buffer.alloc(8);
  p.writeBigUInt64BE(1n);
  return frame(Type.HelloAck, 0n, p);
}

test(
  "disconnect after order send reports an unknown outcome without resubmission",
  { timeout: 5000 },
  async () => {
    let submissions = 0;
    const fake = await listener((socket) => {
      const decoder = new Decoder();
      socket.on("data", (data) =>
        decoder.push(data, (f) => {
          if (f.type === Type.Hello) socket.write(helloAck());
          else if (f.type === Type.Submit) {
            ++submissions;
            socket.destroy();
          }
        }),
      );
    });
    let done!: () => void;
    const unknown = new Promise<void>((resolve) => {
      done = resolve;
    });
    const connection = new ExchangeConnection(
      fake.port,
      "private",
      (message) => {
        if (
          message.type === "connection.status" &&
          message.state === "connected"
        )
          connection.submit("browser-1", 7n, 1, "buy", "limit", 100n, 1n);
        if (message.type === "order.unknown") {
          assert.equal(message.browserId, "browser-1");
          done();
        }
      },
    );
    connection.start();
    await unknown;
    connection.stop();
    await new Promise<void>((resolve) => fake.server.close(() => resolve()));
    assert.equal(submissions, 1);
  },
);

test(
  "feed sequence gap resubscribes and waits for a fresh snapshot",
  { timeout: 5000 },
  async () => {
    let subscriptions = 0;
    const fake = await listener((socket) => {
      const decoder = new Decoder();
      socket.on("data", (data) =>
        decoder.push(data, (f) => {
          if (f.type === Type.Hello) {
            socket.write(helloAck());
            return;
          }
          if (f.type !== Type.Subscribe) return;
          ++subscriptions;
          const ack = Buffer.alloc(5);
          ack.writeUInt32BE(1, 0);
          const snap = Buffer.alloc(61);
          snap.writeUInt32BE(1, 0);
          snap.writeBigUInt64BE(subscriptions === 1 ? 5n : 7n, 4);
          socket.write(
            Buffer.concat([
              frame(Type.MarketAck, f.requestId, ack),
              frame(Type.MarketData, 0n, snap),
            ]),
          );
          if (subscriptions === 1) {
            const gap = Buffer.from(snap);
            gap.writeBigUInt64BE(7n, 4);
            gap[12] = 1;
            socket.write(frame(Type.MarketData, 0n, gap));
          }
        }),
      );
    });
    const events: string[] = [];
    let done!: () => void;
    const finished = new Promise<void>((resolve) => {
      done = resolve;
    });
    const connection = new ExchangeConnection(
      fake.port,
      "public",
      (message) => {
        if (
          message.type === "connection.status" &&
          message.state === "connected"
        )
          connection.subscribe(1);
        if (message.type === "market.snapshot") {
          events.push(`snapshot:${message.sequence}`);
          if (message.sequence === "7") done();
        }
        if (message.type === "market.gap")
          events.push(`gap:${message.expected}:${message.received}`);
      },
    );
    connection.start();
    await finished;
    connection.stop();
    await new Promise<void>((resolve) => fake.server.close(() => resolve()));
    assert.deepEqual(events, ["snapshot:5", "gap:6:7", "snapshot:7"]);
    assert.equal(subscriptions, 2);
  },
);

test(
  "pending command capacity is bounded and rejected writes are not reported accepted",
  { timeout: 5000 },
  async () => {
    const fake = await listener((socket) => {
      const decoder = new Decoder();
      socket.on("data", (data) =>
        decoder.push(data, (f) => {
          if (f.type === Type.Hello) socket.write(helloAck());
        }),
      );
    });
    let finished!: () => void;
    const ready = new Promise<void>((resolve) => {
      finished = resolve;
    });
    let unknown = 0;
    const connection = new ExchangeConnection(
      fake.port,
      "private",
      (message) => {
        if (
          message.type === "connection.status" &&
          message.state === "connected"
        ) {
          for (let i = 1; i <= 256; ++i)
            connection.submit(
              `id-${i}`,
              BigInt(i),
              1,
              "buy",
              "limit",
              100n,
              1n,
            );
          assert.throws(
            () =>
              connection.submit("overflow", 999n, 1, "buy", "limit", 100n, 1n),
            /capacity/,
          );
          finished();
        }
        if (message.type === "order.unknown") ++unknown;
      },
    );
    connection.start();
    await ready;
    connection.stop();
    await new Promise<void>((resolve) => fake.server.close(() => resolve()));
    assert.equal(unknown, 256);
  },
);

test(
  "an old subscription acknowledgment cannot activate a replacement snapshot",
  { timeout: 5000 },
  async () => {
    let first = 0n,
      unsubscribe = 0n;
    const fake = await listener((socket) => {
      const decoder = new Decoder();
      socket.on("data", (data) =>
        decoder.push(data, (f) => {
          if (f.type === Type.Hello) {
            socket.write(helloAck());
            return;
          }
          if (f.type === Type.Unsubscribe) {
            unsubscribe = f.requestId;
            return;
          }
          if (f.type !== Type.Subscribe) return;
          if (first === 0n) {
            first = f.requestId;
            return;
          }
          const ack = Buffer.alloc(5);
          ack.writeUInt32BE(1, 0);
          const old = Buffer.alloc(61);
          old.writeUInt32BE(1, 0);
          old.writeBigUInt64BE(5n, 4);
          const current = Buffer.from(old);
          current.writeBigUInt64BE(7n, 4);
          socket.write(
            Buffer.concat([
              frame(Type.MarketAck, first, ack),
              frame(Type.MarketData, 0n, old),
              frame(Type.MarketAck, unsubscribe, ack),
              frame(Type.MarketAck, f.requestId, ack),
              frame(Type.MarketData, 0n, current),
            ]),
          );
        }),
      );
    });
    const snapshots: string[] = [];
    let done!: () => void;
    const finished = new Promise<void>((resolve) => {
      done = resolve;
    });
    const connection = new ExchangeConnection(
      fake.port,
      "public",
      (message) => {
        if (
          message.type === "connection.status" &&
          message.state === "connected"
        ) {
          connection.subscribe(1);
          connection.unsubscribe(1);
          connection.subscribe(1);
        }
        if (message.type === "market.snapshot") {
          snapshots.push(String(message.sequence));
          done();
        }
      },
    );
    connection.start();
    await finished;
    connection.stop();
    await new Promise<void>((resolve) => fake.server.close(() => resolve()));
    assert.deepEqual(snapshots, ["7"]);
  },
);
