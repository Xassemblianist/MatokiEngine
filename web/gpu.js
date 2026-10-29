// web/gpu.js — MATOKI WebGPU RHI komut akışı yürütücüsü (ARCHITECTURE.md §2, D-108).
// Bağımlılıksız saf ES modülü; motor mantığı içermez.
// Paylaşımlı bellekteki düz ikili komut akışını (RhiStreamHeader + komutlar) okuyup
// WebGPU çağrılarına çevirir; WGSL ve doğrulama hatalarını RhiErrorRing halkasına yazar.

const decoder = new TextDecoder("utf-8");
const encoder = new TextEncoder();

export const RHI_STREAM_MAGIC = 0x4d475055; // "MGPU"
export const RHI_STREAM_VERSION = 1;

export const RHI_CMD_NOP = 0;
export const RHI_CMD_CREATE_BUFFER = 1;
export const RHI_CMD_CREATE_TEXTURE = 2;
export const RHI_CMD_CREATE_SAMPLER = 3;
export const RHI_CMD_CREATE_SHADER = 4;
export const RHI_CMD_CREATE_PIPELINE = 5;
export const RHI_CMD_CREATE_BIND_GROUP = 6;
export const RHI_CMD_DESTROY_RESOURCE = 7;
export const RHI_CMD_WRITE_BUFFER = 8;
export const RHI_CMD_WRITE_TEXTURE = 9;
export const RHI_CMD_BEGIN_PASS = 10;
export const RHI_CMD_SET_PIPELINE = 11;
export const RHI_CMD_SET_BIND_GROUP = 12;
export const RHI_CMD_SET_VERTEX_BUFFER = 13;
export const RHI_CMD_SET_INDEX_BUFFER = 14;
export const RHI_CMD_DRAW = 15;
export const RHI_CMD_DRAW_INDEXED = 16;
export const RHI_CMD_SET_VIEWPORT = 17;
export const RHI_CMD_END_PASS = 18;
export const RHI_CMD_PRESENT = 19;
export const RHI_CMD_CREATE_COMPUTE_PIPELINE = 20;
export const RHI_CMD_BEGIN_COMPUTE_PASS = 21;
export const RHI_CMD_SET_COMPUTE_PIPELINE = 22;
export const RHI_CMD_DISPATCH = 23;
export const RHI_CMD_END_COMPUTE_PASS = 24;

export const RHI_ERR_WGSL_COMPILE = 1;
export const RHI_ERR_VALIDATION = 2;
export const RHI_ERR_OUT_OF_MEMORY = 3;
export const RHI_ERR_DEVICE_LOST = 4;
export const RHI_ERR_STREAM = 5;

const ERR_RING_OFF_WRITE = 0;
const ERR_RING_OFF_READ = 4;
const ERR_RING_OFF_CAP = 8;
const ERR_RING_OFF_DROPPED = 12;
const ERR_RING_OFF_ENTRIES = 16;
const ERR_ENTRY_SIZE = 256;
const ERR_MSG_MAX = 236;

// WebGPU GPUBufferUsage, GPUTextureUsage ve GPUMapMode bayrak sabitleri (tarayıcıda global yoksa yedek)
const GPU_BUF_MAP_READ = 0x0001;
const GPU_BUF_COPY_SRC = 0x0004;
const GPU_BUF_COPY_DST = 0x0008;
const GPU_BUF_INDEX = 0x0010;
const GPU_BUF_VERTEX = 0x0020;
const GPU_BUF_UNIFORM = 0x0040;
const GPU_BUF_STORAGE = 0x0080;
const GPU_BUF_QUERY_RESOLVE = 0x0200;
const GPU_MAP_READ = 0x0001;

const GPU_TEX_COPY_SRC = 0x01;
const GPU_TEX_COPY_DST = 0x02;
const GPU_TEX_TEXTURE_BINDING = 0x04;
const GPU_TEX_STORAGE_BINDING = 0x08;
const GPU_TEX_RENDER_ATTACHMENT = 0x10;

function decodeBytes(memory, ptr, len) {
  const bytes = new Uint8Array(memory.buffer, ptr, len);
  if (typeof SharedArrayBuffer !== "undefined" && memory.buffer instanceof SharedArrayBuffer) {
    return decoder.decode(bytes.slice());
  }
  return decoder.decode(bytes);
}

function readFixedString16(memory, ptr) {
  const bytes = new Uint8Array(memory.buffer, ptr, 16);
  let end = 0;
  while (end < 16 && bytes[end] !== 0) end++;
  return decodeBytes(memory, ptr, end);
}

function getUploadBytes(memory, ptr, len) {
  const bytes = new Uint8Array(memory.buffer, ptr, len);
  if (typeof SharedArrayBuffer !== "undefined" && memory.buffer instanceof SharedArrayBuffer) {
    return bytes.slice();
  }
  return bytes;
}

export function pushGpuError(exports, memory, kind, handle, line, col, message) {
  if (!exports || typeof exports.engine_gpu_error_ring !== "function") return false;
  const ringPtr = exports.engine_gpu_error_ring() >>> 0;
  if (!ringPtr) return false;

  const buf = memory.buffer;
  const dv = new DataView(buf);
  const i32 = new Int32Array(buf);
  const baseIdx = ringPtr >>> 2;

  const w = Atomics.load(i32, baseIdx + (ERR_RING_OFF_WRITE >>> 2)) >>> 0;
  const r = Atomics.load(i32, baseIdx + (ERR_RING_OFF_READ >>> 2)) >>> 0;
  let cap = Atomics.load(i32, baseIdx + (ERR_RING_OFF_CAP >>> 2)) >>> 0;
  if (cap === 0) {
    cap = 16;
    Atomics.store(i32, baseIdx + (ERR_RING_OFF_CAP >>> 2), cap);
  }

  if ((w - r) >>> 0 >= cap) {
    const dropped = dv.getUint32(ringPtr + ERR_RING_OFF_DROPPED, true);
    dv.setUint32(ringPtr + ERR_RING_OFF_DROPPED, (dropped + 1) >>> 0, true);
    return false;
  }

  const slotOff = ringPtr + ERR_RING_OFF_ENTRIES + (w & (cap - 1)) * ERR_ENTRY_SIZE;
  const slotBytes = new Uint8Array(buf, slotOff, ERR_ENTRY_SIZE);
  slotBytes.fill(0);

  const encoded = encoder.encode(String(message || ""));
  const msgLen = Math.min(encoded.length, ERR_MSG_MAX - 1);

  dv.setUint32(slotOff + 0, kind >>> 0, true);
  dv.setUint32(slotOff + 4, handle >>> 0, true);
  dv.setUint32(slotOff + 8, line >>> 0, true);
  dv.setUint32(slotOff + 12, col >>> 0, true);
  dv.setUint32(slotOff + 16, msgLen >>> 0, true);
  new Uint8Array(buf, slotOff + 20, msgLen).set(encoded.subarray(0, msgLen));

  Atomics.store(i32, baseIdx + (ERR_RING_OFF_WRITE >>> 2), (w + 1) >>> 0);
  return true;
}

