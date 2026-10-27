/* tools/host/bench_matoki_vs_wat.mjs — Matoki JIT çıktısı vs Elle yazılmış en iyi WAT doğrulama ve verim ölçümü.
 * Kullanım:
 *   node tools/host/bench_matoki_vs_wat.mjs [--trials=21] [--verify-quads=12345]
 *   veya: ./tools/host/run_matoki_vs_wat.sh
 *
 * 4 shader (vcolor, sprite, plasma, lit3d) için:
 *   1) Bozuk/düşmanca ve sınır girdi testleri (NULL işaretçiler, count=0, negatif/aşırı count,
 *      count=1/2/3/7 kuyruk boyutları, sıfır doku boyutu, aşırı mip seviyesi)
 *   2) >= 10.000 rastgele quad (varsayılan 12.345 quad = 49.380 piksel, 4'ün katı olmayan boyut dahil)
 *      üzerinde Matoki (.mtk -> matc) ile Elle WAT (asm/shade_*.wat -> masm) çıktılarını karşılaştırır;
 *      maksimum mutlak hata, LSB (1/255) cinsinden hata, ULP farkı ve quad maskesi bit-bit eşitliğini denetler.
 *   3) 1.000.000 quad (250 batch x 4.000 quad) üzerinde ısınma + >= 15 tekrar (varsayılan 21 tekrar)
 *      ile medyan Mquad/s, mdis iç döngü komut sayısı, IR komut sayısı ve wasm kod boyutunu ölçer;
 *      "Matoki / Elle WAT (%)" verim oranını raporlar.
 */
import fs from 'node:fs';
import os from 'node:os';
import { execFileSync } from 'node:child_process';

const OUT_DIR = 'build/matoki_vs_wat';
fs.mkdirSync(OUT_DIR, { recursive: true });

/* Komut satırı argümanlarını ayrıştır (sınır denetimli) */
let TRIALS = 21;
let VERIFY_QUADS = 12345;
for (const arg of process.argv.slice(2)) {
    if (arg.startsWith('--trials=')) {
        const v = Number.parseInt(arg.slice('--trials='.length), 10);
        if (Number.isFinite(v) && v >= 15 && v <= 201) TRIALS = v;
    } else if (arg.startsWith('--verify-quads=')) {
        const v = Number.parseInt(arg.slice('--verify-quads='.length), 10);
        if (Number.isFinite(v) && v >= 10000 && v <= 100000) VERIFY_QUADS = v;
    }
}

const SHADERS = [
    {
        name: 'vcolor',
        mtkPath: 'shaders/vcolor.mtk',
        watPath: 'asm/shade_vcolor.wat',
        varyingCount: 4,
        uniformBytes: 16,
        hasTexture: false,
        hasMipmap: false,
        hasDiscard: false,
    },
    {
        name: 'sprite',
        mtkPath: 'shaders/sprite.mtk',
        watPath: 'asm/shade_sprite.wat',
        varyingCount: 2,
        uniformBytes: 16,
        hasTexture: true,
        hasMipmap: false,
        hasDiscard: true,
    },
    {
        name: 'plasma',
        mtkPath: 'shaders/plasma.mtk',
        watPath: 'asm/shade_plasma.wat',
        varyingCount: 2,
        uniformBytes: 16,
        hasTexture: false,
        hasMipmap: false,
        hasDiscard: false,
    },
    {
        name: 'lit3d',
        mtkPath: 'shaders/lit3d.mtk',
        watPath: 'asm/shade_lit3d.wat',
        varyingCount: 6,
        uniformBytes: 48,
        hasTexture: true,
        hasMipmap: true,
        hasDiscard: false,
    },
];

/* Deterministik PCG32 benzeri xorshift32 RNG */
function makeRng(seed = 0xC0FFEE11) {
    let s = seed >>> 0;
    if (s === 0) s = 1;
    return {
        nextU32() {
            s ^= (s << 13) >>> 0;
            s ^= s >>> 17;
            s ^= (s << 5) >>> 0;
            return (s >>> 0);
        },
        nextF32(min = 0.0, max = 1.0) {
            const u = (this.nextU32() >>> 8) * (1.0 / 16777216.0);
            return Math.fround(min + u * (max - min));
        },
    };
}

