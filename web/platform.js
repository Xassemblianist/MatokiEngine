// web/platform.js — MATOKI tarayıcı platform katmanı (ARCHITECTURE.md §2, D-109).
// Yetenek tespiti (web/caps.js) ile uygun wasm derlemesini (engine_relaxed.wasm / engine.wasm)
// seçer; OffscreenCanvas + SharedArrayBuffer varsa motor döngüsünü bir worker'da çalıştırır
// (ana iş parçacığı yalnız girdi/ses/dosya/DOM köprüsü), desteklenmezse eski ana iş parçacığı
// rAF döngüsüne düşer.

import { detectCaps, applyCapsToEngine, CAP_WORKER_RENDER } from "./caps.js";
import { createJitLoader } from "./jit.js";
import { startThreads, startRenderWorker } from "./threads.js";

const decoder = new TextDecoder("utf-8");
const canvas = document.getElementById("screen");
const panicOverlay = document.getElementById("panic-overlay");

let memory = null;
let table = null;

let running = true;
let rafId = 0;

function readCString(ptr, len) {
  const bytes = new Uint8Array(memory.buffer, ptr, len);
  if (typeof SharedArrayBuffer !== "undefined" && memory.buffer instanceof SharedArrayBuffer) {
    return decoder.decode(bytes.slice());
  }
  return decoder.decode(bytes);
}

function showPanic(msg) {
  running = false;
  if (rafId) {
    cancelAnimationFrame(rafId);
    rafId = 0;
  }
  console.error(`[MATOKI PANIC] ${msg}`);
  if (panicOverlay) {
    panicOverlay.textContent = `MATOKI PANIC\n\n${msg}`;
    panicOverlay.style.display = "block";
  }
}

