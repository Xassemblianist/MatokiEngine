/* tests/test_raster_wasm.mjs — Wasm SIMD rasterizer ile Host skaler rasterizer bit-bit eşdeğerlik testi.
 * Aynı sahneyi (doku örnekleme, BLEND_NONE / BLEND_ALPHA / BLEND_ADD, perspektif varying ve early-z
 * kesişen üçgenler dahil) hem wasm32 (-msimd128) hem native host (skaler yedek) olarak derler,
 * çalıştırır ve üretilen 256x256 RGBA8 framebuffer'ları byte-byte karşılaştırır.
 */
import fs from 'node:fs';
import os from 'node:os';
import path from 'node:path';
import { execFileSync } from 'node:child_process';

const tmpDir = fs.mkdtempSync(path.join(os.tmpdir(), 'matoki_raster_test_'));

const harnessCode = `
#include "core/types.h"
#include "core/arena.h"
#include "core/math.h"
#include "core/rng.h"
#include "plat/platform.h"
#include "raster/framebuffer.h"
#include "raster/texture.h"
#include "raster/ref_shaders.h"
#include "raster/raster.h"
#include "gfx2d/batch2d.h"

#define W 256
#define H 256

static u8 g_mem[MB(16)];
static Framebuffer g_fb;

static void render_test_scene(void) {
    Arena perm, frame;
    arena_init(&perm, g_mem, MB(4));
    arena_init(&frame, g_mem + MB(4), MB(12));

    fb_init(&g_fb, &perm, W, H);
    fb_init_depth(&g_fb, &perm);
    fb_clear(&g_fb, FB_RGBA(18, 22, 34, 255));
    fb_clear_depth(&g_fb, 1.0f);

    Texture tex0, tex1, tex2;
    tex_init(&tex0, &perm, 32, 32, 0);
    tex_gen_checker(&tex0, 4, FB_RGBA(255, 90, 60, 230), FB_RGBA(40, 200, 255, 230));

    tex_init(&tex1, &perm, 32, 32, TEX_FLAG_BILINEAR);
    tex_gen_gradient(&tex1,
                     FB_RGBA(255, 230, 50, 220),
                     FB_RGBA(255, 50, 180, 220),
                     FB_RGBA(50, 255, 160, 220),
                     FB_RGBA(70, 90, 255, 220));

    tex_init(&tex2, &perm, 32, 32, TEX_FLAG_CLAMP);
    tex_gen_noise(&tex2, 0x9E3779B97F4A7C15ULL);
    for (u32 y = 0; y < 32; y++) {
        f32 dy = (f32)y - 15.5f;
        for (u32 x = 0; x < 32; x++) {
            f32 dx = (f32)x - 15.5f;
            if (dx * dx + dy * dy > 15.0f * 15.0f) {
                tex2.px[y * 32 + x] &= 0x00FFFFFFu;
            }
        }
    }

    Rasterizer r;
    raster_begin(&r, &g_fb, &frame);

    /* 1) Early-Z kesişen 3D üçgenler + perspektif-doğru vcolor */
    raster_set_depth(&r, TRUE, TRUE, DEPTH_LESS);
    raster_set_shader(&r, REF_SHADE_VCOLOR_FN, NULL, NULL, 4, BLEND_NONE);
    {
        RasterVert a0 = {  20.25f,  20.75f, 0.2f, 1.0f, { 1.0f, 0.1f, 0.2f, 1.0f } };
        RasterVert a1 = { 180.50f,  35.25f, 0.8f, 0.5f, { 0.1f, 1.0f, 0.2f, 1.0f } };
        RasterVert a2 = { 100.75f, 170.50f, 0.5f, 0.8f, { 0.2f, 0.2f, 1.0f, 1.0f } };
        raster_triangle(&r, a0, a1, a2);

        RasterVert b0 = {  20.25f,  30.75f, 0.8f, 0.6f, { 1.0f, 0.9f, 0.1f, 1.0f } };
        RasterVert b1 = { 190.50f,  25.25f, 0.2f, 1.0f, { 0.9f, 0.1f, 1.0f, 1.0f } };
        RasterVert b2 = { 110.75f, 180.50f, 0.5f, 0.7f, { 0.1f, 1.0f, 0.9f, 1.0f } };
        raster_triangle(&r, b0, b1, b2);
    }
    raster_flush(&r);

    /* 2) 2D Sprite yığını: BLEND_NONE, BLEND_ALPHA ve BLEND_ADD */
    raster_set_depth(&r, FALSE, FALSE, DEPTH_ALWAYS);
    Batch2D b;
    b2d_begin(&b, &r, mat3_identity());

    Rng rng;
    rng_seed(&rng, 0x123456789ABCDEF0ULL);
    const Texture* texs[3] = { &tex0, &tex1, &tex2 };
    BlendMode modes[3] = { BLEND_NONE, BLEND_ALPHA, BLEND_ADD };

    for (u32 m = 0; m < 3; m++) {
        b2d_set_blend(&b, modes[m]);
        for (u32 t = 0; t < 3; t++) {
            for (u32 i = 0; i < 60; i++) {
                f32 x = 16.0f + rng_f32(&rng) * (f32)(W - 32);
                f32 y = 16.0f + rng_f32(&rng) * (f32)(H - 32);
                f32 sz = 5.0f + rng_f32(&rng) * 28.0f;
                f32 rot = rng_f32(&rng) * M_TAU;
                Vec4 tint = vec4(0.4f + 0.6f * rng_f32(&rng),
                                 0.4f + 0.6f * rng_f32(&rng),
                                 0.4f + 0.6f * rng_f32(&rng),
                                 0.35f + 0.65f * rng_f32(&rng));
                b2d_sprite(&b, texs[t], rect(0.0f, 0.0f, 1.0f, 1.0f),
                           vec2(x, y), vec2(sz, sz), rot, tint);
            }
        }
    }
    b2d_end(&b);
}

#if defined(__wasm32__)
EXPORT(run_scene) u32* run_scene(void) {
    render_test_scene();
    return g_fb.px;
}
#else
#include <stdio.h>
int main(int argc, char** argv) {
    if (argc < 2) return 1;
    render_test_scene();
    FILE* f = fopen(argv[1], "wb");
    if (!f) return 1;
    fwrite(g_fb.px, sizeof(u32), W * H, f);
    fclose(f);
    return 0;
}
#endif
`;

