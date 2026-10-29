// web/threads.js — MATOKI Web Worker havuzu başlatıcı ve yöneticisi.
//
// E hattı (platform.js) için kullanım arayüzü:
//   import { startThreads } from "./threads.js";
//   const pool = await startThreads(n, module, memory, table, { onLog, onPanic, instance });
//
// Parametreler:
//   - n      : İstenen worker sayısı. undefined / null / <= 0 verilirse
//              Math.min(Math.max((navigator.hardwareConcurrency || 4) - 1, 1), 8) kullanılır.
//              0 açıkça test/benchmark için tek-thread istendiğinde stopThreads() ile kullanılır.
//   - module : Derlenmiş WebAssembly.Module (veya ArrayBuffer/Uint8Array wasm baytları).
//   - memory : Paylaşımlı WebAssembly.Memory ({ initial: 4096, maximum: 16384, shared: true }).
//   - table  : Ana thread'in WebAssembly.Table nesnesi (boyutu ve mevcut JIT kayıtları okunur;
//              WebAssembly.Table nesnesi worker'lara kopyalanamadığından jit.js içindeki
//              JIT kayıt listesi worker'lara gönderilir).
//   - opts   : Opsiyonel { instance, onLog(text, threadId), onPanic(text, threadId) }.

import { getJitRegistry, registerJitWorker, unregisterJitWorker } from "./jit.js";

let activeWorkers = [];
let sharedMemory = null;

export async function stopThreads(exportsObj = null) {
  if (exportsObj && typeof exportsObj.engine_worker_stop === "function") {
    exportsObj.engine_worker_stop();
  }
  for (const w of activeWorkers) {
    unregisterJitWorker(w);
    w.terminate();
  }
  activeWorkers = [];
  if (exportsObj && typeof exportsObj.engine_set_worker_count === "function") {
    exportsObj.engine_set_worker_count(0);
  }
  if (exportsObj && typeof exportsObj.engine_reset_ready_workers === "function") {
    exportsObj.engine_reset_ready_workers();
  }
  if (exportsObj && typeof exportsObj.engine_jit_ack_buf === "function" && sharedMemory) {
    const ackPtr = exportsObj.engine_jit_ack_buf();
    const view = new Int32Array(sharedMemory.buffer, ackPtr, 1024);
    view.fill(0);
  }
}

export async function startThreads(n, moduleOrBytes, memory, table, opts = {}) {
  const defaultCount = Math.min(Math.max((navigator.hardwareConcurrency || 4) - 1, 1), 8);
  const workerCount =
    typeof n === "number" && !Number.isNaN(n)
      ? Math.max(0, Math.min(8, Math.floor(n)))
      : defaultCount;

  const wasmModule =
    moduleOrBytes instanceof WebAssembly.Module
      ? moduleOrBytes
      : await WebAssembly.compile(moduleOrBytes);

  const exportsObj = opts.instance ? opts.instance.exports : opts.exports || null;
  sharedMemory = memory;

  // Önceki worker'lar varsa temizle
  await stopThreads(exportsObj);

  if (exportsObj) {
    if (typeof exportsObj.engine_thread_init === "function") {
      exportsObj.engine_thread_init(0, 0);
    }
    if (typeof exportsObj.engine_set_worker_count === "function") {
      exportsObj.engine_set_worker_count(workerCount);
    }
    if (typeof exportsObj.engine_jit_ack_buf === "function") {
      const ackPtr = exportsObj.engine_jit_ack_buf();
      const view = new Int32Array(memory.buffer, ackPtr, 1024);
      view.fill(0);
    }
  }

  if (workerCount === 0) {
    return { workers: [], count: 0 };
  }

  const jitEntries = getJitRegistry();
  const tableSize = table ? table.length : 1;

  const readyPromises = [];

  for (let i = 1; i <= workerCount; i++) {
    const worker = new Worker(new URL("./worker.js", import.meta.url), {
      type: "module",
      name: `matoki-worker-${i}`,
    });

    activeWorkers.push(worker);
    registerJitWorker(worker);

    const readyPromise = new Promise((resolve, reject) => {
      const onMessage = (e) => {
        const data = e.data;
        if (!data || typeof data !== "object") return;

        if (data.type === "ready" && data.threadId === i) {
          resolve(data);
        } else if (data.type === "error" && data.threadId === i) {
          reject(new Error(data.message || `Worker ${i} başlatılamadı`));
        } else if (data.type === "log") {
          if (typeof opts.onLog === "function") {
            opts.onLog(data.text, data.threadId);
          }
        } else if (data.type === "panic") {
          if (typeof opts.onPanic === "function") {
            opts.onPanic(data.text, data.threadId);
          }
        }
      };

      worker.addEventListener("message", onMessage);
      worker.addEventListener("error", (err) => {
        reject(new Error(`Worker ${i} script hatası: ${err.message || err}`));
      });
    });

    const stackTop =
      exportsObj && typeof exportsObj.engine_thread_stack_top === "function"
        ? exportsObj.engine_thread_stack_top(i)
        : 0;

    worker.postMessage({
      type: "init",
      threadId: i,
      module: wasmModule,
      memory,
      tableSize,
      jitEntries,
      stackTop,
    });

    readyPromises.push(readyPromise);
  }

  await Promise.all(readyPromises);

  return {
    workers: activeWorkers.slice(),
    count: workerCount,
  };
}

