/* tools/host/bench_shade.mjs — Matoki JIT vs clang -O2 -msimd128 vs Blend Fusion benchmark (1M quad).
 * Kullanım:
 *   node tools/host/bench_shade.mjs
 *
 * 4 shader (sprite, vcolor, plasma, branchy) için:
 *   (a) Matoki JIT tarafından üretilen SIMD WebAssembly modülü (MTK_FUSE_NONE)
 *   (b) Aynı shader'ın C referans kodu (src/raster/ref_shaders.c + plasma/branchy C karşılıkları),
 *       clang -O2 -msimd128 ile WebAssembly'ye derlenmiş hali
 *   (c) Matoki Blend Fusion varyantı (MTK_FUSE_ALPHA / MTK_FUSE_REPLACE) vs ayrı shade + blend
 * 1.000.000 quad (250 iterasyon x 4.000 quad/batch) üzerinden ölçülür ve Mquad/s tablosu basılır.
 */
import fs from 'node:fs';
import { execSync } from 'node:child_process';

const BENCH_DIR = 'build/bench_shade';
fs.mkdirSync(BENCH_DIR, { recursive: true });

/* 1. Matoki JIT modüllerini (unopt/before-LICM, opt/after-LICM, fused, specialized) üreten
 *    küçük host C aracını derle ve çalıştır. */