const cPath = path.join(tmpDir, 'scene_harness.c');
const wasmPath = path.join(tmpDir, 'scene.wasm');
const hostBin = path.join(tmpDir, 'scene_host');
const refOut = path.join(tmpDir, 'ref_fb.bin');

fs.writeFileSync(cPath, harnessCode);

const commonSrc = [
    'src/core/arena.c',
    'src/core/math.c',
    'src/core/rng.c',
    'src/core/log.c',
    'src/core/profiler.c',
    'src/thread/thread.c',
    'src/thread/job.c',
    'src/raster/framebuffer.c',
    'src/raster/texture.c',
    'src/raster/ref_shaders.c',
    'src/raster/raster.c',
    'src/gfx2d/batch2d.c',
];

try {
    /* 1) Host skaler derleme ve çalıştırma */
    execFileSync('clang', [
        '-std=c11', '-O2', '-Isrc',
        cPath, ...commonSrc, 'tools/host/plat_host.c',
        '-o', hostBin
    ], { stdio: 'inherit' });

    execFileSync(hostBin, [refOut], { stdio: 'inherit' });
    const refBytes = fs.readFileSync(refOut);

    /* 2) Wasm32 SIMD derleme ve çalıştırma */
    execFileSync('clang', [
        '--target=wasm32', '-std=c11', '-O2', '-ffreestanding', '-fno-builtin', '-nostdlib',
        '-msimd128', '-mbulk-memory', '-mnontrapping-fptoint', '-msign-ext', '-Isrc',
        '-Wl,--no-entry', '-Wl,--export-dynamic', '-Wl,--initial-memory=33554432',
        '-Wl,--stack-first', '-z', 'stack-size=1048576',
        cPath, ...commonSrc, 'src/core/mem.c', 'src/raster/raster_simd_wasm.c',
        '-o', wasmPath
    ], { stdio: 'inherit' });

    const wasmBytes = fs.readFileSync(wasmPath);
    const mod = new WebAssembly.Module(wasmBytes);
    const inst = new WebAssembly.Instance(mod, {
        env: {
            plat_log: () => {},
            plat_panic: () => { throw new Error('plat_panic'); },
            plat_time_ms: () => performance.now(),
            plat_jit_load: () => -1,
        }
    });

    const fbPtr = inst.exports.run_scene();
    const wasmBytesOut = new Uint8Array(inst.exports.memory.buffer, fbPtr, 256 * 256 * 4);

    let diffs = 0;
    let firstDiff = -1;
    const refView = new DataView(refBytes.buffer, refBytes.byteOffset, refBytes.byteLength);
    const wasmView = new DataView(wasmBytesOut.buffer, wasmBytesOut.byteOffset, wasmBytesOut.byteLength);

    for (let i = 0; i < 256 * 256; i++) {
        const rp = refView.getUint32(i * 4, true);
        const wp = wasmView.getUint32(i * 4, true);
        if (rp !== wp) {
            if (diffs === 0) firstDiff = i;
            diffs++;
        }
    }

    if (diffs !== 0) {
        const x = firstDiff % 256;
        const y = (firstDiff / 256) | 0;
        const rp = refView.getUint32(firstDiff * 4, true).toString(16).padStart(8, '0');
        const wp = wasmView.getUint32(firstDiff * 4, true).toString(16).padStart(8, '0');
        console.error(`FAIL: ${diffs} pixel mismatch! First at (${x},${y}): host=0x${rp} wasm=0x${wp}`);
        process.exit(1);
    }
} finally {
    fs.rmSync(tmpDir, { recursive: true, force: true });
}
