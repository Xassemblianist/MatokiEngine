// web/caps.js — Tarayıcı ve WebAssembly platform yetenek tespiti (ARCHITECTURE.md §2, D-109).
// SAB/crossOriginIsolated, wasm SIMD128, relaxed SIMD, threads, bulk memory, memory64,
// WebGPU (+ timestamp-query, subgroups, shader-f16, limitler), OffscreenCanvas,
// Atomics.waitAsync, WebCodecs ve WebTransport özelliklerini açılışta tespit eder;
// uygun wasm derlemesini (engine_relaxed.wasm / engine.wasm) seçer ve sonucu
// engine_set_caps(mask, gpu_max_tex2d, gpu_max_buf_mb) ile motora bildirir.

export const CAP_SAB           = 1 << 0;
export const CAP_SIMD128       = 1 << 1;
export const CAP_RELAXED_SIMD  = 1 << 2;
export const CAP_THREADS       = 1 << 3;
export const CAP_BULK_MEMORY   = 1 << 4;
export const CAP_MEMORY64      = 1 << 5;
export const CAP_WEBGPU        = 1 << 6;
export const CAP_GPU_TIMESTAMP = 1 << 7;
export const CAP_GPU_SUBGROUPS = 1 << 8;
export const CAP_GPU_F16       = 1 << 9;
export const CAP_OFFSCREEN     = 1 << 10;
export const CAP_WAIT_ASYNC    = 1 << 11;
export const CAP_WEBCODECS     = 1 << 12;
export const CAP_WEBTRANSPORT  = 1 << 13;
export const CAP_WORKER_RENDER = 1 << 14;
export const CAP_RELAXED_BUILD = 1 << 15;

// Minimal geçerli Wasm modülleri (WebAssembly.validate probları)
// 1) SIMD128: (module (func (result v128) (i32x4.splat (i32.const 0))))
const WASM_PROBE_SIMD128 = new Uint8Array([
  0x00, 0x61, 0x73, 0x6d, 0x01, 0x00, 0x00, 0x00,
  0x01, 0x05, 0x01, 0x60, 0x00, 0x01, 0x7b,
  0x03, 0x02, 0x01, 0x00,
  0x0a, 0x08, 0x01, 0x06, 0x00, 0x41, 0x00, 0xfd, 0x0f, 0x0b,
]);

// 2) Relaxed SIMD: (module (func (param v128) (result v128) local.get 0 local.get 0 local.get 0 f32x4.relaxed_madd))
const WASM_PROBE_RELAXED_SIMD = new Uint8Array([
  0x00, 0x61, 0x73, 0x6d, 0x01, 0x00, 0x00, 0x00,
  0x01, 0x06, 0x01, 0x60, 0x01, 0x7b, 0x01, 0x7b,
  0x03, 0x02, 0x01, 0x00,
  0x0a, 0x0d, 0x01, 0x0b, 0x00, 0x20, 0x00, 0x20, 0x00, 0x20, 0x00, 0xfd, 0x85, 0x02, 0x0b,
]);

// 3) Threads (paylaşımlı bellek + i32.atomic.load)
const WASM_PROBE_THREADS = new Uint8Array([
  0x00, 0x61, 0x73, 0x6d, 0x01, 0x00, 0x00, 0x00,
  0x01, 0x04, 0x01, 0x60, 0x00, 0x00,
  0x03, 0x02, 0x01, 0x00,
  0x05, 0x04, 0x01, 0x03, 0x01, 0x01,
  0x0a, 0x0b, 0x01, 0x09, 0x00, 0x41, 0x00, 0xfe, 0x10, 0x02, 0x00, 0x1a, 0x0b,
]);

// 4) Bulk memory (memory.fill)
const WASM_PROBE_BULK_MEMORY = new Uint8Array([
  0x00, 0x61, 0x73, 0x6d, 0x01, 0x00, 0x00, 0x00,
  0x01, 0x04, 0x01, 0x60, 0x00, 0x00,
  0x03, 0x02, 0x01, 0x00,
  0x05, 0x03, 0x01, 0x00, 0x01,
  0x0a, 0x0d, 0x01, 0x0b, 0x00, 0x41, 0x00, 0x41, 0x00, 0x41, 0x00, 0xfc, 0x0b, 0x00, 0x0b,
]);

// 5) Memory64 (64-bit bellek sınırı bayrağı 0x04 — yalnız rapor)
const WASM_PROBE_MEMORY64 = new Uint8Array([
  0x00, 0x61, 0x73, 0x6d, 0x01, 0x00, 0x00, 0x00,
  0x05, 0x03, 0x01, 0x04, 0x01,
]);

function safeValidate(bytes) {
  try {
    return typeof WebAssembly !== "undefined" && typeof WebAssembly.validate === "function"
      ? WebAssembly.validate(bytes)
      : false;
  } catch (_e) {
    return false;
  }
}

function detectSharedMemory() {
  if (typeof SharedArrayBuffer === "undefined") return false;
  try {
    const mem = new WebAssembly.Memory({ initial: 1, maximum: 1, shared: true });
    return mem.buffer instanceof SharedArrayBuffer;
  } catch (_e) {
    return false;
  }
}