const genMtkC = `
#define _POSIX_C_SOURCE 200809L
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "core/arena.h"
#include "matoki/matoki.h"
#include "matoki/lexer.h"
#include "matoki/parser.h"
#include "matoki/sema.h"
#include "matoki/lower.h"
#include "matoki/ir.h"
#include "matoki/codegen.h"
#include "matoki/variant.h"

static char* read_file(const char* path, u32* out_len) {
    FILE* f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    char* buf = (char*)malloc((size_t)sz + 1);
    if (sz > 0) {
        size_t nr = fread(buf, 1, (size_t)sz, f);
        (void)nr;
    }
    buf[sz] = '\\0';
    fclose(f);
    *out_len = (u32)sz;
    return buf;
}

static void write_file(const char* path, const void* data, u32 len) {
    FILE* f = fopen(path, "wb");
    if (!f) { perror(path); exit(1); }
    fwrite(data, 1, len, f);
    fclose(f);
}

int main(void) {
    static u8 persist_buf[MB(8)];
    static u8 scratch_buf[MB(8)];
    Arena persist, scratch;
    arena_init(&persist, persist_buf, sizeof(persist_buf));
    arena_init(&scratch, scratch_buf, sizeof(scratch_buf));

    MtkVariantCache cache;
    mtk_variant_cache_init(&cache, &persist);

    const char* names[4] = { "sprite", "vcolor", "plasma", "branchy" };
    const char* paths[4] = {
        "shaders/sprite.mtk",
        "shaders/vcolor.mtk",
        "shaders/plasma.mtk",
        "shaders/branchy.mtk"
    };
    const u32 fuse_modes[4] = {
        MTK_FUSE_ALPHA,
        MTK_FUSE_ALPHA,
        MTK_FUSE_REPLACE,
        MTK_FUSE_ALPHA
    };

    printf("{\\n  \\"shaders\\": {\\n");
    for (int i = 0; i < 4; i++) {
        u32 src_len = 0;
        char* src = read_file(paths[i], &src_len);
        if (!src) { fprintf(stderr, "cannot read %s\\n", paths[i]); return 1; }

        /* LICM öncesi IR döngü komutu sayısını ölç (constfold+CSE+DCE sonrası, LICM öncesi) */
        arena_reset(&scratch);
        MtkDiag diag;
        mtk_diag_init(&diag);
        MtkLayout layout;
        memset(&layout, 0, sizeof(layout));
        AstProgram prog;
        memset(&prog, 0, sizeof(prog));
        parser_parse(src, src_len, paths[i], &scratch, &prog, &diag);
        sema_check(&prog, src, src_len, paths[i], &scratch, &layout, &diag);
        IrFunc f_no_licm;
        memset(&f_no_licm, 0, sizeof(f_no_licm));
        lower_ast(&prog, &layout, &scratch, &f_no_licm);

        ir_opt_constfold(&f_no_licm);
        ir_opt_cse(&f_no_licm);
        ir_opt_dce(&f_no_licm);
        ir_compact(&f_no_licm);
        u32 ir_loop_before = ir_count_loop_insts(&f_no_licm);

        /* Standart JIT varyantı (MTK_FUSE_NONE) */
        MtkVariantKey key;
        memset(&key, 0, sizeof(key));
        key.src = src;
        key.src_len = src_len;
        key.filename = paths[i];
        key.cg_opts.own_memory = TRUE;
        key.cg_opts.fuse_blend = MTK_FUSE_NONE;
        if (i == 0) {
            /* sprite nearest+wrap varyantı (sampler_flags[0] = 0) */
            key.sampler_mask = 1u;
            key.sampler_flags[0] = 0u;
        }
        const MtkVariant* v_std = mtk_variant_get(&cache, &key, &scratch, &diag);
        if (!v_std) { fprintf(stderr, "compile failed %s\\n", names[i]); return 1; }

        char out_std[128], out_fused[128];
        snprintf(out_std, sizeof(out_std), "${BENCH_DIR}/%s_mtk.wasm", names[i]);
        write_file(out_std, v_std->wasm, v_std->wasm_len);

        /* Genel (sampler_mask=0) sprite varyantını da kaydet */
        u32 generic_loop_ops = v_std->wasm_loop_ops;
        u32 generic_wasm_len = v_std->wasm_len;
        if (i == 0) {
            MtkVariantKey key_gen = key;
            key_gen.sampler_mask = 0;
            const MtkVariant* v_gen = mtk_variant_get(&cache, &key_gen, &scratch, &diag);
            generic_loop_ops = v_gen->wasm_loop_ops;
            generic_wasm_len = v_gen->wasm_len;
        }

        /* Kaynaştırılmış (Blend Fusion) varyant */
        MtkVariantKey key_f = key;
        key_f.cg_opts.fuse_blend = fuse_modes[i];
        const MtkVariant* v_fused = mtk_variant_get(&cache, &key_f, &scratch, &diag);
        if (!v_fused) { fprintf(stderr, "fused compile failed %s\\n", names[i]); return 1; }
        snprintf(out_fused, sizeof(out_fused), "${BENCH_DIR}/%s_mtk_fused.wasm", names[i]);
        write_file(out_fused, v_fused->wasm, v_fused->wasm_len);

        printf("    \\"%s\\": { \\"ir_total\\": %u, \\"ir_loop_before\\": %u, \\"ir_loop_after\\": %u, "
               "\\"wasm_len\\": %u, \\"wasm_loop_ops\\": %u, \\"generic_loop_ops\\": %u, \\"generic_wasm_len\\": %u, "
               "\\"fused_wasm_len\\": %u, \\"fused_loop_ops\\": %u }%s\\n",
               names[i], v_std->ir_inst_count, ir_loop_before, v_std->loop_inst_count,
               v_std->wasm_len, v_std->wasm_loop_ops, generic_loop_ops, generic_wasm_len,
               v_fused->wasm_len, v_fused->wasm_loop_ops, (i < 3) ? "," : "");
        free(src);
    }
    printf("  }\\n}\\n");
    return 0;
}
`;

fs.writeFileSync(`${BENCH_DIR}/gen_mtk.c`, genMtkC);
execSync(
    `clang -std=c11 -O2 -Wall -Wextra -Werror -Isrc ${BENCH_DIR}/gen_mtk.c ` +
    `tools/host/plat_host.c src/core/arena.c src/core/log.c src/core/math.c src/core/rng.c ` +
    `src/mbin/mbin.c src/matoki/*.c -lm -o ${BENCH_DIR}/gen_mtk`
);
const statsJson = execSync(`./${BENCH_DIR}/gen_mtk`, { encoding: 'utf8' });
const mtkStats = JSON.parse(statsJson).shaders;

/* 2. clang -O2 -msimd128 ile C referans shader'larını (ref_shaders.c + vcolor_tint + plasma + branchy + blend)
 *    WebAssembly modülüne derle. */