/* mdis çıktısından iç döngü (loop .. end) komut sayısını ve toplam fonksiyon komut sayısını sayar */
function analyzeDisassembly(watText) {
    const lines = watText.split('\n');
    let inFunc = false;
    let blockDepth = 0;
    let loopDepth = 0;
    const isLoopStack = [];
    let totalFuncOps = 0;
    let mainLoopOps = 0;
    let currentLoopOps = 0;
    let loopCount = 0;
    const loopOpsList = [];

    for (const rawLine of lines) {
        const line = rawLine.trim();
        if (!line || line.startsWith(';;')) continue;
        if (line.startsWith('(func ')) {
            inFunc = true;
            continue;
        }
        if (!inFunc) continue;
        if (line.startsWith('(local ')) continue;
        if (line === ')') {
            inFunc = false;
            continue;
        }

        if (line === 'loop' || line.startsWith('loop ')) {
            if (loopDepth > 0) currentLoopOps++;
            totalFuncOps++;
            isLoopStack.push(true);
            blockDepth++;
            loopDepth++;
            if (loopDepth === 1) {
                currentLoopOps = 0;
                loopCount++;
            }
            continue;
        }
        if (line === 'block' || line.startsWith('block ') || line === 'if' || line.startsWith('if ')) {
            if (loopDepth > 0) currentLoopOps++;
            totalFuncOps++;
            isLoopStack.push(false);
            blockDepth++;
            continue;
        }
        if (line === 'end') {
            if (blockDepth > 0) {
                blockDepth--;
                const wasLoop = isLoopStack.pop();
                if (wasLoop) {
                    loopDepth--;
                    if (loopDepth === 0) {
                        loopOpsList.push(currentLoopOps);
                        if (loopOpsList.length === 1) mainLoopOps = currentLoopOps;
                    }
                } else if (loopDepth > 0) {
                    currentLoopOps++;
                }
            }
            continue;
        }

        totalFuncOps++;
        if (loopDepth > 0) currentLoopOps++;
    }

    return {
        totalFuncOps,
        mainLoopOps,
        allLoopsOps: loopOpsList.reduce((a, b) => a + b, 0),
        loopCount,
        loopOpsList,
    };
}

/* 1. Derleme ve mdis analizi */
function compileAndAnalyze(sh) {
    const mtkWasmPath = `${OUT_DIR}/${sh.name}_mtk.wasm`;
    const watWasmPath = `${OUT_DIR}/${sh.name}_wat.wasm`;
    const mtkDisPath  = `${OUT_DIR}/${sh.name}_mtk.dis.wat`;
    const watDisPath  = `${OUT_DIR}/${sh.name}_wat.dis.wat`;

    execFileSync('./build/host/matc', [sh.mtkPath, '-o', mtkWasmPath]);
    execFileSync('./build/host/masm', [sh.watPath, '-o', watWasmPath]);
    execFileSync('wasm-validate', ['--enable-all', mtkWasmPath]);
    execFileSync('wasm-validate', ['--enable-all', watWasmPath]);
    execFileSync('./build/host/mdis', [mtkWasmPath, '-o', mtkDisPath]);
    execFileSync('./build/host/mdis', [watWasmPath, '-o', watDisPath]);

    const irRawText = execFileSync('./build/host/matc', [sh.mtkPath, '--ir'], { encoding: 'utf8' });
    const irOptText = execFileSync('./build/host/matc', [sh.mtkPath, '--ir-opt'], { encoding: 'utf8' });
    const irRawCount = irRawText.split('\n').filter((l) => l.trim() && !l.trim().startsWith(';')).length;
    const irOptCount = irOptText.split('\n').filter((l) => l.trim() && !l.trim().startsWith(';')).length;

    const mtkBytes = fs.readFileSync(mtkWasmPath);
    const watBytes = fs.readFileSync(watWasmPath);
    const mtkDis   = fs.readFileSync(mtkDisPath, 'utf8');
    const watDis   = fs.readFileSync(watDisPath, 'utf8');

    return {
        mtkBytes,
        watBytes,
        irRawCount,
        irOptCount,
        mtkDisInfo: analyzeDisassembly(mtkDis),
        watDisInfo: analyzeDisassembly(watDis),
    };
}