// OffscreenCanvas ile motor döngüsünü ayrı bir worker'da (tid=0) başlatır (D-109).
// Ana iş parçacığı yalnız girdi / ses / dosya bırakma / DOM köprüsü olarak kalır.
export async function startRenderWorker(offscreenCanvas, moduleOrBytes, memory, caps, opts = {}) {
  const wasmModule =
    moduleOrBytes instanceof WebAssembly.Module
      ? moduleOrBytes
      : await WebAssembly.compile(moduleOrBytes);

  const hc = (typeof navigator !== "undefined" && navigator.hardwareConcurrency) || 8;
  const workerCount =
    typeof opts.workerCount === "number"
      ? opts.workerCount
      : Math.min(Math.max(hc - 1, 7), 8);
  const seed = typeof opts.seed === "number" ? opts.seed >>> 0 : Date.now() >>> 0;

  const worker = new Worker(new URL("./worker.js", import.meta.url), {
    type: "module",
    name: "matoki-render",
  });

  let nextCallId = 1;
  const pendingCalls = new Map();

  function callAsync(fn, ...args) {
    const id = nextCallId++;
    return new Promise((resolve, reject) => {
      pendingCalls.set(id, { resolve, reject });
      worker.postMessage({ type: "call", id, fn, args });
    });
  }

  const readyInfo = await new Promise((resolve, reject) => {
    const onMessage = (e) => {
      const data = e.data;
      if (!data || typeof data !== "object") return;

      if (data.type === "render_ready") {
        resolve(data);
        return;
      }
      if (data.type === "error" && data.threadId === 0) {
        reject(new Error(data.message || "Render worker başlatılamadı"));
        return;
      }
      if (data.type === "log") {
        if (typeof opts.onLog === "function") opts.onLog(data.text, data.threadId);
        return;
      }
      if (data.type === "panic") {
        if (typeof opts.onPanic === "function") opts.onPanic(data.text, data.threadId);
        return;
      }
      if (data.type === "download") {
        if (typeof opts.onDownload === "function") opts.onDownload(data.filename, data.bytes);
        return;
      }
      if (data.type === "wants_pointer_lock") {
        if (typeof opts.onWantsPointerLock === "function") opts.onWantsPointerLock(Boolean(data.wants));
        return;
      }
      if (data.type === "call_result") {
        const entry = pendingCalls.get(data.id);
        if (entry) {
          pendingCalls.delete(data.id);
          entry.resolve(data.result);
        }
        return;
      }
      if (data.type === "call_error") {
        const entry = pendingCalls.get(data.id);
        if (entry) {
          pendingCalls.delete(data.id);
          entry.reject(new Error(data.message || "Worker çağrı hatası"));
        }
      }
    };

    worker.addEventListener("message", onMessage);
    worker.addEventListener("error", (err) => {
      reject(new Error(`Render worker hatası: ${err.message || err}`));
    });

    worker.postMessage(
      {
        type: "init_render",
        canvas: offscreenCanvas,
        module: wasmModule,
        memory,
        caps,
        seed,
        workerCount,
      },
      [offscreenCanvas]
    );
  });

  let wantsLockState = Boolean(readyInfo.wantsPointerLock);
  let dropCursor = readyInfo.assetBasePtr >>> 0;
  let dropFiles = [];

  const baseExports = {
    engine_fb_width: () => readyInfo.fbW >>> 0,
    engine_fb_height: () => readyInfo.fbH >>> 0,
    engine_framebuffer: () => readyInfo.fbPtr >>> 0,
    engine_input_buf: () => readyInfo.inputPtr >>> 0,
    engine_wants_pointer_lock: () => (wantsLockState ? 1 : 0),
    engine_set_pointer_lock(locked) {
      worker.postMessage({ type: "set_pointer_lock", locked: locked ? 1 : 0 });
    },
    engine_audio_init(sampleRate) {
      const ptr = readyInfo.audioRingPtr >>> 0;
      if (ptr > 0 && memory.buffer instanceof SharedArrayBuffer) {
        const hdr = new Uint32Array(memory.buffer, ptr, 8);
        Atomics.store(hdr, 0, 0);
        Atomics.store(hdr, 1, 0);
        Atomics.store(hdr, 3, sampleRate >>> 0);
        Atomics.store(hdr, 4, 0);
      }
      worker.postMessage({ type: "audio_init", sampleRate: sampleRate >>> 0 });
      return ptr;
    },
    engine_file_begin() {
      dropCursor = readyInfo.assetBasePtr >>> 0;
      dropFiles = [];
    },
    engine_file_alloc(len) {
      const aligned = (dropCursor + 15) & ~15;
      dropCursor = (aligned + (len >>> 0) + 1 + 15) & ~15;
      return aligned >>> 0;
    },
    engine_file_add(np, nlen, dp, dlen) {
      const name = new Uint8Array(memory.buffer, np >>> 0, nlen >>> 0).slice();
      const data = new Uint8Array(memory.buffer, dp >>> 0, dlen >>> 0).slice();
      dropFiles.push({ name, data });
    },
    engine_file_end() {
      const files = dropFiles;
      dropFiles = [];
      worker.postMessage({ type: "file_drop", files });
      return 1;
    },
    engine_set_caps(mask, maxTex2D, maxBufMB) {
      worker.postMessage({
        type: "call",
        fn: "engine_set_caps",
        args: [mask >>> 0, (maxTex2D || 0) >>> 0, (maxBufMB || 0) >>> 0],
      });
    },
  };

  const exportsProxy = new Proxy(baseExports, {
    get(target, prop) {
      if (prop in target) return target[prop];
      if (typeof prop === "string" && prop.startsWith("engine_")) {
        return (...args) => {
          worker.postMessage({ type: "call", fn: prop, args });
          return 0;
        };
      }
      return undefined;
    },
  });

  return {
    worker,
    fbW: readyInfo.fbW >>> 0,
    fbH: readyInfo.fbH >>> 0,
    fbPtr: readyInfo.fbPtr >>> 0,
    inputPtr: readyInfo.inputPtr >>> 0,
    audioRingPtr: readyInfo.audioRingPtr >>> 0,
    setWantsLock(val) {
      wantsLockState = Boolean(val);
    },
    callAsync,
    exports: exportsProxy,
  };
}