const clangRefC = `
#include "raster/ref_shaders.h"
#include "core/math.h"
#include "core/log.h"

void log_panic(const char* file, i32 line, const char* fmt, ...) {
    (void)file; (void)line; (void)fmt;
    __builtin_trap();
}

/* vcolor.mtk C karşılığı: return v_color * tint; */
void ref_shade_vcolor_tint(ShadeBatch* batch) {
    u32 count = batch->count;
    if (count == 0) return;
    Vec4 tint = vec4(1.0f, 1.0f, 1.0f, 1.0f);
    if (batch->uniforms != NULL) {
        tint = *(const Vec4*)batch->uniforms;
    }
    u32 total_lanes = count * 4u;
    const f32* vr = batch->varyings + 0u * total_lanes;
    const f32* vg = batch->varyings + 1u * total_lanes;
    const f32* vb = batch->varyings + 2u * total_lanes;
    const f32* va = batch->varyings + 3u * total_lanes;
    f32* or_ch = batch->out_rgba + 0u * total_lanes;
    f32* og_ch = batch->out_rgba + 1u * total_lanes;
    f32* ob_ch = batch->out_rgba + 2u * total_lanes;
    f32* oa_ch = batch->out_rgba + 3u * total_lanes;
    for (u32 i = 0; i < total_lanes; i++) {
        or_ch[i] = vr[i] * tint.x;
        og_ch[i] = vg[i] * tint.y;
        ob_ch[i] = vb[i] * tint.z;
        oa_ch[i] = va[i] * tint.w;
    }
}

/* plasma.mtk C referans karşılığı (core/math.c m_sin / m_cos kullanarak) */
void ref_shade_plasma(ShadeBatch* batch) {
    u32 count = batch->count;
    if (count == 0) return;
    f32 t = 0.0f;
    if (batch->uniforms != NULL) {
        t = *(const f32*)batch->uniforms;
    }
    const f32 PI = 3.14159265f;
    u32 total_lanes = count * 4u;
    const f32* uv_x = batch->varyings + 0u * total_lanes;
    const f32* uv_y = batch->varyings + 1u * total_lanes;
    f32* out_r = batch->out_rgba + 0u * total_lanes;
    f32* out_g = batch->out_rgba + 1u * total_lanes;
    f32* out_b = batch->out_rgba + 2u * total_lanes;
    f32* out_a = batch->out_rgba + 3u * total_lanes;

    f32 t05 = t * 0.5f;
    f32 t07 = t * 0.7f;
    f32 t13 = t * 1.3f;
    for (u32 i = 0; i < total_lanes; i++) {
        f32 px = uv_x[i];
        f32 py = uv_y[i];
        f32 v1 = m_sin(px * 10.0f + t);
        f32 v2 = m_cos(py * 10.0f - t05);
        f32 v3 = m_sin((px + py) * 7.0f + t07);
        f32 cx = px - 0.5f;
        f32 cy = py - 0.5f;
        f32 d  = m_sqrt(cx * cx + cy * cy);
        f32 v4 = m_cos(d * 14.0f - t13);
        f32 w  = (v1 + v2 + v3 + v4) * 0.25f;
        f32 wpi = w * PI;
        out_r[i] = m_sin(wpi) * 0.5f + 0.5f;
        out_g[i] = m_cos(wpi + 1.0f) * 0.5f + 0.5f;
        out_b[i] = m_sin(wpi + 2.0f) * 0.5f + 0.5f;
        out_a[i] = 1.0f;
    }
}

/* branchy.mtk C referans karşılığı */
void ref_shade_branchy(ShadeBatch* batch) {
    u32 count = batch->count;
    if (count == 0) return;
    f32 threshold = 1.2f;
    if (batch->uniforms != NULL) {
        threshold = *(const f32*)batch->uniforms;
    }
    u32 total_lanes = count * 4u;
    const f32* uv_x = batch->varyings + 0u * total_lanes;
    const f32* uv_y = batch->varyings + 1u * total_lanes;
    f32* out_r = batch->out_rgba + 0u * total_lanes;
    f32* out_g = batch->out_rgba + 1u * total_lanes;
    f32* out_b = batch->out_rgba + 2u * total_lanes;
    f32* out_a = batch->out_rgba + 3u * total_lanes;

    for (u32 q = 0; q < count; q++) {
        u32 mask = batch->quads[q].mask;
        u32 base = q * 4u;
        for (u32 lane = 0; lane < 4; lane++) {
            u32 idx = base + lane;
            f32 ux = uv_x[idx];
            f32 uy = uv_y[idx];
            f32 cr = 0.1f, cg = 0.2f, cb = 0.3f, ca = 1.0f;
            if (ux > 0.5f) {
                if (uy > 0.5f) {
                    if (ux + uy > threshold) {
                        mask &= ~(1u << lane);
                    }
                    cr = 1.0f; cg = 0.0f; cb = 0.0f; ca = 1.0f;
                } else {
                    cg = ux;
                }
            } else {
                if (uy > 0.5f) {
                    cb = uy;
                } else {
                    cr = ux; cg = uy; cb = 0.5f; ca = 1.0f;
                }
            }
            out_r[idx] = cr;
            out_g[idx] = cg;
            out_b[idx] = cb;
            out_a[idx] = ca;
        }
        batch->quads[q].mask = mask;
    }
}

/* raster.c içindeki ayrı blend aşamasının birebir referansı */
static inline u32 f32_to_u8(f32 v) {
    return (u32)(v * 255.0f + 0.5f);
}

void ref_blend_batch(ShadeBatch* batch, u32 mode) {
    u32 count = batch->count;
    u32 total_lanes = count * 4u;
    const f32* out_r = batch->out_rgba + 0u * total_lanes;
    const f32* out_g = batch->out_rgba + 1u * total_lanes;
    const f32* out_b = batch->out_rgba + 2u * total_lanes;
    const f32* out_a = batch->out_rgba + 3u * total_lanes;
    u32* fb_px = batch->fb_px;
    u32 stride = batch->fb_stride;
    const f32 inv255 = 1.0f / 255.0f;

    for (u32 q = 0; q < count; q++) {
        u32 mask = batch->quads[q].mask;
        if (mask == 0) continue;
        u32 qx = batch->quads[q].x;
        u32 qy = batch->quads[q].y;
        u32 base = q * 4u;
        for (u32 lane = 0; lane < 4; lane++) {
            if (!(mask & (1u << lane))) continue;
            u32 idx = base + lane;
            f32 sr = m_clamp(out_r[idx], 0.0f, 1.0f);
            f32 sg = m_clamp(out_g[idx], 0.0f, 1.0f);
            f32 sb = m_clamp(out_b[idx], 0.0f, 1.0f);
            f32 sa = m_clamp(out_a[idx], 0.0f, 1.0f);
            u32 px_off = (qy + (lane >> 1u)) * stride + (qx + (lane & 1u));
            u32 r8, g8, b8, a8;
            if (mode == 1) {
                r8 = f32_to_u8(sr);
                g8 = f32_to_u8(sg);
                b8 = f32_to_u8(sb);
                a8 = f32_to_u8(sa);
            } else {
                u32 dst = fb_px[px_off];
                f32 dr = (f32)(dst & 0xFFu) * inv255;
                f32 dg = (f32)((dst >> 8u) & 0xFFu) * inv255;
                f32 db = (f32)((dst >> 16u) & 0xFFu) * inv255;
                f32 da = (f32)((dst >> 24u) & 0xFFu) * inv255;
                f32 inv_sa = 1.0f - sa;
                r8 = f32_to_u8(MIN(sr * sa + dr * inv_sa, 1.0f));
                g8 = f32_to_u8(MIN(sg * sa + dg * inv_sa, 1.0f));
                b8 = f32_to_u8(MIN(sb * sa + db * inv_sa, 1.0f));
                a8 = f32_to_u8(MIN(sa + da * inv_sa, 1.0f));
            }
            fb_px[px_off] = r8 | (g8 << 8u) | (b8 << 16u) | (a8 << 24u);
        }
    }
}
`;