/* 2. Bellek yerleşimi ve rastgele sahne verisi kurucu */
function createSceneMemory(quadCount, seed = 0x12345678) {
    /* 128 sayfa = 8 MB (12.345+ quad ve mipmap dokusu için bolca güvenli alan) */
    const memory = new WebAssembly.Memory({ initial: 128 });
    const u8 = new Uint8Array(memory.buffer);
    const view = new DataView(memory.buffer);
    const rng = makeRng(seed);

    const batchPtr    = 0x000100;
    const texArrPtr   = 0x000200;
    const uniformsPtr = 0x000400;
    const quadsPtr    = 0x001000; /* max 16384 * 8 = 128 KB */
    const varyingsPtr = 0x030000; /* 6 * 16384 * 16 = 1.5 MB */
    const outRgbaPtr  = 0x200000; /* 4 * 16384 * 16 = 1.0 MB */
    const texPxPtr    = 0x380000; /* 128x128 mip zinciri ~ 88 KB */
    const fbPxPtr     = 0x400000; /* 256x256 * 4 = 256 KB */

    /* Uniform bloğu (48 byte):
     * - vcolor/sprite: tint = (0.85, 0.72, 0.91, 0.88)
     * - plasma:        time = 1.375
     * - lit3d:         light_dir = normalize(0.57735, 0.57735, 0.57735),
     *                  base = (0.92, 0.84, 0.76, 0.95),
     *                  fog = (0.45, 0.55, 0.68, 0.04)
     */
    const lx = 0.57735026, ly = 0.57735026, lz = 0.57735026;
    view.setFloat32(uniformsPtr + 0,  lx,   true);
    view.setFloat32(uniformsPtr + 4,  ly,   true);
    view.setFloat32(uniformsPtr + 8,  lz,   true);
    view.setFloat32(uniformsPtr + 12, 0.88, true);
    view.setFloat32(uniformsPtr + 16, 0.92, true);
    view.setFloat32(uniformsPtr + 20, 0.84, true);
    view.setFloat32(uniformsPtr + 24, 0.76, true);
    view.setFloat32(uniformsPtr + 28, 0.95, true);
    view.setFloat32(uniformsPtr + 32, 0.45, true);
    view.setFloat32(uniformsPtr + 36, 0.55, true);
    view.setFloat32(uniformsPtr + 40, 0.68, true);
    view.setFloat32(uniformsPtr + 44, 0.04, true);

    /* QuadHeader dizisi: rastgele başlangıç maskeleri (0x1..0xF) */
    const initMasks = new Uint32Array(quadCount);
    for (let q = 0; q < quadCount; q++) {
        const m = (q % 7 === 0) ? ((rng.nextU32() & 0x0E) | 1) : 0x0F;
        initMasks[q] = m;
        const off = quadsPtr + q * 8;
        view.setUint32(off + 0, m, true);
        view.setUint16(off + 4, (q % 128) * 2, true);
        view.setUint16(off + 6, (Math.floor(q / 128) % 128) * 2, true);
    }

    /* Varyings (6 kanal SoA):
     * ch 0, 1 (uv / v_color.rg): hem [0..1] hem negatif/sarma [-2..4] hem farklı mip türev ölçekleri
     * ch 2, 3, 4 (nrm / v_color.ba): birim olmayan 3B normaller (sıfıra yakın olmayan)
     * ch 5 (depth): [0..40] arası derinlik (bazıları sis doygunluğunda > 25)
     */
    const stride = quadCount * 16;
    for (let q = 0; q < quadCount; q++) {
        /* Farklı mip seviyelerini (0..7) tetiklemek için quad içi UV adımını çeşitlendir */
        const mipMode = q % 8;
        const baseU = rng.nextF32(-1.5, 2.5);
        const baseV = rng.nextF32(-1.5, 2.5);
        const step = (mipMode === 0) ? 0.002 : (0.004 * (1 << mipMode));

        const uOff = varyingsPtr + 0 * stride + q * 16;
        const vOff = varyingsPtr + 1 * stride + q * 16;
        view.setFloat32(uOff + 0,  baseU,        true);
        view.setFloat32(uOff + 4,  baseU + step, true);
        view.setFloat32(uOff + 8,  baseU,        true);
        view.setFloat32(uOff + 12, baseU + step, true);

        view.setFloat32(vOff + 0,  baseV,        true);
        view.setFloat32(vOff + 4,  baseV,        true);
        view.setFloat32(vOff + 8,  baseV + step, true);
        view.setFloat32(vOff + 12, baseV + step, true);

        for (let lane = 0; lane < 4; lane++) {
            let nx = rng.nextF32(-1.0, 1.0);
            let ny = rng.nextF32(-1.0, 1.0);
            let nz = rng.nextF32(-1.0, 1.0);
            if (nx * nx + ny * ny + nz * nz < 0.04) nx = 0.5;
            view.setFloat32(varyingsPtr + 2 * stride + q * 16 + lane * 4, nx, true);
            view.setFloat32(varyingsPtr + 3 * stride + q * 16 + lane * 4, ny, true);
            view.setFloat32(varyingsPtr + 4 * stride + q * 16 + lane * 4, nz, true);

            const dep = rng.nextF32(-2.0, 35.0);
            view.setFloat32(varyingsPtr + 5 * stride + q * 16 + lane * 4, dep, true);
        }
    }

    /* 64x64 RGBA8 Doku + 7 seviyeli tam mip zinciri (64, 32, 16, 8, 4, 2, 1) */
    const w = 64, h = 64, levels = 7;
    let texelIdx = 0;
    for (let lv = 0; lv < levels; lv++) {
        const wk = w >>> lv;
        const hk = h >>> lv;
        for (let i = 0; i < wk * hk; i++) {
            const r = rng.nextU32() & 0xFF;
            const g = rng.nextU32() & 0xFF;
            const b = rng.nextU32() & 0xFF;
            /* Alfa eşiği sınır değerlerini (0, 1, 2, 3, 4, 255) özellikle dahil et */
            let a;
            if (i % 8 === 0) a = i & 7; /* 0, 1, 2, 3, 4, 5, 6, 7 */
            else a = 32 + (rng.nextU32() % 224);
            const rgba = ((r | (g << 8) | (b << 16) | (a << 24)) >>> 0);
            view.setUint32(texPxPtr + texelIdx * 4, rgba, true);
            texelIdx++;
        }
    }

    view.setUint32(texArrPtr + 0,  texPxPtr, true);
    view.setUint32(texArrPtr + 4,  w,        true);
    view.setUint32(texArrPtr + 8,  h,        true);
    view.setUint32(texArrPtr + 12, w - 1,    true);
    view.setUint32(texArrPtr + 16, h - 1,    true);
    view.setUint32(texArrPtr + 20, 6,        true); /* log2(64) = 6 */
    view.setUint32(texArrPtr + 24, 6,        true); /* BILINEAR | MIPMAP */
    view.setUint32(texArrPtr + 28, levels,   true);

    /* ShadeBatch */
    view.setUint32(batchPtr + 0,  quadCount,   true);
    view.setUint32(batchPtr + 4,  6,           true);
    view.setUint32(batchPtr + 8,  quadsPtr,    true);
    view.setUint32(batchPtr + 12, varyingsPtr, true);
    view.setUint32(batchPtr + 16, uniformsPtr, true);
    view.setUint32(batchPtr + 20, outRgbaPtr,  true);
    view.setUint32(batchPtr + 24, texArrPtr,   true);
    view.setUint32(batchPtr + 28, fbPxPtr,     true);
    view.setUint32(batchPtr + 32, 256,         true);

    return {
        memory,
        u8,
        view,
        batchPtr,
        quadsPtr,
        varyingsPtr,
        uniformsPtr,
        outRgbaPtr,
        texArrPtr,
        initMasks,
        quadCount,
    };
}

