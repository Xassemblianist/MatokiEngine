// tools/host/gpu_compare.mjs — CPU (Wasm JIT) ve GPU (WebGPU) shader çıktılarını karşılaştırır.
// 16 Matoki çekirdeği (12 pixel + 4 compute) için fark metriklerini hesaplar,
// fark haritalarını build/diff_<ad>.png olarak üretir ve tolerans denetimi yapar.

import { spawn } from 'node:child_process';
import { existsSync, mkdirSync, rmSync, writeFileSync } from 'node:fs';
import zlib from 'node:zlib';

function crc32(buf) {
  let c = 0xffffffff;
  for (let i = 0; i < buf.length; i++) {
    c ^= buf[i];
    for (let k = 0; k < 8; k++) {
      c = (c >>> 1) ^ (0xedb88320 & -(c & 1));
    }
  }
  return (c ^ 0xffffffff) >>> 0;
}

function makePngChunk(type, data) {
  const typeBuf = Buffer.from(type, 'ascii');
  const len = data.length;
  const chunk = Buffer.alloc(8 + len + 4);
  chunk.writeUInt32BE(len, 0);
  typeBuf.copy(chunk, 4);
  data.copy(chunk, 8);
  const toCrc = Buffer.concat([typeBuf, data]);
  chunk.writeUInt32BE(crc32(toCrc), 8 + len);
  return chunk;
}

function encodePngRgba(width, height, rgbaBuffer) {
  const header = Buffer.from([0x89, 0x50, 0x4e, 0x47, 0x0d, 0x0a, 0x1a, 0x0a]);
  const ihdr = Buffer.alloc(13);
  ihdr.writeUInt32BE(width, 0);
  ihdr.writeUInt32BE(height, 4);
  ihdr[8] = 8;  // 8-bit per channel
  ihdr[9] = 6;  // RGBA
  ihdr[10] = 0; // Deflate
  ihdr[11] = 0; // Filter none/standard
  ihdr[12] = 0; // Non-interlaced
  const ihdrChunk = makePngChunk('IHDR', ihdr);

  const stride = width * 4;
  const raw = Buffer.alloc(height * (stride + 1));
  for (let y = 0; y < height; y++) {
    raw[y * (stride + 1)] = 0; // No filter for row
    rgbaBuffer.copy(raw, y * (stride + 1) + 1, y * stride, (y + 1) * stride);
  }

  const idatData = zlib.deflateSync(raw);
  const idatChunk = makePngChunk('IDAT', idatData);
  const iendChunk = makePngChunk('IEND', Buffer.alloc(0));
  return Buffer.concat([header, ihdrChunk, idatChunk, iendChunk]);
}

const args = process.argv.slice(2);
const url = args[0] || 'http://127.0.0.1:8000/gpu_test.html?compare=1';
const extra = args.slice(1);

const port = 9444 + Math.floor(Math.random() * 400);
const prof = 'build/chrome-prof-cmp-' + port;
const chromeArgs = [
  '--headless=new',
  '--no-first-run',
  '--window-size=1280,720',
  '--user-data-dir=' + prof,
  '--remote-debugging-port=' + port,
  '--enable-unsafe-webgpu',
  '--enable-dawn-features=allow_unsafe_apis',
  '--ignore-gpu-blocklist',
  ...extra,
  'about:blank',
];

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
  } catch {}
}

if (!ws) {
  console.error('HATA: Başsız Chrome CDP bağlantısı kurulamadı.');
  cleanup();
  process.exit(1);
}

await new Promise((r) => ws.addEventListener('open', r));
let id = 0;
const pending = new Map();
const consoleLogs = [];
ws.addEventListener('message', (e) => {
  const m = JSON.parse(e.data);
  if (m.id && pending.has(m.id)) {
    pending.get(m.id)(m);
    pending.delete(m.id);
  }
  if (m.method === 'Runtime.consoleAPICalled') {
    consoleLogs.push(m.params.args.map((a) => a.value ?? a.description).join(' '));
  }
});

const send = (method, params = {}) =>
  new Promise((r) => {
    const i = ++id;
    pending.set(i, r);
    ws.send(JSON.stringify({ id: i, method, params }));
  });

