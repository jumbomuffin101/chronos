import assert from "node:assert/strict";
import test from "node:test";
import React from "react";
import { renderToStaticMarkup } from "react-dom/server";
import Home from "./page";

test("terminal renders accessible empty and disconnected-safe controls", () => {
  const html = renderToStaticMarkup(<Home />);
  assert.match(html, /Trading terminal/);
  assert.match(html, /Waiting for the first trade/);
  assert.match(html, /No orders submitted in this session/);
  assert.match(html, /LIMIT PRICE/);
  assert.match(html, /QUANTITY/);
  assert.match(html, /disabled=""/);
  assert.doesNotMatch(html, /NaN/);
});