fs.writeFileSync(`${BENCH_DIR}/clang_ref.c`, clangRefC);
execSync(
    `clang --target=wasm32 -std=c11 -O2 -ffreestanding -fno-builtin -nostdlib ` +
    `-msimd128 -mnontrapping-fptoint -msign-ext -DNDEBUG -Isrc ` +
    `-Wl,--no-entry -Wl,--export-all -Wl,--initial-memory=4194304 -Wl,--max-memory=4194304 ` +
    `${BENCH_DIR}/clang_ref.c src/raster/ref_shaders.c src/core/math.c src/core/mem.c ` +
    `-o ${BENCH_DIR}/clang_ref.wasm`
);

/* 3. Bellek yerleşimi hazırlayıcısı (4.000 quad x 250 iterasyon = 1.000.000 quad) */
const BATCH_QUADS = 4000;
const ITERS = 250;
const TOTAL_QUADS = BATCH_QUADS * ITERS; /* 1.000.000 quad */

function setupMemory(mem) {
    const u8 = new Uint8Array(mem.buffer);
    const view = new DataView(mem.buffer);

    const batchPtr    = 0x00100;
    const texArrPtr   = 0x00200;
    const uniformsPtr = 0x00400;
    const quadsPtr    = 0x01000; /* 4000 * 8 = 32 KB */
    const varyingsPtr = 0x10000; /* 4 * 4000 * 16 = 256 KB */
    const outRgbaPtr  = 0x50000; /* 4 * 4000 * 16 = 256 KB */
    const texPxPtr    = 0x90000; /* 64 * 64 * 4 = 16 KB */
    const fbPxPtr     = 0xA0000; /* 256 * 256 * 4 = 256 KB */

    /* Uniforms: tint = (0.9, 0.8, 0.7, 0.95) / time = 1.25 / threshold = 1.35 */
    view.setFloat32(uniformsPtr + 0, 0.90, true);
    view.setFloat32(uniformsPtr + 4, 0.80, true);
    view.setFloat32(uniformsPtr + 8, 0.70, true);
    view.setFloat32(uniformsPtr + 12, 0.95, true);

    /* Quads (mask = 0xF, x ve y 256x256 fb içinde) */
    for (let q = 0; q < BATCH_QUADS; q++) {
        const off = quadsPtr + q * 8;
        view.setUint32(off + 0, 0x0F, true);
        view.setUint16(off + 4, (q % 128) * 2, true);
        view.setUint16(off + 6, (Math.floor(q / 128) % 128) * 2, true);
    }

    /* Varyings (4 kanal x BATCH_QUADS x 4 lane) */
    for (let c = 0; c < 4; c++) {
        const chBase = varyingsPtr + c * BATCH_QUADS * 16;
        for (let i = 0; i < BATCH_QUADS * 4; i++) {
            const val = ((i * 17 + c * 31) & 255) / 255.0;
            view.setFloat32(chBase + i * 4, val, true);
        }
    }

    /* 64x64 RGBA8 Doku (alfa > 0.01 çoğunlukta, bazı pikseller saydam) */
    const w = 64, h = 64;
    for (let i = 0; i < w * h; i++) {
        const r = (i * 37) & 0xFF;
        const g = (i * 73) & 0xFF;
        const b = (i * 109) & 0xFF;
        const a = (i % 16 === 0) ? 0 : 220;
        view.setUint32(texPxPtr + i * 4, (r | (g << 8) | (b << 16) | (a << 24)) >>> 0, true);
    }
    view.setUint32(texArrPtr + 0, texPxPtr, true);
    view.setUint32(texArrPtr + 4, w, true);
    view.setUint32(texArrPtr + 8, h, true);
    view.setUint32(texArrPtr + 12, w - 1, true);
    view.setUint32(texArrPtr + 16, h - 1, true);
    view.setUint32(texArrPtr + 20, 6, true); /* log2(64) = 6 */
    view.setUint32(texArrPtr + 24, 0, true); /* nearest + wrap */
    view.setUint32(texArrPtr + 28, 0, true);

    /* Framebuffer (256x256 RGBA8) */
    u8.fill(0x80, fbPxPtr, fbPxPtr + 256 * 256 * 4);

    /* ShadeBatch */
    view.setUint32(batchPtr + 0, BATCH_QUADS, true);
    view.setUint32(batchPtr + 4, 4, true);
    view.setUint32(batchPtr + 8, quadsPtr, true);
    view.setUint32(batchPtr + 12, varyingsPtr, true);
    view.setUint32(batchPtr + 16, uniformsPtr, true);
    view.setUint32(batchPtr + 20, outRgbaPtr, true);
    view.setUint32(batchPtr + 24, texArrPtr, true);
    view.setUint32(batchPtr + 28, fbPxPtr, true);
    view.setUint32(batchPtr + 32, 256, true);

    return { batchPtr, quadsPtr, view };
}