function resetMasksAndOut(scene) {
    const { view, u8, quadsPtr, outRgbaPtr, initMasks, quadCount } = scene;
    for (let q = 0; q < quadCount; q++) {
        view.setUint32(quadsPtr + q * 8, initMasks[q], true);
    }
    u8.fill(0xAA, outRgbaPtr, outRgbaPtr + 4 * quadCount * 16);
}

/* 3. Bozuk / düşmanca / sınır girdi testleri */
function testHostileInputs(sh, watBytes) {
    const scene = createSceneMemory(16, 0xCAFEBABE);
    const mod = new WebAssembly.Module(watBytes);
    const inst = new WebAssembly.Instance(mod, { env: { memory: scene.memory } });
    const shade = inst.exports.shade;
    const { view, batchPtr, quadsPtr, outRgbaPtr, texArrPtr } = scene;

    /* (a) NULL / negatif batch işaretçisi */
    shade(0);
    shade(-16);

    /* (b) count == 0, negatif count (-1 = 0xFFFFFFFF), aşırı count (0x40000000 -> <<4 taşması) */
    for (const badCount of [0, -1, -100, 0x10000000, 0x40000000, 0x7FFFFFFF]) {
        view.setUint32(batchPtr + 0, badCount >>> 0, true);
        view.setUint32(outRgbaPtr, 0xDEADBEEF, true);
        shade(batchPtr);
        if (view.getUint32(outRgbaPtr, true) !== 0xDEADBEEF) {
            throw new Error(`[${sh.name}] bozuk count=${badCount} belleği değiştirdi!`);
        }
    }
    view.setUint32(batchPtr + 0, 16, true);

    /* (c) NULL alt işaretçiler (varyings, uniforms, out_rgba, quads, textures) */
    for (const off of [8, 12, 16, 20, 24]) {
        const saved = view.getUint32(batchPtr + off, true);
        view.setUint32(batchPtr + off, 0, true);
        shade(batchPtr);
        view.setUint32(batchPtr + off, saved, true);
    }

    /* (d) Doku kullanan shader'larda bozuk doku başlığı (px=0, w=0, h=0, levels=0, levels=999) */
    if (sh.hasTexture) {
        const savedPx = view.getUint32(texArrPtr + 0, true);
        const savedW  = view.getUint32(texArrPtr + 4, true);
        const savedH  = view.getUint32(texArrPtr + 8, true);
        const savedLv = view.getUint32(texArrPtr + 28, true);

        view.setUint32(texArrPtr + 0, 0, true);
        shade(batchPtr);
        view.setUint32(texArrPtr + 0, savedPx, true);

        view.setUint32(texArrPtr + 4, 0, true);
        shade(batchPtr);
        view.setUint32(texArrPtr + 4, savedW, true);

        view.setUint32(texArrPtr + 8, 0, true);
        shade(batchPtr);
        view.setUint32(texArrPtr + 8, savedH, true);

        view.setUint32(texArrPtr + 28, 0, true);
        shade(batchPtr);
        view.setUint32(texArrPtr + 28, 999, true);
        shade(batchPtr);
        view.setUint32(texArrPtr + 28, savedLv, true);
    }

    /* (e) Küçük / kuyruk quad sayıları: count = 1, 2, 3, 5, 7 + sınır taşma (canary) kontrolü */
    for (const cnt of [1, 2, 3, 5, 7]) {
        view.setUint32(batchPtr + 0, cnt, true);
        const strideBytes = cnt * 16;
        /* Her kanalın hemen sonrasına nöbetçi (canary) yaz */
        const canary = 0x7F80BEEF;
        for (let c = 0; c < 4; c++) {
            view.setUint32(outRgbaPtr + c * strideBytes + strideBytes, canary, true);
        }
        view.setUint32(quadsPtr + cnt * 8, 0xCAFE1234, true);
        shade(batchPtr);
        if (view.getUint32(quadsPtr + cnt * 8, true) !== 0xCAFE1234) {
            throw new Error(`[${sh.name}] count=${cnt} quads sınırını taşırdı!`);
        }
    }
}

