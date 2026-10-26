/* tools/host/run_kernels.mjs — C (clang) vs Elle WAT çekirdeklerinin Node/V8 doğrulama ve benchmark koşucusu.
 * Kullanım:
 *   node tools/host/run_kernels.mjs           -> Tüm 8 WAT çekirdeğini masm+jit_load ile derler, C ile bit-bit doğrular
 *   node tools/host/run_kernels.mjs --bench   -> Bit-bit doğrulama + 15 tekrarlı V8 (TurboFan) mikro-benchmark tablosu
 */
import fs from 'node:fs';
import { performance } from 'node:perf_hooks';
import { createJitLoader } from '../../web/jit.js';

const decoder = new TextDecoder('utf-8');
const args = process.argv.slice(2);
const runBench = args.includes('--bench');
const wasmPath = args.find((a) => !a.startsWith('--')) || 'build/engine.wasm';

const wasmBytes = fs.readFileSync(wasmPath);
const mod = new WebAssembly.Module(wasmBytes);

let initialTable = 64;
for (const imp of WebAssembly.Module.imports(mod)) {
    if (imp.module === 'env' && imp.name === '__indirect_function_table' && imp.kind === 'table') {
        const min = imp.type?.minimum ?? imp.type?.initial ?? 64;
        initialTable = Math.max(initialTable, min);
    }
}

const table = new WebAssembly.Table({ element: 'anyfunc', initial: initialTable });
let memory = null;
let instance = null;

function readCString(ptr, len) {
    const view = new Uint8Array(memory.buffer, ptr, len);
    if (typeof SharedArrayBuffer !== 'undefined' && memory.buffer instanceof SharedArrayBuffer) {
        return decoder.decode(view.slice());
    }
    return decoder.decode(view);
}

for (const tryShared of [true, false]) {
    try {
        memory = new WebAssembly.Memory({ initial: 4096, maximum: 16384, shared: tryShared });
        const env = {
            memory,
            __indirect_function_table: table,
            plat_log(ptr, len) {
                const msg = readCString(ptr, len);
                if (runBench) {
                    console.log(msg);
                }
            },
            plat_panic(ptr, len) {
                const msg = readCString(ptr, len);
                console.error(`[PANIC] ${msg}`);
                process.exit(1);
            },
            plat_time_ms() {
                return performance.now();
            },
            plat_jit_load: createJitLoader(memory, table),
            plat_download() {},
        };
        instance = new WebAssembly.Instance(mod, { env });
        break;
    } catch (err) {
        if (!tryShared) throw err;
    }
}

const exports = instance.exports;
const rc = runBench ? exports.engine_bench_run() : exports.engine_bench_verify_only();
if (rc !== 0) {
    console.error(`[run_kernels] FAIL (rc=${rc})`);
    process.exit(rc);
}
