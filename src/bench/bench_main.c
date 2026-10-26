/* bench/bench_main.c — Elle yazılmış WAT çekirdeklerini çalışma anında masm + jit_load ile
 * derleyip C (clang -O2 -msimd128) referanslarıyla önce bit-bit doğrulayan, ardından
 * mikro-benchmark koşusunu gerçekleştirip sonuç tablosunu üreten ana modül.
 */
#include "bench/bench.h"
#include "bench/bench_kernels.h"
#include "bench/asm_sources.h"
#include "masm/masm.h"
#include "jit/jit.h"
#include "plat/platform.h"
#include "core/arena.h"
#include "core/rng.h"
#include "core/log.h"

#define BENCH_N_PIXELS  65536u
#define BENCH_N_SMALL   16384u
#define BENCH_N_QUADS   1024u
#define BENCH_TEX_DIM   64u
#define BENCH_MANDEL_W  512u

static u8         g_bench_arena_mem[8 * 1024 * 1024];
static BenchSuite g_bench_suite;

/* Benchmark çalışma tamponları (16-bayt hizalı) */
static _Alignas(16) u32 g_buf_dst_c[BENCH_N_PIXELS];
static _Alignas(16) u32 g_buf_dst_w[BENCH_N_PIXELS];
static _Alignas(16) u32 g_buf_src[BENCH_N_PIXELS];

static _Alignas(16) f32 g_buf_r[BENCH_N_SMALL];
static _Alignas(16) f32 g_buf_g[BENCH_N_SMALL];
static _Alignas(16) f32 g_buf_b[BENCH_N_SMALL];
static _Alignas(16) f32 g_buf_a[BENCH_N_SMALL];

static _Alignas(16) f32 g_buf_out_c[BENCH_N_QUADS * 16u];
static _Alignas(16) f32 g_buf_out_w[BENCH_N_QUADS * 16u];
static _Alignas(16) u32 g_tex_px[BENCH_TEX_DIM * BENCH_TEX_DIM];

static volatile u32 g_sink_u32 = 0;
static volatile f32 g_sink_f32 = 0.0f;

typedef struct {
    KernFillU32Fn        wat_fill_u32;
    KernBlendAlphaFn     wat_blend_alpha;
    KernRgbaF32ToU8Fn    wat_rgba_f32_to_u8;
    KernEdgeBlock8x8Fn   wat_edge_block_8x8;
    KernBilinearGatherFn wat_bilinear_gather;
    KernSumF32Fn         wat_sum_f32;
    KernDotF32Fn         wat_dot_f32;
    KernMandelbrotRowFn  wat_mandelbrot_row;

    KernFillU32Fn        c_fill_u32;
    KernBlendAlphaFn     c_blend_alpha;
    KernRgbaF32ToU8Fn    c_rgba_f32_to_u8;
    KernEdgeBlock8x8Fn   c_edge_block_8x8;
    KernBilinearGatherFn c_bilinear_gather;
    KernSumF32Fn         c_sum_f32;
    KernDotF32Fn         c_dot_f32;
    KernMandelbrotRowFn  c_mandelbrot_row;

    Texture              tex;
    i32                  edge_coeffs[9];
    u8                   masks_c[16];
    u8                   masks_w[16];
} BenchKernelsCtx;

static BenchKernelsCtx g_kctx;

static u32 float_bits(f32 f) {
    union { f32 f; u32 u; } conv;
    conv.f = f;
    return conv.u;
}

static void* compile_and_load_wat(Arena* arena, const char* export_name) {
    const EmbeddedAsm* emb = bench_find_asm(export_name);
    if (!emb) {
        LOG("[bench] HATA: gömülü WAT bulunamadı: %s", export_name);
        return NULL;
    }
    MasmResult mr = masm_assemble(arena, emb->src, emb->len);
    if (!mr.ok) {
        LOG("[bench] HATA: masm_assemble(%s) başarısız (%u:%u): %s",
            emb->name, mr.line, mr.col, mr.msg);
        return NULL;
    }
    void* fn = jit_load(mr.wasm, mr.len, emb->export_name);
    if (!fn) {
        LOG("[bench] HATA: jit_load(%s) başarısız", emb->export_name);
        return NULL;
    }
    return fn;
}

