import assert from "node:assert/strict";
import test from "node:test";
import {
  format,
  initialState,
  reducer,
  spread,
  validateOrder,
  type Market,
  type Order,
} from "./terminal-state";

const snapshot: Market = {
  type: "market.snapshot",
  symbol: 1,
  sequence: "9007199254740993",
  kind: 0,
  bid: "9007199254740993",
  bidQuantity: "5",
  ask: "9007199254741001",
  askQuantity: "2",
  tradePrice: "0",
  tradeQuantity: "0",
};
const order: Order = {
  id: "1",
  browserId: "order_1",
  symbol: 1,
  side: "buy",
  orderType: "limit",
  price: "100",
  quantity: "5",
  remaining: "5",
  status: "pending",
};
test("overview retains integer precision and handles empty sides", () => {
  assert.equal(spread(snapshot), "8");
  assert.equal(format(snapshot.bid), "9,007,199,254,740,993");
  assert.equal(format(null), "—");
  assert.equal(spread({ ...snapshot, bidQuantity: "0" }), null);
});
test("entry validation rejects invalid and overflowing values", () => {
  assert.equal(validateOrder("5", "10", "limit"), null);
  assert.ok(validateOrder("0", "10", "limit"));
  assert.ok(validateOrder("18446744073709551616", "1", "limit"));
  assert.ok(validateOrder("1", "9223372036854775808", "limit"));
  assert.equal(validateOrder("1", "", "market"), null);
});
test("gap, duplicate, and disconnected data never stay live", () => {
  let s = reducer(initialState, { type: "message", message: snapshot, at: 0 });
  s = reducer(s, {
    type: "message",
    message: {
      ...snapshot,
      type: "market.trade",
      sequence: "9007199254740994",
      tradePrice: "100",
      tradeQuantity: "1",
    },
    at: 42,
  });
  assert.equal(s.trades.length, 1);
  s = reducer(s, {
    type: "message",
    message: {
      ...snapshot,
      type: "market.trade",
      sequence: "9007199254740994",
    },
    at: 43,
  });
  assert.equal(s.feed, "stale");
  assert.equal(s.market, null);
  s = reducer(s, { type: "disconnect" });
  assert.equal(s.feed, "disconnected");
});
test("risk rejection is explicit and failed cancel retains order status", () => {
  let s = reducer(initialState, { type: "submit", order });
  s = reducer(s, {
    type: "message",
    message: {
      type: "order.rejected",
      requestId: "1",
      browserId: "order_1",
      clientOrderId: "1",
      code: 15,
    },
    at: 0,
  });
  assert.equal(s.orders[0].reason, "Market price bound");
  s = { ...s, orders: [{ ...order, status: "resting" }] };
  s = reducer(s, { type: "cancel", id: "1", browserId: "cancel_1" });
  s = reducer(s, {
    type: "message",
    message: {
      type: "order.rejected",
      requestId: "2",
      browserId: "cancel_1",
      clientOrderId: "1",
      code: 5,
    },
    at: 0,
  });
  assert.equal(s.orders[0].status, "resting");
  assert.match(s.notice, /Order not found/);
});
test("session histories are bounded and disconnect marks orders uncertain", () => {
  let s = reducer(initialState, { type: "message", message: snapshot, at: 0 });
  for (let i = 1; i <= 205; i++)
    s = reducer(s, {
      type: "message",
      message: {
        ...snapshot,
        type: "market.trade",
        sequence: (9007199254740993n + BigInt(i)).toString(),
        tradePrice: "100",
        tradeQuantity: "1",
      },
      at: i,
    });
  assert.equal(s.trades.length, 200);
  assert.equal(s.trades[0].time, 205);
  s = reducer(s, { type: "submit", order });
  s = reducer(s, { type: "disconnect" });
  assert.equal(s.orders[0].status, "uncertain");
  assert.equal(s.trades.length, 0);
});
test("a fill racing a rejected cancellation remains filled", () => {
  let s = { ...initialState, orders: [{ ...order, status: "resting" }] };
  s = reducer(s, { type: "cancel", id: "1", browserId: "cancel_1" });
  s = reducer(s, {
    type: "message",
    at: 0,
    message: {
      type: "order.report",
      requestId: "1",
      clientOrderId: "1",
      sequence: "1",
      commandSequence: "1",
      internalOrderId: "1",
      tradeId: "1",
      symbol: 1,
      kind: 2,
      price: "100",
      quantity: "5",
      remaining: "0",
    },
  });
  s = reducer(s, {
    type: "message",
    at: 0,
    message: {
      type: "order.rejected",
      requestId: "2",
      browserId: "cancel_1",
      clientOrderId: "1",
      code: 5,
    },
  });
  assert.equal(s.orders[0].status, "filled");
  assert.equal(s.orders[0].remaining, "0");
});

test("late cancel errors and uncertain cancellation do not undo a confirmed fill", () => {
  let s = { ...initialState, orders: [{ ...order, status: "resting" }] };
  s = reducer(s, { type: "cancel", id: "1", browserId: "cancel_1" });
  s = reducer(s, {
    type: "message",
    at: 0,
    message: {
      type: "order.report",
      requestId: "1",
      clientOrderId: "1",
      sequence: "1",
      commandSequence: "1",
      internalOrderId: "1",
      tradeId: "1",
      symbol: 1,
      kind: 2,
      price: "100",
      quantity: "5",
      remaining: "0",
    },
  });
  s = reducer(s, {
    type: "message",
    at: 0,
    message: {
      type: "protocol.error",
      requestId: "2",
      browserId: "cancel_1",
      code: 1,
    },
  });
  assert.equal(s.orders[0].status, "filled");
  s = reducer(s, {
    type: "message",
    at: 0,
    message: {
      type: "order.unknown",
      browserId: "cancel_1",
      clientOrderId: "1",
      reason: "disconnected",
    },
  });
  assert.equal(s.orders[0].status, "filled");
});

test("an unsolicited old snapshot cannot overwrite a live book", () => {
  let s = reducer(initialState, { type: "message", at: 0, message: snapshot });
  s = reducer(s, {
    type: "message",
    at: 1,
    message: {
      ...snapshot,
      type: "market.book",
      sequence: "9007199254740994",
      bid: "100",
    },
  });
  s = reducer(s, { type: "message", at: 2, message: snapshot });
  assert.equal(s.sequence, "9007199254740994");
  assert.equal(s.market?.bid, "100");
});