function triggerDownload(filename, bytes) {
  const blob = new Blob([bytes], { type: "image/png" });
  const url = URL.createObjectURL(blob);
  const a = document.createElement("a");
  a.href = url;
  a.download = filename || "matoki.png";
  document.body.appendChild(a);
  a.click();
  document.body.removeChild(a);
  setTimeout(() => URL.revokeObjectURL(url), 1000);
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

async function fetchEngineModule(caps) {
  const urls = [caps.wasmUrl];
  if (caps.wasmUrl !== "./engine.wasm" && !caps.wasmUrl.includes("engine_st.wasm")) {
    urls.push("./engine.wasm");
  }
  if (!urls.includes("./engine_st.wasm")) {
    urls.push("./engine_st.wasm");
  }
  let lastErr = null;
  for (const url of urls) {
    try {
      const resp = await fetch(url, { cache: "no-store" });
      if (!resp.ok) throw new Error(`HTTP ${resp.status} (${url})`);
      const bytes = await resp.arrayBuffer();
      const mod = await WebAssembly.compile(bytes);
      caps.wasmUrl = url;
      caps.isSt = url.includes("engine_st.wasm");
      return mod;
    } catch (err) {
      lastErr = err;
    }
  }
  throw lastErr || new Error("Wasm modülü yüklenemedi");
}

function installInputListeners(exports, fbW, fbH, inputPtr) {
  let cachedBuffer = null;
  let inputKeys = null;
  let inputI32 = null;
  let inputU32 = null;
  let inputChars = null;

  const INPUT_EVENT_NONE        = 0;
  const INPUT_EVENT_KEY_DOWN    = 1;
  const INPUT_EVENT_KEY_UP      = 2;
  const INPUT_EVENT_MOUSE_MOVE  = 3;
  const INPUT_EVENT_MOUSE_DOWN  = 4;
  const INPUT_EVENT_MOUSE_UP    = 5;
  const INPUT_EVENT_MOUSE_WHEEL = 6;
  const INPUT_EVENT_CHAR        = 7;

  const ringPtr = typeof exports.engine_input_ring === "function" ? exports.engine_input_ring() : 0;
  let ringU32 = null;
  let ringFrameCounter = 0;

  function refreshInputViews() {
    if (cachedBuffer === memory.buffer) return;
    cachedBuffer = memory.buffer;
    inputKeys = new Uint8Array(cachedBuffer, inputPtr, 256);
    inputI32 = new Int32Array(cachedBuffer, inputPtr + 256, 6);
    inputU32 = new Uint32Array(cachedBuffer, inputPtr + 256, 6);
    inputChars = new Uint32Array(cachedBuffer, inputPtr + 280, 33);
    if (ringPtr) {
      ringU32 = new Uint32Array(cachedBuffer, ringPtr, 4);
    }
  }

  refreshInputViews();

  function pushRingEvent(type, ev = {}) {
    if (!ringPtr) return;
    refreshInputViews();
    const isSAB = typeof SharedArrayBuffer !== "undefined" && memory.buffer instanceof SharedArrayBuffer;
    const writePos = isSAB ? Atomics.load(ringU32, 0) : ringU32[0];
    const readPos = isSAB ? Atomics.load(ringU32, 1) : ringU32[1];
    const cap = ringU32[2] || 256;
    if ((writePos - readPos) >= cap) {
      if (isSAB) Atomics.add(ringU32, 3, 1);
      else ringU32[3] = (ringU32[3] + 1) >>> 0;
      return;
    }
    const idx = writePos & (cap - 1);
    const evOffset = ringPtr + 16 + idx * 64;
    const evU32 = new Uint32Array(memory.buffer, evOffset, 16);
    const evI32 = new Int32Array(memory.buffer, evOffset, 16);
    const evF64 = new Float64Array(memory.buffer, evOffset + 8, 1);

    evU32[0] = type >>> 0;
    evU32[1] = (ev.frame !== undefined ? ev.frame : ringFrameCounter) >>> 0;
    evF64[0] = ev.time_ms !== undefined ? ev.time_ms : performance.now();
    evI32[4] = (ev.mouse_x || 0) | 0;
    evI32[5] = (ev.mouse_y || 0) | 0;
    evI32[6] = (ev.mouse_dx || 0) | 0;
    evI32[7] = (ev.mouse_dy || 0) | 0;
    evU32[8] = (ev.key || 0) >>> 0;
    evU32[9] = (ev.buttons || 0) >>> 0;
    evI32[10] = (ev.wheel || 0) | 0;
    evU32[11] = (ev.char_code || 0) >>> 0;
    evU32[12] = (ev.modifiers || 0) >>> 0;

    const nextWrite = (writePos + 1) >>> 0;
    if (isSAB) {
      Atomics.store(ringU32, 0, nextWrite);
    } else {
      ringU32[0] = nextWrite;
    }
  }

  function getModifiers(e) {
    let m = 0;
    if (e.shiftKey) m |= 1;
    if (e.ctrlKey)  m |= 2;
    if (e.altKey)   m |= 4;
    if (e.metaKey)  m |= 8;
    return m;
  }

  function pushCharCodepoint(cp) {
    const count = inputChars[32];
    if (count < 32) {
      inputChars[count] = cp >>> 0;
      inputChars[32] = count + 1;
    }
  }

  window.addEventListener("keydown", (e) => {
    refreshInputViews();
    const code = e.keyCode & 0xff;
    inputKeys[code] = 1;
    const mods = getModifiers(e);

    pushRingEvent(INPUT_EVENT_KEY_DOWN, {
      key: code,
      modifiers: mods,
      mouse_x: inputI32[0],
      mouse_y: inputI32[1],
      buttons: inputU32[4],
    });

    if (
      code === 8 ||
      code === 9 ||
      code === 13 ||
      code === 32 ||
      (code >= 33 && code <= 40) ||
      code === 46 ||
      code === 112 ||
      code === 123 ||
      (e.ctrlKey && !e.altKey)
    ) {
      e.preventDefault();
    }

    if (!e.ctrlKey && !e.altKey && !e.metaKey) {
      if (e.key && e.key.length === 1) {
        const cp = e.key.codePointAt(0);
        if (cp >= 32) {
          pushCharCodepoint(cp);
          pushRingEvent(INPUT_EVENT_CHAR, { char_code: cp, key: code, modifiers: mods });
        }
      } else if (e.key && [...e.key].length === 1) {
        const cp = e.key.codePointAt(0);
        if (cp >= 32) {
          pushCharCodepoint(cp);
          pushRingEvent(INPUT_EVENT_CHAR, { char_code: cp, key: code, modifiers: mods });
        }
      }
    }
  });

  window.addEventListener("keyup", (e) => {
    refreshInputViews();
    const code = e.keyCode & 0xff;
    inputKeys[code] = 0;
    const mods = getModifiers(e);

    pushRingEvent(INPUT_EVENT_KEY_UP, {
      key: code,
      modifiers: mods,
      mouse_x: inputI32[0],
      mouse_y: inputI32[1],
      buttons: inputU32[4],
    });
  });

  window.addEventListener("blur", () => {
    refreshInputViews();
    inputKeys.fill(0);
    inputU32[4] = 0;
    inputChars[32] = 0;
  });

  function updateMouseCoords(e) {
    const curW = (typeof exports.engine_fb_width === "function" ? exports.engine_fb_width() : canvas.width) || 640;
    const curH = (typeof exports.engine_fb_height === "function" ? exports.engine_fb_height() : canvas.height) || 360;
    const rect = canvas.getBoundingClientRect();
    const aspectCanvas = curW / curH;
    const aspectRect = rect.width / rect.height;
    let renderW = rect.width;
    let renderH = rect.height;
    let offsetX = 0;
    let offsetY = 0;

    if (aspectRect > aspectCanvas) {
      renderW = rect.height * aspectCanvas;
      offsetX = (rect.width - renderW) * 0.5;
    } else {
      renderH = rect.width / aspectCanvas;
      offsetY = (rect.height - renderH) * 0.5;
    }

    const relX = e.clientX - rect.left - offsetX;
    const relY = e.clientY - rect.top - offsetY;
    const mx = Math.floor((relX * curW) / renderW);
    const my = Math.floor((relY * curH) / renderH);

    const clampedX = Math.max(0, Math.min(curW - 1, mx));
    const clampedY = Math.max(0, Math.min(curH - 1, my));

    const prevX = inputI32[0];
    const prevY = inputI32[1];
    inputI32[0] = clampedX;
    inputI32[1] = clampedY;
    const dx = clampedX - prevX;
    const dy = clampedY - prevY;
    inputI32[2] += dx;
    inputI32[3] += dy;

    return { clampedX, clampedY, dx, dy };
  }

  const wantsLock = () => !!(exports.engine_wants_pointer_lock && exports.engine_wants_pointer_lock());
  const isLocked = () => document.pointerLockElement === canvas;
  document.addEventListener("pointerlockchange", () => {
    if (exports.engine_set_pointer_lock) exports.engine_set_pointer_lock(isLocked() ? 1 : 0);
  });

  window.addEventListener("mousemove", (e) => {
    refreshInputViews();
    const mods = getModifiers(e);
    if (isLocked()) {
      const dx = e.movementX | 0;
      const dy = e.movementY | 0;
      inputI32[2] += dx;
      inputI32[3] += dy;
      pushRingEvent(INPUT_EVENT_MOUSE_MOVE, {
        mouse_x: inputI32[0],
        mouse_y: inputI32[1],
        mouse_dx: dx,
        mouse_dy: dy,
        buttons: inputU32[4],
        modifiers: mods,
      });
      return;
    }
    const coords = updateMouseCoords(e);
    pushRingEvent(INPUT_EVENT_MOUSE_MOVE, {
      mouse_x: coords.clampedX,
      mouse_y: coords.clampedY,
      mouse_dx: coords.dx,
      mouse_dy: coords.dy,
      buttons: inputU32[4],
      modifiers: mods,
    });
  });

  window.addEventListener("mousedown", (e) => {
    refreshInputViews();
    if (!isLocked()) updateMouseCoords(e);
    inputU32[4] = e.buttons;
    const mods = getModifiers(e);
    pushRingEvent(INPUT_EVENT_MOUSE_DOWN, {
      mouse_x: inputI32[0],
      mouse_y: inputI32[1],
      buttons: e.buttons,
      modifiers: mods,
    });
    if (e.target === canvas && !isLocked() && wantsLock() && canvas.requestPointerLock) {
      canvas.requestPointerLock();
    }
  });

  window.addEventListener("mouseup", (e) => {
    refreshInputViews();
    if (!isLocked()) updateMouseCoords(e);
    inputU32[4] = e.buttons;
    const mods = getModifiers(e);
    pushRingEvent(INPUT_EVENT_MOUSE_UP, {
      mouse_x: inputI32[0],
      mouse_y: inputI32[1],
      buttons: e.buttons,
      modifiers: mods,
    });
  });

  window.addEventListener(
    "wheel",
    (e) => {
      refreshInputViews();
      const delta = Math.sign(e.deltaY);
      inputI32[5] += delta;
      const mods = getModifiers(e);
      pushRingEvent(INPUT_EVENT_MOUSE_WHEEL, {
        mouse_x: inputI32[0],
        mouse_y: inputI32[1],
        wheel: delta,
        buttons: inputU32[4],
        modifiers: mods,
      });
    },
    { passive: true }
  );

  window.addEventListener("contextmenu", (e) => e.preventDefault());

  function onNextFrame() {
    ringFrameCounter++;
  }

  return { wantsLock, isLocked, onNextFrame };
}

function readSavedResolutionMode() {
  try {
    const s = localStorage.getItem("matoki_res_mode");
    if (s !== null) {
      const n = parseInt(s, 10);
      if (n >= 0 && n <= 4) return n;
    }
  } catch (err) {
    console.warn("[platform] localStorage okunamadı:", err);
  }
  return 0;
}

function saveResolutionMode(mode) {
  try {
    localStorage.setItem("matoki_res_mode", String(mode));
  } catch (err) {}
}

function getScreenResolution() {
  const dpr = window.devicePixelRatio || 1;
  const sw = Math.max(320, Math.round((window.innerWidth || 1920) * dpr) & ~1);
  const sh = Math.max(180, Math.round((window.innerHeight || 1080) * dpr) & ~1);
  return { sw, sh };
}

async function init() {
  const initialResMode = readSavedResolutionMode();
  const screenRes = getScreenResolution();
  const caps = await detectCaps();
  const mod = await fetchEngineModule(caps);

  const forceMain =
    typeof location !== "undefined" &&
    new URLSearchParams(location.search).get("worker") === "0";

  const canWorkerRender =
    !forceMain &&
    !caps.isSt &&
    caps.offscreenCanvas &&
    caps.sab &&
    canvas &&
    typeof canvas.transferControlToOffscreen === "function";

  // 1) OffscreenCanvas + Worker'da render yolu (D-109 varsayılanı)
  if (canWorkerRender) {
    try {
      memory = new WebAssembly.Memory({ initial: 4096, maximum: 16384, shared: true });
      caps.mask = (caps.mask | CAP_WORKER_RENDER) >>> 0;
      const offscreen = canvas.transferControlToOffscreen();

      let lockHelpers = null;
      const rw = await startRenderWorker(offscreen, mod, memory, caps, {
        initialResMode,
        screenW: screenRes.sw,
        screenH: screenRes.sh,
        onLog(_text, _tid) {},
        onPanic(text, tid) {
          showPanic(tid > 0 ? `[Worker ${tid}] ${text}` : text);
        },
        onDownload(filename, bytes) {
          triggerDownload(filename, bytes);
        },
        onWantsPointerLock(wants) {
          rw.setWantsLock(wants);
          if (lockHelpers && lockHelpers.isLocked() && !wants) {
            document.exitPointerLock();
          }
        },
        onResolutionChanged(mode) {
          saveResolutionMode(mode);
        },
      });

      const exports = rw.exports;
      globalThis.matoki = {
        exports,
        memory,
        caps,
        workerRender: true,
        renderWorker: rw.worker,
        callAsync: rw.callAsync,
        cycleResolution: async () => rw.callAsync("engine_cycle_resolution"),
        setResolutionMode: async (m) => rw.callAsync("engine_set_resolution_mode", m),
        setResolution: async (w, h) => rw.callAsync("engine_set_resolution", w, h),
      };

      window.addEventListener("resize", () => {
        const s = getScreenResolution();
        if (rw && rw.worker) {
          rw.worker.postMessage({
            type: "call",
            fn: "engine_set_screen_size",
            args: [s.sw >>> 0, s.sh >>> 0],
          });
        }
      });

      try {
        const dropMod = await import("./filedrop.js");
        dropMod.installFileDrop(exports, memory);
      } catch (err) {
        console.warn("[platform] worker render filedrop yüklenemedi:", err);
      }

      if (typeof SharedArrayBuffer !== "undefined" && memory.buffer instanceof SharedArrayBuffer) {
        try {
          const audioMod = await import("./audio.js");
          audioMod.installAudio(exports, memory);
        } catch (err) {
          console.warn("[platform] worker render audio yüklenemedi:", err);
        }
      }

      lockHelpers = installInputListeners(exports, rw.fbW, rw.fbH, rw.inputPtr);
      return;
    } catch (err) {
      console.warn("[platform] Worker render başlatılamadı, ana thread yoluna düşülüyor:", err);
      caps.mask = (caps.mask & ~CAP_WORKER_RENDER) >>> 0;
    }
  }

  // 2) Yedek yol: Ana iş parçacığında render (OffscreenCanvas yoksa, ST moduysa veya ?worker=0 ise)
  const ctx = canvas.getContext("2d", { alpha: false });
  table = createTableForModule(mod);

  const isSt = Boolean(caps.isSt || !caps.sab);
  const trySharedList = isSt ? [false] : (caps.sab ? [true, false] : [false]);

  let instance = null;
  for (const tryShared of trySharedList) {
    try {
      memory = new WebAssembly.Memory({ initial: 4096, maximum: 16384, shared: tryShared });
      const env = {
        memory,
        __indirect_function_table: table,
        plat_log(ptr, len) {
          console.log(readCString(ptr, len));
        },
        plat_panic(ptr, len) {
          showPanic(readCString(ptr, len));
        },
        plat_time_ms() {
          return performance.now();
        },
        plat_jit_load: createJitLoader(memory, table),
        plat_download(namePtr, nameLen, dataPtr, dataLen) {
          const filename = readCString(namePtr, nameLen) || "matoki.png";
          const copy = new Uint8Array(dataLen);
          copy.set(new Uint8Array(memory.buffer, dataPtr, dataLen));
          triggerDownload(filename, copy);
        },
      };
      instance = new WebAssembly.Instance(mod, { env });
      break;
    } catch (err) {
      if (tryShared) throw err;
    }
  }

  const exports = instance.exports;

  try {
    const dropMod = await import("./filedrop.js");
    dropMod.installFileDrop(exports, memory);
  } catch (err) {
    console.warn("[platform] filedrop yüklenemedi:", err);
  }

  if (typeof SharedArrayBuffer !== "undefined" && memory.buffer instanceof SharedArrayBuffer) {
    try {
      const audioMod = await import("./audio.js");
      audioMod.installAudio(exports, memory);
    } catch (err) {
      console.warn("[platform] audio yüklenemedi:", err);
    }
  } else {
    console.log("[platform] Tek iş parçacıklı kip veya SharedArrayBuffer yok: ses atlandı");
  }

  try {
    if (typeof SharedArrayBuffer !== "undefined" && memory.buffer instanceof SharedArrayBuffer && !isSt) {
      const THREAD_MAX_WORKERS = 8;
      const hc = navigator.hardwareConcurrency || 4;
      const n = Math.max(1, Math.min(hc - 1, THREAD_MAX_WORKERS));
      await startThreads(n, mod, memory, table, {
        instance,
        onLog(text, tid) {
          console.log(`[W${tid}] ${text}`);
        },
        onPanic(text, tid) {
          showPanic(`[Worker ${tid}] ${text}`);
        },
      });
    }
  } catch (err) {
    console.warn("[platform] Worker thread havuzu başlatılamadı:", err);
  }

  applyCapsToEngine(exports, caps);
  exports.engine_init(Date.now() >>> 0);

  if (typeof exports.engine_set_screen_size === "function") {
    exports.engine_set_screen_size(screenRes.sw, screenRes.sh);
  }
  if (initialResMode !== 0 && typeof exports.engine_set_resolution_mode === "function") {
    exports.engine_set_resolution_mode(initialResMode);
  }

  window.addEventListener("resize", () => {
    const s = getScreenResolution();
    if (typeof exports.engine_set_screen_size === "function") {
      exports.engine_set_screen_size(s.sw, s.sh);
    }
  });

  globalThis.matoki = {
    exports,
    memory,
    caps,
    workerRender: false,
    callAsync: async (fn, ...args) => (typeof exports[fn] === "function" ? exports[fn](...args) : undefined),
    cycleResolution: () => (typeof exports.engine_cycle_resolution === "function" ? exports.engine_cycle_resolution() : undefined),
    setResolutionMode: (m) => (typeof exports.engine_set_resolution_mode === "function" ? exports.engine_set_resolution_mode(m) : undefined),
    setResolution: (w, h) => (typeof exports.engine_set_resolution === "function" ? exports.engine_set_resolution(w, h) : undefined),
  };

  const initialW = exports.engine_fb_width();
  const initialH = exports.engine_fb_height();
  const inputPtr = exports.engine_input_buf();

  canvas.width = initialW;
  canvas.height = initialH;

  const { wantsLock, isLocked, onNextFrame } = installInputListeners(exports, initialW, initialH, inputPtr);

  let curFbW = 0;
  let curFbH = 0;
  let curFbPtr = 0;
  let curResMode = initialResMode;
  let cachedBuffer = null;
  let isShared = false;
  let fbView = null;
  let localFbBuf = null;
  let imageData = null;

  function refreshFbViews() {
    const fbW = typeof exports.engine_fb_width === "function" ? exports.engine_fb_width() : canvas.width;
    const fbH = typeof exports.engine_fb_height === "function" ? exports.engine_fb_height() : canvas.height;
    const fbPtr = typeof exports.engine_framebuffer === "function" ? exports.engine_framebuffer() : 0;

    if (fbW !== curFbW || fbH !== curFbH || fbPtr !== curFbPtr || cachedBuffer !== memory.buffer) {
      curFbW = fbW;
      curFbH = fbH;
      curFbPtr = fbPtr;
      cachedBuffer = memory.buffer;
      isShared = typeof SharedArrayBuffer !== "undefined" && cachedBuffer instanceof SharedArrayBuffer;

      canvas.width = fbW;
      canvas.height = fbH;

      const fbByteLen = fbW * fbH * 4;
      fbView = new Uint8ClampedArray(cachedBuffer, fbPtr, fbByteLen);
      if (isShared) {
        localFbBuf = new Uint8ClampedArray(fbByteLen);
        imageData = new ImageData(localFbBuf, fbW, fbH);
      } else {
        imageData = new ImageData(fbView, fbW, fbH);
      }

      if (typeof exports.engine_get_resolution_mode === "function") {
        const mode = exports.engine_get_resolution_mode();
        if (mode !== curResMode) {
          curResMode = mode;
          saveResolutionMode(mode);
        }
      }
    }
  }

  refreshFbViews();

  let lastTime = performance.now();

  function frame(now) {
    if (!running) return;
    const dt = Math.min((now - lastTime) * 0.001, 0.1);
    lastTime = now;

    try {
      if (isLocked() && !wantsLock()) document.exitPointerLock();
      if (typeof onNextFrame === "function") onNextFrame();
      exports.engine_frame(now, dt);
    } catch (err) {
      if (running) {
        showPanic(`Wasm çalışma hatası: ${err.message}`);
      }
      return;
    }

    if (!running) return;
    refreshFbViews();
    if (isShared) {
      localFbBuf.set(fbView);
    }
    ctx.putImageData(imageData, 0, 0);
    rafId = requestAnimationFrame(frame);
  }

  rafId = requestAnimationFrame(frame);
}

init().catch((err) => {
  showPanic(`Başlatma hatası: ${err.message}`);
});
