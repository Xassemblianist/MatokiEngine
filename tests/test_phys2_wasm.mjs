// tests/test_phys2_wasm.mjs — phys2'yi wasm'da (simd128 + paylaşımlı bellek + gerçek worker thread'ler) sınar ve ölçer.
//   node tests/test_phys2_wasm.mjs test  [worker_sayısı=8] [ad_filtresi]   tüm test_phys2_*.c testleri wasm'da koşar:
//        SIMD ↔ skaler temas çözücüsü bit-bit eşitliği (host'ta SIMD yolu yok) ve 1T ↔ 8T belirlenimcilik testleri
//        gerçek thread'lerle (host'ta paralel yol yalnızca sahte worker sayısıyla, tek thread'de koşar).
//   node tests/test_phys2_wasm.mjs bench [worker_sayısı=0]  [tekrar=3]     adım süresi tablosu (D-111 notlarındaki ölçüm):
//        8 thread için worker_sayısı = 7 (ana thread dahil 8). Makine yükünü (uptime) sonuçla birlikte yaz.
// Gerekenler: clang (wasm32 hedefli) + Node >= 22. Çıktılar build/phys2_wasm/ altına gider. `make check`'e bağlı değildir.
import fs from 'node:fs';
import path from 'node:path';
import { execFileSync } from 'node:child_process';
import { Worker, isMainThread, workerData, parentPort } from 'node:worker_threads';
import { fileURLToPath } from 'node:url';

const here = fileURLToPath(import.meta.url);
const root = path.resolve(path.dirname(here), '..');
const outDir = path.join(root, 'build', 'phys2_wasm');

function makeEnv(memory, tid) {
  const dec = new TextDecoder();
  const text = (p, n) => dec.decode(new Uint8Array(memory.buffer, p, n).slice());
  return {
    memory,
    plat_log(p, n) { console.log((tid ? `[W${tid}] ` : '') + text(p, n)); },
    plat_panic(p, n) { throw new Error('PANIC: ' + text(p, n)); },
    plat_time_ms() { return performance.now(); },
    plat_jit_load() { return -1; },
    plat_download() {},
  };
}

function instantiate(module, memory, tid) {
  const table = new WebAssembly.Table({ element: 'anyfunc', initial: 512 });
  const env = makeEnv(memory, tid);
  env.__indirect_function_table = table;
  return new WebAssembly.Instance(module, { env });
}

function build(kind) {
  fs.mkdirSync(outDir, { recursive: true });
  const out = path.join(outDir, kind + '.wasm');
  const phys2 = fs.readdirSync(path.join(root, 'src', 'phys2')).filter((f) => f.endsWith('.c')).map((f) => 'src/phys2/' + f);
  const core = ['src/thread/thread.c', 'src/thread/job.c', 'src/core/arena.c', 'src/core/log.c', 'src/core/math.c', 'src/core/mem.c'];
  const extra = kind === 'tests'
    ? ['tests/wasm/phys2_shim.c', ...fs.readdirSync(path.join(root, 'tests')).filter((f) => /^test_phys2_.*\.c$/.test(f) && !f.includes('common')).map((f) => 'tests/' + f)]
    : ['tests/wasm/phys2_bench.c'];
  const args = ['--target=wasm32', '-std=c11', '-O2', '-ffreestanding', '-fno-builtin', '-nostdlib', '-msimd128', '-matomics',
    '-mbulk-memory', '-mnontrapping-fptoint', '-msign-ext', '-DPLAT_SHARED_MEMORY=1', '-Wall', '-Wextra', '-Werror', '-Isrc', '-Itests',
    ...phys2, ...core, ...extra,
    '-Wl,--no-entry', '-Wl,--shared-memory', '-Wl,--import-memory', '-Wl,--import-table', '-Wl,--growable-table',
    '-Wl,--initial-memory=536870912', '-Wl,--max-memory=1073741824', '-Wl,--stack-first', '-z', 'stack-size=1048576',
    '-Wl,--export=__wasm_call_ctors', '-o', out];
  execFileSync('clang', args, { cwd: root, stdio: 'inherit' });
  return out;
}