export function selectEngineWasmUrl(caps) {
  if (typeof location !== "undefined" && location.search) {
    const params = new URLSearchParams(location.search);
    if (params.get("st") === "1" || params.get("threads") === "0") return "./engine_st.wasm";
    if (params.get("relaxed") === "0") return "./engine.wasm";
    if (params.get("relaxed") === "1") return "./engine_relaxed.wasm";
  }
  // SharedArrayBuffer veya worker threads yoksa ST yedeğine düş (D-109, H10)
  if (!caps || !caps.sab || !caps.threads) {
    return "./engine_st.wasm";
  }
  return caps.relaxedSimd ? "./engine_relaxed.wasm" : "./engine.wasm";
}

export function applyCapsToEngine(exports, caps) {
  if (!exports || !caps) return;
  if (typeof exports.engine_set_caps === "function") {
    exports.engine_set_caps(
      caps.mask >>> 0,
      (caps.gpuMaxTex2D || 0) >>> 0,
      (caps.gpuMaxBufMB || 0) >>> 0
    );
  }
}

export async function detectCaps() {
  const crossOriginIsolated = Boolean(globalThis.crossOriginIsolated);
  const sab = detectSharedMemory();
  const simd128 = safeValidate(WASM_PROBE_SIMD128);
  const relaxedSimd = simd128 && safeValidate(WASM_PROBE_RELAXED_SIMD);
  const bulkMemory = safeValidate(WASM_PROBE_BULK_MEMORY);
  const memory64 = safeValidate(WASM_PROBE_MEMORY64);
  const threads = sab && typeof Worker !== "undefined" && safeValidate(WASM_PROBE_THREADS);

  const offscreenCanvas =
    typeof OffscreenCanvas !== "undefined" &&
    (typeof HTMLCanvasElement === "undefined" ||
      typeof HTMLCanvasElement.prototype.transferControlToOffscreen === "function");

  const waitAsync = typeof Atomics !== "undefined" && typeof Atomics.waitAsync === "function";
  const webCodecs = typeof VideoEncoder !== "undefined" && typeof VideoDecoder !== "undefined";
  const webTransport = typeof WebTransport !== "undefined";

  let webgpu = false;
  const gpuFeatures = {
    timestampQuery: false,
    subgroups: false,
    shaderF16: false,
  };
  const gpuLimits = {
    maxTextureDimension2D: 0,
    maxBufferSize: 0,
    maxStorageBufferBindingSize: 0,
    maxComputeWorkgroupSizeX: 0,
    maxComputeInvocationsPerWorkgroup: 0,
    maxBindGroups: 0,
  };

  if (typeof navigator !== "undefined" && navigator.gpu && typeof navigator.gpu.requestAdapter === "function") {
    try {
      const adapter = await navigator.gpu.requestAdapter();
      if (adapter) {
        webgpu = true;
        const feats = adapter.features;
        if (feats && typeof feats.has === "function") {
          gpuFeatures.timestampQuery = feats.has("timestamp-query");
          gpuFeatures.subgroups = feats.has("subgroups");
          gpuFeatures.shaderF16 = feats.has("shader-f16");
        }
        const lim = adapter.limits;
        if (lim) {
          gpuLimits.maxTextureDimension2D = lim.maxTextureDimension2D || 0;
          gpuLimits.maxBufferSize = lim.maxBufferSize || 0;
          gpuLimits.maxStorageBufferBindingSize = lim.maxStorageBufferBindingSize || 0;
          gpuLimits.maxComputeWorkgroupSizeX = lim.maxComputeWorkgroupSizeX || 0;
          gpuLimits.maxComputeInvocationsPerWorkgroup = lim.maxComputeInvocationsPerWorkgroup || 0;
          gpuLimits.maxBindGroups = lim.maxBindGroups || 0;
        }
      }
    } catch (_e) {
      webgpu = false;
    }
  }

  let mask = 0;
  if (sab) mask |= CAP_SAB;
  if (simd128) mask |= CAP_SIMD128;
  if (relaxedSimd) mask |= CAP_RELAXED_SIMD;
  if (threads) mask |= CAP_THREADS;
  if (bulkMemory) mask |= CAP_BULK_MEMORY;
  if (memory64) mask |= CAP_MEMORY64;
  if (webgpu) mask |= CAP_WEBGPU;
  if (gpuFeatures.timestampQuery) mask |= CAP_GPU_TIMESTAMP;
  if (gpuFeatures.subgroups) mask |= CAP_GPU_SUBGROUPS;
  if (gpuFeatures.shaderF16) mask |= CAP_GPU_F16;
  if (offscreenCanvas) mask |= CAP_OFFSCREEN;
  if (waitAsync) mask |= CAP_WAIT_ASYNC;
  if (webCodecs) mask |= CAP_WEBCODECS;
  if (webTransport) mask |= CAP_WEBTRANSPORT;

  const gpuMaxTex2D = gpuLimits.maxTextureDimension2D >>> 0;
  const gpuMaxBufMB = Math.floor((gpuLimits.maxBufferSize || 0) / (1024 * 1024)) >>> 0;

  const caps = {
    mask: mask >>> 0,
    crossOriginIsolated,
    sab,
    simd128,
    relaxedSimd,
    threads,
    bulkMemory,
    memory64,
    webgpu,
    gpuFeatures,
    gpuLimits,
    gpuMaxTex2D,
    gpuMaxBufMB,
    offscreenCanvas,
    waitAsync,
    webCodecs,
    webTransport,
    wasmUrl: "",
    isSt: false,
  };
  caps.wasmUrl = selectEngineWasmUrl(caps);
  caps.isSt = caps.wasmUrl.includes("engine_st.wasm");
  if (caps.wasmUrl.includes("relaxed")) {
    caps.mask = (caps.mask | CAP_RELAXED_BUILD) >>> 0;
  }
  return caps;
}
