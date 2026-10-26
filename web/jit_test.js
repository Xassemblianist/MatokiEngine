// web/jit_test.js — engine.wasm JIT öz-testi ve doğrudan C çağrısı vs call_indirect benchmark'ı.
// platform.js'e dokunmadan kendi minimal yükleyicisini kurar, jit.js'den createJitLoader import eder.

import { createJitLoader } from "./jit.js";

const decoder = new TextDecoder("utf-8");
const statusEl = document.getElementById("status");
const outputEl = document.getElementById("output");

const logs = [];
function appendLog(line) {
  logs.push(line);
  outputEl.textContent = logs.join("\n");
}

const memory = new WebAssembly.Memory({ initial: 4096, maximum: 16384 });
const table = new WebAssembly.Table({ element: "anyfunc", initial: 1 });

function readCString(ptr, len) {
  return decoder.decode(new Uint8Array(memory.buffer, ptr, len));
}

const env = {
  memory,
  __indirect_function_table: table,
  plat_log(ptr, len) {
    const msg = readCString(ptr, len);
    console.log(msg);
    appendLog(msg);
  },
  plat_panic(ptr, len) {
    const msg = readCString(ptr, len);
    console.error(`[PANIC] ${msg}`);
    appendLog(`[PANIC] ${msg}`);
    statusEl.textContent = `PANIC: ${msg}`;
    statusEl.className = "fail";
  },
  plat_time_ms() {
    return performance.now();
  },
  plat_jit_load: createJitLoader(memory, table),
};

async function run() {
  const resp = await fetch("./engine.wasm", { cache: "no-store" });
  const bytes = await resp.arrayBuffer();
  const { instance } = await WebAssembly.instantiate(bytes, { env });
  const exports = instance.exports;

  const rc = exports.engine_jit_selftest();
  if (rc !== 0) {
    statusEl.textContent = `JIT FAIL (kod: ${rc})`;
    statusEl.className = "fail";
    return;
  }

  statusEl.textContent = "JIT OK";
  statusEl.className = "ok";

  // Isınma (Warmup — TurboFan tier-up için)
  const N = 10_000_000;
  exports.engine_jit_bench_direct(2_000_000);
  exports.engine_jit_bench_indirect(2_000_000);

  // 10M doğrudan C çağrısı vs call_indirect ile JIT fonksiyonu
  const t0 = performance.now();
  const resDirect = exports.engine_jit_bench_direct(N);
  const t1 = performance.now();
  const directMs = t1 - t0;

  const t2 = performance.now();
  const resIndirect = exports.engine_jit_bench_indirect(N);
  const t3 = performance.now();
  const indirectMs = t3 - t2;

  appendLog("");
  appendLog("=== Çağrı Maliyeti Benchmark (10,000,000 iterasyon) ===");
  appendLog(`Doğrudan C çağrısı (noinline)  : ${directMs.toFixed(2)} ms (${((directMs * 1e6) / N).toFixed(2)} ns/çağrı) [sonuç=${resDirect}]`);
  appendLog(`call_indirect (JIT fonksiyonu) : ${indirectMs.toFixed(2)} ms (${((indirectMs * 1e6) / N).toFixed(2)} ns/çağrı) [sonuç=${resIndirect}]`);
  appendLog(`Oran (indirect / direct)       : ${(indirectMs / Math.max(directMs, 0.001)).toFixed(2)}x`);
}

run().catch((err) => {
  console.error(err);
  statusEl.textContent = `Hata: ${err.message}`;
  statusEl.className = "fail";
  appendLog(String(err.stack || err));
});
