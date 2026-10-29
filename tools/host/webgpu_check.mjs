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

const args = process.argv.slice(2);
const url = args[0];
if (!url) {
  console.error('Kullanım: node tools/host/webgpu_check.mjs <url> [ifade-dosyası|DEFAULT] [chrome-bayrakları...]');
  process.exit(1);
}

let expr = DEFAULT_EXPR;
let extra = [];
if (args.length > 1) {
  if (args[1] === 'DEFAULT' || args[1] === '-') {
    extra = args.slice(2);
  } else if (args[1].startsWith('--') && !existsSync(args[1])) {
    extra = args.slice(1);
  } else {
    expr = readFileSync(args[1], 'utf8');
    extra = args.slice(2);
  }
}

const port = 9333 + Math.floor(Math.random() * 500);
const prof = 'build/chrome-prof-' + port;
const chrome = spawn('google-chrome', [
  '--headless=new',
  '--no-first-run',
  '--user-data-dir=' + prof,
  '--remote-debugging-port=' + port,
  '--enable-unsafe-webgpu',
  '--ignore-gpu-blocklist',
  ...extra,
  'about:blank',
], { stdio: 'ignore' });

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
await send('Page.navigate', { url });
await sleep(4000);
const res = await send('Runtime.evaluate', { expression: `(async()=>{${expr}})()`, awaitPromise: true, returnByValue: true, timeout: 60000 });
const shot = await send('Page.captureScreenshot', { format: 'png' });
if (shot.result?.data) writeFileSync('build/webgpu_shot.png', Buffer.from(shot.result.data, 'base64'));
const val = res.result?.result?.value ?? res.result;
console.log(JSON.stringify({ result: val, logs: logs.slice(-15) }, null, 1));
ws.close();
cleanup();

const failed =
  Boolean(res.result?.exceptionDetails) ||
  (val && typeof val === 'object' && (val.ok === false || val.passed === false || Boolean(val.err)));
process.exit(failed ? 1 : 0);