try {
  await send('Runtime.enable');
  await send('Page.enable');
  await send('Page.navigate', { url });

  // Sayfanın yüklenmesini ve matokiGpu nesnesinin hazır olmasını bekle
  let ready = false;
  for (let i = 0; i < 60; i++) {
    await sleep(250);
    const chk = await send('Runtime.evaluate', {
      expression: 'Boolean(globalThis.matokiGpu && globalThis.matokiGpu.gpu && globalThis.matokiGpu.runCompare)',
      returnByValue: true,
    });
    if (chk.result?.result?.value === true) {
      ready = true;
      break;
    }
  }

  if (!ready) {
    console.error('HATA: matokiGpu.runCompare hazır hale gelmedi.');
    console.error('Son konsol logları:', consoleLogs.slice(-10));
    cleanup();
    process.exit(1);
  }

  console.log('CPU ↔ GPU Karşılaştırma testi başlatılıyor...');
  const cmpEval = await send('Runtime.evaluate', {
    expression: 'globalThis.matokiGpu.runCompare()',
    awaitPromise: true,
    returnByValue: true,
    timeout: 60000,
  });

  const cmpData = cmpEval.result?.result?.value;
  if (!cmpData || cmpData.error) {
    console.error('Karşılaştırma yürütme hatası:', cmpData?.error || cmpEval.result);
    cleanup();
    process.exit(1);
  }

  if (!existsSync('build')) {
    mkdirSync('build', { recursive: true });
  }

  const results = cmpData.results || [];
  console.log('\n========================================================================================');
  console.log(
    '#   Çekirdek              Tür      Boyut      Max Fark      Ort Fark      Tol       Durum'
  );
  console.log('----------------------------------------------------------------------------------------');

  let allPass = true;
  for (const r of results) {
    const typeStr = r.isCompute ? 'Compute' : 'Pixel';
    const statusStr = r.passed ? 'PASS' : 'FAIL';
    if (!r.passed) allPass = false;

    const safeName = r.name.replace(/\.mtk$/, '');
    if (r.diffMap && r.diffMap.length === 128 * 128 * 4) {
      const pngBuf = encodePngRgba(128, 128, Buffer.from(r.diffMap));
      writeFileSync(`build/diff_${safeName}.png`, pngBuf);
    }

    const idxStr = String(r.index).padStart(2, ' ');
    const nameStr = r.name.padEnd(20, ' ');
    const tStr = typeStr.padEnd(8, ' ');
    const elemStr = String(r.elementCount).padStart(8, ' ');
    const maxStr = (typeof r.maxDiff === 'number' ? r.maxDiff.toFixed(6) : '-').padStart(12, ' ');
    const avgStr = (typeof r.avgDiff === 'number' ? r.avgDiff.toFixed(6) : '-').padStart(12, ' ');
    const tolStr = (typeof r.tolerance === 'number' ? r.tolerance.toFixed(6) : '-').padStart(10, ' ');
    const errStr = r.error ? ` (${r.error})` : '';
    console.log(`${idxStr}  ${nameStr}  ${tStr} ${elemStr}  ${maxStr}  ${avgStr}  ${tolStr}   ${statusStr}${errStr}`);
  }
  console.log('========================================================================================\n');
  if (consoleLogs.length > 0) {
    console.log('--- Tarayıcı Konsol Çıktıları ---');
    console.log(consoleLogs.join('\n'));
    console.log('---------------------------------\n');
  }

  // Karşılaştırma anı ekran görüntüsü
  const shot = await send('Page.captureScreenshot', { format: 'png' });
  if (shot.result?.data) {
    writeFileSync('build/gpu_compare_shot.png', Buffer.from(shot.result.data, 'base64'));
  }

  ws.close();
  cleanup();

  if (allPass && results.length > 0) {
    console.log(`Tüm ${results.length} çekirdek karşılaştırma testini başarıyla geçti.`);
    process.exit(0);
  } else {
    console.error(`Karşılaştırma testinde başarısız çekirdekler var veya sonuç yok!`);
    process.exit(1);
  }
} catch (err) {
  console.error('Beklenmeyen hata:', err);
  cleanup();
  process.exit(1);
}