static b32 init_kernels_and_data(void) {
    Arena arena;
    arena_init(&arena, g_bench_arena_mem, sizeof(g_bench_arena_mem));

    g_kctx.c_fill_u32        = bench_c_fill_u32;
    g_kctx.c_blend_alpha     = bench_c_blend_alpha_rgba8;
    g_kctx.c_rgba_f32_to_u8  = bench_c_rgba_f32_to_u8;
    g_kctx.c_edge_block_8x8  = bench_c_edge_block_8x8;
    g_kctx.c_bilinear_gather = bench_c_bilinear_gather;
    g_kctx.c_sum_f32         = bench_c_sum_f32;
    g_kctx.c_dot_f32         = bench_c_dot_f32;
    g_kctx.c_mandelbrot_row  = bench_c_mandelbrot_row;

    g_kctx.wat_fill_u32        = (KernFillU32Fn)compile_and_load_wat(&arena, "fill_u32");
    g_kctx.wat_blend_alpha     = (KernBlendAlphaFn)compile_and_load_wat(&arena, "blend_alpha_rgba8");
    g_kctx.wat_rgba_f32_to_u8  = (KernRgbaF32ToU8Fn)compile_and_load_wat(&arena, "rgba_f32_to_u8");
    g_kctx.wat_edge_block_8x8  = (KernEdgeBlock8x8Fn)compile_and_load_wat(&arena, "edge_block_8x8");
    g_kctx.wat_bilinear_gather = (KernBilinearGatherFn)compile_and_load_wat(&arena, "bilinear_gather");
    g_kctx.wat_sum_f32         = (KernSumF32Fn)compile_and_load_wat(&arena, "sum_f32");
    g_kctx.wat_dot_f32         = (KernDotF32Fn)compile_and_load_wat(&arena, "dot_f32");
    g_kctx.wat_mandelbrot_row  = (KernMandelbrotRowFn)compile_and_load_wat(&arena, "mandelbrot_row");

    if (!g_kctx.wat_fill_u32 || !g_kctx.wat_blend_alpha || !g_kctx.wat_rgba_f32_to_u8 ||
        !g_kctx.wat_edge_block_8x8 || !g_kctx.wat_bilinear_gather || !g_kctx.wat_sum_f32 ||
        !g_kctx.wat_dot_f32 || !g_kctx.wat_mandelbrot_row) {
        return FALSE;
    }

    Rng rng;
    rng_seed(&rng, 0xB34C55AA12345678ULL);

    for (u32 i = 0; i < BENCH_N_PIXELS; i++) {
        g_buf_src[i]   = rng_u32(&rng);
        g_buf_dst_c[i] = rng_u32(&rng);
        g_buf_dst_w[i] = g_buf_dst_c[i];
    }

    /* sum_f32 ve dot_f32 için tam sayı / ikili kesir olmayan rastgele float'lar + rgba_f32_to_u8 için [0..1] ve sınır dışı değerler */
    for (u32 i = 0; i < BENCH_N_SMALL; i++) {
        g_buf_r[i] = rng_f32(&rng) * 2.7182818f - 1.3591409f;
        g_buf_g[i] = rng_f32(&rng) * 3.1415927f - 1.5707963f;
        g_buf_b[i] = rng_f32(&rng) * 1.4f - 0.2f;
        g_buf_a[i] = rng_f32(&rng);
    }

    for (u32 i = 0; i < BENCH_TEX_DIM * BENCH_TEX_DIM; i++) {
        g_tex_px[i] = rng_u32(&rng);
    }
    g_kctx.tex.px     = g_tex_px;
    g_kctx.tex.w      = BENCH_TEX_DIM;
    g_kctx.tex.h      = BENCH_TEX_DIM;
    g_kctx.tex.w_mask = BENCH_TEX_DIM - 1u;
    g_kctx.tex.h_mask = BENCH_TEX_DIM - 1u;
    g_kctx.tex.log2_w = 6u;
    g_kctx.tex.flags  = TEX_FLAG_BILINEAR;
    g_kctx.tex.levels = 1u;

    /* 3 kenar fonksiyonu katsayıları (8x8 bloğu kısmen kesen üçgen) */
    g_kctx.edge_coeffs[0] =  12; g_kctx.edge_coeffs[1] = -7;  g_kctx.edge_coeffs[2] = -140;
    g_kctx.edge_coeffs[3] = -9;  g_kctx.edge_coeffs[4] =  15; g_kctx.edge_coeffs[5] =  110;
    g_kctx.edge_coeffs[6] = -3;  g_kctx.edge_coeffs[7] = -8;  g_kctx.edge_coeffs[8] =  260;

    return TRUE;
}

