// tools/host/webgpu_check.mjs — Başsız Chrome + CDP ile gerçek GPU'da (Vulkan) ya da SwiftShader'da WebGPU doğrulaması.
// Kullanım: node tools/host/webgpu_check.mjs <url> [ifade-dosyası|DEFAULT] [ek-chrome-bayrakları...]
import { spawn } from 'node:child_process';
import { existsSync, readFileSync, rmSync, writeFileSync } from 'node:fs';

const DEFAULT_EXPR = `
  const m = globalThis.matokiGpu;
  if (!m || !m.gpu) return { ok: false, err: "matokiGpu başlatılamadı" };
  const summary = await m.verify();
  const ringPtr = m.exports.engine_gpu_error_ring() >>> 0;
  const dv = new DataView(m.memory.buffer);
  const ring = {
    write: dv.getUint32(ringPtr + 0, true),
    read: dv.getUint32(ringPtr + 4, true),
    dropped: dv.getUint32(ringPtr + 12, true),
  };
  const wgslNames = [
    "solid.wgsl", "vcolor.wgsl", "sprite.wgsl", "plasma.wgsl", "branchy.wgsl",
    "unlit3d.wgsl", "lit3d.wgsl", "dungeon3d.wgsl", "pbr3d.wgsl", "pbr3d_vs.wgsl"
  ];
  const device = m.gpu.device;
  let wgslCompiled = 0;
  const wgslErrors = [];
  for (const name of wgslNames) {
    const resp = await fetch("/build/wgsl/" + name, { cache: "no-store" });
    if (!resp.ok) {
      wgslErrors.push(name + ": HTTP " + resp.status);
      continue;
    }
    const code = await resp.text();
    device.pushErrorScope("validation");
    const mod = device.createShaderModule({ code, label: name });
    const info = mod.getCompilationInfo ? await mod.getCompilationInfo() : { messages: [] };
    const valErr = await device.popErrorScope();
    const errs = (info.messages || []).filter((x) => x.type === "error");
    if (valErr || errs.length > 0) {
      wgslErrors.push(name + ": " + (valErr ? valErr.message : errs.map((e) => e.message).join("; ")));
    } else {
      wgslCompiled++;
    }
  }
  const ringClean = ring.write === 0 && ring.read === 0 && ring.dropped === 0;
  const ok = Boolean(summary && summary.passed && ringClean && wgslCompiled === wgslNames.length && wgslErrors.length === 0);
  return {
    ok,
    hud: document.getElementById("hud")?.textContent || "",
    readbacks: m.gpu.getReadbackCount(),
    ring,
    summary,
    wgsl: { total: wgslNames.length, compiled: wgslCompiled, errors: wgslErrors },
  };
`;

