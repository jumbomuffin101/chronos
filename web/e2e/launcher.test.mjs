import assert from "node:assert/strict";
import { spawn } from "node:child_process";
import { once } from "node:events";
import { mkdtemp, writeFile, readFile, rm } from "node:fs/promises";
import net from "node:net";
import os from "node:os";
import path from "node:path";
import test from "node:test";

test(
  "launcher refuses an occupied port without stopping its owner",
  { timeout: 10000 },
  async () => {
    const listener = net.createServer();
    await new Promise((resolve, reject) => {
      listener.once("error", reject);
      listener.listen(0, "127.0.0.1", resolve);
    });
    try {
      const occupied = listener.address().port;
      const root = path.resolve(process.env.CHRONOS_PROJECT_ROOT ?? "..");
      const child = spawn(
        process.execPath,
        [path.join(root, "scripts/chronos-dev.mjs")],
        {
          cwd: root,
          env: {
            ...process.env,
            CHRONOS_WEB_DIR: process.cwd(),
            CHRONOS_GATEWAY_PORT: String(occupied),
            CHRONOS_ADAPTER_PORT: "8090",
            CHRONOS_WEB_PORT: "3000",
          },
          stdio: ["ignore", "pipe", "pipe"],
        },
      );
      let error = "";
      child.stderr.on("data", (chunk) => {
        error += chunk;
      });
      const [code] = await once(child, "close");
      assert.equal(code, 1);
      assert.match(error, /unavailable/);
      assert.equal(listener.listening, true);
    } finally {
      await new Promise((resolve) => listener.close(resolve));
    }
  },
);

test(
  "launcher stops a descendant after its owned gateway leader fails",
  { timeout: 12000 },
  async () => {
    const dir = await mkdtemp(path.join(os.tmpdir(), "chronos-launcher-test-"));
    const marker = path.join(dir, "pid");
    const fixture = path.join(dir, "gateway");
    let descendant;
    let descendantAlive = false;
    try {
      await writeFile(
        fixture,
        `#!/bin/sh\nsleep 30 &\necho $! > '${marker}'\nexit 1\n`,
        { mode: 0o700 },
      );
      const root = path.resolve(process.env.CHRONOS_PROJECT_ROOT ?? "..");
      const ports = new Set();
      while (ports.size < 3) {
        const socket = net.createServer();
        await new Promise((resolve) => socket.listen(0, "127.0.0.1", resolve));
        ports.add(socket.address().port);
        await new Promise((resolve) => socket.close(resolve));
      }
      const [gateway, adapter, web] = ports;
      const child = spawn(
        process.execPath,
        [path.join(root, "scripts/chronos-dev.mjs")],
        {
          cwd: root,
          env: {
            ...process.env,
            CHRONOS_WEB_DIR: process.cwd(),
            CHRONOS_GATEWAY: fixture,
            CHRONOS_GATEWAY_PORT: String(gateway),
            CHRONOS_ADAPTER_PORT: String(adapter),
            CHRONOS_WEB_PORT: String(web),
          },
          stdio: ["ignore", "pipe", "pipe"],
        },
      );
      child.stdout.resume();
      child.stderr.resume();
      const [code] = await once(child, "close");
      assert.equal(code, 1);
      descendant = Number(await readFile(marker, "utf8"));
      let stat;
      try {
        stat = await readFile(`/proc/${descendant}/stat`, "utf8");
      } catch {
        /* Reaped. */
      }
      descendantAlive = Boolean(stat && !/\) Z /.test(stat));
      assert.equal(
        descendantAlive,
        false,
        "owned descendant survived launcher shutdown",
      );
    } finally {
      if (descendant && descendantAlive) {
        try {
          process.kill(descendant, "SIGKILL");
        } catch {
          /* Already stopped. */
        }
      }
      await rm(dir, { recursive: true, force: true });
    }
  },
);