static b32 verify_fill_u32(u32 val) {
    static const u32 counts[] = { 1u, 3u, 4u, 7u, 16u, 19u, 64u, 259u, 4096u };
    for (u32 c = 0; c < ARRAY_LEN(counts); c++) {
        u32 n = counts[c];
        for (u32 i = 0; i < n + 8u; i++) {
            g_buf_dst_c[i] = 0xDEADBEEFu ^ i;
            g_buf_dst_w[i] = 0xDEADBEEFu ^ i;
        }
        g_kctx.c_fill_u32(g_buf_dst_c, val, n);
        g_kctx.wat_fill_u32(g_buf_dst_w, val, n);
        for (u32 i = 0; i < n + 8u; i++) {
            if (g_buf_dst_c[i] != g_buf_dst_w[i]) {
                LOG("[bench] FAIL fill_u32(val=0x%x, n=%u) i=%u: C=0x%x WAT=0x%x",
                    val, n, i, g_buf_dst_c[i], g_buf_dst_w[i]);
                return FALSE;
            }
        }
    }
    return TRUE;
}

static b32 verify_blend_alpha(void) {
    static const u32 counts[] = { 1u, 3u, 4u, 7u, 19u, 259u, 4096u };
    Rng rng;
    rng_seed(&rng, 0x9988776655443322ULL);
    for (u32 c = 0; c < ARRAY_LEN(counts); c++) {
        u32 n = counts[c];
        for (u32 i = 0; i < n + 4u; i++) {
            u32 d = rng_u32(&rng);
            g_buf_dst_c[i] = d;
            g_buf_dst_w[i] = d;
        }
        g_kctx.c_blend_alpha(g_buf_dst_c, g_buf_src, n);
        g_kctx.wat_blend_alpha(g_buf_dst_w, g_buf_src, n);
        for (u32 i = 0; i < n + 4u; i++) {
            if (g_buf_dst_c[i] != g_buf_dst_w[i]) {
                LOG("[bench] FAIL blend_alpha_rgba8(n=%u) i=%u: C=0x%x WAT=0x%x",
                    n, i, g_buf_dst_c[i], g_buf_dst_w[i]);
                return FALSE;
            }
        }
    }
    return TRUE;
}

static b32 verify_rgba_f32_to_u8(void) {
    static const u32 counts[] = { 1u, 3u, 4u, 8u, 11u, 19u, 259u, 4096u };
    for (u32 c = 0; c < ARRAY_LEN(counts); c++) {
        u32 n = counts[c];
        for (u32 i = 0; i < n + 4u; i++) {
            g_buf_dst_c[i] = 0x13579BDFu;
            g_buf_dst_w[i] = 0x13579BDFu;
        }
        g_kctx.c_rgba_f32_to_u8(g_buf_dst_c, g_buf_r, g_buf_g, g_buf_b, g_buf_a, n);
        g_kctx.wat_rgba_f32_to_u8(g_buf_dst_w, g_buf_r, g_buf_g, g_buf_b, g_buf_a, n);
        for (u32 i = 0; i < n + 4u; i++) {
            if (g_buf_dst_c[i] != g_buf_dst_w[i]) {
                LOG("[bench] FAIL rgba_f32_to_u8(n=%u) i=%u: C=0x%x WAT=0x%x",
                    n, i, g_buf_dst_c[i], g_buf_dst_w[i]);
                return FALSE;
            }
        }
    }
    return TRUE;
}

