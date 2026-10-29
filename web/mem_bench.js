// web/mem_bench.js — MATOKI L0 Bellek Modeli Benchmark Koşucusu.
// Wasm modülünü paylaşımlı bellek ve worker'larla yükler, ölçümleri yapar ve DOM'a yazar.

import { startThreads, stopThreads } from "./threads.js";

const statusTextEl = document.getElementById("status-text");
const runBtn = document.getElementById("run-btn");
const outputEl = document.getElementById("output");
const tbodyCopy = document.getElementById("tbody-copy");
const tbodyParallel = document.getElementById("tbody-parallel");
const tbodyHash = document.getElementById("tbody-hash");
const tbodyGrow = document.getElementById("tbody-grow");
const h4Result = document.getElementById("h4-result");
const fourGbStatus = document.getElementById("four-gb-status");
const mem64Status = document.getElementById("mem64-status");

const decoder = new TextDecoder("utf-8");
let sharedMemory = null;
let sharedTable = null;
let wasmInstance = null;
let wasmModule = null;
const logs = [];

function log(msg) {
  console.log(msg);
  logs.push(msg);
  if (outputEl) {
    outputEl.textContent = logs.join("\n");
    outputEl.scrollTop = outputEl.scrollHeight;
  }
}

function readCString(ptr, len) {
  const bytes = new Uint8Array(sharedMemory.buffer, ptr, len);
  if (typeof SharedArrayBuffer !== "undefined" && sharedMemory.buffer instanceof SharedArrayBuffer) {
    return decoder.decode(bytes.slice());
  }
  return decoder.decode(bytes);
}

function detectMemory64() {
  // (module (memory i64 1))
  const mem64Wasm = new Uint8Array([
    0x00, 0x61, 0x73, 0x6d, 0x01, 0x00, 0x00, 0x00,
    0x05, 0x03, 0x01, 0x04, 0x01
  ]);
  const supported = WebAssembly.validate(mem64Wasm);
  if (supported) {
    mem64Status.innerHTML = '<span class="ok">✔ memory64 Bu Ortamda Destekleniyor (Aktif)</span>';
  } else {
    mem64Status.innerHTML = '<span class="fail">✖ memory64 Desteklenmiyor (Chrome için: --js-flags="--experimental-wasm-memory64")</span>';
  }
  return supported;
}

async function testFourGbMemory() {
  let resText = "";
  try {
    const mem4g = new WebAssembly.Memory({ initial: 65536, maximum: 65536, shared: true });
    resText += `<span class="ok">✔ 4 GB (65536 sayfa) SharedArrayBuffer başarıyla oluşturuldu!</span><br/>`;
    const v = new Uint8Array(mem4g.buffer);
    v[0] = 0x11;
    v[2 * 1024 * 1024 * 1024] = 0x22;
    v[4294967295] = 0x33;
    resText += `✔ 4 GB sınır erişimi (Adres 0xFFFFFFFF = 0x${v[4294967295].toString(16).toUpperCase()}) başarılı.<br/>`;
  } catch (err) {
    resText += `<span class="fail">✖ 4 GB SAB Doğrudan Tahsisi Başarısız: ${err.message}</span><br/>`;
    try {
      const mem1g = new WebAssembly.Memory({ initial: 16384, maximum: 65536, shared: true });
      resText += `✔ 1 GB başlangıç + 4 GB tavan SAB oluşturuldu. Tarayıcı 4 GB rezervasyonuna izin veriyor.<br/>`;
    } catch (e2) {
      resText += `<span class="fail">✖ 1 GB başlangıç / 4 GB tavan da reddedildi: ${e2.message}</span><br/>`;
    }
  }
  fourGbStatus.innerHTML = resText;
}

async function initWasm() {
  log("wasm modülü yükleniyor (web/engine.wasm)...");
  const resp = await fetch("./engine.wasm");
  const bytes = await resp.arrayBuffer();
  wasmModule = await WebAssembly.compile(bytes);

  // 256 MB başlangıç, 1 GB tavan paylaşımlı bellek (D-003)
  sharedMemory = new WebAssembly.Memory({
    initial: 4096,
    maximum: 16384,
    shared: true,
  });

  // Dinamik tablo
  const imports = WebAssembly.Module.imports(wasmModule);
  let tableInit = 64;
  for (const imp of imports) {
    if (imp.module === "env" && imp.name === "__indirect_function_table" && imp.kind === "table") {
      if (imp.type && typeof imp.type.minimum === "number") {
        tableInit = Math.max(tableInit, imp.type.minimum);
      }
    }
  }
  sharedTable = new WebAssembly.Table({ element: "anyfunc", initial: tableInit });

  const importObj = {
    env: {
      memory: sharedMemory,
      __indirect_function_table: sharedTable,
      plat_log: (ptr, len) => {
        const str = readCString(ptr, len);
        log(str);
      },
      plat_panic: (ptr, len) => {
        const str = readCString(ptr, len);
        log("PANIC: " + str);
      },
      plat_time_ms: () => performance.now(),
      plat_jit_load: () => -1,
      plat_download: () => {},
    },
  };

  wasmInstance = await WebAssembly.instantiate(wasmModule, importObj);
  log("wasm modülü yüklendi. Fonksiyonlar hazır.");
}

