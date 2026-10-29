// web/worker.js — MATOKI Web Worker giriş noktası (D-050, D-109).
// İki rolü destekler:
// 1) "init_render" (tid=0): OffscreenCanvas ile motorun ana render döngüsünü (engine_init,
//    engine_frame, putImageData) çalıştırır ve alt raster worker havuzunu yönetir.
// 2) "init" (tid=1..N): Paylaşımlı bellekte kendi 1 MB yığını/TLS'i ile engine_worker_loop
//    üzerinden paralel raster/3D iş kuyruğunu ve JIT tablo çoğaltmasını yürütür.

import { applyJitEntryToTable, createJitLoader } from "./jit.js";
import { startThreads } from "./threads.js";

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

async function handleInitRender(msg) {
  try {
    threadId = 0;
    memory = msg.memory;
    const mod = msg.module;
    const offscreenCanvas = msg.canvas;
    const caps = msg.caps || { mask: 0, gpuMaxTex2D: 0, gpuMaxBufMB: 0 };
    table = createTableForModule(mod);

    const env = {
      memory,
      __indirect_function_table: table,
      plat_log(ptr, len) {
        const text = readCString(ptr, len);
        console.log(text);
        self.postMessage({ type: "log", threadId: 0, text });
      },
      plat_panic(ptr, len) {
        const text = readCString(ptr, len);
        running = false;
        self.postMessage({ type: "panic", threadId: 0, text });
      },
      plat_time_ms() {
        return performance.now();
      },
      plat_jit_load: createJitLoader(memory, table),
      plat_download(namePtr, nameLen, dataPtr, dataLen) {
        const filename = readCString(namePtr, nameLen) || "matoki.png";
        const copy = new Uint8Array(dataLen);
        copy.set(new Uint8Array(memory.buffer, dataPtr, dataLen));
        self.postMessage({ type: "download", filename, bytes: copy }, [copy.buffer]);
      },
    };

    const inst = new WebAssembly.Instance(mod, { env });
    exports = inst.exports;
    self.matoki = { exports, memory, caps, workerRender: true };

    if (memory.buffer instanceof SharedArrayBuffer && msg.workerCount > 0) {
      try {
        await startThreads(msg.workerCount, mod, memory, table, {
          instance: inst,
          onLog(text, tid) {
            self.postMessage({ type: "log", threadId: tid, text });
          },
          onPanic(text, tid) {
            running = false;
            self.postMessage({ type: "panic", threadId: tid, text });
          },
        });
      } catch (_e) {
        // Alt worker başlatılamazsa tek thread devam et
      }
    }

    if (typeof exports.engine_set_caps === "function") {
      exports.engine_set_caps(
        caps.mask >>> 0,
        (caps.gpuMaxTex2D || 0) >>> 0,
        (caps.gpuMaxBufMB || 0) >>> 0
      );
    }

    exports.engine_init((msg.seed || Date.now()) >>> 0);

    const fbW = exports.engine_fb_width() >>> 0;
    const fbH = exports.engine_fb_height() >>> 0;
    const fbPtr = exports.engine_framebuffer() >>> 0;
    const inputPtr = exports.engine_input_buf() >>> 0;
    const audioRingPtr =
      typeof exports.engine_audio_init === "function" ? exports.engine_audio_init(48000) >>> 0 : 0;

    let assetBasePtr = 0;
    if (
      typeof exports.engine_file_begin === "function" &&
      typeof exports.engine_file_alloc === "function"
    ) {
      exports.engine_file_begin();
      assetBasePtr = exports.engine_file_alloc(0) >>> 0;
      exports.engine_file_begin();
    }

    let prevWantsLock =
      typeof exports.engine_wants_pointer_lock === "function"
        ? exports.engine_wants_pointer_lock() >>> 0
        : 0;

    offscreenCanvas.width = fbW;
    offscreenCanvas.height = fbH;
    const ctx = offscreenCanvas.getContext("2d", { alpha: false });

    const fbByteLen = fbW * fbH * 4;
    let cachedBuffer = null;
    let isShared = false;
    let fbView = null;
    let localFbBuf = null;
    let imageData = null;

    function refreshFbViews() {
      if (cachedBuffer === memory.buffer) return;
      cachedBuffer = memory.buffer;
      isShared = typeof SharedArrayBuffer !== "undefined" && cachedBuffer instanceof SharedArrayBuffer;
      fbView = new Uint8ClampedArray(cachedBuffer, fbPtr, fbByteLen);
      if (isShared) {
        if (!localFbBuf || localFbBuf.length !== fbByteLen) {
          localFbBuf = new Uint8ClampedArray(fbByteLen);
          imageData = new ImageData(localFbBuf, fbW, fbH);
        }
      } else {
        imageData = new ImageData(fbView, fbW, fbH);
      }
    }

    refreshFbViews();
    running = true;

    self.postMessage({
      type: "render_ready",
      fbW,
      fbH,
      fbPtr,
      inputPtr,
      audioRingPtr,
      assetBasePtr,
      wantsPointerLock: prevWantsLock,
    });

    const scheduleFrame =
      typeof self.requestAnimationFrame === "function"
        ? (cb) => self.requestAnimationFrame(cb)
        : (cb) => setTimeout(() => cb(performance.now()), 16);

    let lastTime = performance.now();

    function renderFrame(now) {
      if (!running) return;
      const dt = Math.min((now - lastTime) * 0.001, 0.1);
      lastTime = now;

      try {
        exports.engine_frame(now, dt);
      } catch (err) {
        if (running) {
          running = false;
          self.postMessage({
            type: "panic",
            threadId: 0,
            text: `Wasm çalışma hatası: ${err && err.message ? err.message : err}`,
          });
        }
        return;
      }

      if (!running) return;
      refreshFbViews();
      if (isShared) {
        localFbBuf.set(fbView);
      }
      ctx.putImageData(imageData, 0, 0);

      if (typeof exports.engine_wants_pointer_lock === "function") {
        const wants = exports.engine_wants_pointer_lock() >>> 0;
        if (wants !== prevWantsLock) {
          prevWantsLock = wants;
          self.postMessage({ type: "wants_pointer_lock", wants });
        }
      }

      scheduleFrame(renderFrame);
    }

    scheduleFrame(renderFrame);
  } catch (err) {
    console.error("[MATOKI Render Worker] Başlatma hatası:", err);
    self.postMessage({
      type: "error",
      threadId: 0,
      message: err ? String(err.message || err) : "Bilinmeyen render worker hatası",
    });
  }
}

