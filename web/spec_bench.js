// web/spec_bench.js — MATOKI F0_B (ARCH_V2 H1) arketip başına JIT ECS özelleştirme tarayıcı koşucusu.
// engine_spec_bench_run() export'unu çağırır ve sonuç tablosunu DOM'a yazar.

import { createJitLoader } from "./jit.js";

const decoder = new TextDecoder("utf-8");
const statusTextEl = document.getElementById("status-text");
const metaBarEl = document.getElementById("meta-bar");
const tbodyEl = document.getElementById("bench-tbody");
const outputEl = document.getElementById("output");
const entitySelectEl = document.getElementById("entity-select");
const rerunBtn = document.getElementById("rerun-btn");

let memory = null;
let table = null;
let exportsObj = null;
const logs = [];

function readCString(ptr, len) {
  const bytes = new Uint8Array(memory.buffer, ptr, len);
  if (typeof SharedArrayBuffer !== "undefined" && memory.buffer instanceof SharedArrayBuffer) {
    return decoder.decode(bytes.slice());
  }
  return decoder.decode(bytes);
}

function appendLog(line) {
  logs.push(line);
  outputEl.textContent = logs.join("\n");
}

function createTableForModule(mod) {
  const imports = WebAssembly.Module.imports(mod);
  let initial = 64;
  for (const imp of imports) {
    if (imp.module === "env" && imp.name === "__indirect_function_table" && imp.kind === "table") {
      if (imp.type && typeof imp.type.minimum === "number") {
        initial = Math.max(initial, imp.type.minimum);
      } else if (imp.type && typeof imp.type.initial === "number") {
        initial = Math.max(initial, imp.type.initial);
      }
    }
  }
  return new WebAssembly.Table({ element: "anyfunc", initial });
}

function renderReportTable(reportStr) {
  tbodyEl.innerHTML = "";
  metaBarEl.textContent = "";
  const tsvIdx = reportStr.indexOf("#TSV\n");
  if (tsvIdx < 0) return;
  const lines = reportStr.slice(tsvIdx + 5).trim().split("\n");
  for (const line of lines) {
    if (!line.trim()) continue;
    const parts = line.split("\t");
    if (parts[0] === "#META" && parts.length >= 5) {
      const [, entities, archCount, compileMs, wasmBytes] = parts;
      metaBarEl.textContent =
        `Varlık: ${entities} | Arketip: ${archCount} | JIT Derleme Süresi (12 modül): ${compileMs} ms | Toplam Wasm Boyutu: ${wasmBytes} bayt`;
      continue;
    }
    if (parts.length < 8) continue;
    const [name, bitExact, baseMed, baseMin, specMed, specMin, ratio, status] = parts;
    const tr = document.createElement("tr");

    const tdName = document.createElement("td");
    tdName.textContent = bitExact === "1" ? name : `${name} [BİT FARKI!]`;
    tr.appendChild(tdName);

    const tdBase = document.createElement("td");
    tdBase.className = "num";
    tdBase.textContent = `${baseMed} ms (min ${baseMin})`;
    tr.appendChild(tdBase);

    const tdSpec = document.createElement("td");
    tdSpec.className = "num";
    tdSpec.textContent = `${specMed} ms (min ${specMin})`;
    tr.appendChild(tdSpec);

    const tdRatio = document.createElement("td");
    tdRatio.className = "num";
    tdRatio.textContent = `${ratio}x`;
    tr.appendChild(tdRatio);

    const tdStatus = document.createElement("td");
    tdStatus.textContent = status;
    tdStatus.className = status.includes("GEÇTİ") ? "badge-pass" : "badge-analyze";
    tr.appendChild(tdStatus);

    tbodyEl.appendChild(tr);
  }
}

function executeBench() {
  if (!exportsObj) return;
  logs.length = 0;
  const entityCount = Number(entitySelectEl.value) || 100000;
  statusTextEl.textContent = `Benchmark çalışıyor (${entityCount} varlık, ısınma + 15 tekrar)...`;
  statusTextEl.className = "";

  const rc = exportsObj.engine_spec_bench_run(entityCount);
  const rptPtr = exportsObj.engine_spec_report_ptr();
  const rptLen = exportsObj.engine_spec_report_len();
  const rptText = readCString(rptPtr, rptLen);

  renderReportTable(rptText);

  const asciiPart = rptText.split("\n#TSV\n")[0];
  outputEl.textContent = asciiPart;

  if (typeof location !== "undefined" && location.search.includes("post=1")) {
    fetch("/__report", { method: "POST", body: asciiPart }).catch(() => {});
  }

  if (rc === 0) {
    statusTextEl.textContent = "Benchmark tamamlandı — Özelleştirilmiş JIT ve genel C çıktıları 100% bit-bit eşit";
    statusTextEl.className = "ok";
  } else {
    statusTextEl.textContent = `Benchmark HATA (kod: ${rc})`;
    statusTextEl.className = "fail";
  }
}

async function init() {
  const resp = await fetch("./engine.wasm", { cache: "no-store" });
  const bytes = await resp.arrayBuffer();
  const mod = await WebAssembly.compile(bytes);
  table = createTableForModule(mod);

  let instance = null;
  for (const tryShared of [true, false]) {
    try {
      memory = new WebAssembly.Memory({ initial: 4096, maximum: 16384, shared: tryShared });
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
        },
        plat_time_ms() {
          return performance.now();
        },
        plat_jit_load: createJitLoader(memory, table),
        plat_download() {},
      };
      instance = new WebAssembly.Instance(mod, { env });
      break;
    } catch (err) {
      if (!tryShared) throw err;
    }
  }

  exportsObj = instance.exports;
  rerunBtn.addEventListener("click", () => executeBench());
  entitySelectEl.addEventListener("change", () => executeBench());
  executeBench();
}

init().catch((err) => {
  console.error(err);
  statusTextEl.textContent = `Hata: ${err.message}`;
  statusTextEl.className = "fail";
  appendLog(String(err.stack || err));
});
