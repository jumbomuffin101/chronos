"use client";
import { useEffect, useReducer, useRef, useState } from "react";
import type { ServerMessage } from "../adapter/messages";
import PriceChart from "./price-chart";
import {
  format,
  initialState,
  reducer,
  spread,
  validateOrder,
} from "./terminal-state";

export default function Home() {
  const [state, dispatch] = useReducer(reducer, initialState);
  const [side, setSide] = useState<"buy" | "sell">("buy"),
    [kind, setKind] = useState<"limit" | "market">("limit"),
    [price, setPrice] = useState(""),
    [quantity, setQuantity] = useState(""),
    [symbolInput, setSymbolInput] = useState("1"),
    [generation, setGeneration] = useState(0);
  const socket = useRef<WebSocket | null>(null),
    nextOrder = useRef(0n),
    nextCommand = useRef(0n),
    selected = useRef(1);
  useEffect(() => {
    let disposed = false;
    const ws = new WebSocket(
      process.env.NEXT_PUBLIC_ADAPTER_URL ?? "ws://127.0.0.1:8090/ws",
    );
    socket.current = ws;
    ws.onmessage = (event) => {
      if (disposed) return;
      const m = JSON.parse(event.data) as ServerMessage;
      dispatch({ type: "message", message: m, at: Date.now() });
      if (
        m.type === "connection.status" &&
        m.flow === "public" &&
        m.state === "connected"
      )
        ws.send(
          JSON.stringify({ type: "subscribe", symbol: selected.current }),
        );
    };
    ws.onerror = () => {
      if (!disposed)
        dispatch({
          type: "notice",
          text: "Adapter unavailable. Start the gateway and adapter.",
        });
    };
    ws.onclose = () => {
      if (!disposed) dispatch({ type: "disconnect" });
    };
    return () => {
      disposed = true;
      if (socket.current === ws) socket.current = null;
      ws.close();
    };
  }, [generation]);
  function send(value: object) {
    if (socket.current?.readyState !== WebSocket.OPEN) {
      dispatch({
        type: "notice",
        text: "Adapter disconnected. No command was sent.",
      });
      return false;
    }
    socket.current.send(JSON.stringify(value));
    return true;
  }
  function selectSymbol(e: React.FormEvent) {
    e.preventDefault();
    const n = Number(symbolInput);
    if (
      !/^\d+$/.test(symbolInput) ||
      !Number.isInteger(n) ||
      n < 0 ||
      n > 0xffffffff
    ) {
      dispatch({ type: "notice", text: "Enter a valid uint32 symbol." });
      return;
    }
    if (n === state.symbol) return;
    send({ type: "unsubscribe", symbol: state.symbol });
    selected.current = n;
    dispatch({ type: "select", symbol: n });
    send({ type: "subscribe", symbol: n });
  }
  function submit(e: React.FormEvent) {
    e.preventDefault();
    const error = validateOrder(quantity, price, kind);
    if (error) {
      dispatch({ type: "notice", text: error });
      return;
    }
    const id = (++nextOrder.current).toString(),
      browserId = `order_${++nextCommand.current}`;
    if (
      send({
        type: "order.submit",
        browserId,
        clientOrderId: id,
        symbol: state.symbol,
        side,
        orderType: kind,
        price: kind === "market" ? "0" : price,
        quantity,
      })
    ) {
      dispatch({
        type: "submit",
        order: {
          id,
          browserId,
          symbol: state.symbol,
          side,
          orderType: kind,
          price: kind === "market" ? "0" : price,
          quantity,
          remaining: quantity,
          status: "pending",
        },
      });
    }
  }
  function cancel(id: string) {
    const browserId = `cancel_${++nextCommand.current}`;
    if (send({ type: "order.cancel", browserId, clientOrderId: id }))
      dispatch({ type: "cancel", id, browserId });
  }
  const market = state.feed === "live" ? state.market : null;
  const bid = market && BigInt(market.bidQuantity) > 0n ? market.bid : null,
    ask = market && BigInt(market.askQuantity) > 0n ? market.ask : null;
  const last = state.trades[0];
  return (
    <main className="terminal">
      <header className="topbar">
        <div className="brand">
          <span className="mark">C</span>
          <span>
            CHRONOS <small>EXCHANGE TERMINAL</small>
          </span>
        </div>
        <div className="top-meta">
          <span className="symbol-chip">SYMBOL {state.symbol}</span>
          <span className="environment">● LOCAL SIMULATION</span>
          <span
            className={
              state.privateStatus === "connected" &&
              state.publicStatus === "connected"
                ? "live"
                : "warn"
            }
          >
            ●{" "}
            {state.privateStatus === "connected" &&
            state.publicStatus === "connected"
              ? "EXCHANGE CONNECTED"
              : state.privateStatus === "connecting" ||
                  state.publicStatus === "connecting"
                ? "EXCHANGE CONNECTING"
                : "EXCHANGE DISCONNECTED"}
          </span>
          <span className={state.feed === "live" ? "live" : "warn"}>
            ● FEED {state.feed.toUpperCase()}
          </span>
          {(state.privateStatus === "disconnected" ||
            state.publicStatus === "disconnected") && (
            <button
              className="link-button"
              onClick={() => {
                dispatch({ type: "disconnect" });
                setGeneration((x) => x + 1);
              }}
            >
              New session
            </button>
          )}
        </div>
      </header>
      <div className="workspace">
        <div className="workspace-title">
          <div>
            <div className="eyebrow">
              WORKSPACE <span>/</span> TRADING
            </div>
            <h1>Trading terminal</h1>
            <p>Exchange activity · observed in this browser session</p>
          </div>
          <form onSubmit={selectSymbol} className="symbol-form">
            <label htmlFor="symbol">SYMBOL ID</label>
            <input
              id="symbol"
              value={symbolInput}
              onChange={(e) => setSymbolInput(e.target.value)}
              inputMode="numeric"
            />
            <button>Load</button>
          </form>
        </div>
        <section className="overview" aria-label="Market overview">
          <div>
            <label>BEST BID</label>
            <strong className="buy">{format(bid)}</strong>
            <small>
              {bid ? format(market?.bidQuantity) + " units" : "No bid"}
            </small>
          </div>
          <div>
            <label>BEST ASK</label>
            <strong className="sell">{format(ask)}</strong>
            <small>
              {ask ? format(market?.askQuantity) + " units" : "No ask"}
            </small>
          </div>
          <div>
            <label>SPREAD</label>
            <strong>{format(spread(market))}</strong>
            <small>ticks</small>
          </div>
          <div>
            <label>LAST TRADE</label>
            <strong>{format(last?.price)}</strong>
            <small>
              {last ? format(last.quantity) + " units" : "Not observed"}
            </small>
          </div>
          <div>
            <label>FEED SEQUENCE</label>
            <strong>{format(state.sequence)}</strong>
            <small>{state.feed}</small>
          </div>
        </section>
        <div className="grid">
          <div className="main-column">
            <section className="panel chart-panel">
              <div className="panel-head">
                <div>
                  <h2>Price & volume</h2>
                  <p>Symbol {state.symbol} · observed session trades</p>
                </div>
                <span className="tag">SESSION ONLY</span>
              </div>
              {state.trades.length ? (
                <>
                  <PriceChart trades={state.trades} />
                  <p className="chart-note">
                    Observed time in UTC · volume per second · values beyond
                    chart precision omitted
                  </p>
                </>
              ) : (
                <div className="chart-empty">
                  <div className="chart-icon">⌁</div>
                  <b>Waiting for the first trade</b>
                  <span>
                    The chart displays trades observed in this session.
                    Historical data is unavailable.
                  </span>
                </div>
              )}
              <div className="attribution">
                Charts by{" "}
                <a
                  href="https://www.tradingview.com/"
                  target="_blank"
                  rel="noreferrer"
                >
                  TradingView
                </a>
              </div>
            </section>
            <div className="market-grid">
              <section className="panel">
                <div className="panel-head">
                  <div>
                    <h2>Order book</h2>
                    <p>Authoritative best prices only</p>
                  </div>
                  <span className={state.feed === "live" ? "live" : "warn"}>
                    ● {state.feed}
                  </span>
                </div>
                <div className="book-head">
                  <span>SIDE</span>
                  <span>PRICE (TICKS)</span>
                  <span>QUANTITY</span>
                </div>
                <div className="book-row">
                  <span className="sell">ASK</span>
                  <b className="sell">{format(ask)}</b>
                  <span>{ask ? format(market?.askQuantity) : "—"}</span>
                </div>
                <div className="book-spread">
                  Spread <b>{format(spread(market))}</b> ticks
                </div>
                <div className="book-row">
                  <span className="buy">BID</span>
                  <b className="buy">{format(bid)}</b>
                  <span>{bid ? format(market?.bidQuantity) : "—"}</span>
                </div>
                <p className="panel-foot">
                  Full market depth is not available from the exchange.
                </p>
              </section>
              <section className="panel">
                <div className="panel-head">
                  <div>
                    <h2>Recent trades</h2>
                    <p>Public feed · observed locally</p>
                  </div>
                  <span className="tag">{state.trades.length} OBSERVED</span>
                </div>
                <div className="trade-table">
                  <div className="trade-row headings">
                    <span>TIME</span>
                    <span>PRICE</span>
                    <span>QTY</span>
                    <span>SEQ</span>
                  </div>
                  {state.trades.length ? (
                    state.trades.slice(0, 30).map((t) => (
                      <div className="trade-row" key={t.sequence}>
                        <span>{new Date(t.time).toLocaleTimeString()}</span>
                        <span>{format(t.price)}</span>
                        <span>{format(t.quantity)}</span>
                        <span>{format(t.sequence)}</span>
                      </div>
                    ))
                  ) : (
                    <div className="table-empty">
                      No trades observed for symbol {state.symbol}.
                    </div>
                  )}
                </div>
              </section>
            </div>
            <section className="panel orders-panel">
              <div className="panel-head">
                <div>
                  <h2>My orders & executions</h2>
                  <p>Session-observed only · not recoverable account history</p>
                </div>
                <span className="tag">PRIVATE SESSION</span>
              </div>
              <div className="scroll-table">
                <table>
                  <thead>
                    <tr>
                      <th>ORDER</th>
                      <th>SYMBOL</th>
                      <th>SIDE</th>
                      <th>TYPE</th>
                      <th>PRICE</th>
                      <th>QTY</th>
                      <th>REMAINING</th>
                      <th>STATUS</th>
                      <th></th>
                    </tr>
                  </thead>
                  <tbody>
                    {state.orders.map((o) => (
                      <tr key={o.id}>
                        <td>#{o.id}</td>
                        <td>{o.symbol}</td>
                        <td className={o.side}>{o.side.toUpperCase()}</td>
                        <td>{o.orderType}</td>
                        <td>
                          {o.orderType === "market" ? "MKT" : format(o.price)}
                        </td>
                        <td>{format(o.quantity)}</td>
                        <td>{format(o.remaining)}</td>
                        <td title={o.reason}>
                          {o.status}
                          {o.reason ? `: ${o.reason}` : ""}
                        </td>
                        <td>
                          {["accepted", "resting", "partial fill"].includes(
                            o.status,
                          ) && (
                            <button
                              className="link-button"
                              disabled={state.privateStatus !== "connected"}
                              onClick={() => cancel(o.id)}
                            >
                              Cancel
                            </button>
                          )}
                        </td>
                      </tr>
                    ))}
                  </tbody>
                </table>
                {!state.orders.length && (
                  <div className="table-empty">
                    No orders submitted in this session.
                  </div>
                )}
              </div>
              {state.reports.length > 0 && (
                <div className="reports">
                  <b>EXECUTIONS</b>
                  {state.reports.slice(0, 5).map((r, i) => (
                    <span key={i}>{r}</span>
                  ))}
                </div>
              )}
            </section>
          </div>
          <aside className="panel entry">
            <div className="panel-head">
              <div>
                <h2>Order entry</h2>
                <p>Simulated execution</p>
              </div>
              <span className="tag">SYMBOL {state.symbol}</span>
            </div>
            <form onSubmit={submit}>
              <div className="side-tabs">
                <button
                  type="button"
                  aria-pressed={side === "buy"}
                  className={side === "buy" ? "active-buy" : ""}
                  onClick={() => setSide("buy")}
                >
                  BUY
                </button>
                <button
                  type="button"
                  aria-pressed={side === "sell"}
                  className={side === "sell" ? "active-sell" : ""}
                  onClick={() => setSide("sell")}
                >
                  SELL
                </button>
              </div>
              <label htmlFor="order-type">ORDER TYPE</label>
              <select
                id="order-type"
                value={kind}
                onChange={(e) => setKind(e.target.value as "limit" | "market")}
              >
                <option value="limit">Limit</option>
                <option value="market">Market</option>
              </select>
              {kind === "limit" && (
                <>
                  <label htmlFor="price">
                    LIMIT PRICE <em>ticks</em>
                  </label>
                  <input
                    id="price"
                    inputMode="numeric"
                    value={price}
                    onChange={(e) => setPrice(e.target.value)}
                    placeholder="0"
                    required
                  />
                </>
              )}
              <label htmlFor="qty">
                QUANTITY <em>units</em>
              </label>
              <input
                id="qty"
                inputMode="numeric"
                value={quantity}
                onChange={(e) => setQuantity(e.target.value)}
                placeholder="0"
                required
              />
              <button
                className={
                  side === "buy" ? "submit buy-button" : "submit sell-button"
                }
                disabled={state.privateStatus !== "connected"}
              >
                Submit {side} order
              </button>
            </form>
            <p className="order-warning">
              Orders are simulated. The exchange determines acceptance and risk
              eligibility.
            </p>
            <div className="notice" role="status">
              {state.notice}
            </div>
            <div className="availability">
              <b>ACCOUNT DATA</b>
              <p>
                Positions, balances and P&amp;L are unavailable from the current
                protocol.
              </p>
            </div>
          </aside>
        </div>
        <footer>
          CHRONOS · LOCAL DEVELOPMENT{" "}
          <span>Simulated data only · no durable recovery</span>
        </footer>
      </div>
    </main>
  );
}
