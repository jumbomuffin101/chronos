#!/usr/bin/env node
import { spawn } from "node:child_process";
import { access } from "node:fs/promises";
import http from "node:http";
import net from "node:net";
import path from "node:path";
import { fileURLToPath } from "node:url";

const root = path.resolve(path.dirname(fileURLToPath(import.meta.url)), "..");
const web = path.resolve(process.env.CHRONOS_WEB_DIR ?? path.join(root, "web"));
const gateway = path.resolve(
  process.env.CHRONOS_GATEWAY ?? path.join(root, "build/debug/chronos_gateway"),
);
const ports = [
  process.env.CHRONOS_GATEWAY_PORT ?? "8088",
  process.env.CHRONOS_ADAPTER_PORT ?? "8090",
  process.env.CHRONOS_WEB_PORT ?? "3000",
].map(Number);
const children = [];
let stopping = false;
let failed = false;
function stop() {
  if (stopping) return;
  stopping = true;
  for (const child of children)
    if (child.pid !== undefined) {
      try {
        process.kill(-child.pid, "SIGTERM");
      } catch {
        /* Already exited. */
      }
    }
  const timer = setTimeout(() => {
    for (const child of children)
      if (
        child.pid !== undefined &&
        child.exitCode === null &&
        child.signalCode === null
      ) {
        try {
          process.kill(-child.pid, "SIGKILL");
        } catch {
          /* Already exited. */
        }
      }
  }, 3000);
  timer.unref();
}
process.on("SIGINT", stop);
process.on("SIGTERM", stop);
function launch(name, command, args, env = {}, readyMarker) {
  const child = spawn(command, args, {
    cwd: web,
    env: { ...process.env, ...env },
    detached: true,
    stdio: ["ignore", "pipe", "pipe"],
  });
  children.push(child);
  child.announced = false;
  let startupOutput = "";
  child.stdout.on("data", (data) => {
    if (!child.announced && readyMarker) {
      startupOutput = (startupOutput + data.toString()).slice(-8192);
      child.announced = startupOutput.includes(readyMarker);
    }
  });
  for (const stream of [child.stdout, child.stderr])
    stream.on("data", (data) => process.stdout.write(`[${name}] ${data}`));
  child.on("error", (error) => {
    console.error(`[${name}] ${error.message}`);
    failed = true;
    stop();
  });
  child.on("exit", (code, signal) => {
    if (!stopping) {
      console.error(`[${name}] exited unexpectedly (${signal ?? code})`);
      failed = true;
      stop();
    }
  });
  return child;
}
function probePort(port) {
  return new Promise((resolve, reject) => {
    const s = net.createServer();
    s.once("error", reject);
    s.listen(port, "127.0.0.1", () => s.close(resolve));
  });
}
function probeHttp(url) {
  return new Promise((resolve) => {
    const req = http.get(url, { timeout: 500 }, (res) => {
      res.resume();
      resolve(res.statusCode === 200);
    });
    req.on("timeout", () => req.destroy());
    req.on("error", () => resolve(false));
  });
}
function probeTcp(port) {
  return new Promise((resolve) => {
    const s = net.connect({ port, host: "127.0.0.1" });
    s.setTimeout(500);
    s.on("connect", () => {
      s.destroy();
      resolve(true);
    });
    s.on("error", () => resolve(false));
    s.on("timeout", () => {
      s.destroy();
      resolve(false);
    });
  });
}
async function ready(name, probe) {
  const deadline = Date.now() + 30000;
  while (!stopping && Date.now() < deadline) {
    if (await probe()) return;
    await new Promise((resolve) => setTimeout(resolve, 100));
  }
  throw new Error(`${name} did not become ready within 30 seconds`);
}
try {
  if (process.platform !== "linux")
    throw new Error("Use Linux or WSL to run the loopback gateway.");
  if (Number(process.versions.node.split(".")[0]) !== 22)
    throw new Error("Node.js 22 LTS is required (see web/.nvmrc).");
  if (
    ports.some((p) => !Number.isInteger(p) || p < 1 || p > 65535) ||
    new Set(ports).size !== ports.length
  )
    throw new Error(
      "Gateway, adapter and web ports must be distinct integers from 1 to 65535.",
    );
  for (const file of [
    gateway,
    path.join(web, "node_modules/tsx/dist/cli.mjs"),
    path.join(web, "node_modules/next/dist/bin/next"),
  ]) {
    try {
      await access(file);
    } catch {
      throw new Error(
        `Missing ${file}. Build the Debug gateway and run npm ci in the web directory.`,
      );
    }
  }
  for (const port of ports)
    try {
      await probePort(port);
    } catch {
      throw new Error(
        `Port ${port} is unavailable on 127.0.0.1; no existing process was stopped.`,
      );
    }
  const [gp, ap, wp] = ports;
  const gatewayChild = launch(
    "gateway",
    gateway,
    [String(gp)],
    {},
    "Chronos gateway listening on",
  );
  await ready("gateway", () => gatewayChild.announced && probeTcp(gp));
  const origin = `http://127.0.0.1:${wp}`;
  const adapterChild = launch(
    "adapter",
    process.execPath,
    [path.join(web, "node_modules/tsx/dist/cli.mjs"), "adapter/server.ts"],
    {
      CHRONOS_GATEWAY_PORT: String(gp),
      CHRONOS_ADAPTER_PORT: String(ap),
      CHRONOS_WEB_ORIGIN: origin,
    },
    "Chronos adapter:",
  );
  await ready(
    "adapter",
    () => adapterChild.announced && probeHttp(`http://127.0.0.1:${ap}/health`),
  );
  const dashboardChild = launch(
    "dashboard",
    process.execPath,
    [
      path.join(web, "node_modules/next/dist/bin/next"),
      "dev",
      "--hostname",
      "127.0.0.1",
      "--port",
      String(wp),
    ],
    {
      NEXT_PUBLIC_ADAPTER_URL: `ws://127.0.0.1:${ap}/ws`,
      NEXT_TELEMETRY_DISABLED: "1",
    },
    "Ready in",
  );
  await ready("dashboard", () => dashboardChild.announced && probeHttp(origin));
  console.log(
    `\nChronos local simulation ready: ${origin}\nAdapter: http://127.0.0.1:${ap}/health  Gateway: 127.0.0.1:${gp}\nPress Ctrl+C to stop only these services.`,
  );
  await Promise.all(
    children.map(
      (child) => new Promise((resolve) => child.once("close", resolve)),
    ),
  );
} catch (error) {
  console.error(`Chronos startup: ${error.message}`);
  failed = true;
  stop();
}
process.exitCode = failed ? 1 : 0;