static b32 verify_edge_block_8x8(void) {
    for (i32 by = 0; by < 32; by += 4) {
        for (i32 bx = 0; bx < 32; bx += 4) {
            for (u32 i = 0; i < 16u; i++) {
                g_kctx.masks_c[i] = 0xAAu;
                g_kctx.masks_w[i] = 0x55u;
            }
            g_kctx.c_edge_block_8x8(g_kctx.edge_coeffs, bx, by, g_kctx.masks_c);
            g_kctx.wat_edge_block_8x8(g_kctx.edge_coeffs, bx, by, g_kctx.masks_w);
            for (u32 i = 0; i < 16u; i++) {
                if (g_kctx.masks_c[i] != g_kctx.masks_w[i]) {
                    LOG("[bench] FAIL edge_block_8x8(bx=%d, by=%d) q=%u: C=0x%x WAT=0x%x",
                        bx, by, i, (u32)g_kctx.masks_c[i], (u32)g_kctx.masks_w[i]);
                    return FALSE;
                }
            }
        }
    }
    return TRUE;
}

static b32 verify_bilinear_gather(void) {
    static const u32 qcounts[] = { 1u, 3u, 17u, 256u, BENCH_N_QUADS };
    for (u32 c = 0; c < ARRAY_LEN(qcounts); c++) {
        u32 qc = qcounts[c];
        u32 total = qc * 16u;
        for (u32 i = 0; i < total; i++) {
            g_buf_out_c[i] = -1.0f;
            g_buf_out_w[i] = -2.0f;
        }
        g_kctx.c_bilinear_gather(&g_kctx.tex, g_buf_b, g_buf_a, g_buf_out_c, qc);
        g_kctx.wat_bilinear_gather(&g_kctx.tex, g_buf_b, g_buf_a, g_buf_out_w, qc);
        for (u32 i = 0; i < total; i++) {
            u32 bc = float_bits(g_buf_out_c[i]);
            u32 bw = float_bits(g_buf_out_w[i]);
            if (bc != bw) {
                LOG("[bench] FAIL bilinear_gather(qc=%u) i=%u: C=0x%x WAT=0x%x",
                    qc, i, bc, bw);
                return FALSE;
            }
        }
    }
    return TRUE;
}

static b32 verify_sum_and_dot_f32(b32* out_sum_ok, b32* out_dot_ok) {
    static const u32 counts[] = { 1u, 3u, 4u, 7u, 16u, 19u, 67u, 1024u, BENCH_N_SMALL };
    *out_sum_ok = TRUE;
    *out_dot_ok = TRUE;
    for (u32 c = 0; c < ARRAY_LEN(counts); c++) {
        u32 n = counts[c];
        f32 sc = g_kctx.c_sum_f32(g_buf_r, n);
        f32 sw = g_kctx.wat_sum_f32(g_buf_r, n);
        if (float_bits(sc) != float_bits(sw)) {
            LOG("[bench] FAIL sum_f32(n=%u): C=0x%x WAT=0x%x", n, float_bits(sc), float_bits(sw));
            *out_sum_ok = FALSE;
        }
        f32 dc = g_kctx.c_dot_f32(g_buf_r, g_buf_g, n);
        f32 dw = g_kctx.wat_dot_f32(g_buf_r, g_buf_g, n);
        if (float_bits(dc) != float_bits(dw)) {
            LOG("[bench] FAIL dot_f32(n=%u): C=0x%x WAT=0x%x", n, float_bits(dc), float_bits(dw));
            *out_dot_ok = FALSE;
        }
    }
    return (*out_sum_ok) && (*out_dot_ok);
}

static b32 verify_mandelbrot_row(void) {
    static const u32 widths[] = { 1u, 3u, 4u, 7u, 19u, BENCH_MANDEL_W };
    for (u32 c = 0; c < ARRAY_LEN(widths); c++) {
        u32 w = widths[c];
        for (u32 i = 0; i < w + 4u; i++) {
            g_buf_dst_c[i] = 0xCAFEBABEu;
            g_buf_dst_w[i] = 0xCAFEBABEu;
        }
        g_kctx.c_mandelbrot_row(g_buf_dst_c, w, -2.0f, 0.15f, 3.0f / (f32)BENCH_MANDEL_W, 128u);
        g_kctx.wat_mandelbrot_row(g_buf_dst_w, w, -2.0f, 0.15f, 3.0f / (f32)BENCH_MANDEL_W, 128u);
        for (u32 i = 0; i < w + 4u; i++) {
            if (g_buf_dst_c[i] != g_buf_dst_w[i]) {
                LOG("[bench] FAIL mandelbrot_row(w=%u) i=%u: C=%u WAT=%u",
                    w, i, g_buf_dst_c[i], g_buf_dst_w[i]);
                return FALSE;
            }
        }
    }
    return TRUE;
}