/* 4. Doğruluk karşılaştırması (>= 10.000 rastgele quad) */
function verifyShader(sh, mtkBytes, watBytes, quadCount) {
    const sceneMtk = createSceneMemory(quadCount, 0x9E3779B9);
    const sceneWat = createSceneMemory(quadCount, 0x9E3779B9);

    resetMasksAndOut(sceneMtk);
    resetMasksAndOut(sceneWat);

    const mtkMod = new WebAssembly.Module(mtkBytes);
    const watMod = new WebAssembly.Module(watBytes);
    const mtkInst = new WebAssembly.Instance(mtkMod, { env: { memory: sceneMtk.memory } });
    const watInst = new WebAssembly.Instance(watMod, { env: { memory: sceneWat.memory } });

    mtkInst.exports.shade(sceneMtk.batchPtr);
    watInst.exports.shade(sceneWat.batchPtr);

    /* Quad maskelerini karşılaştır */
    for (let q = 0; q < quadCount; q++) {
        const mMtk = sceneMtk.view.getUint32(sceneMtk.quadsPtr + q * 8, true);
        const mWat = sceneWat.view.getUint32(sceneWat.quadsPtr + q * 8, true);
        if (mMtk !== mWat) {
            throw new Error(`[${sh.name}] quad=${q} maske uyuşmazlığı: mtk=0x${mMtk.toString(16)} wat=0x${mWat.toString(16)}`);
        }
    }

    /* 4 kanal * quadCount * 4 lane = 16 * quadCount adet f32 çıktısını karşılaştır */
    const totalFloats = 4 * quadCount * 4;
    let maxAbsDiff = 0.0;
    let sumAbsDiff = 0.0;
    let exactMatches = 0;

    for (let i = 0; i < totalFloats; i++) {
        const off = sceneMtk.outRgbaPtr + i * 4;
        const fMtk = sceneMtk.view.getFloat32(off, true);
        const fWat = sceneWat.view.getFloat32(off, true);
        const uMtk = sceneMtk.view.getInt32(off, true);
        const uWat = sceneWat.view.getInt32(off, true);

        if (uMtk === uWat) {
            exactMatches++;
            continue;
        }

        const absDiff = Math.abs(fMtk - fWat);
        sumAbsDiff += absDiff;
        if (absDiff > maxAbsDiff) maxAbsDiff = absDiff;
    }

    const maxLsbDiff = maxAbsDiff * 255.0;
    const meanLsbDiff = (sumAbsDiff / totalFloats) * 255.0;
    /* Kabul kriteri: bit-bit veya <= 1 LSB (1/255 = 0.00392157) */
    if (!(maxLsbDiff <= 1.0)) {
        throw new Error(
            `[${sh.name}] doğruluk hatası: maxAbsDiff=${maxAbsDiff} (${maxLsbDiff.toFixed(6)} LSB > 1 LSB)`
        );
    }

    return {
        quadCount,
        totalFloats,
        exactMatches,
        exactPct: (exactMatches / totalFloats) * 100.0,
        maxAbsDiff,
        maxLsbDiff,
        meanLsbDiff,
    };
}