function resetMasks(view, quadsPtr) {
    for (let q = 0; q < BATCH_QUADS; q++) {
        view.setUint32(quadsPtr + q * 8, 0x0F, true);
    }
}

function benchFn(fn, memInfo, needsMaskReset) {
    /* Isınma (50 iterasyon = 200k quad) */
    for (let i = 0; i < 50; i++) {
        if (needsMaskReset) resetMasks(memInfo.view, memInfo.quadsPtr);
        fn(memInfo.batchPtr);
    }

    /* 5 ölçüm turu, medyan Mquad/s seçilir */
    const samples = [];
    for (let trial = 0; trial < 5; trial++) {
        let elapsedNs = 0n;
        for (let i = 0; i < ITERS; i++) {
            if (needsMaskReset) resetMasks(memInfo.view, memInfo.quadsPtr);
            const t0 = process.hrtime.bigint();
            fn(memInfo.batchPtr);
            const t1 = process.hrtime.bigint();
            elapsedNs += (t1 - t0);
        }
        const sec = Number(elapsedNs) / 1e9;
        samples.push((TOTAL_QUADS / 1e6) / sec);
    }
    samples.sort((a, b) => a - b);
    return samples[2];
}

const clangMod = new WebAssembly.Module(fs.readFileSync(`${BENCH_DIR}/clang_ref.wasm`));
const clangInst = new WebAssembly.Instance(clangMod, {});
const clangMemInfo = setupMemory(clangInst.exports.memory);