/* --- Benchmark sarmalayıcıları (iters döngüsü) --- */

static void run_c_fill_rgba(void* ctx, u32 iters) {
    BenchKernelsCtx* k = (BenchKernelsCtx*)ctx;
    for (u32 i = 0; i < iters; i++) {
        k->c_fill_u32(g_buf_dst_c, 0xFF336699u ^ (i & 1u), BENCH_N_PIXELS);
    }
    g_sink_u32 = g_buf_dst_c[BENCH_N_PIXELS - 1u];
}

static void run_wat_fill_rgba(void* ctx, u32 iters) {
    BenchKernelsCtx* k = (BenchKernelsCtx*)ctx;
    for (u32 i = 0; i < iters; i++) {
        k->wat_fill_u32(g_buf_dst_w, 0xFF336699u ^ (i & 1u), BENCH_N_PIXELS);
    }
    g_sink_u32 = g_buf_dst_w[BENCH_N_PIXELS - 1u];
}

static void run_c_fill_memfill(void* ctx, u32 iters) {
    BenchKernelsCtx* k = (BenchKernelsCtx*)ctx;
    for (u32 i = 0; i < iters; i++) {
        u32 v = (i & 1u) ? 0xFFFFFFFFu : 0x00000000u;
        k->c_fill_u32(g_buf_dst_c, v, BENCH_N_PIXELS);
    }
    g_sink_u32 = g_buf_dst_c[BENCH_N_PIXELS - 1u];
}

static void run_wat_fill_memfill(void* ctx, u32 iters) {
    BenchKernelsCtx* k = (BenchKernelsCtx*)ctx;
    for (u32 i = 0; i < iters; i++) {
        u32 v = (i & 1u) ? 0xFFFFFFFFu : 0x00000000u;
        k->wat_fill_u32(g_buf_dst_w, v, BENCH_N_PIXELS);
    }
    g_sink_u32 = g_buf_dst_w[BENCH_N_PIXELS - 1u];
}

static void run_c_blend_alpha(void* ctx, u32 iters) {
    BenchKernelsCtx* k = (BenchKernelsCtx*)ctx;
    for (u32 i = 0; i < iters; i++) {
        k->c_blend_alpha(g_buf_dst_c, g_buf_src, BENCH_N_SMALL);
    }
    g_sink_u32 = g_buf_dst_c[0];
}

static void run_wat_blend_alpha(void* ctx, u32 iters) {
    BenchKernelsCtx* k = (BenchKernelsCtx*)ctx;
    for (u32 i = 0; i < iters; i++) {
        k->wat_blend_alpha(g_buf_dst_w, g_buf_src, BENCH_N_SMALL);
    }
    g_sink_u32 = g_buf_dst_w[0];
}

static void run_c_rgba_f32_to_u8(void* ctx, u32 iters) {
    BenchKernelsCtx* k = (BenchKernelsCtx*)ctx;
    for (u32 i = 0; i < iters; i++) {
        k->c_rgba_f32_to_u8(g_buf_dst_c, g_buf_r, g_buf_g, g_buf_b, g_buf_a, BENCH_N_SMALL);
    }
    g_sink_u32 = g_buf_dst_c[0];
}

static void run_wat_rgba_f32_to_u8(void* ctx, u32 iters) {
    BenchKernelsCtx* k = (BenchKernelsCtx*)ctx;
    for (u32 i = 0; i < iters; i++) {
        k->wat_rgba_f32_to_u8(g_buf_dst_w, g_buf_r, g_buf_g, g_buf_b, g_buf_a, BENCH_N_SMALL);
    }
    g_sink_u32 = g_buf_dst_w[0];
}

static void run_c_edge_block(void* ctx, u32 iters) {
    BenchKernelsCtx* k = (BenchKernelsCtx*)ctx;
    for (u32 i = 0; i < iters; i++) {
        k->c_edge_block_8x8(k->edge_coeffs, (i32)(i & 15u), 8, k->masks_c);
    }
    g_sink_u32 = k->masks_c[0];
}

