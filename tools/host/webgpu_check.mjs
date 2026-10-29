// tools/host/webgpu_check.mjs — Başsız Chrome + CDP ile gerçek GPU'da (Vulkan) ya da SwiftShader'da WebGPU doğrulaması.
// Kullanım: node gpucheck.mjs <url> <ifade-dosyası> [ek-chrome-bayrakları...]
import { spawn } from 'node:child_process';
import { readFileSync } from 'node:fs';

const [url, exprFile, ...extra] = process.argv.slice(2);
const expr = readFileSync(exprFile, 'utf8');
const port = 9333 + Math.floor(Math.random() * 500);
const prof = 'build/chrome-prof-' + port;
const chrome = spawn('google-chrome', ['--headless=new', '--no-first-run', '--user-data-dir=' + prof,
  '--remote-debugging-port=' + port, '--enable-unsafe-webgpu', '--ignore-gpu-blocklist', ...extra, 'about:blank'],
  { stdio: 'ignore' });
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
if (shot.result?.data) (await import('node:fs')).writeFileSync('build/webgpu_shot.png', Buffer.from(shot.result.data, 'base64'));
console.log(JSON.stringify({ result: res.result?.result?.value ?? res.result, logs: logs.slice(-15) }, null, 1));
ws.close();
chrome.kill();
process.exit(0);