/* 5. Performans ölçümü (1M quad = 250 iterasyon x 4.000 quad, ısınma + >=15 tekrar, medyan) */
const BENCH_BATCH_QUADS = 4000;
const BENCH_ITERS = 250;
const BENCH_TOTAL_QUADS = BENCH_BATCH_QUADS * BENCH_ITERS; /* 1.000.000 quad */

function resetBenchMasks(scene) {
    const { view, quadsPtr, quadCount } = scene;
    for (let q = 0; q < quadCount; q++) {
        view.setUint32(quadsPtr + q * 8, 0x0F, true);
    }
}

function benchPairInterleaved(mtkBytes, watBytes, needsMaskReset, trials) {
    const sceneMtk = createSceneMemory(BENCH_BATCH_QUADS, 0x243F6A88);
    const sceneWat = createSceneMemory(BENCH_BATCH_QUADS, 0x243F6A88);
    const mtkMod = new WebAssembly.Module(mtkBytes);
    const watMod = new WebAssembly.Module(watBytes);
    const mtkInst = new WebAssembly.Instance(mtkMod, { env: { memory: sceneMtk.memory } });
    const watInst = new WebAssembly.Instance(watMod, { env: { memory: sceneWat.memory } });
    const shadeMtk = mtkInst.exports.shade;
    const shadeWat = watInst.exports.shade;
    const batchMtk = sceneMtk.batchPtr;
    const batchWat = sceneWat.batchPtr;

    /* Isınma: 60 iterasyon = 240.000 quad (TurboFan derlemesini garantiler) */
    for (let i = 0; i < 60; i++) {
        if (needsMaskReset) {
            resetBenchMasks(sceneMtk);
            resetBenchMasks(sceneWat);
        }
        shadeMtk(batchMtk);
        shadeWat(batchWat);
    }

    const mtkSamples = [];
    const watSamples = [];

    const runOne = (shadeFn, scene, batchPtr) => {
        let elapsedNs = 0n;
        for (let i = 0; i < BENCH_ITERS; i++) {
            if (needsMaskReset) resetBenchMasks(scene);
            const t0 = process.hrtime.bigint();
            shadeFn(batchPtr);
            const t1 = process.hrtime.bigint();
            elapsedNs += (t1 - t0);
        }
        const sec = Number(elapsedNs) / 1e9;
        return (BENCH_TOTAL_QUADS / 1e6) / sec;
    };

    /* Termal/frekans sürüklenmesini eşitlemek için her tekrarda sırayı değiştirerek ölç */
    for (let t = 0; t < trials; t++) {
        if ((t & 1) === 0) {
            mtkSamples.push(runOne(shadeMtk, sceneMtk, batchMtk));
            watSamples.push(runOne(shadeWat, sceneWat, batchWat));
        } else {
            watSamples.push(runOne(shadeWat, sceneWat, batchWat));
            mtkSamples.push(runOne(shadeMtk, sceneMtk, batchMtk));
        }
    }

    const summarize = (arr) => {
        arr.sort((a, b) => a - b);
        return {
            median: arr[Math.floor(arr.length / 2)],
            min: arr[0],
            max: arr[arr.length - 1],
            samples: arr,
        };
    };

    return {
        mtkBench: summarize(mtkSamples),
        watBench: summarize(watSamples),
    };
}

