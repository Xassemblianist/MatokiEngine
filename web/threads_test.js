// web/threads_test.js — M6_1 çoklu thread öz-testi ve 1/2/4/8 thread Mandelbrot hızlanma ölçümü.
// Paylaşımlı belleği (SharedArrayBuffer) kurar, worker havuzunu başlatır,
// engine_threads_selftest() çalıştırır ve ardından 1, 2, 4, 8 toplam thread ile
// 1024x1024 Mandelbrot süresini ölçerek tabloya yazar.

import { createJitLoader } from "./jit.js";
import { startThreads, stopThreads } from "./threads.js";

const decoder = new TextDecoder("utf-8");
const statusEl = document.getElementById("status");
const outputEl = document.getElementById("output");
const benchBodyEl = document.getElementById("bench-body");
const canvas = document.getElementById("mandel-canvas");
const ctx = canvas ? canvas.getContext("2d") : null;

const logs = [];
function appendLog(line) {
  console.log(line);
  logs.push(line);
  if (outputEl) {
    outputEl.textContent = logs.join("\n");
  }
}

const memory = new WebAssembly.Memory({
  initial: 4096,
  maximum: 16384,
  shared: true,
});
const table = new WebAssembly.Table({ element: "anyfunc", initial: 64 });

function readCString(ptr, len) {
  return decoder.decode(new Uint8Array(memory.buffer, ptr, len).slice());
}

const env = {
  memory,
  __indirect_function_table: table,
  plat_log(ptr, len) {
    const msg = readCString(ptr, len);
    appendLog(msg);
  },
  plat_panic(ptr, len) {
    const msg = readCString(ptr, len);
    console.error(`[PANIC] ${msg}`);
    appendLog(`[PANIC] ${msg}`);
    if (statusEl) {
      statusEl.textContent = `PANIC: ${msg}`;
      statusEl.className = "fail";
    }
  },
  plat_time_ms() {
    return performance.now();
  },
  plat_jit_load: createJitLoader(memory, table),
  plat_download() {},
};

function renderMandelbrotToCanvas(exports) {
  if (!ctx || typeof exports.engine_mandelbrot_framebuffer !== "function") return;
  const ptr = exports.engine_mandelbrot_framebuffer();
  const w = 1024;
  const h = 1024;
  const byteLen = w * h * 4;
  // SharedArrayBuffer doğrudan ImageData'ya verilemediği için tek kopya:
  const copy = new Uint8ClampedArray(byteLen);
  copy.set(new Uint8Array(memory.buffer, ptr, byteLen));
  const imgData = new ImageData(copy, w, h);
  ctx.putImageData(imgData, 0, 0);
}

function median(values) {
  const sorted = values.slice().sort((a, b) => a - b);
  return sorted[Math.floor(sorted.length / 2)];
}

const rasterBodyEl = document.getElementById("raster-body");

function measureSceneFrames(exports, warmupFrames, sampleFrames) {
  let simTime = 1000.0;
  for (let i = 0; i < warmupFrames; i++) {
    simTime += 16.667;
    exports.engine_frame(simTime, 0.0);
  }
  const samples = [];
  for (let i = 0; i < sampleFrames; i++) {
    simTime += 16.667;
    const t0 = performance.now();
    exports.engine_frame(simTime, 0.0);
    const t1 = performance.now();
    samples.push(t1 - t0);
  }
  return median(samples);
}

