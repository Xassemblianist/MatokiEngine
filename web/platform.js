// web/platform.js — MATOKI tarayıcı platform katmanı (ARCHITECTURE.md §2).
// Wasm modülünü yükler, rAF döngüsünü çalıştırır, girdileri InputState'e yazar
// ve her karede framebuffer'ı ImageData ile canvas'a aktarır.

import { createJitLoader } from "./jit.js";

const decoder = new TextDecoder("utf-8");
const canvas = document.getElementById("screen");
const ctx = canvas.getContext("2d", { alpha: false });
const panicOverlay = document.getElementById("panic-overlay");

let memory = null;
let table = null;

let running = true;
let rafId = 0;

function readCString(ptr, len) {
  const bytes = new Uint8Array(memory.buffer, ptr, len);
  if (memory.buffer instanceof SharedArrayBuffer) {
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

function createMemoryForModule(mod) {
  const imports = WebAssembly.Module.imports(mod);
  for (const imp of imports) {
    if (imp.module === "env" && imp.name === "memory" && imp.kind === "memory") {
      try {
        return new WebAssembly.Memory({ initial: 4096, maximum: 16384, shared: false });
      } catch (_e) {
        return new WebAssembly.Memory({ initial: 4096, maximum: 16384, shared: true });
      }
    }
  }
  return new WebAssembly.Memory({ initial: 4096, maximum: 16384 });
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

async function init() {
  const resp = await fetch("./engine.wasm", { cache: "no-store" });
  const bytes = await resp.arrayBuffer();
  const mod = await WebAssembly.compile(bytes);

  table = createTableForModule(mod);

  let instance = null;
  for (const tryShared of [false, true]) {
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
          const blob = new Blob([copy], { type: "image/png" });
          const url = URL.createObjectURL(blob);
          const a = document.createElement("a");
          a.href = url;
          a.download = filename;
          document.body.appendChild(a);
          a.click();
          document.body.removeChild(a);
          setTimeout(() => URL.revokeObjectURL(url), 1000);
        },
      };
      instance = new WebAssembly.Instance(mod, { env });
      break;
    } catch (err) {
      if (tryShared) throw err;
    }
  }

  const exports = instance.exports;
  // Hata ayıklama / benchmark betikleri için (konsoldan engine_* export'larına erişim)
  globalThis.matoki = { exports, memory };

  // B hattı web/threads.js eklediğinde worker'ları opsiyonel olarak başlat
  try {
    const threadsMod = await import("./threads.js");
    if (threadsMod && typeof threadsMod.startThreads === "function" && memory.buffer instanceof SharedArrayBuffer) {
      const hc = navigator.hardwareConcurrency || 8;
      const n = Math.min(Math.max(hc - 1, 7), 8);
      await threadsMod.startThreads(n, mod, memory, table, {
        instance,
        onLog(text, tid) {
          console.log(`[W${tid}] ${text}`);
        },
        onPanic(text, tid) {
          showPanic(`[Worker ${tid}] ${text}`);
        },
      });
    }
  } catch (_e) {
    // threads.js henüz yoksa veya tek thread modundaysa devam et
  }

  exports.engine_init(Date.now() >>> 0);

  const fbW = exports.engine_fb_width();
  const fbH = exports.engine_fb_height();
  const fbPtr = exports.engine_framebuffer();
  const inputPtr = exports.engine_input_buf();

  canvas.width = fbW;
  canvas.height = fbH;

  let cachedBuffer = null;
  let isShared = false;
  let fbView = null;
  let localFbBuf = null;
  let imageData = null;
  let inputKeys = null;
  let inputI32 = null;
  let inputU32 = null;
  let inputChars = null;

  function refreshViews() {
    if (cachedBuffer === memory.buffer) return;
    cachedBuffer = memory.buffer;
    isShared = typeof SharedArrayBuffer !== "undefined" && cachedBuffer instanceof SharedArrayBuffer;
    const fbByteLen = fbW * fbH * 4;
    fbView = new Uint8ClampedArray(cachedBuffer, fbPtr, fbByteLen);
    if (isShared) {
      if (!localFbBuf || localFbBuf.length !== fbByteLen) {
        localFbBuf = new Uint8ClampedArray(fbByteLen);
        imageData = new ImageData(localFbBuf, fbW, fbH);
      }
    } else {
      imageData = new ImageData(fbView, fbW, fbH);
    }
    inputKeys = new Uint8Array(cachedBuffer, inputPtr, 256);
    inputI32 = new Int32Array(cachedBuffer, inputPtr + 256, 6);
    inputU32 = new Uint32Array(cachedBuffer, inputPtr + 256, 6);
    inputChars = new Uint32Array(cachedBuffer, inputPtr + 280, 33);
  }

  refreshViews();

  function pushCharCodepoint(cp) {
    const count = inputChars[32];
    if (count < 32) {
      inputChars[count] = cp >>> 0;
      inputChars[32] = count + 1;
    }
  }

  // Girdi dinleyicileri — doğrudan paylaşılan InputState belleğine yazar
  window.addEventListener("keydown", (e) => {
    refreshViews();
    const code = e.keyCode & 0xff;
    inputKeys[code] = 1;

    // Tarayıcı kısayollarını engelle (Tab, F1, F12, Backspace, Oklar, Ctrl+Z/Y/C/V/X/A/S/Enter)
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
        }
      } else if (e.key && [...e.key].length === 1) {
        const cp = e.key.codePointAt(0);
        if (cp >= 32) {
          pushCharCodepoint(cp);
        }
      }
    }
  });

  window.addEventListener("keyup", (e) => {
    refreshViews();
    const code = e.keyCode & 0xff;
    inputKeys[code] = 0;
  });

  window.addEventListener("blur", () => {
    refreshViews();
    inputKeys.fill(0);
    inputU32[4] = 0;
    inputChars[32] = 0;
  });

  function updateMouseCoords(e) {
    const rect = canvas.getBoundingClientRect();
    const aspectCanvas = fbW / fbH;
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
    const mx = Math.floor((relX * fbW) / renderW);
    const my = Math.floor((relY * fbH) / renderH);

    const clampedX = Math.max(0, Math.min(fbW - 1, mx));
    const clampedY = Math.max(0, Math.min(fbH - 1, my));

    const prevX = inputI32[0];
    const prevY = inputI32[1];
    inputI32[0] = clampedX;
    inputI32[1] = clampedY;
    inputI32[2] += clampedX - prevX;
    inputI32[3] += clampedY - prevY;
  }

  window.addEventListener("mousemove", (e) => {
    refreshViews();
    updateMouseCoords(e);
  });

  window.addEventListener("mousedown", (e) => {
    refreshViews();
    updateMouseCoords(e);
    inputU32[4] = e.buttons;
  });

  window.addEventListener("mouseup", (e) => {
    refreshViews();
    updateMouseCoords(e);
    inputU32[4] = e.buttons;
  });

  window.addEventListener(
    "wheel",
    (e) => {
      refreshViews();
      inputI32[5] += Math.sign(e.deltaY);
    },
    { passive: true }
  );

  window.addEventListener("contextmenu", (e) => e.preventDefault());

  let lastTime = performance.now();

  function frame(now) {
    if (!running) return;
    const dt = Math.min((now - lastTime) * 0.001, 0.1);
    lastTime = now;

    try {
      exports.engine_frame(now, dt);
    } catch (err) {
      if (running) {
        showPanic(`Wasm çalışma hatası: ${err.message}`);
      }
      return;
    }

    if (!running) return;
    refreshViews();
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
