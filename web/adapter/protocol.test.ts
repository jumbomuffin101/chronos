import assert from "node:assert/strict";
import test from "node:test";
import {
  Decoder,
  frame,
  Type,
  submit,
  uint,
  U64_MAX,
  decode,
} from "./protocol";

test("encodes large integers and decodes arbitrary fragmentation and coalescing", () => {
  const first = submit(
    U64_MAX,
    U64_MAX,
    0xffffffff,
    "sell",
    "limit",
    9223372036854775807n,
    U64_MAX,
  );
  assert.equal(first.readUInt32BE(0), 46);
  assert.equal(first.readBigUInt64BE(8), U64_MAX);
  assert.equal(first.readBigUInt64BE(16), U64_MAX);
  assert.equal(first.readBigInt64BE(30), 9223372036854775807n);
  const second = frame(Type.Hello, 7n);
  const bytes = Buffer.concat([first, second]);
  for (let split = 0; split <= bytes.length; ++split) {
    const decoder = new Decoder(),
      frames: bigint[] = [];
    decoder.push(bytes.subarray(0, split), (f) => frames.push(f.requestId));
    decoder.push(bytes.subarray(split), (f) => frames.push(f.requestId));
    decoder.finish();
    assert.deepEqual(frames, [U64_MAX, 7n]);
  }
});

test("rejects oversized, malformed, truncated, and imprecise values", () => {
  assert.throws(() => uint("18446744073709551616"));
  assert.throws(() => uint(9007199254740993));
  assert.throws(() => uint("01"));
  const truncated = new Decoder();
  truncated.push(frame(Type.Hello, 0n).subarray(0, 5), () => {});
  assert.throws(() => truncated.finish());
  for (const mutate of [
    (b: Buffer) => b.writeUInt32BE(4097),
    (b: Buffer) => {
      b[4] = 2;
    },
    (b: Buffer) => {
      b[5] = 255;
    },
    (b: Buffer) => {
      b[7] = 1;
    },
    (b: Buffer) => b.writeUInt32BE(17),
  ]) {
    const bytes = frame(Type.Hello, 0n);
    mutate(bytes);
    assert.throws(() => new Decoder().push(bytes, () => {}));
  }
});

test("execution values remain decimal strings including signed price", () => {
  const payload = Buffer.alloc(69);
  payload.writeBigUInt64BE(U64_MAX, 0);
  payload.writeBigInt64BE(-9223372036854775808n, 45);
  payload.writeBigUInt64BE(U64_MAX, 53);
  const decoded = decode({ type: Type.Execution, requestId: 1n, payload });
  assert.equal(decoded.sequence, "18446744073709551615");
  assert.equal(decoded.price, "-9223372036854775808");
  assert.equal(decoded.quantity, "18446744073709551615");
});
