import http from "node:http";
import { pathToFileURL } from "node:url";
import { WebSocket, WebSocketServer } from "ws";
import { ExchangeConnection } from "./connection";
import { I64_MAX, symbol, uint } from "./protocol";

const MAX_CLIENTS = 8;
const MAX_WS_BUFFER = 65536;
const MAX_COMMAND_IDS = 1024;
function port(value: string | undefined, fallback: number): number {
  const n = value === undefined ? fallback : Number(value);
  if (!Number.isInteger(n) || n < 1 || n > 65535)
    throw new Error("invalid port");
  return n;
}
function plain(value: unknown): Record<string, unknown> {
  if (!value || typeof value !== "object" || Array.isArray(value))
    throw new Error("expected object");
  return value as Record<string, unknown>;
}
export function createAdapter(gatewayPort: number, allowedOrigin: string) {
  const server = http.createServer((req, res) => {
    if (req.method === "GET" && req.url === "/health") {
      res.writeHead(200);
      res.end("ok\n");
    } else {
      res.writeHead(404);
      res.end();
    }
  });
  const wss = new WebSocketServer({
    noServer: true,
    maxPayload: 8192,
    perMessageDeflate: false,
  });
  server.on("upgrade", (req, socket, head) => {
    const remote = req.socket.remoteAddress;
    if (
      req.url !== "/ws" ||
      req.headers.origin !== allowedOrigin ||
      (remote !== "127.0.0.1" &&
        remote !== "::1" &&
        remote !== "::ffff:127.0.0.1") ||
      wss.clients.size >= MAX_CLIENTS
    ) {
      socket.destroy();
      return;
    }
    wss.handleUpgrade(req, socket, head, (ws) =>
      wss.emit("connection", ws, req),
    );
  });
  wss.on("connection", (ws) => {
    let closed = false;
    const commandIds = new Set<string>();
    const send = (message: Record<string, string | number | boolean>) => {
      if (closed || ws.readyState !== WebSocket.OPEN) return;
      const payload = JSON.stringify(message);
      if (ws.bufferedAmount + Buffer.byteLength(payload) > MAX_WS_BUFFER) {
        ws.terminate();
        return;
      }
      ws.send(payload, (error) => {
        if (error) ws.terminate();
      });
    };
    const privateFlow = new ExchangeConnection(gatewayPort, "private", send);
    const publicFlow = new ExchangeConnection(gatewayPort, "public", send);
    privateFlow.start();
    publicFlow.start();
    ws.on("message", (data, binary) => {
      let browserId: string | undefined;
      try {
        if (binary)
          throw new Error("binary browser messages are not supported");
        const command = plain(JSON.parse(data.toString()));
        if (
          typeof command.browserId === "string" &&
          /^[A-Za-z0-9_-]{1,64}$/.test(command.browserId)
        )
          browserId = command.browserId;
        if (command.type === "subscribe") {
          publicFlow.subscribe(symbol(command.symbol));
          return;
        }
        if (command.type === "unsubscribe") {
          publicFlow.unsubscribe(symbol(command.symbol));
          return;
        }
        if (command.type !== "order.submit" && command.type !== "order.cancel")
          throw new Error("unknown command");
        if (
          typeof command.browserId !== "string" ||
          !/^[A-Za-z0-9_-]{1,64}$/.test(command.browserId) ||
          commandIds.has(command.browserId) ||
          commandIds.size >= MAX_COMMAND_IDS
        )
          throw new Error("invalid or duplicate browser ID");
        const orderId = uint(command.clientOrderId);
        if (command.type === "order.cancel") {
          privateFlow.cancel(command.browserId, orderId);
        } else {
          if (command.side !== "buy" && command.side !== "sell")
            throw new Error("invalid side");
          if (command.orderType !== "limit" && command.orderType !== "market")
            throw new Error("invalid order type");
          const price = uint(command.price, I64_MAX),
            quantity = uint(command.quantity);
          privateFlow.submit(
            command.browserId,
            orderId,
            symbol(command.symbol),
            command.side,
            command.orderType,
            price,
            quantity,
          );
        }
        commandIds.add(command.browserId);
      } catch (error) {
        send({
          type: "command.error",
          message: error instanceof Error ? error.message : "invalid command",
          ...(browserId ? { browserId } : {}),
        });
      }
    });
    const stop = () => {
      if (closed) return;
      closed = true;
      privateFlow.stop();
      publicFlow.stop();
    };
    ws.on("error", stop);
    ws.on("close", stop);
  });
  return {
    server,
    wss,
    close: () => {
      for (const client of wss.clients) client.terminate();
      wss.close();
      server.close();
    },
  };
}

if (
  process.argv[1] &&
  import.meta.url === pathToFileURL(process.argv[1]).href
) {
  const gatewayPort = port(process.env.CHRONOS_GATEWAY_PORT, 8088);
  const adapterPort = port(process.env.CHRONOS_ADAPTER_PORT, 8090);
  const origin = process.env.CHRONOS_WEB_ORIGIN ?? "http://127.0.0.1:3000";
  const adapter = createAdapter(gatewayPort, origin);
  adapter.server.listen(adapterPort, "127.0.0.1", () =>
    console.log(`Chronos adapter: 127.0.0.1:${adapterPort}`),
  );
  const stop = () => adapter.close();
  process.once("SIGINT", stop);
  process.once("SIGTERM", stop);
}