function mapBufferUsage(rhiUsage) {
  let u = 0;
  if (rhiUsage & 1) u |= GPU_BUF_VERTEX;
  if (rhiUsage & 2) u |= GPU_BUF_INDEX;
  if (rhiUsage & 4) u |= GPU_BUF_UNIFORM;
  if (rhiUsage & 8) u |= GPU_BUF_COPY_DST;
  if (rhiUsage & 16) u |= GPU_BUF_COPY_SRC;
  if (rhiUsage & 32) u |= GPU_BUF_STORAGE;
  return u;
}

function mapTextureFormat(rhiFormat, canvasFormat) {
  switch (rhiFormat) {
    case 0:
      return canvasFormat;
    case 1:
      return "rgba8unorm";
    case 2:
      return "bgra8unorm";
    case 3:
      return "depth24plus";
    case 4:
      return "depth32float";
    case 5:
      return "rgba16float";
    case 6:
      return "rgba32float";
    default:
      return "rgba8unorm";
  }
}

function mapTextureUsage(rhiUsage) {
  let u = 0;
  if (rhiUsage & 1) u |= GPU_TEX_TEXTURE_BINDING;
  if (rhiUsage & 2) u |= GPU_TEX_RENDER_ATTACHMENT;
  if (rhiUsage & 4) u |= GPU_TEX_COPY_DST;
  if (rhiUsage & 8) u |= GPU_TEX_COPY_SRC;
  if (rhiUsage & 16) u |= GPU_TEX_STORAGE_BINDING;
  return u;
}

function mapAddressMode(mode) {
  return mode === 1 ? "clamp-to-edge" : "repeat";
}

function mapFilterMode(mode) {
  return mode === 1 ? "linear" : "nearest";
}

function mapVertexFormat(fmt) {
  switch (fmt) {
    case 1:
      return "float32";
    case 2:
      return "float32x2";
    case 3:
      return "float32x3";
    case 4:
      return "float32x4";
    case 5:
      return "uint32";
    case 6:
      return "unorm8x4";
    default:
      return "float32x4";
  }
}

function mapTopology(top) {
  return top === 1 ? "line-list" : "triangle-list";
}

function mapCullMode(cull) {
  if (cull === 1) return "back";
  if (cull === 2) return "front";
  return "none";
}

function mapFrontFace(ff) {
  return ff === 1 ? "cw" : "ccw";
}

function mapCompareFunc(cmp) {
  switch (cmp) {
    case 0:
      return "always";
    case 1:
      return "less";
    case 2:
      return "less-equal";
    case 3:
      return "greater";
    case 4:
      return "greater-equal";
    case 5:
      return "equal";
    case 6:
      return "not-equal";
    case 7:
      return "never";
    default:
      return "always";
  }
}

function mapBlendState(blendMode) {
  if (blendMode === 1) {
    return {
      color: { srcFactor: "src-alpha", dstFactor: "one-minus-src-alpha", operation: "add" },
      alpha: { srcFactor: "one", dstFactor: "one-minus-src-alpha", operation: "add" },
    };
  }
  if (blendMode === 2) {
    return {
      color: { srcFactor: "src-alpha", dstFactor: "one", operation: "add" },
      alpha: { srcFactor: "one", dstFactor: "one", operation: "add" },
    };
  }
  return undefined;
}