static void run_wat_edge_block(void* ctx, u32 iters) {
    BenchKernelsCtx* k = (BenchKernelsCtx*)ctx;
    for (u32 i = 0; i < iters; i++) {
        k->wat_edge_block_8x8(k->edge_coeffs, (i32)(i & 15u), 8, k->masks_w);
    }
    g_sink_u32 = k->masks_w[0];
}

static void run_c_bilinear_gather(void* ctx, u32 iters) {
    BenchKernelsCtx* k = (BenchKernelsCtx*)ctx;
    for (u32 i = 0; i < iters; i++) {
        k->c_bilinear_gather(&k->tex, g_buf_b, g_buf_a, g_buf_out_c, BENCH_N_QUADS);
    }
    g_sink_f32 = g_buf_out_c[0];
}

static void run_wat_bilinear_gather(void* ctx, u32 iters) {
    BenchKernelsCtx* k = (BenchKernelsCtx*)ctx;
    for (u32 i = 0; i < iters; i++) {
        k->wat_bilinear_gather(&k->tex, g_buf_b, g_buf_a, g_buf_out_w, BENCH_N_QUADS);
    }
    g_sink_f32 = g_buf_out_w[0];
}

static void run_c_sum_f32(void* ctx, u32 iters) {
    BenchKernelsCtx* k = (BenchKernelsCtx*)ctx;
    f32 acc = 0.0f;
    for (u32 i = 0; i < iters; i++) {
        acc += k->c_sum_f32(g_buf_r, BENCH_N_SMALL);
    }
    g_sink_f32 = acc;
}

static void run_wat_sum_f32(void* ctx, u32 iters) {
    BenchKernelsCtx* k = (BenchKernelsCtx*)ctx;
    f32 acc = 0.0f;
    for (u32 i = 0; i < iters; i++) {
        acc += k->wat_sum_f32(g_buf_r, BENCH_N_SMALL);
    }
    g_sink_f32 = acc;
}

static void run_c_dot_f32(void* ctx, u32 iters) {
    BenchKernelsCtx* k = (BenchKernelsCtx*)ctx;
    f32 acc = 0.0f;
    for (u32 i = 0; i < iters; i++) {
        acc += k->c_dot_f32(g_buf_r, g_buf_g, BENCH_N_SMALL);
    }
    g_sink_f32 = acc;
}

static void run_wat_dot_f32(void* ctx, u32 iters) {
    BenchKernelsCtx* k = (BenchKernelsCtx*)ctx;
    f32 acc = 0.0f;
    for (u32 i = 0; i < iters; i++) {
        acc += k->wat_dot_f32(g_buf_r, g_buf_g, BENCH_N_SMALL);
    }
    g_sink_f32 = acc;
}

static void run_c_mandelbrot_row(void* ctx, u32 iters) {
    BenchKernelsCtx* k = (BenchKernelsCtx*)ctx;
    for (u32 i = 0; i < iters; i++) {
        k->c_mandelbrot_row(g_buf_dst_c, BENCH_MANDEL_W, -2.0f, 0.15f, 3.0f / (f32)BENCH_MANDEL_W, 128u);
    }
    g_sink_u32 = g_buf_dst_c[BENCH_MANDEL_W / 2u];
}

static void run_wat_mandelbrot_row(void* ctx, u32 iters) {
    BenchKernelsCtx* k = (BenchKernelsCtx*)ctx;
    for (u32 i = 0; i < iters; i++) {
        k->wat_mandelbrot_row(g_buf_dst_w, BENCH_MANDEL_W, -2.0f, 0.15f, 3.0f / (f32)BENCH_MANDEL_W, 128u);
    }
    g_sink_u32 = g_buf_dst_w[BENCH_MANDEL_W / 2u];
}

