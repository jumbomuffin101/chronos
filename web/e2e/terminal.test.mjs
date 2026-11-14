import assert from "node:assert/strict";
import { spawn } from "node:child_process";
import { once } from "node:events";
import { mkdir } from "node:fs/promises";
import net from "node:net";
import path from "node:path";
import test from "node:test";
import { chromium } from "playwright";
import AxeBuilder from "@axe-core/playwright";

const root = path.resolve(process.env.CHRONOS_PROJECT_ROOT ?? "..");
async function freePort() {
  const s = net.createServer();
  await new Promise((resolve, reject) => {
    s.once("error", reject);
    s.listen(0, "127.0.0.1", resolve);
  });
  const port = s.address().port;
  await new Promise((resolve) => s.close(resolve));
  return port;
}
async function assertPortAvailable(port) {
  const server = net.createServer();
  await new Promise((resolve, reject) => {
    server.once("error", reject);
    server.listen(port, "127.0.0.1", resolve);
  });
  await new Promise((resolve) => server.close(resolve));
}
async function waitText(locator, text) {
  await locator.filter({ hasText: text }).first().waitFor({ timeout: 10000 });
}

test(
  "two browser sessions trade through the real exchange",
  { timeout: 120000 },
  async (t) => {
    const ports = new Set();
    while (ports.size < 3) ports.add(await freePort());
    const [gateway, adapter, web] = ports;
    const child = spawn(
      process.execPath,
      [path.join(root, "scripts/chronos-dev.mjs")],
      {
        cwd: root,
        env: {
          ...process.env,
          CHRONOS_WEB_DIR: process.cwd(),
          CHRONOS_GATEWAY_PORT: String(gateway),
          CHRONOS_ADAPTER_PORT: String(adapter),
          CHRONOS_WEB_PORT: String(web),
        },
        stdio: ["ignore", "pipe", "pipe"],
      },
    );
    let output = "";
    child.stdout.on("data", (chunk) => {
      output += chunk;
    });
    child.stderr.on("data", (chunk) => {
      output += chunk;
    });
    t.after(async () => {
      if (child.exitCode === null && child.signalCode === null) {
        child.kill("SIGTERM");
        await Promise.race([
          once(child, "close"),
          new Promise((resolve) => {
            const timer = setTimeout(resolve, 6000);
            timer.unref();
          }),
        ]);
        if (child.exitCode === null && child.signalCode === null)
          child.kill("SIGKILL");
      }
      for (const port of [gateway, adapter, web])
        await assertPortAvailable(port);
    });
    await new Promise((resolve, reject) => {
      const timer = setTimeout(
        () => reject(new Error(`startup timeout: ${output}`)),
        45000,
      );
      const poll = setInterval(() => {
        if (output.includes("Chronos local simulation ready:")) {
          clearInterval(poll);
          clearTimeout(timer);
          resolve();
        } else if (child.exitCode !== null) {
          clearInterval(poll);
          clearTimeout(timer);
          reject(new Error(`startup failed: ${output}`));
        }
      }, 100);
    });
    const browser = await chromium.launch({ headless: true });
    t.after(() => browser.close());
    const a = await browser.newContext({
      viewport: { width: 1440, height: 900 },
    });
    const b = await browser.newContext({
      viewport: { width: 1440, height: 900 },
    });
    const seller = await a.newPage(),
      buyer = await b.newPage();
    await Promise.all([
      seller.goto(`http://127.0.0.1:${web}`),
      buyer.goto(`http://127.0.0.1:${web}`),
    ]);
    await Promise.all([
      waitText(seller.locator(".top-meta"), "FEED LIVE"),
      waitText(buyer.locator(".top-meta"), "FEED LIVE"),
    ]);
    await seller.getByRole("button", { name: "SELL", exact: true }).click();
    await seller.getByLabel("LIMIT PRICE ticks").fill("100");
    await seller.getByLabel("QUANTITY units").fill("5");
    await seller.getByRole("button", { name: "Submit sell order" }).click();
    await waitText(seller.locator("tbody tr"), "resting");
    await waitText(buyer.locator(".overview"), "5 units");
    assert.equal(
      await buyer.locator("tbody tr").count(),
      0,
      "seller private order leaked to buyer",
    );
    await buyer.getByLabel("ORDER TYPE").selectOption("market");
    await buyer.getByLabel("QUANTITY units").fill("3");
    await buyer.getByRole("button", { name: "Submit buy order" }).click();
    await waitText(buyer.locator("tbody tr"), "filled");
    await waitText(seller.locator("tbody tr"), "partial fill");
    await waitText(buyer.locator(".trade-table"), "100");
    await waitText(seller.locator(".overview"), "2 units");
    assert.match(await buyer.locator("tbody").innerText(), /BUY/);
    assert.doesNotMatch(await buyer.locator("tbody").innerText(), /SELL/);
    const screenshots = process.env.CHRONOS_SCREENSHOTS;
    if (screenshots) await mkdir(screenshots, { recursive: true });
    for (const [name, width, height] of [
      ["desktop-1440", 1440, 900],
      ["desktop-1920", 1920, 1080],
      ["tablet-768", 768, 1024],
      ["mobile-375", 375, 812],
    ]) {
      await seller.setViewportSize({ width, height });
      await seller.waitForFunction(
        () => document.documentElement.scrollWidth <= innerWidth,
        null,
        { timeout: 5000 },
      );
      assert.equal(
        await seller.evaluate(
          () => document.documentElement.scrollWidth <= innerWidth,
        ),
        true,
        `${name} overflows`,
      );
      if (screenshots)
        await seller.screenshot({
          path: path.join(screenshots, `${name}.png`),
          fullPage: true,
        });
    }
    const accessibility = await new AxeBuilder({ page: seller })
      .withTags(["wcag2a", "wcag2aa"])
      .analyze();
    assert.deepEqual(
      accessibility.violations.map((v) => ({
        id: v.id,
        nodes: v.nodes.map((n) => ({
          target: n.target,
          summary: n.failureSummary,
        })),
      })),
      [],
    );
    await seller.setViewportSize({ width: 375, height: 812 });
    const orders = seller.getByRole("region", { name: /Session orders/ });
    await orders.focus();
    assert.equal(
      await orders.evaluate((element) => element === document.activeElement),
      true,
    );
    await seller.keyboard.press("ArrowRight");
    await seller.waitForFunction(
      () => document.querySelector(".scroll-table").scrollLeft > 0,
    );
    await seller.getByRole("button", { name: "Cancel", exact: true }).click();
    await waitText(seller.locator("tbody tr"), "cancelled");
    await seller.getByLabel("LIMIT PRICE ticks").fill("1000001");
    await seller.getByLabel("QUANTITY units").fill("1");
    await seller.getByRole("button", { name: "Submit sell order" }).click();
    await waitText(seller.locator("tbody tr"), "resting");
    await buyer.getByLabel("QUANTITY units").fill("1");
    await buyer.getByRole("button", { name: "Submit buy order" }).click();
    await waitText(buyer.locator("tbody tr"), "Market price bound");
    await seller.getByRole("button", { name: "Cancel", exact: true }).click();
    await waitText(seller.locator("tbody tr").first(), "cancelled");
    await seller.getByLabel("LIMIT PRICE ticks").fill("101");
    await seller.getByRole("button", { name: "Submit sell order" }).click();
    await waitText(seller.locator("tbody tr").first(), "resting");
    child.kill("SIGTERM");
    await waitText(seller.locator(".top-meta"), "BROWSER DISCONNECTED");
    await waitText(seller.locator("tbody tr").first(), "uncertain");
    await waitText(seller.locator(".session-warning"), "cannot recover");
  },
);
