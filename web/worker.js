// web/worker.js — MATOKI arka plan iş (worker) thread'i.
// Ana thread'den gelen (module, memory, jitEntries, threadId) mesajıyla
// aynı engine.wasm'ı paylaşımlı bellek ve kendi yerel fonksiyon tablosuyla instantiate eder,
// mevcut JIT kayıtlarını tabloya yükler, engine_thread_init ile kendi 1 MB yığınını/TLS'ini
// kurar ve periyodik olarak engine_worker_loop çalıştırarak hem C iş kuyruğunu hem de
// JS JIT çoğaltma mesajlarını işler.

import { applyJitEntryToTable } from "./jit.js";

const decoder = new TextDecoder("utf-8");

let memory = null;
let table = null;
let exports = null;
let jitAckView = null;
let running = false;
let threadId = 0;

function readCString(ptr, len) {
  return decoder.decode(new Uint8Array(memory.buffer, ptr, len).slice());
}

function handleJitEntry(entry, sendAck) {
  try {
    applyJitEntryToTable(memory, table, entry);
    if (sendAck && jitAckView && entry.idx >= 0 && entry.idx < jitAckView.length) {
      Atomics.add(jitAckView, entry.idx, 1);
    }
  } catch (err) {
    console.error(`[MATOKI Worker #${threadId}] JIT yükleme hatası (idx=${entry.idx}):`, err);
  }
}

function scheduleWorkSlice() {
  if (!running || !exports) return;
  try {
    exports.engine_worker_loop();
  } catch (err) {
    console.error(`[MATOKI Worker #${threadId}] engine_worker_loop hatası:`, err);
    running = false;
    return;
  }
  if (running) {
    setTimeout(scheduleWorkSlice, 0);
  }
}

self.onmessage = (e) => {
  const msg = e.data;
  if (!msg || typeof msg !== "object") return;

  if (msg.type === "init") {
    try {
      threadId = msg.threadId >>> 0;
      memory = msg.memory;
      const initialTableSize = Math.max(64, msg.tableSize || 64);
      table = new WebAssembly.Table({ element: "anyfunc", initial: initialTableSize });

      const env = {
        memory,
        __indirect_function_table: table,
        plat_log(ptr, len) {
          const text = readCString(ptr, len);
          console.log(`[W${threadId}] ${text}`);
          self.postMessage({ type: "log", threadId, text });
        },
        plat_panic(ptr, len) {
          const text = readCString(ptr, len);
          console.error(`[W${threadId} PANIC] ${text}`);
          self.postMessage({ type: "panic", threadId, text });
          running = false;
        },
        plat_time_ms() {
          return performance.now();
        },
        plat_jit_load() {
          // JIT derlemesi yalnızca ana thread'den başlatılır; worker mesaj ile alır.
          return -1;
        },
        plat_download() {},
      };

      const inst = new WebAssembly.Instance(msg.module, { env });
      exports = inst.exports;

      const ackPtr = exports.engine_jit_ack_buf();
      jitAckView = new Int32Array(memory.buffer, ackPtr, 1024);

      // Worker başlatılmadan önce yüklenmiş JIT modüllerini aynı tablo indekslerine yerleştir
      // ve ack sayacını artır (sonradan başlatılan worker'ların da sayaca dahil olması için).
      if (Array.isArray(msg.jitEntries)) {
        for (const entry of msg.jitEntries) {
          handleJitEntry(entry, true);
        }
      }

      const stackTop = msg.stackTop || exports.engine_thread_stack_top(threadId);
      exports.engine_thread_init(threadId, stackTop);

      running = true;
      self.postMessage({ type: "ready", threadId, stackTop });
      setTimeout(scheduleWorkSlice, 0);
    } catch (err) {
      console.error(`[MATOKI Worker #${threadId}] Başlatma hatası:`, err);
      self.postMessage({
        type: "error",
        threadId,
        message: err ? String(err.message || err) : "Bilinmeyen worker hatası",
      });
    }
    return;
  }

  if (msg.type === "jit_load") {
    handleJitEntry(msg, true);
    return;
  }

  if (msg.type === "stop") {
    running = false;
    return;
  }
};