if (!isMainThread) {
  const { module, memory, tid } = workerData;
  const ex = instantiate(module, memory, tid).exports;
  ex.engine_thread_init(tid, ex.engine_thread_stack_top(tid));
  parentPort.postMessage('ready');
  let running = true;
  parentPort.on('message', (m) => { if (m === 'stop') running = false; });
  const slice = () => { if (running) { ex.engine_worker_loop(); setImmediate(slice); } };
  slice();
} else {
  const [, , mode = 'test', nw, arg] = process.argv;
  const nworkers = parseInt(nw ?? (mode === 'test' ? '8' : '0'), 10);
  const wasm = build(mode === 'test' ? 'tests' : 'bench');
  const module = new WebAssembly.Module(fs.readFileSync(wasm));
  const memory = new WebAssembly.Memory({ initial: 8192, maximum: 16384, shared: true });
  const ex = instantiate(module, memory, 0).exports;
  ex.__wasm_call_ctors();
  ex.engine_thread_init(0, 0);
  const workers = [];
  await Promise.all(Array.from({ length: nworkers }, (_, i) => new Promise((res) => {
    const w = new Worker(here, { workerData: { module, memory, tid: i + 1 } });
    w.once('message', () => res());
    w.on('error', (e) => { console.error('worker hatası', e); process.exit(2); });
    workers.push(w);
  })));
  ex.engine_set_worker_count(nworkers);
  let rc = 0;
  if (mode === 'test') {
    new TextEncoder().encodeInto(arg ?? '', new Uint8Array(memory.buffer, ex.filter_buf(), 63));
    rc = ex.run_tests() ? 1 : 0;
  } else {
    const reps = parseInt(arg ?? '3', 10);
    const threads = nworkers + 1;
    const mt = nworkers ? 1 : 0;
    const med = (a) => a.slice().sort((x, y) => x - y)[a.length >> 1];
    const row = (label, f) => {
      const v = [];
      for (let r = 0; r < reps; r++) v.push(f());
      console.log(`${String(threads).padStart(2)}T | ${label.padEnd(48)} | ${med(v).toFixed(3).padStart(9)} ms | min ${Math.min(...v).toFixed(3)} max ${Math.max(...v).toFixed(3)}`);
    };
    const detail = (label) => console.log(`   |   ${label}: dar faz ${ex.bench_get(2).toFixed(3)} ms, çözücü ${ex.bench_get(3).toFixed(3)} ms, manifold ${ex.bench_get(4)}, çift ${ex.bench_get(5)}`);
    row('piramit 650 kutu: oturmuş adım (medyan)', () => ex.bench_rigid(1, 650, mt, 60, 120, 1));
    detail('son koşu');
    row('1000 kutu yığını: oturmuş adım (adım 120..240)', () => ex.bench_rigid(0, 1000, mt, 120, 120, 1));
    detail('son koşu');
    row('1000 kutu yığını: skaler çözücü (simd kapalı)', () => ex.bench_rigid(0, 1000, mt, 120, 120, 0));
    row('2000 kutu yığını: oturmuş adım', () => ex.bench_rigid(0, 2000, mt, 120, 60, 1));
    detail('son koşu');
    row('kumaş 100x100 dizi sırası, öz-çarpışma kapalı', () => ex.bench_cloth(100, 0, mt, 20, 0));
    row('kumaş 100x100 renk sırası, öz-çarpışma kapalı', () => ex.bench_cloth(100, 1, mt, 20, 0));
    row('kumaş 200x200 dizi sırası, öz-çarpışma kapalı', () => ex.bench_cloth(200, 0, mt, 10, 0));
    row('kumaş 200x200 renk sırası, öz-çarpışma kapalı', () => ex.bench_cloth(200, 1, mt, 10, 0));
    row('kumaş 100x100 renk sırası, öz-çarpışma AÇIK', () => ex.bench_cloth(100, 1, mt, 10, 1));
    row('kumaş 32x32 renk sırası, öz-çarpışma kapalı', () => ex.bench_cloth(32, 1, mt, 40, 0));
  }
  for (const w of workers) w.postMessage('stop');
  setTimeout(() => process.exit(rc), 50);
}