function parseAndRenderReport(rawReport) {
  // 1. Kopyalama Kıyaslaması
  const lines = rawReport.split("\n");
  let section = 0;

  const copyRows = [];
  const parallelRows = [];
  const hashRows = [];
  const growRows = [];
  let h4Text = "";

  for (const line of lines) {
    const t = line.trim();
    if (t.includes("--- 1. Kopyalama Kıyaslaması")) { section = 1; continue; }
    if (t.includes("--- 2. 16 MB Durum Bölgesi Snapshot")) { section = 2; continue; }
    if (t.includes("--- 3. Paralel Kopya")) { section = 3; continue; }
    if (t.includes("--- 4. Durum Özeti")) { section = 4; continue; }
    if (t.includes("--- 5. memory.grow Maliyeti")) { section = 5; continue; }

    if (section === 1 && t.includes("\t") && !t.startsWith("Boyut")) {
      const parts = t.split("\t");
      if (parts.length >= 5) {
        copyRows.push(parts);
      }
    } else if (section === 2) {
      if (t.startsWith("16 MB") || t.startsWith("H4")) {
        h4Text += t + "<br/>";
      }
    } else if (section === 3) {
      if (t.startsWith("Hedef:")) {
        parallelRows.push({ isHeader: true, text: t });
      } else if (t.includes("\t") && !t.startsWith("Thread")) {
        const parts = t.split("\t");
        parallelRows.push({ isHeader: false, parts });
      }
    } else if (section === 4) {
      if (t.includes("\t") && !t.startsWith("Boyut")) {
        const parts = t.split("\t");
        if (parts.length >= 4) {
          hashRows.push(parts);
        }
      }
    } else if (section === 5) {
      if (t.includes("sayfa")) {
        growRows.push(t);
      }
    }
  }

  // Render Copy
  if (copyRows.length > 0) {
    tbodyCopy.innerHTML = copyRows.map(r => `
      <tr>
        <td><strong>${r[0]}</strong></td>
        <td><code>${r[1]}</code></td>
        <td class="num">${r[2]}</td>
        <td class="num">${r[3]}</td>
        <td class="num"><strong>${r[4]}</strong></td>
      </tr>
    `).join("");
  }

  // Render H4
  if (h4Text) {
    const isPass = h4Text.includes("GECERLI") || h4Text.includes("DOGRULANDI");
    h4Result.innerHTML = `
      <div style="margin-bottom: 8px;">
        <span class="${isPass ? 'badge-pass' : 'badge-warn'}">
          ${isPass ? 'H4 DOĞRULANDI (GEÇTİ)' : 'H4 EŞİĞİ AŞILDI'}
        </span>
      </div>
      <div>${h4Text}</div>
    `;
  }

  // Render Parallel
  if (parallelRows.length > 0) {
    let curHeader = "";
    const htmlParts = [];
    for (const item of parallelRows) {
      if (item.isHeader) {
        curHeader = item.text.replace("Hedef: ", "");
      } else {
        htmlParts.push(`
          <tr>
            <td><strong>${curHeader}</strong></td>
            <td>${item.parts[0]}</td>
            <td class="num">${item.parts[1]}</td>
            <td class="num"><strong>${item.parts[2]}</strong></td>
            <td class="num">${item.parts[3]}</td>
          </tr>
        `);
      }
    }
    tbodyParallel.innerHTML = htmlParts.join("");
  }

  // Render Hash
  if (hashRows.length > 0) {
    tbodyHash.innerHTML = hashRows.map(r => `
      <tr>
        <td><strong>${r[0]}</strong></td>
        <td class="num">${r[1]}</td>
        <td class="num"><strong>${r[2]}</strong></td>
        <td><code>${r[3]}</code></td>
      </tr>
    `).join("");
  }

  // Render Grow
  if (growRows.length > 0) {
    tbodyGrow.innerHTML = growRows.map(line => {
      const idx = line.indexOf(":");
      const title = line.substring(0, idx).trim();
      const val = line.substring(idx + 1).trim();
      return `
        <tr>
          <td><strong>${title}</strong></td>
          <td class="num">${val.split(" ")[0]}</td>
          <td>${val}</td>
        </tr>
      `;
    }).join("");
  }
}

async function runBenchmark() {
  runBtn.disabled = true;
  statusTextEl.textContent = "Ölçümler yürütülüyor (Wasm + Worker havuzu)...";
  logs.length = 0;

  try {
    if (!wasmInstance) {
      await initWasm();
    }

    // 7 Worker başlat (toplam 8 thread, threads.js)
    log("7 Web Worker başlatılıyor (toplam 8 thread)...");
    try {
      await startThreads(7, wasmModule, sharedMemory, sharedTable, {
        instance: wasmInstance,
        onLog: (text, tid) => log(`[Worker ${tid}] ${text}`),
        onPanic: (text, tid) => log(`[Worker ${tid} PANIC] ${text}`),
      });
      log("Worker havuzu hazır (7 worker + 1 ana thread = 8 thread).");
    } catch (wErr) {
      log("Worker başlatma uyarısı: " + wErr.message);
    }

    // Wasm C benchmark'ını çalıştır
    log("engine_bench_mem_run() çağrılıyor...");
    const rc = wasmInstance.exports.engine_bench_mem_run();
    log(`engine_bench_mem_run() tamamlandı (kod: ${rc})`);

    const rptPtr = wasmInstance.exports.engine_bench_mem_report_ptr();
    const rptLen = wasmInstance.exports.engine_bench_mem_report_len();
    const rptText = readCString(rptPtr, rptLen);

    // Tabloları doldur
    parseAndRenderReport(rptText);

    // 4 GB ve memory64 testleri
    await testFourGbMemory();
    detectMemory64();

    statusTextEl.textContent = "Tüm ölçümler başarıyla tamamlandı.";
    window.benchComplete = true;
  } catch (err) {
    log("HATA: " + err.message);
    statusTextEl.textContent = "Hata oluştu: " + err.message;
  } finally {
    runBtn.disabled = false;
  }
}

runBtn.addEventListener("click", runBenchmark);

// Açılışta ortam tespitleri
window.addEventListener("DOMContentLoaded", () => {
  detectMemory64();
  // Kullanıcı butona basmadan otomatik başlat
  runBenchmark();
});