self.onmessage = (e) => {
  const msg = e.data;
  if (!msg || typeof msg !== "object") return;

  if (msg.type === "init_render") {
    handleInitRender(msg);
    return;
  }

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
          // JIT derlemesi yalnızca render/ana thread'den başlatılır; worker mesaj ile alır.
          return -1;
        },
        plat_download() {},
      };

      const inst = new WebAssembly.Instance(msg.module, { env });
      exports = inst.exports;

      const ackPtr = exports.engine_jit_ack_buf();
      jitAckView = new Int32Array(memory.buffer, ackPtr, 1024);

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

  if (msg.type === "set_pointer_lock") {
    if (exports && typeof exports.engine_set_pointer_lock === "function") {
      exports.engine_set_pointer_lock(msg.locked ? 1 : 0);
    }
    return;
  }

  if (msg.type === "audio_init") {
    if (exports && typeof exports.engine_audio_init === "function") {
      exports.engine_audio_init(msg.sampleRate >>> 0);
    }
    return;
  }

  if (msg.type === "file_drop") {
    if (exports && typeof exports.engine_file_begin === "function") {
      exports.engine_file_begin();
      for (const f of msg.files || []) {
        const name = new Uint8Array(f.name);
        const data = new Uint8Array(f.data);
        const np = exports.engine_file_alloc(name.length);
        const dp = exports.engine_file_alloc(data.length);
        new Uint8Array(memory.buffer, np, name.length).set(name);
        new Uint8Array(memory.buffer, dp, data.length).set(data);
        exports.engine_file_add(np, name.length, dp, data.length);
      }
      const ok = exports.engine_file_end();
      self.postMessage({ type: "file_drop_done", ok });
    }
    return;
  }

  if (msg.type === "call") {
    try {
      const fn = exports && exports[msg.fn];
      const res = typeof fn === "function" ? fn(...(msg.args || [])) : undefined;
      if (msg.id != null) {
        self.postMessage({ type: "call_result", id: msg.id, result: res });
      }
    } catch (err) {
      if (msg.id != null) {
        self.postMessage({
          type: "call_error",
          id: msg.id,
          message: err ? String(err.message || err) : "Worker çağrı hatası",
        });
      }
    }
    return;
  }

  if (msg.type === "stop") {
    running = false;
    return;
  }
};
