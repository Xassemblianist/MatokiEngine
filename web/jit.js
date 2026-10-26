// web/jit.js — Çalışma anında üretilen wasm shader modüllerini yükler ve worker'lara çoğaltır.
// Ana thread'de byte dizisini WebAssembly.Module'e derler, kendi tablosuna ekler,
// JIT kayıt listesine (jitRegistry) kaydeder ve tüm aktif worker'lara (bytes, exportName, idx)
// yayınlar. Worker'lar kendi tablolarında aynı indekse fonksiyonu koyup paylaşımlı bellekteki
// jit_ack[idx] sayacını atomik olarak artırır.

const decoder = new TextDecoder("utf-8");

const jitRegistry = [];
const jitWorkers = new Set();

export function registerJitWorker(worker) {
  jitWorkers.add(worker);
}

export function unregisterJitWorker(worker) {
  jitWorkers.delete(worker);
}

export function getJitRegistry() {
  return jitRegistry.slice();
}

export function applyJitEntryToTable(memory, table, entry) {
  const { wasmBytes, exportName, idx } = entry;
  const mod = new WebAssembly.Module(wasmBytes);
  const inst = new WebAssembly.Instance(mod, {
    env: {
      memory,
      __indirect_function_table: table,
    },
  });
  const fn = inst.exports[exportName];
  if (typeof fn !== "function") {
    throw new Error(`Export bulunamadı veya fonksiyon değil: ${exportName}`);
  }
  if (table.length <= idx) {
    table.grow(idx - table.length + 1);
  }
  table.set(idx, fn);
}

export function createJitLoader(memory, table) {
  return function plat_jit_load(ptr, len, namePtr, nameLen) {
    try {
      const wasmBytes = new Uint8Array(memory.buffer, ptr, len).slice();
      const nameBytes = new Uint8Array(memory.buffer, namePtr, nameLen).slice();
      const exportName = decoder.decode(nameBytes);
      const mod = new WebAssembly.Module(wasmBytes);
      const inst = new WebAssembly.Instance(mod, {
        env: {
          memory,
          __indirect_function_table: table,
        },
      });
      const fn = inst.exports[exportName];
      if (typeof fn !== "function") {
        console.error(`[MATOKI JIT] Export bulunamadı veya fonksiyon değil: ${exportName}`);
        return -1;
      }
      const idx = table.grow(1);
      table.set(idx, fn);

      const entry = { idx, wasmBytes, exportName };
      jitRegistry.push(entry);

      for (const worker of jitWorkers) {
        worker.postMessage({
          type: "jit_load",
          idx,
          wasmBytes,
          exportName,
        });
      }

      return idx;
    } catch (err) {
      console.error("[MATOKI JIT] Modül yükleme hatası:", err);
      return -1;
    }
  };
}