EXPORT(engine_bench_verify_only) i32 engine_bench_verify_only(void) {
    if (!init_kernels_and_data()) return 1;

    b32 ok_fill_rgba    = verify_fill_u32(0xFF336699u);
    b32 ok_fill_memfill = verify_fill_u32(0x00000000u) && verify_fill_u32(0xFFFFFFFFu);
    b32 ok_blend        = verify_blend_alpha();
    b32 ok_rgba         = verify_rgba_f32_to_u8();
    b32 ok_edge         = verify_edge_block_8x8();
    b32 ok_bilerp       = verify_bilinear_gather();
    b32 ok_sum = FALSE, ok_dot = FALSE;
    verify_sum_and_dot_f32(&ok_sum, &ok_dot);
    b32 ok_mandel       = verify_mandelbrot_row();

    b32 all_ok = ok_fill_rgba && ok_fill_memfill && ok_blend && ok_rgba &&
                 ok_edge && ok_bilerp && ok_sum && ok_dot && ok_mandel;
    if (!all_ok) {
        LOG("[bench] HATA: bit-bit doğrulama başarısız!");
        return 2;
    }
    LOG("[bench] Tüm 8 WAT çekirdeği C referansıyla bit-bit EŞİT doğrulandı.");
    return 0;
}

EXPORT(engine_bench_run) i32 engine_bench_run(void) {
    bench_suite_init(&g_bench_suite);

    if (!init_kernels_and_data()) {
        return 1;
    }

    b32 ok_fill_rgba    = verify_fill_u32(0xFF336699u);
    b32 ok_fill_memfill = verify_fill_u32(0x00000000u) && verify_fill_u32(0xFFFFFFFFu);
    b32 ok_blend        = verify_blend_alpha();
    b32 ok_rgba         = verify_rgba_f32_to_u8();
    b32 ok_edge         = verify_edge_block_8x8();
    b32 ok_bilerp       = verify_bilinear_gather();
    b32 ok_sum = FALSE, ok_dot = FALSE;
    verify_sum_and_dot_f32(&ok_sum, &ok_dot);
    b32 ok_mandel       = verify_mandelbrot_row();

    const u32 runs = 15u;

    bench_suite_add(&g_bench_suite, "fill_u32 (rgba)",    ok_fill_rgba,
                    run_c_fill_rgba,    run_wat_fill_rgba,    &g_kctx, 100u,   400u, runs);
    bench_suite_add(&g_bench_suite, "fill_u32 (memfill)", ok_fill_memfill,
                    run_c_fill_memfill, run_wat_fill_memfill, &g_kctx, 100u,   400u, runs);
    bench_suite_add(&g_bench_suite, "blend_alpha_rgba8",  ok_blend,
                    run_c_blend_alpha,  run_wat_blend_alpha,  &g_kctx, 100u,   300u, runs);
    bench_suite_add(&g_bench_suite, "rgba_f32_to_u8",     ok_rgba,
                    run_c_rgba_f32_to_u8, run_wat_rgba_f32_to_u8, &g_kctx, 100u, 300u, runs);
    bench_suite_add(&g_bench_suite, "edge_block_8x8",     ok_edge,
                    run_c_edge_block,   run_wat_edge_block,   &g_kctx, 10000u, 50000u, runs);
    bench_suite_add(&g_bench_suite, "bilinear_gather",    ok_bilerp,
                    run_c_bilinear_gather, run_wat_bilinear_gather, &g_kctx, 50u, 200u, runs);
    bench_suite_add(&g_bench_suite, "sum_f32",            ok_sum,
                    run_c_sum_f32,      run_wat_sum_f32,      &g_kctx, 100u,   400u, runs);
    bench_suite_add(&g_bench_suite, "dot_f32",            ok_dot,
                    run_c_dot_f32,      run_wat_dot_f32,      &g_kctx, 100u,   400u, runs);
    bench_suite_add(&g_bench_suite, "mandelbrot_row",     ok_mandel,
                    run_c_mandelbrot_row, run_wat_mandelbrot_row, &g_kctx, 10u, 40u, runs);

    bench_suite_format_and_log(&g_bench_suite);

    return g_bench_suite.all_bit_exact ? 0 : 2;
}

EXPORT(engine_bench_report_ptr) const char* engine_bench_report_ptr(void) {
    return g_bench_suite.report;
}

EXPORT(engine_bench_report_len) u32 engine_bench_report_len(void) {
    return g_bench_suite.report_len;
}

EXPORT(engine_bench_count) u32 engine_bench_count(void) {
    return g_bench_suite.count;
}