async function run() {
  if (typeof SharedArrayBuffer === "undefined") {
    throw new Error("SharedArrayBuffer kullanılamıyor (COOP/COEP başlıklarını kontrol edin).");
  }

  const resp = await fetch("./engine.wasm", { cache: "no-store" });
  const bytes = await resp.arrayBuffer();
  const wasmModule = await WebAssembly.compile(bytes);
  const instance = await WebAssembly.instantiate(wasmModule, { env });
  const exports = instance.exports;

  const hc = navigator.hardwareConcurrency || 4;
  appendLog(`Donanım eşzamanlılığı (hardwareConcurrency): ${hc}`);
  appendLog(`7 worker (toplam 8 thread) başlatılıyor...`);

  await startThreads(7, wasmModule, memory, table, {
    instance,
    onLog(text, tid) {
      appendLog(`[W${tid}] ${text}`);
    },
    onPanic(text, tid) {
      appendLog(`[W${tid} PANIC] ${text}`);
      if (statusEl) {
        statusEl.textContent = `WORKER ${tid} PANIC: ${text}`;
        statusEl.className = "fail";
      }
    },
  });

  // 1. Öz-test (1M atomik artırma + 1024x1024 Mandelbrot C ve WAT + Worker JIT çağrısı)
  const rc = exports.engine_threads_selftest();
  if (rc !== 0) {
    statusEl.textContent = `THREADS SELFTEST FAIL (kod: ${rc})`;
    statusEl.className = "fail";
    return;
  }

  if (typeof exports.engine_jit_selftest === "function") {
    const jitRc = exports.engine_jit_selftest();
    if (jitRc !== 0) {
      statusEl.textContent = `JIT SELFTEST FAIL (kod: ${jitRc})`;
      statusEl.className = "fail";
      return;
    }
  }

  renderMandelbrotToCanvas(exports);

  // 2. Motor ilklendirme ve Paralel Rasterizer 1T vs NT bit-bit doğruluk öz-testi
  exports.engine_init(0x12345678);
  exports.engine_set_worker_count(7);
  const rasterRc = exports.engine_raster_parallel_selftest();
  if (rasterRc !== 0) {
    statusEl.textContent = `RASTER PARALLEL SELFTEST FAIL (kod: ${rasterRc})`;
    statusEl.className = "fail";
    return;
  }
  appendLog("Paralel Rasterizer Öz-testi: 6/6 sahne (Sprite, Alpha, Plasma, Branchy, Editor, 3D Depth) 1T vs 8T bit-bit ÖZDEŞ.");

  // Canlı editör yeniden derleme + worker ack testi
  const liveCompileMs = exports.engine_compile_editor_now();
  const postEditRc = exports.engine_raster_parallel_selftest();
  if (postEditRc !== 0) {
    statusEl.textContent = `LIVE EDITOR ACK FAIL (kod: ${postEditRc})`;
    statusEl.className = "fail";
    return;
  }
  appendLog(`Canlı Editör yeniden derleme + 7 worker ack süresi: ${liveCompileMs.toFixed(3)} ms (bit-bit doğrulandı)`);

  statusEl.textContent = "SELFTEST OK — Benchmarklar ölçülüyor...";
  statusEl.className = "ok";

  // 3. Mandelbrot 1024x1024 (C vs WAT SIMD) — 1 / 2 / 4 / 6 / 8 Thread (3 ısınma + 9 tekrar medyanı)
  const threadConfigs = [1, 2, 4, 6, 8];
  const results = [];
  let baseCMs = 0;
  let baseWatMs = 0;

  appendLog("");
  appendLog("=== 1024x1024 Mandelbrot Hızlanma Benchmark (C vs WAT SIMD, grain=4, 3 ısınma + 9 tekrar medyanı) ===");

  // Önce tüm worker'ları TurboFan tier-up için ısıt
  exports.engine_set_worker_count(7);
  exports.engine_mandelbrot_set_use_wat(0);
  exports.engine_bench_mandelbrot_par(4);
  exports.engine_mandelbrot_set_use_wat(1);
  exports.engine_bench_mandelbrot_par(4);

  for (const totalThreads of threadConfigs) {
    const numWorkers = totalThreads - 1;
    exports.engine_set_worker_count(numWorkers);

    // C Mandelbrot
    exports.engine_mandelbrot_set_use_wat(0);
    for (let w = 0; w < 3; w++) {
      if (numWorkers === 0) exports.engine_bench_mandelbrot_seq();
      else exports.engine_bench_mandelbrot_par(4);
    }
    const samplesC = [];
    for (let r = 0; r < 9; r++) {
      samplesC.push(
        numWorkers === 0
          ? exports.engine_bench_mandelbrot_seq()
          : exports.engine_bench_mandelbrot_par(4)
      );
    }
    const medC = median(samplesC);
    if (totalThreads === 1) baseCMs = medC;
    const speedupC = baseCMs / medC;

    // WAT SIMD Mandelbrot
    exports.engine_mandelbrot_set_use_wat(1);
    for (let w = 0; w < 3; w++) {
      if (numWorkers === 0) exports.engine_bench_mandelbrot_seq();
      else exports.engine_bench_mandelbrot_par(4);
    }
    const samplesWat = [];
    for (let r = 0; r < 9; r++) {
      samplesWat.push(
        numWorkers === 0
          ? exports.engine_bench_mandelbrot_seq()
          : exports.engine_bench_mandelbrot_par(4)
      );
    }
    const medWat = median(samplesWat);
    if (totalThreads === 1) baseWatMs = medWat;
    const speedupWat = baseWatMs / medWat;

    results.push({
      totalThreads,
      numWorkers,
      ms: medC,
      speedup: speedupC,
      efficiency: (speedupC / totalThreads) * 100.0,
      watMs: medWat,
      watSpeedup: speedupWat,
      watVsCRatio: medC / medWat,
    });

    const line = `${totalThreads}T (${numWorkers}W) | C: ${medC.toFixed(2)} ms (${speedupC.toFixed(2)}x) | WAT: ${medWat.toFixed(2)} ms (${speedupWat.toFixed(2)}x) | C/WAT: ${(medC / medWat).toFixed(2)}x`;
    console.log(line);
    appendLog(line);
  }

  if (benchBodyEl) {
    benchBodyEl.innerHTML = results
      .map(
        (r) => `
        <tr>
          <td><strong>${r.totalThreads}T</strong></td>
          <td>${r.numWorkers}</td>
          <td>${r.ms.toFixed(2)} ms</td>
          <td><strong>${r.speedup.toFixed(2)}x</strong> (%${r.efficiency.toFixed(0)})</td>
          <td>${r.watMs.toFixed(2)} ms</td>
          <td><strong>${r.watSpeedup.toFixed(2)}x</strong> (${r.watVsCRatio.toFixed(2)}x vs C)</td>
        </tr>`
      )
      .join("");
  }

  // 4. Sahne × Thread Sayısı (1 / 2 / 4 / 6 / 8) Paralel Rasterizer Tablosu
  appendLog("");
  appendLog("=== Sahne × Thread Sayısı (1/2/4/6/8) Paralel Rasterizer (ms/frame, 6 ısınma + 15 kare medyanı) ===");
  exports.engine_set_wat_raster_mode(0);
  exports.engine_set_parallel_dynamic(1);

  const scenes = [
    { key: "sprites_10k", label: "10k Sprite (Opaque)", scene: 0, sprites: 10000, blend: 0 },
    { key: "sprites_50k", label: "50k Sprite (Opaque)", scene: 0, sprites: 50000, blend: 0 },
    { key: "plasma",      label: "Tam Ekran Plazma",    scene: 1, sprites: 10000, blend: 0 },
    { key: "editor",      label: "Canlı Editör",        scene: 3, sprites: 10000, blend: 0 },
  ];

  const rasterSceneResults = [];
  for (const sc of scenes) {
    exports.engine_set_scene(sc.scene, sc.sprites, sc.blend, 1);
    const byThreads = {};
    let baseMs = 0;
    for (const t of threadConfigs) {
      exports.engine_set_worker_count(t - 1);
      const ms = measureSceneFrames(exports, 6, 15);
      if (t === 1) baseMs = ms;
      byThreads[t] = { ms, speedup: baseMs / ms };
    }
    rasterSceneResults.push({ ...sc, byThreads });
    const rowStr = threadConfigs
      .map((t) => `${t}T: ${byThreads[t].ms.toFixed(2)}ms (${byThreads[t].speedup.toFixed(2)}x)`)
      .join(" | ");
    appendLog(`${sc.label.padEnd(20)} -> ${rowStr}`);
  }

  if (rasterBodyEl) {
    rasterBodyEl.innerHTML = rasterSceneResults
      .map((sc) => {
        const b = sc.byThreads;
        return `
        <tr>
          <td><strong>${sc.label}</strong></td>
          <td>${b[1].ms.toFixed(2)} ms</td>
          <td>${b[2].ms.toFixed(2)} ms (<strong>${b[2].speedup.toFixed(2)}x</strong>)</td>
          <td>${b[4].ms.toFixed(2)} ms (<strong>${b[4].speedup.toFixed(2)}x</strong>)</td>
          <td>${b[6].ms.toFixed(2)} ms (<strong>${b[6].speedup.toFixed(2)}x</strong>)</td>
          <td>${b[8].ms.toFixed(2)} ms (<strong>${b[8].speedup.toFixed(2)}x</strong>)</td>
        </tr>`;
      })
      .join("");
  }

  // 5. Dinamik (grain=1 atomik sayaç) vs Statik Karo Dağıtımı (8 Thread)
  appendLog("");
  appendLog("=== Yük Dengeleme: Dinamik (grain=1) vs Statik Karo Bölme (8 Thread) ===");
  const schedResults = [];
  exports.engine_set_worker_count(7);
  for (const sc of scenes) {
    exports.engine_set_scene(sc.scene, sc.sprites, sc.blend, 1);
    exports.engine_set_parallel_dynamic(0);
    const staticMs = measureSceneFrames(exports, 5, 15);
    exports.engine_set_parallel_dynamic(1);
    const dynamicMs = measureSceneFrames(exports, 5, 15);
    schedResults.push({
      label: sc.label,
      staticMs,
      dynamicMs,
      ratio: staticMs / dynamicMs,
    });
    appendLog(
      `${sc.label.padEnd(20)} | Statik: ${staticMs.toFixed(2)} ms | Dinamik (grain=1): ${dynamicMs.toFixed(2)} ms | Kazanç: ${(staticMs / dynamicMs).toFixed(2)}x`
    );
  }

  // 6. Elle Yazılmış WAT Çekirdekleri (edge_block_8x8, blend_alpha_rgba8) Gerçek Sahne Öncesi/Sonrası
  appendLog("");
  appendLog("=== Elle WAT Çekirdekleri Gerçek Sahne Öncesi / Sonrası (1T ve 8T) ===");
  const watScenes = [
    { label: "10k Sprite (BLEND_NONE)",  scene: 0, sprites: 10000, blend: 0, mode: 1, kernel: "edge_block_8x8" },
    { label: "50k Sprite (BLEND_NONE)",  scene: 0, sprites: 50000, blend: 0, mode: 1, kernel: "edge_block_8x8" },
    { label: "10k Sprite (BLEND_ALPHA)", scene: 0, sprites: 10000, blend: 1, mode: 2, kernel: "blend_alpha_rgba8" },
    { label: "50k Sprite (BLEND_ALPHA)", scene: 0, sprites: 50000, blend: 1, mode: 2, kernel: "blend_alpha_rgba8" },
  ];
  const watRealResults = [];
  for (const ws of watScenes) {
    exports.engine_set_scene(ws.scene, ws.sprites, ws.blend, 1);

    // 1 Thread
    exports.engine_set_worker_count(0);
    exports.engine_set_wat_raster_mode(0);
    const before1T = measureSceneFrames(exports, 5, 15);
    exports.engine_set_wat_raster_mode(ws.mode);
    const after1T = measureSceneFrames(exports, 5, 15);

    // 8 Thread
    exports.engine_set_worker_count(7);
    exports.engine_set_wat_raster_mode(0);
    const before8T = measureSceneFrames(exports, 5, 15);
    exports.engine_set_wat_raster_mode(ws.mode);
    const after8T = measureSceneFrames(exports, 5, 15);

    exports.engine_set_wat_raster_mode(0);
    watRealResults.push({
      label: ws.label,
      kernel: ws.kernel,
      before1T,
      after1T,
      gain1T: before1T / after1T,
      before8T,
      after8T,
      gain8T: before8T / after8T,
    });
    appendLog(
      `${ws.label.padEnd(25)} (${ws.kernel}) | 1T: ${before1T.toFixed(2)} -> ${after1T.toFixed(2)} ms (${(before1T / after1T).toFixed(2)}x) | 8T: ${before8T.toFixed(2)} -> ${after8T.toFixed(2)} ms (${(before8T / after8T).toFixed(2)}x)`
    );
  }

  await stopThreads(exports);
  statusEl.textContent = "THREADS & PARALLEL RASTER SELFTEST OK";
  statusEl.className = "ok";

  window.__MATOKI_THREADS_RESULTS__ = {
    ok: true,
    hardwareConcurrency: hc,
    liveCompileMs,
    results,
    rasterSceneResults,
    schedResults,
    watRealResults,
  };
}

run().catch((err) => {
  console.error(err);
  if (statusEl) {
    statusEl.textContent = `Hata: ${err.message}`;
    statusEl.className = "fail";
  }
  appendLog(String(err.stack || err));
  window.__MATOKI_THREADS_RESULTS__ = {
    ok: false,
    error: String(err.message || err),
  };
});
