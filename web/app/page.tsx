"use client";

import { useEffect, useRef, useState } from "react";

type Event = Record<string, string | number | boolean>;
const panel = "rounded-xl border border-[#27313b] bg-[#10161d]";
const label = "text-[10px] uppercase tracking-[0.16em] text-[#82929e]";
const reportNames = [
  "accepted",
  "partial fill",
  "filled",
  "resting",
  "cancelled",
  "market remainder cancelled",
];
const rejectionNames: Record<number, string> = {
  1: "unknown session",
  2: "duplicate request",
  3: "duplicate order",
  4: "not owner",
  5: "not found",
  6: "invalid order",
  7: "capacity",
  8: "counter exhausted",
  9: "kill switch",
  10: "symbol denied",
  11: "quantity limit",
  12: "notional limit",
  13: "open-order limit",
  14: "position limit",
  15: "market price bound",
  16: "arithmetic overflow",
};

export default function Home() {
  const socket = useRef<WebSocket | null>(null);
  const orderId = useRef(0n);
  const selectedSymbol = useRef(1);
  const [privateStatus, setPrivateStatus] = useState("connecting");
  const [publicStatus, setPublicStatus] = useState("connecting");
  const [symbol, setSymbol] = useState("1");
  const [feed, setFeed] = useState("waiting for snapshot");
  const [quote, setQuote] = useState<Event | null>(null);
  const [trades, setTrades] = useState<Event[]>([]);
  const [reports, setReports] = useState<Event[]>([]);
  const [side, setSide] = useState("buy");
  const [kind, setKind] = useState("limit");
  const [price, setPrice] = useState("");
  const [quantity, setQuantity] = useState("");
  const [cancelId, setCancelId] = useState("");
  const [notice, setNotice] = useState(
    "Sessions are local and cannot be recovered after disconnect.",
  );
  useEffect(() => {
    let disposed = false;
    const ws = new WebSocket(
      process.env.NEXT_PUBLIC_ADAPTER_URL ?? "ws://127.0.0.1:8090/ws",
    );
    socket.current = ws;
    ws.onmessage = (event) => {
      if (disposed) return;
      const msg = JSON.parse(event.data) as Event;
      if (msg.type === "connection.status") {
        if (msg.flow === "private") setPrivateStatus(String(msg.state));
        if (msg.flow === "public") {
          setPublicStatus(String(msg.state));
          if (msg.state === "connected")
            ws.send(JSON.stringify({ type: "subscribe", symbol: 1 }));
          else {
            setFeed("disconnected");
            setQuote(null);
            setTrades([]);
          }
        }
      } else if (
        msg.type === "market.snapshot" &&
        msg.symbol === selectedSymbol.current
      ) {
        setQuote(msg);
        setFeed("live");
      } else if (
        msg.type === "market.book" &&
        msg.symbol === selectedSymbol.current
      )
        setQuote(msg);
      else if (
        msg.type === "market.trade" &&
        msg.symbol === selectedSymbol.current
      )
        setTrades((previous) => [msg, ...previous].slice(0, 20));
      else if (msg.type === "market.gap" || msg.type === "market.status") {
        if (msg.symbol === selectedSymbol.current) {
          setFeed(
            msg.type === "market.gap"
              ? "gap — resynchronizing"
              : String(msg.state),
          );
          setQuote(null);
          if (msg.type === "market.gap") setTrades([]);
        }
      } else if (
        msg.type === "order.report" ||
        msg.type === "order.rejected" ||
        msg.type === "order.unknown"
      ) {
        setReports((previous) => [msg, ...previous].slice(0, 20));
      } else if (
        msg.type === "command.error" ||
        msg.type === "connection.error" ||
        msg.type === "protocol.error" ||
        msg.type === "session.uncertain"
      )
        setNotice(
          String(msg.message ?? msg.reason ?? `Protocol error ${msg.code}`),
        );
    };
    ws.onerror = () =>
      !disposed &&
      setNotice(
        "Adapter unavailable. Start the gateway and adapter to connect.",
      );
    ws.onclose = () => {
      if (disposed) return;
      setPrivateStatus("disconnected");
      setPublicStatus("disconnected");
      setFeed("disconnected");
      setQuote(null);
      setNotice(
        "Disconnected. Order outcomes may be unknown and resting orders may remain; this session cannot be recovered.",
      );
    };
    return () => {
      disposed = true;
      if (socket.current === ws) socket.current = null;
      ws.close();
    };
  }, []);
  function send(value: object): boolean {
    if (socket.current?.readyState !== WebSocket.OPEN) {
      setNotice("Adapter is disconnected.");
      return false;
    }
    socket.current.send(JSON.stringify(value));
    return true;
  }
  function chooseSymbol(value: string) {
    const next = Number(value);
    if (!Number.isInteger(next) || next < 0 || next > 0xffffffff) {
      setNotice("Enter a valid 32-bit symbol.");
      return;
    }
    send({ type: "unsubscribe", symbol: Number(symbol) });
    selectedSymbol.current = next;
    setSymbol(value);
    setQuote(null);
    setTrades([]);
    setFeed("waiting for snapshot");
    send({ type: "subscribe", symbol: next });
  }
  function placeOrder(event: React.FormEvent) {
    event.preventDefault();
    const id = (++orderId.current).toString();
    if (
      send({
        type: "order.submit",
        browserId: `order_${id}`,
        clientOrderId: id,
        symbol: Number(symbol),
        side,
        orderType: kind,
        price: kind === "market" ? "0" : price,
        quantity,
      })
    )
      setNotice(
        `Submitted simulated order ${id}; acceptance is determined by the exchange.`,
      );
  }
  const value = (key: string) =>
    quote && feed === "live" ? String(quote[key] ?? "—") : "—";
  return (
    <main className="min-h-screen">
      <header className="flex flex-wrap items-center justify-between gap-3 border-b border-[#26313a] px-6 py-4">
        <div className="flex items-center gap-3">
          <span className="grid h-9 w-9 place-items-center rounded-lg bg-[#a0ead7] text-lg font-bold text-[#0a211c]">
            C
          </span>
          <div>
            <div className="font-bold tracking-[0.18em]">CHRONOS</div>
            <div className="text-[10px] uppercase tracking-widest text-[#8ca19f]">
              Exchange simulator
            </div>
          </div>
        </div>
        <div className="flex items-center gap-3 text-xs">
          <span className="rounded-full border border-[#3a4a4b] px-3 py-1 text-[#b2c9c4]">
            LOCAL SIMULATION
          </span>
          <span
            className={
              privateStatus === "connected" && publicStatus === "connected"
                ? "text-[#83ddbc]"
                : "text-[#e4b771]"
            }
          >
            ● Exchange{" "}
            {privateStatus === "connected" && publicStatus === "connected"
              ? "connected"
              : "disconnected"}
          </span>
        </div>
      </header>
      <div className="mx-auto max-w-[1600px] px-4 py-6 sm:px-8">
        <div className="mb-6 flex flex-wrap items-end justify-between gap-4">
          <div>
            <p className={label}>Workspace / Terminal</p>
            <h1 className="mt-2 text-2xl font-semibold">Market overview</h1>
            <p className="mt-1 text-sm text-[#82929e]">
              Live session data from the local C++ exchange.
            </p>
          </div>
          <form
            onSubmit={(e) => {
              e.preventDefault();
              chooseSymbol(
                new FormData(e.currentTarget).get("symbol") as string,
              );
            }}
            className="flex items-end gap-2"
          >
            <label className="text-xs text-[#9aabb4]">
              Symbol ID
              <input
                name="symbol"
                defaultValue="1"
                inputMode="numeric"
                className="mt-1 block w-32 rounded border border-[#34434d] bg-[#0a1015] px-3 py-2 text-white"
              />
            </label>
            <button className="rounded border border-[#3a4d51] px-3 py-2 text-xs hover:bg-[#1c2a2b]">
              View
            </button>
          </form>
        </div>
        <div className="mb-4 flex flex-wrap gap-3 text-xs text-[#91a3aa]">
          <span>
            Symbol <b className="text-white">{symbol}</b>
          </span>
          <span>·</span>
          <span>
            Feed{" "}
            <b
              className={feed === "live" ? "text-[#83ddbc]" : "text-[#e4b771]"}
            >
              {feed}
            </b>
          </span>
          <span>·</span>
          <span>Top of book only</span>
        </div>
        <div className="grid gap-4 lg:grid-cols-[1fr_310px]">
          <div className="grid gap-4">
            <section className={`${panel} p-5`}>
              <div className="mb-5 flex justify-between">
                <div>
                  <p className={label}>Market data</p>
                  <h2 className="mt-1 font-semibold">Best bid & ask</h2>
                </div>
                <span className="text-xs text-[#7c919a]">
                  Sequence {value("sequence")}
                </span>
              </div>
              <div className="grid grid-cols-2 gap-4">
                <div className="rounded-lg bg-[#14231f] p-4">
                  <p className={label}>Best bid</p>
                  <p className="mt-2 font-mono text-2xl text-[#8ee0bc]">
                    {value("bid")}
                  </p>
                  <p className="mt-1 text-xs text-[#8da9a0]">
                    Quantity {value("bidQuantity")}
                  </p>
                </div>
                <div className="rounded-lg bg-[#291b20] p-4">
                  <p className={label}>Best ask</p>
                  <p className="mt-2 font-mono text-2xl text-[#eca7aa]">
                    {value("ask")}
                  </p>
                  <p className="mt-1 text-xs text-[#bba0a3]">
                    Quantity {value("askQuantity")}
                  </p>
                </div>
              </div>
              <p className="mt-4 text-xs text-[#80919b]">
                {feed === "live"
                  ? "Zero indicates an empty side. Full depth is not available."
                  : "Waiting for a consistent exchange snapshot."}
              </p>
            </section>
            <section className={`${panel} min-h-56 p-5`}>
              <p className={label}>Price & volume</p>
              <h2 className="mt-1 font-semibold">Chart workspace</h2>
              <div className="grid min-h-36 place-items-center text-center text-sm text-[#758793]">
                Historical candles and volume are not available yet.
              </div>
            </section>
            <section className={`${panel} p-5`}>
              <p className={label}>Public feed</p>
              <h2 className="mt-1 font-semibold">Recent trades</h2>
              {trades.length === 0 ? (
                <p className="py-8 text-center text-sm text-[#758793]">
                  No trades observed in this session.
                </p>
              ) : (
                <ul className="mt-4 space-y-2 font-mono text-sm">
                  {trades.map((trade, i) => (
                    <li
                      key={i}
                      className="flex justify-between border-t border-[#25313a] pt-2"
                    >
                      <span>{String(trade.tradePrice)} ticks</span>
                      <span>{String(trade.tradeQuantity)} units</span>
                    </li>
                  ))}
                </ul>
              )}
            </section>
          </div>
          <aside className="grid content-start gap-4">
            <section className={`${panel} p-5`}>
              <p className={label}>Manual entry</p>
              <h2 className="mt-1 font-semibold">Simulated order</h2>
              <form className="mt-5 space-y-4" onSubmit={placeOrder}>
                <label className="block text-xs text-[#9aabb4]">
                  Side
                  <select
                    value={side}
                    onChange={(e) => setSide(e.target.value)}
                    className="mt-1 w-full rounded border border-[#34434d] bg-[#0a1015] px-3 py-2 text-white"
                  >
                    <option value="buy">Buy</option>
                    <option value="sell">Sell</option>
                  </select>
                </label>
                <label className="block text-xs text-[#9aabb4]">
                  Type
                  <select
                    value={kind}
                    onChange={(e) => setKind(e.target.value)}
                    className="mt-1 w-full rounded border border-[#34434d] bg-[#0a1015] px-3 py-2 text-white"
                  >
                    <option value="limit">Limit</option>
                    <option value="market">Market</option>
                  </select>
                </label>
                {kind === "limit" && (
                  <label className="block text-xs text-[#9aabb4]">
                    Price in ticks
                    <input
                      value={price}
                      onChange={(e) => setPrice(e.target.value)}
                      inputMode="numeric"
                      required
                      pattern="[0-9]+"
                      className="mt-1 w-full rounded border border-[#34434d] bg-[#0a1015] px-3 py-2 text-white"
                    />
                  </label>
                )}
                <label className="block text-xs text-[#9aabb4]">
                  Quantity
                  <input
                    value={quantity}
                    onChange={(e) => setQuantity(e.target.value)}
                    inputMode="numeric"
                    required
                    pattern="[0-9]+"
                    className="mt-1 w-full rounded border border-[#34434d] bg-[#0a1015] px-3 py-2 text-white"
                  />
                </label>
                <button
                  disabled={privateStatus !== "connected"}
                  className="w-full rounded bg-[#99e7cd] px-3 py-3 text-sm font-bold text-[#0a211c] disabled:cursor-not-allowed disabled:opacity-40"
                >
                  Submit simulated order
                </button>
              </form>
              <p
                role="status"
                className="mt-4 text-xs leading-5 text-[#a8b9bc]"
              >
                {notice}
              </p>
            </section>
            <section className={`${panel} p-5`}>
              <p className={label}>Private session</p>
              <h2 className="mt-1 font-semibold">Execution reports</h2>
              {reports.length === 0 ? (
                <p className="py-6 text-center text-sm text-[#758793]">
                  No reports received.
                </p>
              ) : (
                <ul className="mt-4 space-y-3 text-xs text-[#afbdc4]">
                  {reports.map((report, i) => (
                    <li key={i} className="border-t border-[#25313a] pt-2">
                      {String(report.type)} · order{" "}
                      {String(report.clientOrderId ?? "—")} ·{" "}
                      {typeof report.kind === "number"
                        ? (reportNames[report.kind] ?? "unknown")
                        : typeof report.code === "number"
                          ? (rejectionNames[report.code] ??
                            `code ${report.code}`)
                          : "unknown"}
                    </li>
                  ))}
                </ul>
              )}
              <form
                onSubmit={(e) => {
                  e.preventDefault();
                  const id = (++orderId.current).toString();
                  send({
                    type: "order.cancel",
                    browserId: `cancel_${id}`,
                    clientOrderId: cancelId,
                  });
                }}
                className="mt-4 flex gap-2"
              >
                <input
                  aria-label="Client order ID to cancel"
                  placeholder="Order ID"
                  value={cancelId}
                  onChange={(e) => setCancelId(e.target.value)}
                  pattern="[0-9]+"
                  required
                  className="min-w-0 flex-1 rounded border border-[#34434d] bg-[#0a1015] px-2 py-2 text-sm"
                />
                <button
                  disabled={privateStatus !== "connected"}
                  className="rounded border border-[#46565f] px-3 text-xs disabled:opacity-40"
                >
                  Cancel
                </button>
              </form>
              <p className="mt-3 text-xs text-[#758793]">
                Risk balances and recovery are not available over the current
                protocol.
              </p>
            </section>
          </aside>
        </div>
      </div>
    </main>
  );
}
