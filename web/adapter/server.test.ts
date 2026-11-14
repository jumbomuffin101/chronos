import assert from "node:assert/strict";
import { spawn } from "node:child_process";
import { once } from "node:events";
import net from "node:net";
import test from "node:test";
import { WebSocket } from "ws";
import { createAdapter } from "./server";
import { Decoder, frame, Type } from "./protocol";

type Message = Record<string, unknown>;
class Inbox {
  readonly messages: Message[] = [];
  constructor(readonly ws: WebSocket) {
    ws.on("message", (data) =>
      this.messages.push(JSON.parse(data.toString()) as Message),
    );
  }
  async wait(predicate: (message: Message) => boolean): Promise<Message> {
    const deadline = Date.now() + 5000;
    for (;;) {
      const index = this.messages.findIndex(predicate);
      if (index >= 0) return this.messages.splice(index, 1)[0];
      if (Date.now() >= deadline)
        throw new Error("timed out waiting for message");
      await new Promise((resolve) => setTimeout(resolve, 10));
    }
  }
  send(message: object) {
    this.ws.send(JSON.stringify(message));
  }
}
async function ready(client: Inbox) {
  await client.wait(
    (m) =>
      m.type === "connection.status" &&
      m.flow === "private" &&
      m.state === "connected",
  );
  await client.wait(
    (m) =>
      m.type === "connection.status" &&
      m.flow === "public" &&
      m.state === "connected",
  );
}

test(
  "real gateway: private isolation, validation, order correlation and public feed",
  { timeout: 20000 },
  async () => {
    const gateway = spawn(
      process.env.CHRONOS_TEST_GATEWAY ?? "../build/debug/chronos_gateway",
      ["0"],
      { stdio: ["ignore", "pipe", "pipe"] },
    );
    let output = "";
    const gatewayPort = await new Promise<number>((resolve, reject) => {
      const timer = setTimeout(
        () => reject(new Error("gateway startup timeout")),
        5000,
      );
      gateway.on("error", reject);
      gateway.stdout.on("data", (chunk) => {
        output += chunk.toString();
        const match = output.match(/127\.0\.0\.1:(\d+)/);
        if (match) {
          clearTimeout(timer);
          resolve(Number(match[1]));
        }
      });
    });
    const origin = "http://127.0.0.1:3000";
    const adapter = createAdapter(gatewayPort, origin);
    await new Promise<void>((resolve) =>
      adapter.server.listen(0, "127.0.0.1", resolve),
    );
    const address = adapter.server.address();
    if (!address || typeof address === "string")
      throw new Error("invalid adapter address");
    const url = `ws://127.0.0.1:${address.port}/ws`;
    const a = new Inbox(new WebSocket(url, { origin }));
    const b = new Inbox(new WebSocket(url, { origin }));
    try {
      await Promise.all([ready(a), ready(b)]);
      b.send({ type: "subscribe", symbol: 1 });
      const snapshot = await b.wait((m) => m.type === "market.snapshot");
      assert.equal(snapshot.sequence, "0");
      a.send({
        type: "order.submit",
        browserId: "bad",
        clientOrderId: 9007199254740993,
        symbol: 1,
        side: "sell",
        orderType: "limit",
        price: "100",
        quantity: "5",
      });
      const invalid = await a.wait((m) => m.type === "command.error");
      assert.equal(invalid.browserId, "bad");
      a.send({
        type: "order.submit",
        browserId: "sell",
        clientOrderId: "101",
        symbol: 1,
        side: "sell",
        orderType: "limit",
        price: "100",
        quantity: "5",
      });
      const accepted = await a.wait(
        (m) => m.type === "order.report" && m.kind === 0,
      );
      assert.equal(accepted.browserId, "sell");
      await a.wait((m) => m.type === "order.report" && m.kind === 3);
      const book = await b.wait((m) => m.type === "market.book");
      assert.equal(book.ask, "100");
      assert.equal(book.askQuantity, "5");
      b.send({
        type: "order.submit",
        browserId: "buy",
        clientOrderId: "202",
        symbol: 1,
        side: "buy",
        orderType: "market",
        price: "0",
        quantity: "3",
      });
      await b.wait((m) => m.type === "order.report" && m.kind === 0);
      const filled = await b.wait(
        (m) => m.type === "order.report" && m.kind === 2,
      );
      const resting = await a.wait(
        (m) => m.type === "order.report" && m.kind === 1,
      );
      assert.equal(filled.clientOrderId, "202");
      assert.equal(resting.clientOrderId, "101");
      const trade = await b.wait((m) => m.type === "market.trade");
      assert.equal(trade.tradePrice, "100");
      assert.equal(trade.tradeQuantity, "3");
      a.send({
        type: "order.submit",
        browserId: "high-ask",
        clientOrderId: "102",
        symbol: 2,
        side: "sell",
        orderType: "limit",
        price: "1000001",
        quantity: "1",
      });
      await a.wait(
        (m) =>
          m.type === "order.report" &&
          m.browserId === "high-ask" &&
          m.kind === 0,
      );
      await a.wait(
        (m) =>
          m.type === "order.report" &&
          m.browserId === "high-ask" &&
          m.kind === 3,
      );
      b.send({
        type: "order.submit",
        browserId: "risk-check",
        clientOrderId: "203",
        symbol: 2,
        side: "buy",
        orderType: "market",
        price: "0",
        quantity: "1",
      });
      const rejection = await b.wait(
        (m) => m.type === "order.rejected" && m.browserId === "risk-check",
      );
      assert.equal(rejection.code, 15);
      a.send({
        type: "order.cancel",
        browserId: "cancel-sell",
        clientOrderId: "101",
      });
      const cancelled = await a.wait(
        (m) => m.type === "order.report" && m.browserId === "cancel-sell",
      );
      assert.equal(cancelled.kind, 4);
      assert.equal(cancelled.quantity, "2");
      assert.equal(cancelled.remaining, "0");
      assert.equal(
        b.messages.some(
          (m) => m.type === "order.report" && m.clientOrderId === "101",
        ),
        false,
      );
      const evil = new WebSocket(url, { origin: "http://evil.invalid" });
      await once(evil, "error");
      const oversized = new WebSocket(url, { origin });
      await once(oversized, "open");
      const closed = once(oversized, "close");
      oversized.send("x".repeat(8193));
      const [code] = await closed;
      assert.equal(code, 1009);
    } finally {
      a.ws.terminate();
      b.ws.terminate();
      adapter.close();
      gateway.kill("SIGTERM");
      await once(gateway, "exit");
    }
  },
);