const configs = [
    { name: 'sprite',  clangFn: 'ref_shade_sprite',      blendMode: 2, resetMask: true  },
    { name: 'vcolor',  clangFn: 'ref_shade_vcolor_tint', blendMode: 2, resetMask: false },
    { name: 'plasma',  clangFn: 'ref_shade_plasma',      blendMode: 1, resetMask: false },
    { name: 'branchy', clangFn: 'ref_shade_branchy',     blendMode: 2, resetMask: true  },
];

console.log('# Matoki JIT vs clang -O2 -msimd128 Benchmark (1M Quad = 4M Piksel)\n');
console.log('| Shader | IR Döngü (Önce -> Sonra) | Wasm Döngü Op | Matoki JIT (Mquad/s) | clang -O2 -msimd128 (Mquad/s) | JIT / clang | Matoki Fused Shade+Blend (Mquad/s) | Ayrı Shade+Blend (Mquad/s) | Fused Hızlanma |');
console.log('|---|---:|---:|---:|---:|---:|---:|---:|---:|');

for (const cfg of configs) {
    const st = mtkStats[cfg.name];

    const mtkMod = new WebAssembly.Module(fs.readFileSync(`${BENCH_DIR}/${cfg.name}_mtk.wasm`));
    const mtkInst = new WebAssembly.Instance(mtkMod, {});
    const mtkMemInfo = setupMemory(mtkInst.exports.memory);

    const fusedMod = new WebAssembly.Module(fs.readFileSync(`${BENCH_DIR}/${cfg.name}_mtk_fused.wasm`));
    const fusedInst = new WebAssembly.Instance(fusedMod, {});
    const fusedMemInfo = setupMemory(fusedInst.exports.memory);

    const cShade = clangInst.exports[cfg.clangFn];
    const cBlend = clangInst.exports.ref_blend_batch;

    const mtkMq   = benchFn(mtkInst.exports.shade, mtkMemInfo, cfg.resetMask);
    const clangMq = benchFn(cShade, clangMemInfo, cfg.resetMask);
    const fusedMq = benchFn(fusedInst.exports.shade, fusedMemInfo, cfg.resetMask);
    const sepShadeBlendMq = benchFn((bp) => {
        cShade(bp);
        cBlend(bp, cfg.blendMode);
    }, clangMemInfo, cfg.resetMask);

    const ratio = mtkMq / clangMq;
    const fusedRatio = fusedMq / sepShadeBlendMq;

    console.log(
        `| \`${cfg.name}\` | ${st.ir_loop_before} -> ${st.ir_loop_after} | ${st.wasm_loop_ops} | ` +
        `**${mtkMq.toFixed(1)}** | ${clangMq.toFixed(1)} | **${ratio.toFixed(2)}x** | ` +
        `**${fusedMq.toFixed(1)}** | ${sepShadeBlendMq.toFixed(1)} | **${fusedRatio.toFixed(2)}x** |`
    );
}
