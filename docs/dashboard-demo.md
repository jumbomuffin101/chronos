# Chronos dashboard demonstration

Chronos is a local exchange simulator, not a real-money trading platform.
Use two independent browser windows or profiles at `http://127.0.0.1:3000`.

1. Build the Debug gateway, install web dependencies with `npm ci`, then run
   `node scripts/chronos-dev.mjs` from the repository. Wait for the ready line.
2. Open two sessions and wait for **BROWSER CONNECTED**, **EXCHANGE CONNECTED**
   and **FEED LIVE**. Both start from an exchange snapshot of symbol 1.
3. In session A select Sell, Limit, price `100` ticks and quantity `5`, then
   submit. The ask becomes 100 with five units and A sees its resting order.
4. In session B select Buy, Market, quantity `3`, then submit. Three units
   trade at the resting price of 100. B sees a fill; A sees a partial fill with
   two remaining. Each private order table belongs to its own session.
5. Both sessions see the public trade, observed-price chart, and updated ask of
   two units. The chart has only trades observed since the session began.
6. In A click Cancel for the remainder. The ask clears and A sees cancellation.
7. In A place a sell limit for one at `1000001`. In B submit a market buy for
   one. The exchange rejects it because it exceeds the configured market-price
   risk bound. Cancel A's remaining order.
8. Explain that a deterministic, single-owner C++ matching engine applies
   price-time priority; the Node adapter translates the versioned TCP protocol
   for the browser and never makes matching or risk decisions.

Stop the launcher with Ctrl+C. Disconnecting does not cancel resting orders;
orders, reports and account state cannot be recovered after a session is lost.
The [desktop screenshot](assets/dashboard/desktop-1440.png) shows a real
simulated partial fill. See [development](development.md) for WSL-native setup
and reproducible browser screenshots.