test(
  "WebSocket forwards an uncertain outcome when the exchange disconnects",
  { timeout: 10000 },
  async () => {
    let submissions = 0;
    const fake = net.createServer((socket) => {
      const decoder = new Decoder();
      socket.on("data", (data) =>
        decoder.push(data, (f) => {
          if (f.type === Type.Hello) {
            const payload = Buffer.alloc(8);
            payload.writeBigUInt64BE(1n);
            socket.write(frame(Type.HelloAck, 0n, payload));
          } else if (f.type === Type.Submit) {
            ++submissions;
            socket.destroy();
          }
        }),
      );
    });
    await new Promise<void>((resolve) => fake.listen(0, "127.0.0.1", resolve));
    const gateway = fake.address();
    if (!gateway || typeof gateway === "string")
      throw new Error("invalid fake address");
    const origin = "http://127.0.0.1:3000";
    const adapter = createAdapter(gateway.port, origin);
    await new Promise<void>((resolve) =>
      adapter.server.listen(0, "127.0.0.1", resolve),
    );
    const address = adapter.server.address();
    if (!address || typeof address === "string")
      throw new Error("invalid adapter address");
    const client = new Inbox(
      new WebSocket(`ws://127.0.0.1:${address.port}/ws`, { origin }),
    );
    try {
      await ready(client);
      client.send({
        type: "order.submit",
        browserId: "uncertain",
        clientOrderId: "8",
        symbol: 1,
        side: "buy",
        orderType: "limit",
        price: "100",
        quantity: "1",
      });
      const unknown = await client.wait((m) => m.type === "order.unknown");
      assert.equal(unknown.browserId, "uncertain");
      assert.equal(unknown.clientOrderId, "8");
      assert.equal(submissions, 1);
    } finally {
      client.ws.terminate();
      adapter.close();
      await new Promise<void>((resolve) => fake.close(() => resolve()));
    }
  },
);