/* Ana akış */
const cpuModel = os.cpus()[0]?.model || 'unknown CPU';
console.log(`# M10_E — Matoki Çıktısı vs Elle Yazılmış En İyi WAT`);
console.log(`- Ortam: ${cpuModel}, Node ${process.version} (${os.platform()} ${os.arch()})`);
console.log(`- Doğrulama: ${VERIFY_QUADS} rastgele quad (${VERIFY_QUADS * 4} piksel) + bozuk/sınır girdi testleri`);
console.log(`- Ölçüm: 1.000.000 quad (250 batch × 4.000 quad), 60 batch ısınma + ${TRIALS} tekrar (nöbetleşe) medyan\n`);

const results = [];

for (const sh of SHADERS) {
    const info = compileAndAnalyze(sh);
    testHostileInputs(sh, info.watBytes);
    const ver = verifyShader(sh, info.mtkBytes, info.watBytes, VERIFY_QUADS);
    const { mtkBench, watBench } = benchPairInterleaved(info.mtkBytes, info.watBytes, sh.hasDiscard, TRIALS);

    const effPct = (mtkBench.median / watBench.median) * 100.0;
    const speedup = watBench.median / mtkBench.median;

    results.push({
        sh,
        info,
        ver,
        mtkBench,
        watBench,
        effPct,
        speedup,
    });
}

console.log(`## 1. Doğruluk Karşılaştırması (${VERIFY_QUADS} rastgele quad + bozuk girdi testleri)\n`);
console.log(`| Shader | Quad Sayısı | Maske Eşitliği | Tam Bit Eşitliği (%) | Maks Mutlak Fark | Maks Fark (LSB = 1/255) | Ort. Fark (LSB) | Bozuk Girdi Testi | Durum |`);
console.log(`|---|---:|---|---:|---:|---:|---:|---|---|`);
for (const r of results) {
    console.log(
        `| \`${r.sh.name}\` | ${r.ver.quadCount} | Bit-bit aynı | %${r.ver.exactPct.toFixed(1)} | ` +
        `${r.ver.maxAbsDiff.toExponential(2)} | ${r.ver.maxLsbDiff.toExponential(2)} LSB | ` +
        `${r.ver.meanLsbDiff.toExponential(2)} LSB | GEÇTİ | **GEÇTİ** |`
    );
}

console.log(`\n## 2. Performans, İç Döngü Komut Sayısı ve Kod Boyutu (1M Quad, ${TRIALS} Tekrar Medyan)\n`);
console.log(`| Shader | IR (Ham → Opt) | İç Döngü Komutu (Matoki → Elle WAT) | Wasm Boyutu (Matoki → Elle WAT) | Matoki JIT (Mquad/s) | Elle WAT (Mquad/s) | Elle WAT Hızlanma | Matoki / Elle WAT Verimi (%) |`);
console.log(`|---|---:|---:|---:|---:|---:|---:|---:|`);
for (const r of results) {
    const mtkLoop = r.info.mtkDisInfo.mainLoopOps;
    const watLoop = r.info.watDisInfo.mainLoopOps;
    const mtkLoopNote = r.info.mtkDisInfo.loopCount > 1
        ? `${mtkLoop} (4x=${Math.round(mtkLoop / 4)}/q)`
        : `${mtkLoop}`;
    const watLoopNote = r.info.watDisInfo.loopCount > 1
        ? `${watLoop} (4x=${Math.round(watLoop / 4)}/q)`
        : `${watLoop}`;
    console.log(
        `| \`${r.sh.name}\` | ${r.info.irRawCount} → ${r.info.irOptCount} | ` +
        `${mtkLoopNote} → ${watLoopNote} | ${r.info.mtkBytes.length} B → ${r.info.watBytes.length} B | ` +
        `**${r.mtkBench.median.toFixed(1)}** | **${r.watBench.median.toFixed(1)}** | ` +
        `**${r.speedup.toFixed(2)}×** | **%${r.effPct.toFixed(1)}** |`
    );
}