const OCEAN_EXPR = `
  const sleep = (ms) => new Promise((r) => setTimeout(r, ms));
  let app = globalThis.matoki;
  for (let i = 0; i < 30 && (!app || !app.exports); i++) {
    await sleep(100);
    app = globalThis.matoki;
  }
  if (!app || !app.exports) return { ok: false, err: "matoki başlatılamadı" };
  app.exports.engine_set_scene(8, 0, 0, 1);
  const samplesUs = [];
  for (let i = 0; i < 20; i++) {
    await sleep(60);
    if (app.exports.engine_gpu_frame_time_us) {
      const us = app.exports.engine_gpu_frame_time_us() >>> 0;
      if (us > 0) samplesUs.push(us);
    }
  }
  const gpuAvail = Boolean(app.exports.engine_gpu_available && app.exports.engine_gpu_available());
  const vpW = app.exports.engine_gpu_viewport_width ? (app.exports.engine_gpu_viewport_width() >>> 0) : 0;
  const vpH = app.exports.engine_gpu_viewport_height ? (app.exports.engine_gpu_viewport_height() >>> 0) : 0;
  const hasTimestamp = Boolean(app.exports.engine_gpu_has_timestamp && app.exports.engine_gpu_has_timestamp());
  const lastUs = app.exports.engine_gpu_frame_time_us ? (app.exports.engine_gpu_frame_time_us() >>> 0) : 0;
  samplesUs.sort((a, b) => a - b);
  const medianUs = samplesUs.length > 0 ? samplesUs[Math.floor(samplesUs.length / 2)] : lastUs;

  const dv = new DataView(app.memory.buffer);
  const dbgPtr = app.exports.engine_ocean_debug ? (app.exports.engine_ocean_debug() >>> 0) : 0;
  const oceanDebug = dbgPtr ? {
    fftMs: dv.getFloat32(dbgPtr + 0, true),
    meshMs: dv.getFloat32(dbgPtr + 4, true),
    shadeMs: dv.getFloat32(dbgPtr + 8, true),
    hs: dv.getFloat32(dbgPtr + 12, true),
    theoryHs: dv.getFloat32(dbgPtr + 16, true),
    windSpeed: dv.getFloat32(dbgPtr + 20, true),
    choppy: dv.getFloat32(dbgPtr + 24, true),
    timeScale: dv.getFloat32(dbgPtr + 28, true),
    boatY: dv.getFloat32(dbgPtr + 32, true),
    boatPitch: dv.getFloat32(dbgPtr + 36, true),
    boatRoll: dv.getFloat32(dbgPtr + 40, true),
    tris: dv.getFloat32(dbgPtr + 44, true),
  } : null;

  const ringPtr = app.exports.engine_gpu_error_ring ? (app.exports.engine_gpu_error_ring() >>> 0) : 0;
  const ring = ringPtr ? {
    write: dv.getUint32(ringPtr + 0, true),
    read: dv.getUint32(ringPtr + 4, true),
    dropped: dv.getUint32(ringPtr + 12, true),
  } : { write: 0, read: 0, dropped: 0 };

  const targetCanvas = gpuAvail ? document.getElementById("gpu-screen") : document.getElementById("screen");
  const tctx = targetCanvas ? targetCanvas.getContext("2d") : null;
  let skyPx = 0, waterPx = 0, brightPx = 0, warmBoatOrGlarePx = 0;
  if (tctx && targetCanvas.width > 0 && targetCanvas.height > 0) {
    const w = targetCanvas.width;
    const h = targetCanvas.height;
    const data = tctx.getImageData(0, 0, w, h).data;
    const step = Math.max(1, Math.floor((w * h) / 25000));
    for (let p = 0; p < w * h; p += step) {
      const i = p * 4;
      const r = data[i], g = data[i + 1], b = data[i + 2];
      const y = Math.floor(p / w);
      if (y < h * 0.42 && b > r && g > 40) skyPx++;
      if (y >= h * 0.42 && y < h * 0.90 && (b > r || g > r) && (g + b) > 40) waterPx++;
      if (r > 180 && g > 170 && b > 140) brightPx++;
      if (r > b + 35 && r > 90 && g > 50) warmBoatOrGlarePx++;
    }
  }
  const ringClean = ring.write === 0 && ring.read === 0 && ring.dropped === 0;
  const pixelsOk = skyPx > 100 && waterPx > 100 && brightPx > 5 && warmBoatOrGlarePx > 5;
  const ok = Boolean(ringClean && pixelsOk && oceanDebug && oceanDebug.tris > 1000);
  return {
    ok,
    mode: gpuAvail ? "GPU" : "CPU",
    gpuAvailable: gpuAvail,
    resolution: gpuAvail ? (vpW + "x" + vpH) : (targetCanvas ? (targetCanvas.width + "x" + targetCanvas.height) : ""),
    hasTimestamp,
    gpuFrameTimeMs: Number((medianUs / 1000).toFixed(3)),
    gpuFrameTimeLastMs: Number((lastUs / 1000).toFixed(3)),
    samplesCount: samplesUs.length,
    ring,
    oceanDebug,
    pixelCheck: { skyPx, waterPx, brightPx, warmBoatOrGlarePx },
  };
`;

