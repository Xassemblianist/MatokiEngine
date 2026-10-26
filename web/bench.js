// web/bench.js — MATOKI M5_3 Elle WAT vs Clang (-O2 -msimd128) tarayıcı koşucusu.
// engine_bench_run() export'unu çağırır ve sonuç tablosunu DOM'a yazar.

import { createJitLoader } from "./jit.js";

const decoder = new TextDecoder("utf-8");
const statusTextEl = document.getElementById("status-text");
const tbodyEl = document.getElementById("bench-tbody");
const outputEl = document.getElementById("output");
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
  const tsvIdx = reportStr.indexOf("#TSV\n");
  if (tsvIdx < 0) return;
  const lines = reportStr.slice(tsvIdx + 5).trim().split("\n");
  for (const line of lines) {
    if (!line.trim()) continue;
    const parts = line.split("\t");
    if (parts.length < 8) continue;
    const [name, bitExact, cMed, cMin, watMed, watMin, ratio, decision] = parts;
    const tr = document.createElement("tr");

    const tdName = document.createElement("td");
    tdName.textContent = bitExact === "1" ? name : `${name} [BİT FARKI!]`;
    tr.appendChild(tdName);

    const tdC = document.createElement("td");
    tdC.className = "num";
    tdC.textContent = `${cMed} ms (min ${cMin})`;
    tr.appendChild(tdC);

    const tdWat = document.createElement("td");
    tdWat.className = "num";
    tdWat.textContent = `${watMed} ms (min ${watMin})`;
    tr.appendChild(tdWat);

    const tdRatio = document.createElement("td");
    tdRatio.className = "num";
    tdRatio.textContent = `${ratio}x`;
    tr.appendChild(tdRatio);

    const tdDecision = document.createElement("td");
    tdDecision.textContent = decision;
    tdDecision.className = decision.includes("GİR") ? "badge-enter" : "badge-reject";
    tr.appendChild(tdDecision);

    tbodyEl.appendChild(tr);
  }
}

function executeBench() {
  if (!exportsObj) return;
  logs.length = 0;
  statusTextEl.textContent = "Benchmark çalışıyor...";
  statusTextEl.className = "";

  const rc = exportsObj.engine_bench_run();
  const rptPtr = exportsObj.engine_bench_report_ptr();
  const rptLen = exportsObj.engine_bench_report_len();
  const rptText = readCString(rptPtr, rptLen);

  renderReportTable(rptText);

  const asciiPart = rptText.split("\n#TSV\n")[0];
  outputEl.textContent = asciiPart;

  if (typeof location !== "undefined" && location.search.includes("post=1")) {
    fetch("/__report", { method: "POST", body: asciiPart }).catch(() => {});
  }

  if (rc === 0) {
    statusTextEl.textContent = "Benchmark tamamlandı — Tüm çekirdekler bit-bit eşit (kural: >%15 => MOTORA GİR)";
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
  executeBench();
}

init().catch((err) => {
  console.error(err);
  statusTextEl.textContent = `Hata: ${err.message}`;
  statusTextEl.className = "fail";
  appendLog(String(err.stack || err));
});