export async function initGpu(canvas, exports, memory, options = {}) {
  const gpuRoot = options.gpu || (typeof navigator !== "undefined" ? navigator.gpu : null);
  if (!gpuRoot) {
    if (exports && typeof exports.engine_gpu_set_available === "function") {
      exports.engine_gpu_set_available(0);
    }
    return null;
  }

  let adapter = null;
  let device = null;
  let hasTimestampQuery = false;
  try {
    adapter = await gpuRoot.requestAdapter();
    if (!adapter) throw new Error("WebGPU adapter bulunamadı");
    const requiredFeatures = [];
    if (adapter.features && typeof adapter.features.has === "function" && adapter.features.has("timestamp-query")) {
      requiredFeatures.push("timestamp-query");
      hasTimestampQuery = true;
    }
    try {
      device = await adapter.requestDevice(
        requiredFeatures.length > 0 ? { requiredFeatures } : undefined
      );
    } catch (_featErr) {
      hasTimestampQuery = false;
      device = await adapter.requestDevice();
    }
  } catch (_err) {
    if (exports && typeof exports.engine_gpu_set_available === "function") {
      exports.engine_gpu_set_available(0);
    }
    return null;
  }

  let surfaceWidth = (canvas && canvas.width ? canvas.width : options.width || 640) >>> 0;
  let surfaceHeight = (canvas && canvas.height ? canvas.height : options.height || 360) >>> 0;
  const context = options.context || null;
  const ctx2d = !context && canvas && typeof canvas.getContext === "function" ? canvas.getContext("2d") : null;
  if (!context && !ctx2d && canvas) {
    if (exports && typeof exports.engine_gpu_set_available === "function") {
      exports.engine_gpu_set_available(0);
    }
    return null;
  }

  if (exports && typeof exports.engine_gpu_set_viewport_size === "function") {
    exports.engine_gpu_set_viewport_size(surfaceWidth, surfaceHeight);
  }
  if (exports && typeof exports.engine_gpu_set_has_timestamp === "function") {
    exports.engine_gpu_set_has_timestamp(hasTimestampQuery ? 1 : 0);
  }

  const canvasFormat = context
    ? typeof gpuRoot.getPreferredCanvasFormat === "function"
      ? gpuRoot.getPreferredCanvasFormat()
      : "bgra8unorm"
    : "rgba8unorm";

  if (context && typeof context.configure === "function") {
    context.configure({
      device,
      format: canvasFormat,
      alphaMode: "opaque",
    });
  }

  let surfaceTexture = null;
  let surfaceView = null;
  let readbackBytesPerRow = 0;
  let readbackByteSize = 0;
  let readbackBuffer = null;
  let imageData = null;
  let msaaColorTex = null;
  let msaaColorView = null;
  let msaaColorW = 0;
  let msaaColorH = 0;
  let msaaColorSamples = 0;
  let msaaColorFmt = "";

  function allocSurfaceTargets(w, h) {
    surfaceWidth = w >>> 0;
    surfaceHeight = h >>> 0;
    if (exports && typeof exports.engine_gpu_set_viewport_size === "function") {
      exports.engine_gpu_set_viewport_size(surfaceWidth, surfaceHeight);
    }
    if (surfaceTexture && typeof surfaceTexture.destroy === "function") surfaceTexture.destroy();
    if (readbackBuffer && typeof readbackBuffer.destroy === "function") readbackBuffer.destroy();
    if (msaaColorTex && typeof msaaColorTex.destroy === "function") {
      msaaColorTex.destroy();
      msaaColorTex = null;
      msaaColorView = null;
    }
    surfaceTexture =
      !context && typeof device.createTexture === "function"
        ? device.createTexture({
            size: { width: surfaceWidth, height: surfaceHeight, depthOrArrayLayers: 1 },
            mipLevelCount: 1,
            format: canvasFormat,
            usage: GPU_TEX_RENDER_ATTACHMENT | GPU_TEX_COPY_SRC | GPU_TEX_TEXTURE_BINDING,
          })
        : null;
    surfaceView = surfaceTexture ? surfaceTexture.createView() : null;
    readbackBytesPerRow = (surfaceWidth * 4 + 255) & ~255;
    readbackByteSize = readbackBytesPerRow * surfaceHeight;
    readbackBuffer =
      surfaceTexture && typeof device.createBuffer === "function"
        ? device.createBuffer({
            size: readbackByteSize,
            usage: GPU_BUF_COPY_DST | GPU_BUF_MAP_READ,
          })
        : null;
    imageData =
      ctx2d && typeof ctx2d.createImageData === "function"
        ? ctx2d.createImageData(surfaceWidth, surfaceHeight)
        : null;
  }

  allocSurfaceTargets(surfaceWidth, surfaceHeight);

  const MAX_TIMESTAMP_QUERIES = 16;
  let querySet = null;
  let queryResolveBuf = null;
  let queryReadbackBuf = null;
  let queryIndex = 0;
  let queryReadbackInFlight = false;
  let lastGpuTimeUs = 0;

  if (hasTimestampQuery && typeof device.createQuerySet === "function") {
    try {
      querySet = device.createQuerySet({ type: "timestamp", count: MAX_TIMESTAMP_QUERIES });
      queryResolveBuf = device.createBuffer({
        size: MAX_TIMESTAMP_QUERIES * 8,
        usage: GPU_BUF_QUERY_RESOLVE | GPU_BUF_COPY_SRC,
      });
      queryReadbackBuf = device.createBuffer({
        size: MAX_TIMESTAMP_QUERIES * 8,
        usage: GPU_BUF_COPY_DST | GPU_BUF_MAP_READ,
      });
    } catch (_qErr) {
      querySet = null;
      hasTimestampQuery = false;
      if (exports && typeof exports.engine_gpu_set_has_timestamp === "function") {
        exports.engine_gpu_set_has_timestamp(0);
      }
    }
  }
  let readbackInFlight = false;
  let readbackPromise = null;
  let lastPixels = null;
  let readbackCount = 0;

  async function finishReadback() {
    try {
      if (!readbackBuffer || typeof readbackBuffer.mapAsync !== "function") return null;
      await readbackBuffer.mapAsync(GPU_MAP_READ);
      const mapped = new Uint8Array(readbackBuffer.getMappedRange(0, readbackByteSize));
      const rowBytes = surfaceWidth * 4;
      const totalBytes = rowBytes * surfaceHeight;
      if (!lastPixels || lastPixels.length !== totalBytes) {
        lastPixels = new Uint8Array(totalBytes);
      }
      if (readbackBytesPerRow === rowBytes) {
        lastPixels.set(mapped.subarray(0, totalBytes));
      } else {
        for (let y = 0; y < surfaceHeight; y++) {
          const srcOff = y * readbackBytesPerRow;
          lastPixels.set(mapped.subarray(srcOff, srcOff + rowBytes), y * rowBytes);
        }
      }
      readbackBuffer.unmap();

      if (
        options.enableTestReadback !== false &&
        exports &&
        memory &&
        typeof exports.engine_gpu_test_readback_buf === "function"
      ) {
        const dstPtr = exports.engine_gpu_test_readback_buf() >>> 0;
        const dstLen =
          typeof exports.engine_gpu_test_readback_bytes === "function"
            ? exports.engine_gpu_test_readback_bytes() >>> 0
            : 0;
        if (dstPtr && dstLen === totalBytes) {
          new Uint8Array(memory.buffer, dstPtr, totalBytes).set(lastPixels);
          if (typeof exports.engine_gpu_test_analyze_readback === "function") {
            exports.engine_gpu_test_analyze_readback();
          }
        }
      }

      if (ctx2d && imageData) {
        imageData.data.set(lastPixels);
        ctx2d.putImageData(imageData, 0, 0);
      }
      readbackCount++;
      return lastPixels;
    } catch (_err) {
      return null;
    } finally {
      readbackInFlight = false;
    }
  }

  if (typeof device.addEventListener === "function") {
    device.addEventListener("uncapturederror", (ev) => {
      const msg = ev && ev.error ? ev.error.message : "WebGPU yakalanmamış hata";
      pushGpuError(exports, memory, RHI_ERR_VALIDATION, 0, 0, 0, msg);
    });
  }

  if (device.lost && typeof device.lost.then === "function") {
    device.lost.then((info) => {
      const msg = info && info.message ? info.message : "WebGPU cihazı kaybedildi";
      pushGpuError(exports, memory, RHI_ERR_DEVICE_LOST, 0, 0, 0, msg);
      if (exports && typeof exports.engine_gpu_set_available === "function") {
        exports.engine_gpu_set_available(0);
      }
    });
  }

  if (exports && typeof exports.engine_gpu_set_available === "function") {
    exports.engine_gpu_set_available(1);
  }

  const resources = new Map();
  let cmdEncoder = null;
  let passEncoder = null;
  let computePassEncoder = null;

  function ensureMsaaColorView(w, h, fmt, samples) {
    if (
      msaaColorView &&
      msaaColorW === w &&
      msaaColorH === h &&
      msaaColorFmt === fmt &&
      msaaColorSamples === samples
    ) {
      return msaaColorView;
    }
    if (msaaColorTex && typeof msaaColorTex.destroy === "function") {
      msaaColorTex.destroy();
    }
    msaaColorTex = device.createTexture({
      size: { width: w, height: h, depthOrArrayLayers: 1 },
      sampleCount: samples,
      format: fmt,
      usage: GPU_TEX_RENDER_ATTACHMENT,
    });
    msaaColorView = msaaColorTex ? msaaColorTex.createView() : null;
    msaaColorW = w;
    msaaColorH = h;
    msaaColorFmt = fmt;
    msaaColorSamples = samples;
    return msaaColorView;
  }

  function reportStreamError(handle, msg) {
    pushGpuError(exports, memory, RHI_ERR_STREAM, handle, 0, 0, msg);
  }

  function withValidationScope(handle, fn) {
    const hasScope =
      typeof device.pushErrorScope === "function" && typeof device.popErrorScope === "function";
    if (hasScope) device.pushErrorScope("validation");
    try {
      return fn();
    } finally {
      if (hasScope) {
        device.popErrorScope().then((err) => {
          if (err) {
            pushGpuError(exports, memory, RHI_ERR_VALIDATION, handle, 0, 0, err.message || String(err));
          }
        });
      }
    }
  }

  function executeStream(streamPtr, byteLen) {
    const ptr = streamPtr >>> 0;
    const len = byteLen >>> 0;
    if (!ptr || len < 16 || (len & 3) !== 0) {
      reportStreamError(0, `Geçersiz akış başlığı (ptr=${ptr}, len=${len})`);
      return 0;
    }

    const dv = new DataView(memory.buffer);
    const magic = dv.getUint32(ptr + 0, true);
    const version = dv.getUint32(ptr + 4, true);
    const headerLen = dv.getUint32(ptr + 8, true);
    const cmdCount = dv.getUint32(ptr + 12, true);

    if (magic !== RHI_STREAM_MAGIC || version !== RHI_STREAM_VERSION || headerLen !== len) {
      reportStreamError(
        0,
        `Akış başlığı uyuşmazlığı (magic=0x${magic.toString(16)}, v=${version}, len=${headerLen}/${len})`
      );
      return 0;
    }

    let off = ptr + 16;
    const end = ptr + len;
    let executed = 0;

    while (off < end) {
      if (end - off < 8) {
        reportStreamError(0, "Kesik komut başlığı");
        break;
      }
      const opcode = dv.getUint32(off + 0, true);
      const size = dv.getUint32(off + 4, true);
      if (size < 8 || (size & 3) !== 0 || size > end - off) {
        reportStreamError(0, `Geçersiz komut boyutu (op=${opcode}, size=${size})`);
        break;
      }

      switch (opcode) {
        case RHI_CMD_NOP:
          break;

        case RHI_CMD_CREATE_BUFFER: {
          const handle = dv.getUint32(off + 8, true);
          const byteSize = dv.getUint32(off + 12, true);
          const usage = dv.getUint32(off + 16, true);
          const alignedSize = (byteSize + 3) & ~3;
          const buf = withValidationScope(handle, () =>
            device.createBuffer({
              size: Math.max(alignedSize, 4),
              usage: mapBufferUsage(usage),
            })
          );
          resources.set(handle, buf);
          break;
        }

        case RHI_CMD_CREATE_TEXTURE: {
          const handle = dv.getUint32(off + 8, true);
          const width = dv.getUint32(off + 12, true);
          const height = dv.getUint32(off + 16, true);
          const mipLevels = dv.getUint32(off + 20, true);
          const format = dv.getUint32(off + 24, true);
          const usage = dv.getUint32(off + 28, true);
          const gpuFormat = mapTextureFormat(format, canvasFormat);
          const tex = withValidationScope(handle, () =>
            device.createTexture({
              size: { width, height, depthOrArrayLayers: 1 },
              mipLevelCount: mipLevels,
              format: gpuFormat,
              usage: mapTextureUsage(usage),
            })
          );
          resources.set(handle, {
            texture: tex,
            view: tex.createView(),
            width,
            height,
            format: gpuFormat,
          });
          break;
        }

        case RHI_CMD_CREATE_SAMPLER: {
          const handle = dv.getUint32(off + 8, true);
          const addrU = dv.getUint32(off + 12, true);
          const addrV = dv.getUint32(off + 16, true);
          const magFilter = dv.getUint32(off + 20, true);
          const minFilter = dv.getUint32(off + 24, true);
          const mipFilter = dv.getUint32(off + 28, true);
          const sampler = withValidationScope(handle, () =>
            device.createSampler({
              addressModeU: mapAddressMode(addrU),
              addressModeV: mapAddressMode(addrV),
              magFilter: mapFilterMode(magFilter),
              minFilter: mapFilterMode(minFilter),
              mipmapFilter: mapFilterMode(mipFilter),
            })
          );
          resources.set(handle, sampler);
          break;
        }

        case RHI_CMD_CREATE_SHADER: {
          const handle = dv.getUint32(off + 8, true);
          const codeLen = dv.getUint32(off + 12, true);
          const code = decodeBytes(memory, off + 16, codeLen);
          const mod = withValidationScope(handle, () => device.createShaderModule({ code }));
          resources.set(handle, mod);
          if (mod && typeof mod.getCompilationInfo === "function") {
            mod.getCompilationInfo().then((info) => {
              if (!info || !info.messages) return;
              for (const m of info.messages) {
                if (m.type === "error") {
                  pushGpuError(
                    exports,
                    memory,
                    RHI_ERR_WGSL_COMPILE,
                    handle,
                    m.lineNum || 0,
                    m.linePos || 0,
                    m.message || "WGSL derleme hatası"
                  );
                }
              }
            });
          }
          break;
        }

        case RHI_CMD_CREATE_PIPELINE: {
          const handle = dv.getUint32(off + 8, true);
          const vsHandle = dv.getUint32(off + 12, true);
          const fsHandle = dv.getUint32(off + 16, true);
          const vsEntry = readFixedString16(memory, off + 20) || "vs_main";
          const fsEntry = readFixedString16(memory, off + 36) || "fs_main";
          const vertexStride = dv.getUint32(off + 52, true);
          const attrCount = dv.getUint32(off + 56, true);
          const topology = dv.getUint32(off + 60, true);
          const cullMode = dv.getUint32(off + 64, true);
          const frontFace = dv.getUint32(off + 68, true);
          const colorFormat = dv.getUint32(off + 72, true);
          const blendMode = dv.getUint32(off + 76, true);
          const depthFormat = dv.getUint32(off + 80, true);
          const depthTestRaw = dv.getUint32(off + 84, true);
          const depthTest = depthTestRaw & 0xff;
          const sampleCount = ((depthTestRaw >>> 8) & 0xff) || 1;
          const depthWrite = dv.getUint32(off + 88, true);
          const depthCompare = dv.getUint32(off + 92, true);

          const vsMod = resources.get(vsHandle);
          const fsMod = fsHandle ? resources.get(fsHandle) : null;
          if (!vsMod) {
            reportStreamError(handle, "Pipeline için geçersiz vertex shader tutamacı");
            break;
          }

          const attrs = [];
          for (let i = 0; i < attrCount; i++) {
            const aOff = off + 96 + i * 12;
            attrs.push({
              shaderLocation: dv.getUint32(aOff + 0, true),
              offset: dv.getUint32(aOff + 4, true),
              format: mapVertexFormat(dv.getUint32(aOff + 8, true)),
            });
          }

          const pDesc = {
            layout: "auto",
            vertex: {
              module: vsMod,
              entryPoint: vsEntry,
              buffers:
                attrCount > 0
                  ? [
                      {
                        arrayStride: vertexStride,
                        stepMode: "vertex",
                        attributes: attrs,
                      },
                    ]
                  : [],
            },
            primitive: {
              topology: mapTopology(topology),
              cullMode: mapCullMode(cullMode),
              frontFace: mapFrontFace(frontFace),
            },
          };

          if (sampleCount > 1) {
            pDesc.multisample = { count: sampleCount };
          }

          if (fsMod) {
            const target = {
              format: mapTextureFormat(colorFormat, canvasFormat),
            };
            const blend = mapBlendState(blendMode);
            if (blend) target.blend = blend;
            pDesc.fragment = {
              module: fsMod,
              entryPoint: fsEntry,
              targets: [target],
            };
          }

          if (depthFormat !== 0) {
            pDesc.depthStencil = {
              format: mapTextureFormat(depthFormat, canvasFormat),
              depthWriteEnabled: depthWrite !== 0,
              depthCompare: depthTest !== 0 ? mapCompareFunc(depthCompare) : "always",
            };
          }

          const pipe = withValidationScope(handle, () => device.createRenderPipeline(pDesc));
          resources.set(handle, pipe);
          break;
        }

        case RHI_CMD_CREATE_COMPUTE_PIPELINE: {
          const handle = dv.getUint32(off + 8, true);
          const csHandle = dv.getUint32(off + 12, true);
          const entry = readFixedString16(memory, off + 16) || "main";
          const csMod = resources.get(csHandle);
          if (!csMod) {
            reportStreamError(handle, "ComputePipeline için geçersiz compute shader tutamacı");
            break;
          }
          const cpipe = withValidationScope(handle, () =>
            device.createComputePipeline({
              layout: "auto",
              compute: {
                module: csMod,
                entryPoint: entry,
              },
            })
          );
          resources.set(handle, cpipe);
          break;
        }

        case RHI_CMD_CREATE_BIND_GROUP: {
          const handle = dv.getUint32(off + 8, true);
          const pipeHandle = dv.getUint32(off + 12, true);
          const groupIndex = dv.getUint32(off + 16, true);
          const entryCount = dv.getUint32(off + 20, true);
          const pipe = resources.get(pipeHandle);
          if (!pipe) {
            reportStreamError(handle, "BindGroup için geçersiz pipeline tutamacı");
            break;
          }

          const entries = [];
          for (let i = 0; i < entryCount; i++) {
            const eOff = off + 24 + i * 20;
            const binding = dv.getUint32(eOff + 0, true);
            const kind = dv.getUint32(eOff + 4, true);
            const resHandle = dv.getUint32(eOff + 8, true);
            const bufOffset = dv.getUint32(eOff + 12, true);
            const bufSize = dv.getUint32(eOff + 16, true);
            const resObj = resources.get(resHandle);
            if (!resObj) {
              reportStreamError(handle, `BindGroup girdisi için geçersiz kaynak (h=${resHandle})`);
              continue;
            }
            if (kind === 1 || kind === 4) {
              const bEntry = { buffer: resObj, offset: bufOffset };
              if (bufSize > 0) bEntry.size = (bufSize + 3) & ~3;
              entries.push({ binding, resource: bEntry });
            } else if (kind === 2 || kind === 5) {
              entries.push({ binding, resource: resObj.view || resObj });
            } else if (kind === 3) {
              entries.push({ binding, resource: resObj });
            }
          }

          const bg = withValidationScope(handle, () =>
            device.createBindGroup({
              layout: pipe.getBindGroupLayout(groupIndex),
              entries,
            })
          );
          resources.set(handle, bg);
          break;
        }

        case RHI_CMD_DESTROY_RESOURCE: {
          const handle = dv.getUint32(off + 8, true);
          const obj = resources.get(handle);
          if (obj) {
            if (typeof obj.destroy === "function") obj.destroy();
            else {
              if (obj.texture && typeof obj.texture.destroy === "function") obj.texture.destroy();
              if (obj.msaaTex && typeof obj.msaaTex.destroy === "function") obj.msaaTex.destroy();
            }
            resources.delete(handle);
          }
          break;
        }

        case RHI_CMD_WRITE_BUFFER: {
          const dstHandle = dv.getUint32(off + 8, true);
          const dstOffset = dv.getUint32(off + 12, true);
          const dataLen = dv.getUint32(off + 16, true);
          const alignedLen = (dataLen + 3) & ~3;
          const buf = resources.get(dstHandle);
          if (buf) {
            const upload = getUploadBytes(memory, off + 20, alignedLen);
            device.queue.writeBuffer(buf, dstOffset, upload, 0, alignedLen);
          }
          break;
        }

        case RHI_CMD_WRITE_TEXTURE: {
          const dstHandle = dv.getUint32(off + 8, true);
          const mipLevel = dv.getUint32(off + 12, true);
          const x = dv.getUint32(off + 16, true);
          const y = dv.getUint32(off + 20, true);
          const width = dv.getUint32(off + 24, true);
          const height = dv.getUint32(off + 28, true);
          const bytesPerRow = dv.getUint32(off + 32, true);
          const dataLen = dv.getUint32(off + 36, true);
          const texEntry = resources.get(dstHandle);
          if (texEntry && texEntry.texture) {
            const upload = getUploadBytes(memory, off + 40, dataLen);
            device.queue.writeTexture(
              { texture: texEntry.texture, mipLevel, origin: { x, y, z: 0 } },
              upload,
              { offset: 0, bytesPerRow, rowsPerImage: height },
              { width, height, depthOrArrayLayers: 1 }
            );
          }
          break;
        }

        case RHI_CMD_BEGIN_PASS: {
          const colorHandle = dv.getUint32(off + 8, true);
          const depthHandle = dv.getUint32(off + 12, true);
          const flags = dv.getUint32(off + 16, true);
          const sampleCount = ((flags >>> 8) & 0xff) || 1;
          const clearR = dv.getFloat32(off + 20, true);
          const clearG = dv.getFloat32(off + 24, true);
          const clearB = dv.getFloat32(off + 28, true);
          const clearA = dv.getFloat32(off + 32, true);
          const clearDepth = dv.getFloat32(off + 36, true);

          if (!cmdEncoder) {
            cmdEncoder = device.createCommandEncoder();
            queryIndex = 0;
          }

          let colorView = null;
          let targetW = surfaceWidth;
          let targetH = surfaceHeight;
          let targetFmt = canvasFormat;
          if (colorHandle === 0) {
            colorView = context ? context.getCurrentTexture().createView() : surfaceView;
          } else {
            const cEntry = resources.get(colorHandle);
            if (cEntry) {
              colorView = cEntry.view;
              targetW = cEntry.width;
              targetH = cEntry.height;
              targetFmt = cEntry.format;
            }
          }

          const colorAttachment = {
            view: sampleCount > 1 ? ensureMsaaColorView(targetW, targetH, targetFmt, sampleCount) : colorView,
            loadOp: flags & 1 ? "clear" : "load",
            clearValue: { r: clearR, g: clearG, b: clearB, a: clearA },
            storeOp: "store",
          };
          if (sampleCount > 1 && colorView) {
            colorAttachment.resolveTarget = colorView;
          }

          const passDesc = {
            colorAttachments: [colorAttachment],
          };

          if (depthHandle !== 0) {
            const dEntry = resources.get(depthHandle);
            if (dEntry && dEntry.view) {
              let depthView = dEntry.view;
              if (sampleCount > 1) {
                if (
                  !dEntry.msaaView ||
                  dEntry.msaaSamples !== sampleCount ||
                  dEntry.msaaW !== dEntry.width ||
                  dEntry.msaaH !== dEntry.height
                ) {
                  if (dEntry.msaaTex && typeof dEntry.msaaTex.destroy === "function") {
                    dEntry.msaaTex.destroy();
                  }
                  dEntry.msaaTex = device.createTexture({
                    size: { width: dEntry.width, height: dEntry.height, depthOrArrayLayers: 1 },
                    sampleCount,
                    format: dEntry.format,
                    usage: GPU_TEX_RENDER_ATTACHMENT,
                  });
                  dEntry.msaaView = dEntry.msaaTex ? dEntry.msaaTex.createView() : dEntry.view;
                  dEntry.msaaSamples = sampleCount;
                  dEntry.msaaW = dEntry.width;
                  dEntry.msaaH = dEntry.height;
                }
                depthView = dEntry.msaaView;
              }
              passDesc.depthStencilAttachment = {
                view: depthView,
                depthLoadOp: flags & 2 ? "clear" : "load",
                depthClearValue: clearDepth,
                depthStoreOp: "store",
              };
            }
          }

          if (querySet && queryIndex + 2 <= MAX_TIMESTAMP_QUERIES) {
            passDesc.timestampWrites = {
              querySet,
              beginningOfPassWriteIndex: queryIndex,
              endOfPassWriteIndex: queryIndex + 1,
            };
            queryIndex += 2;
          }

          passEncoder = cmdEncoder.beginRenderPass(passDesc);
          break;
        }

        case RHI_CMD_SET_PIPELINE: {
          const pipeHandle = dv.getUint32(off + 8, true);
          const pipe = resources.get(pipeHandle);
          if (passEncoder && pipe) {
            passEncoder.setPipeline(pipe);
          }
          break;
        }

        case RHI_CMD_SET_BIND_GROUP: {
          const groupIndex = dv.getUint32(off + 8, true);
          const bgHandle = dv.getUint32(off + 12, true);
          const bg = resources.get(bgHandle);
          if (passEncoder && bg) {
            passEncoder.setBindGroup(groupIndex, bg);
          } else if (computePassEncoder && bg) {
            computePassEncoder.setBindGroup(groupIndex, bg);
          }
          break;
        }

        case RHI_CMD_SET_VERTEX_BUFFER: {
          const slot = dv.getUint32(off + 8, true);
          const bufHandle = dv.getUint32(off + 12, true);
          const offset = dv.getUint32(off + 16, true);
          const bSize = dv.getUint32(off + 20, true);
          const buf = resources.get(bufHandle);
          if (passEncoder && buf) {
            if (bSize > 0) passEncoder.setVertexBuffer(slot, buf, offset, bSize);
            else passEncoder.setVertexBuffer(slot, buf, offset);
          }
          break;
        }

        case RHI_CMD_SET_INDEX_BUFFER: {
          const bufHandle = dv.getUint32(off + 8, true);
          const idxFmt = dv.getUint32(off + 12, true);
          const offset = dv.getUint32(off + 16, true);
          const bSize = dv.getUint32(off + 20, true);
          const buf = resources.get(bufHandle);
          if (passEncoder && buf) {
            const fmtStr = idxFmt === 0 ? "uint16" : "uint32";
            if (bSize > 0) passEncoder.setIndexBuffer(buf, fmtStr, offset, bSize);
            else passEncoder.setIndexBuffer(buf, fmtStr, offset);
          }
          break;
        }

        case RHI_CMD_DRAW: {
          const vertexCount = dv.getUint32(off + 8, true);
          const instanceCount = dv.getUint32(off + 12, true);
          const firstVertex = dv.getUint32(off + 16, true);
          const firstInstance = dv.getUint32(off + 20, true);
          if (passEncoder) {
            passEncoder.draw(vertexCount, instanceCount, firstVertex, firstInstance);
          }
          break;
        }

        case RHI_CMD_DRAW_INDEXED: {
          const indexCount = dv.getUint32(off + 8, true);
          const instanceCount = dv.getUint32(off + 12, true);
          const firstIndex = dv.getUint32(off + 16, true);
          const baseVertex = dv.getInt32(off + 20, true);
          const firstInstance = dv.getUint32(off + 24, true);
          if (passEncoder) {
            passEncoder.drawIndexed(indexCount, instanceCount, firstIndex, baseVertex, firstInstance);
          }
          break;
        }

        case RHI_CMD_SET_VIEWPORT: {
          const x = dv.getFloat32(off + 8, true);
          const y = dv.getFloat32(off + 12, true);
          const w = dv.getFloat32(off + 16, true);
          const h = dv.getFloat32(off + 20, true);
          const minD = dv.getFloat32(off + 24, true);
          const maxD = dv.getFloat32(off + 28, true);
          if (passEncoder) {
            passEncoder.setViewport(x, y, w, h, minD, maxD);
          }
          break;
        }

        case RHI_CMD_END_PASS: {
          if (passEncoder) {
            passEncoder.end();
            passEncoder = null;
          }
          break;
        }

        case RHI_CMD_BEGIN_COMPUTE_PASS: {
          if (!cmdEncoder) {
            cmdEncoder = device.createCommandEncoder();
            queryIndex = 0;
          }
          const cpassDesc = {};
          if (querySet && queryIndex + 2 <= MAX_TIMESTAMP_QUERIES) {
            cpassDesc.timestampWrites = {
              querySet,
              beginningOfPassWriteIndex: queryIndex,
              endOfPassWriteIndex: queryIndex + 1,
            };
            queryIndex += 2;
          }
          computePassEncoder = cmdEncoder.beginComputePass(cpassDesc);
          break;
        }

        case RHI_CMD_SET_COMPUTE_PIPELINE: {
          const pipeHandle = dv.getUint32(off + 8, true);
          const cpipe = resources.get(pipeHandle);
          if (computePassEncoder && cpipe) {
            computePassEncoder.setPipeline(cpipe);
          }
          break;
        }

        case RHI_CMD_DISPATCH: {
          const gx = dv.getUint32(off + 8, true);
          const gy = dv.getUint32(off + 12, true);
          const gz = dv.getUint32(off + 16, true);
          if (computePassEncoder) {
            computePassEncoder.dispatchWorkgroups(gx, gy, gz);
          }
          break;
        }

        case RHI_CMD_END_COMPUTE_PASS: {
          if (computePassEncoder) {
            computePassEncoder.end();
            computePassEncoder = null;
          }
          break;
        }

        case RHI_CMD_PRESENT: {
          if (passEncoder) {
            passEncoder.end();
            passEncoder = null;
          }
          if (computePassEncoder) {
            computePassEncoder.end();
            computePassEncoder = null;
          }
          if (cmdEncoder) {
            const resolvedQueries =
              querySet &&
              queryResolveBuf &&
              queryReadbackBuf &&
              queryIndex > 0 &&
              !queryReadbackInFlight &&
              typeof cmdEncoder.resolveQuerySet === "function"
                ? queryIndex
                : 0;
            if (resolvedQueries > 0) {
              queryReadbackInFlight = true;
              cmdEncoder.resolveQuerySet(querySet, 0, resolvedQueries, queryResolveBuf, 0);
              cmdEncoder.copyBufferToBuffer(
                queryResolveBuf,
                0,
                queryReadbackBuf,
                0,
                resolvedQueries * 8
              );
            }

            const shouldCopy =
              !readbackInFlight &&
              readbackBuffer &&
              surfaceTexture &&
              typeof cmdEncoder.copyTextureToBuffer === "function";
            if (shouldCopy) {
              readbackInFlight = true;
              cmdEncoder.copyTextureToBuffer(
                { texture: surfaceTexture, mipLevel: 0, origin: { x: 0, y: 0, z: 0 } },
                {
                  buffer: readbackBuffer,
                  offset: 0,
                  bytesPerRow: readbackBytesPerRow,
                  rowsPerImage: surfaceHeight,
                },
                { width: surfaceWidth, height: surfaceHeight, depthOrArrayLayers: 1 }
              );
            }
            const submitT0 = typeof performance !== "undefined" ? performance.now() : 0;
            device.queue.submit([cmdEncoder.finish()]);
            cmdEncoder = null;
            queryIndex = 0;

            if (resolvedQueries > 0) {
              queryReadbackBuf
                .mapAsync(GPU_MAP_READ)
                .then(() => {
                  const u64 = new BigUint64Array(
                    queryReadbackBuf.getMappedRange(0, resolvedQueries * 8)
                  );
                  let totalNs = 0n;
                  for (let i = 0; i + 1 < resolvedQueries; i += 2) {
                    const t0 = u64[i];
                    const t1 = u64[i + 1];
                    if (t1 >= t0) totalNs += t1 - t0;
                  }
                  queryReadbackBuf.unmap();
                  queryReadbackInFlight = false;
                  const us = Math.max(1, Number((totalNs + 500n) / 1000n)) >>> 0;
                  lastGpuTimeUs = us;
                  if (exports && typeof exports.engine_gpu_set_frame_time_us === "function") {
                    exports.engine_gpu_set_frame_time_us(us);
                  }
                })
                .catch(() => {
                  queryReadbackInFlight = false;
                });
            } else if (
              !hasTimestampQuery &&
              device.queue &&
              typeof device.queue.onSubmittedWorkDone === "function"
            ) {
              device.queue
                .onSubmittedWorkDone()
                .then(() => {
                  const dtMs =
                    (typeof performance !== "undefined" ? performance.now() : submitT0) - submitT0;
                  const us = Math.max(1, Math.round(dtMs * 1000)) >>> 0;
                  lastGpuTimeUs = us;
                  if (exports && typeof exports.engine_gpu_set_frame_time_us === "function") {
                    exports.engine_gpu_set_frame_time_us(us);
                  }
                })
                .catch(() => {});
            }

            if (shouldCopy) {
              readbackPromise = finishReadback();
            }
          }
          break;
        }

        default:
          reportStreamError(0, `Bilinmeyen RHI komutu: ${opcode}`);
          break;
      }

      off += size;
      executed++;
    }

    if (executed !== cmdCount) {
      reportStreamError(0, `Komut sayısı uyuşmazlığı (${executed}/${cmdCount})`);
    }
    return executed;
  }

  function executeFrame() {
    if (!exports || typeof exports.engine_gpu_cmd_stream !== "function") return 0;
    const ptr = exports.engine_gpu_cmd_stream() >>> 0;
    const len = exports.engine_gpu_cmd_bytes() >>> 0;
    if (!ptr || len < 16) return 0;
    return executeStream(ptr, len);
  }

  async function readbackPixels() {
    if (readbackPromise) {
      await readbackPromise;
    }
    if (!lastPixels && !readbackInFlight && readbackBuffer && surfaceTexture) {
      const enc = device.createCommandEncoder();
      readbackInFlight = true;
      enc.copyTextureToBuffer(
        { texture: surfaceTexture, mipLevel: 0, origin: { x: 0, y: 0, z: 0 } },
        {
          buffer: readbackBuffer,
          offset: 0,
          bytesPerRow: readbackBytesPerRow,
          rowsPerImage: surfaceHeight,
        },
        { width: surfaceWidth, height: surfaceHeight, depthOrArrayLayers: 1 }
      );
      device.queue.submit([enc.finish()]);
      readbackPromise = finishReadback();
      await readbackPromise;
    }
    return lastPixels;
  }

  function resize(newW, newH) {
    const w = newW >>> 0;
    const h = newH >>> 0;
    if (w === 0 || h === 0) return;
    if (w === surfaceWidth && h === surfaceHeight) return;
    allocSurfaceTargets(w, h);
  }

  function destroy() {
    for (const obj of resources.values()) {
      if (obj && typeof obj.destroy === "function") obj.destroy();
      else if (obj) {
        if (obj.texture && typeof obj.texture.destroy === "function") obj.texture.destroy();
        if (obj.msaaTex && typeof obj.msaaTex.destroy === "function") obj.msaaTex.destroy();
      }
    }
    resources.clear();
    if (surfaceTexture && typeof surfaceTexture.destroy === "function") surfaceTexture.destroy();
    if (readbackBuffer && typeof readbackBuffer.destroy === "function") readbackBuffer.destroy();
    if (msaaColorTex && typeof msaaColorTex.destroy === "function") msaaColorTex.destroy();
    if (querySet && typeof querySet.destroy === "function") querySet.destroy();
    if (queryResolveBuf && typeof queryResolveBuf.destroy === "function") queryResolveBuf.destroy();
    if (queryReadbackBuf && typeof queryReadbackBuf.destroy === "function") queryReadbackBuf.destroy();
  }

  return {
    adapter,
    device,
    context,
    get surfaceTexture() {
      return surfaceTexture;
    },
    format: canvasFormat,
    hasTimestampQuery,
    resources,
    executeStream,
    executeFrame,
    readbackPixels,
    resize,
    getLastPixels: () => lastPixels,
    getReadbackCount: () => readbackCount,
    getLastGpuTimeUs: () => lastGpuTimeUs,
    destroy,
  };
}