const args = process.argv.slice(2);
const url = args[0];
if (!url) {
  console.error('Kullanım: node tools/host/webgpu_check.mjs <url> [ifade-dosyası|DEFAULT|OCEAN] [chrome-bayrakları...]');
  process.exit(1);
}

let expr = DEFAULT_EXPR;
let extra = [];
if (args.length > 1) {
  if (args[1] === 'DEFAULT' || args[1] === '-') {
    extra = args.slice(2);
  } else if (args[1] === 'OCEAN') {
    expr = OCEAN_EXPR;
    extra = args.slice(2);
  } else if (args[1].startsWith('--') && !existsSync(args[1])) {
    extra = args.slice(1);
  } else {
    expr = readFileSync(args[1], 'utf8');
    extra = args.slice(2);
  }
}

const disableWebGpu = extra.some((f) => f.includes('disable-features=WebGPU') || f.includes('disable-webgpu'));
const port = 9333 + Math.floor(Math.random() * 500);
const prof = 'build/chrome-prof-' + port;
const chromeArgs = [
  '--headless=new',
  '--no-first-run',
  '--window-size=1920,1080',
  '--user-data-dir=' + prof,
  '--remote-debugging-port=' + port,
];
if (!disableWebGpu) {
  chromeArgs.push(
    '--enable-unsafe-webgpu',
    '--enable-dawn-features=allow_unsafe_apis',
    '--ignore-gpu-blocklist'
  );
}
chromeArgs.push(...extra, 'about:blank');
const chrome = spawn('google-chrome', chromeArgs, { stdio: 'ignore' });

const cleanup = () => {
  try { chrome.kill(); } catch {}
  try { rmSync(prof, { recursive: true, force: true }); } catch {}
};

const sleep = (ms) => new Promise((r) => setTimeout(r, ms));
let ws;
for (let i = 0; i < 50 && !ws; i++) {
  await sleep(200);
  try {
    const list = await (await fetch(`http://127.0.0.1:${port}/json/list`)).json();
    const page = list.find((t) => t.type === 'page');
    if (page) ws = new WebSocket(page.webSocketDebuggerUrl);
  } catch { /* henüz hazır değil */ }
}
if (!ws) {
  console.error('HATA: Başsız Chrome CDP bağlantısı kurulamadı.');
  cleanup();
  process.exit(1);
}

await new Promise((r) => ws.addEventListener('open', r));
let id = 0;
const pending = new Map();
const logs = [];
ws.addEventListener('message', (e) => {
  const m = JSON.parse(e.data);
  if (m.id && pending.has(m.id)) { pending.get(m.id)(m); pending.delete(m.id); }
  if (m.method === 'Runtime.consoleAPICalled') logs.push(m.params.args.map((a) => a.value ?? a.description).join(' '));
});
const send = (method, params = {}) => new Promise((r) => { const i = ++id; pending.set(i, r); ws.send(JSON.stringify({ id: i, method, params })); });
await send('Runtime.enable');
await send('Page.enable');
if (disableWebGpu) {
  await send('Page.addScriptToEvaluateOnNewDocument', {
    source: 'try { Object.defineProperty(navigator, "gpu", { value: undefined, configurable: true }); } catch {}',
  });
}
await send('Emulation.setDeviceMetricsOverride', { width: 1920, height: 1080, deviceScaleFactor: 1, mobile: false });
await send('Page.navigate', { url });
await sleep(4000);
const res = await send('Runtime.evaluate', { expression: `(async()=>{${expr}})()`, awaitPromise: true, returnByValue: true, timeout: 60000 });
const shot = await send('Page.captureScreenshot', { format: 'png' });
if (shot.result?.data) writeFileSync('build/webgpu_shot.png', Buffer.from(shot.result.data, 'base64'));
const val = res.result?.result?.value ?? res.result;
console.log(JSON.stringify({ result: val, logs: logs.slice(0, 20) }, null, 1));
ws.close();
cleanup();

const failed =
  Boolean(res.result?.exceptionDetails) ||
  (val && typeof val === 'object' && (val.ok === false || val.passed === false || Boolean(val.err)));
process.exit(failed ? 1 : 0);
