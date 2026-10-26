/* thread/thread_selftest.c — Çoklu thread, iş kuyruğu ve JIT çoğaltma öz-testi.
 * 1) Tüm thread'lerden toplam 1.000.000 atomik artırma (doğruluk).
 * 2) 1024x1024 Mandelbrot kümesi: tek thread vs job_parallel_for bit-bit eşleşme + hızlanma.
 * 3) JIT ile çalışma anında derlenen fonksiyonun worker'lar tarafından paralel çağrılması.
 */
#include "thread/atomic.h"
#include "thread/thread.h"
#include "thread/job.h"
#include "jit/jit.h"
#include "masm/masm.h"
#include "bench/asm_sources.h"
#include "bench/bench_kernels.h"
#include "mbin/mbin.h"
#include "mbin/mbin_ops.h"
#include "plat/platform.h"
#include "core/arena.h"
#include "core/log.h"

#define MANDEL_W        1024u
#define MANDEL_H        1024u
#define MANDEL_MAX_ITER 256u
#define ATOMIC_TOTAL    1000000u
#define ATOMIC_CHUNKS   1000u
#define JIT_TEST_COUNT  65536u

static _Alignas(16) u32 g_mandel_ref[MANDEL_W * MANDEL_H];
static _Alignas(16) u32 g_mandel_par[MANDEL_W * MANDEL_H];
static _Alignas(16) u32 g_jit_par_out[JIT_TEST_COUNT];
static _Alignas(16) u32 g_thread_participation[THREAD_MAX_COUNT];
static _Alignas(16) u8  g_mandel_masm_mem[1048576];

static f64                 g_last_seq_ms         = 0.0;
static f64                 g_last_par_ms         = 0.0;
static b32                 g_mandel_ref_computed = FALSE;
static b32                 g_mandel_use_wat      = TRUE;
static KernMandelbrotRowFn g_wat_mandel_row      = NULL;

static void ensure_mandel_wat_loaded(void) {
    if (g_wat_mandel_row != NULL) return;
    const EmbeddedAsm* emb = bench_find_asm("mandelbrot_row");
    if (!emb) return;
    Arena arena;
    arena_init(&arena, g_mandel_masm_mem, sizeof(g_mandel_masm_mem));
    MasmResult mr = masm_assemble(&arena, emb->src, emb->len);
    if (!mr.ok) return;
    g_wat_mandel_row = (KernMandelbrotRowFn)jit_load(mr.wasm, mr.len, emb->export_name);
}

EXPORT(engine_mandelbrot_set_use_wat) void engine_mandelbrot_set_use_wat(b32 use_wat) {
    if (use_wat) {
        ensure_mandel_wat_loaded();
    }
    g_mandel_use_wat = use_wat ? TRUE : FALSE;
}

/* ============================================================================
 * 1. Test: 1,000,000 Atomik Artırma
 * ============================================================================ */

typedef struct {
    _Alignas(4) u32 counter;
    u32             iters_per_chunk;
} AtomicTestCtx;

static void atomic_inc_range(u32 start, u32 end, void* arg) {
    AtomicTestCtx* ctx = (AtomicTestCtx*)arg;
    u32 tid = thread_id();
    if (tid < THREAD_MAX_COUNT) {
        atomic_add_u32(&g_thread_participation[tid], end - start);
    }
    for (u32 c = start; c < end; c++) {
        for (u32 i = 0; i < ctx->iters_per_chunk; i++) {
            atomic_add_u32(&ctx->counter, 1u);
        }
    }
}

static b32 selftest_atomic_1m(void) {
    for (u32 t = 0; t < THREAD_MAX_COUNT; t++) {
        atomic_store_relaxed_u32(&g_thread_participation[t], 0u);
    }

    AtomicTestCtx ctx;
    atomic_store_relaxed_u32(&ctx.counter, 0u);
    ctx.iters_per_chunk = ATOMIC_TOTAL / ATOMIC_CHUNKS;

    job_parallel_for(ATOMIC_CHUNKS, 4u, atomic_inc_range, &ctx);

    u32 val = atomic_load_u32(&ctx.counter);
    if (val != ATOMIC_TOTAL) {
        LOG("[thread_selftest] FAIL: atomic counter = %u (expected %u)", val, ATOMIC_TOTAL);
        return FALSE;
    }

    u32 active_threads = 0;
    for (u32 t = 0; t < THREAD_MAX_COUNT; t++) {
        if (atomic_load_relaxed_u32(&g_thread_participation[t]) > 0u) {
            active_threads++;
        }
    }
    LOG("[thread_selftest] 1) Atomik 1M artirma OK (deger=%u, katilan_thread=%u/%u)",
        val, active_threads, thread_count());
    return TRUE;
}

/* ============================================================================
 * 2. Test: 1024x1024 Mandelbrot (Tek Thread vs job_parallel_for, C + WAT)
 * ============================================================================ */

static void mandel_compute_rows(u32 y_start, u32 y_end, u32* dst) {
    const f32 x_min = -2.0f;
    const f32 x_max =  0.7f;
    const f32 y_min = -1.35f;
    const f32 y_max =  1.35f;
    const f32 dx    = (x_max - x_min) / (f32)MANDEL_W;
    const f32 dy    = (y_max - y_min) / (f32)MANDEL_H;

    KernMandelbrotRowFn wat_fn = g_mandel_use_wat ? g_wat_mandel_row : NULL;
    _Alignas(16) u32 iter_row[MANDEL_W];

    for (u32 y = y_start; y < y_end; y++) {
        f32 ci = y_min + (f32)y * dy;
        u32* row = dst + y * MANDEL_W;

        if (wat_fn != NULL) {
            wat_fn(iter_row, MANDEL_W, x_min, ci, dx, MANDEL_MAX_ITER);
            for (u32 x = 0; x < MANDEL_W; x++) {
                u32 iter = iter_row[x];
                if (iter == MANDEL_MAX_ITER) {
                    row[x] = 0xFF0A0C12u;
                } else {
                    u32 r = (iter * 9u) & 0xFFu;
                    u32 g = (iter * 17u) & 0xFFu;
                    u32 b = (iter * 29u + 40u) & 0xFFu;
                    row[x] = 0xFF000000u | (b << 16) | (g << 8) | r;
                }
            }
        } else {
            for (u32 x = 0; x < MANDEL_W; x++) {
                f32 cr = x_min + (f32)x * dx;
                f32 zr = 0.0f;
                f32 zi = 0.0f;
                u32 iter = 0;
                while (iter < MANDEL_MAX_ITER) {
                    f32 zr2 = zr * zr;
                    f32 zi2 = zi * zi;
                    if (zr2 + zi2 > 4.0f) {
                        break;
                    }
                    zi = (zr + zr) * zi + ci;
                    zr = zr2 - zi2 + cr;
                    iter++;
                }
                if (iter == MANDEL_MAX_ITER) {
                    row[x] = 0xFF0A0C12u;
                } else {
                    u32 r = (iter * 9u) & 0xFFu;
                    u32 g = (iter * 17u) & 0xFFu;
                    u32 b = (iter * 29u + 40u) & 0xFFu;
                    row[x] = 0xFF000000u | (b << 16) | (g << 8) | r;
                }
            }
        }
    }
}

static void mandel_par_task(u32 start, u32 end, void* ctx) {
    u32* dst = (u32*)ctx;
    mandel_compute_rows(start, end, dst);
}

EXPORT(engine_bench_mandelbrot_seq) f64 engine_bench_mandelbrot_seq(void) {
    if (g_mandel_use_wat) {
        ensure_mandel_wat_loaded();
    }
    f64 t0 = plat_time_ms();
    mandel_compute_rows(0u, MANDEL_H, g_mandel_ref);
    f64 t1 = plat_time_ms();
    g_last_seq_ms = t1 - t0;
    g_mandel_ref_computed = TRUE;
    return g_last_seq_ms;
}

EXPORT(engine_bench_mandelbrot_par) f64 engine_bench_mandelbrot_par(u32 grain) {
    if (grain == 0) grain = 4u;
    if (g_mandel_use_wat) {
        ensure_mandel_wat_loaded();
    }
    f64 t0 = plat_time_ms();
    job_parallel_for(MANDEL_H, grain, mandel_par_task, g_mandel_par);
    f64 t1 = plat_time_ms();
    g_last_par_ms = t1 - t0;
    return g_last_par_ms;
}

EXPORT(engine_mandelbrot_framebuffer) u32* engine_mandelbrot_framebuffer(void) {
    return g_mandel_par;
}

static b32 selftest_mandelbrot(void) {
    /* Önce skaler C referansını 1 thread'de çalıştır */
    g_mandel_use_wat = FALSE;
    f64 seq_c_ms = engine_bench_mandelbrot_seq();

    /* Elle WAT çekirdeğini yükle ve 1 thread + N thread ile C referansına karşı bit-bit doğrula */
    ensure_mandel_wat_loaded();
    g_mandel_use_wat = (g_wat_mandel_row != NULL);

    for (u32 i = 0; i < MANDEL_W * MANDEL_H; i++) {
        g_mandel_par[i] = 0u;
    }

    f64 par_ms = engine_bench_mandelbrot_par(4u);

    /* Bit-bit karşılaştırma (1T C referansı vs NT WAT paralel çıktı) */
    for (u32 i = 0; i < MANDEL_W * MANDEL_H; i++) {
        if (g_mandel_ref[i] != g_mandel_par[i]) {
            LOG("[thread_selftest] FAIL: Mandelbrot mismatch at pixel %u (ref=0x%08x, par=0x%08x)",
                i, g_mandel_ref[i], g_mandel_par[i]);
            return FALSE;
        }
    }

    f64 seq_wat_ms = engine_bench_mandelbrot_seq();
    f64 speedup = (par_ms > 0.001) ? (seq_wat_ms / par_ms) : 1.0;
    LOG("[thread_selftest] 2) Mandelbrot 1024x1024 bit-bit AYNI (1T C=%.2f ms, 1T WAT=%.2f ms, %uT WAT=%.2f ms, hizlanma=%.2fx)",
        seq_c_ms, seq_wat_ms, thread_count(), par_ms, speedup);
    return TRUE;
}

/* ============================================================================
 * 3. Test: JIT Fonksiyonunu Worker Thread'lerden Paralel Çağırma
 * ============================================================================ */

typedef u32 (*JitHashFn)(u32 x);

typedef struct {
    JitHashFn       fn;
    u32*            out;
    u32*            worker_hits;
    _Alignas(4) u32 next_chunk;
    u32             chunk_size;
    u32             total_count;
} JitParCtx;

static void jit_worker_only_task(void* arg) {
    JitParCtx* ctx = (JitParCtx*)arg;
    u32 tid = thread_id();
    for (;;) {
        u32 start = atomic_add_u32(&ctx->next_chunk, ctx->chunk_size);
        if (start >= ctx->total_count) {
            break;
        }
        u32 end = start + ctx->chunk_size;
        if (end > ctx->total_count) {
            end = ctx->total_count;
        }
        if (tid > 0 && tid < THREAD_MAX_COUNT) {
            atomic_add_u32(&ctx->worker_hits[tid], end - start);
        }
        JitHashFn fn = ctx->fn;
        for (u32 i = start; i < end; i++) {
            /* Her öğe için 16 tur JIT fonksiyonu çağır ki tüm worker'lar pay alsın */
            u32 v = i;
            for (u32 r = 0; r < 16u; r++) {
                v = fn(v);
            }
            ctx->out[i] = v;
        }
    }
}

static u32 ref_hash_fn(u32 x) {
    /* 16 tur (x * 1664525 + 1013904223) ^ (x << 13) */
    u32 v = x;
    for (u32 r = 0; r < 16u; r++) {
        v = (v * 1664525u + 1013904223u) ^ (v << 13u);
    }
    return v;
}

static b32 selftest_jit_workers(void) {
    /* mbin ile çalışma anında yeni bir (i32) -> i32 hash modülü derle */
    u8 work_mem[4096];
    u8 wasm_mem[1024];
    MbModule m;
    MbBuf out;

    mb_module_init(&m, work_mem, sizeof(work_mem));
    mb_buf_init(&out, wasm_mem, sizeof(wasm_mem));

    const u8 params[1]  = { MB_VAL_I32 };
    const u8 results[1] = { MB_VAL_I32 };
    u32 t_hash = mb_type(&m, params, 1, results, 1);

    MbFunc* f = mb_func_begin(&m, t_hash);
    /* (local.get 0) * 1664525 + 1013904223 */
    mb_local_get(f, 0);
    mb_i32_const(f, 1664525);
    mb_i32_mul(f);
    mb_i32_const(f, 1013904223);
    mb_i32_add(f);
    /* ^ (local.get 0 << 13) */
    mb_local_get(f, 0);
    mb_i32_const(f, 13);
    mb_i32_shl(f);
    mb_i32_xor(f);
    mb_func_end(f);

    mb_export_func(&m, "worker_hash", f->func_idx);
    mb_module_finish(&m, &out);

    /* jit_load ana thread'de tabloya ekler, worker'lara yayınlar ve ack bekler */
    JitHashFn fn_hash = (JitHashFn)jit_load(out.p, out.len, "worker_hash");
    if (!fn_hash) {
        LOG("[thread_selftest] FAIL: jit_load(worker_hash) failed");
        return FALSE;
    }

    u32 worker_hits[THREAD_MAX_COUNT];
    for (u32 t = 0; t < THREAD_MAX_COUNT; t++) {
        atomic_store_relaxed_u32(&worker_hits[t], 0u);
    }
    for (u32 i = 0; i < JIT_TEST_COUNT; i++) {
        g_jit_par_out[i] = 0u;
    }

    JitParCtx ctx;
    ctx.fn          = fn_hash;
    ctx.out         = g_jit_par_out;
    ctx.worker_hits = worker_hits;
    atomic_store_relaxed_u32(&ctx.next_chunk, 0u);
    ctx.chunk_size  = 256u;
    ctx.total_count = JIT_TEST_COUNT;

    u32 workers = thread_worker_count();
    if (workers > 0u) {
        JobCounter counter;
        job_counter_init(&counter, 0u);
        for (u32 w = 0; w < workers; w++) {
            job_submit(jit_worker_only_task, &ctx, &counter);
        }
        /* Ana thread burada iş çalmaz (job_wait yerine counter->pending spin),
         * böylece JIT fonksiyonunun %100 worker thread'lerin kendi tablolarından
         * çağrıldığı kesin olarak doğrulanır!
         */
        while (atomic_load_acquire_u32(&counter.pending) > 0u) {
            atomic_spin_pause();
        }
    } else {
        jit_worker_only_task(&ctx);
    }

    for (u32 i = 0; i < JIT_TEST_COUNT; i++) {
        u32 expected = ref_hash_fn(i);
        if (g_jit_par_out[i] != expected) {
            LOG("[thread_selftest] FAIL: JIT worker output mismatch at %u (got=0x%08x, exp=0x%08x)",
                i, g_jit_par_out[i], expected);
            return FALSE;
        }
    }

    u32 total_worker_calls = 0;
    for (u32 t = 1; t < THREAD_MAX_COUNT; t++) {
        total_worker_calls += atomic_load_relaxed_u32(&worker_hits[t]);
    }
    if (workers > 0u && total_worker_calls != JIT_TEST_COUNT) {
        LOG("[thread_selftest] FAIL: expected worker_calls=%u, got=%u", JIT_TEST_COUNT, total_worker_calls);
        return FALSE;
    }
    LOG("[thread_selftest] 3) JIT cogaltma ve worker call_indirect OK (toplam=%u, worker_payi=%u)",
        JIT_TEST_COUNT, total_worker_calls);
    return TRUE;
}

EXPORT(engine_threads_selftest) i32 engine_threads_selftest(void) {
    LOG("=== MATOKI M6_1 Threads Selftest Basliyor (aktif thread=%u, worker=%u) ===",
        thread_count(), thread_worker_count());

    if (!selftest_atomic_1m()) {
        return 1;
    }
    if (!selftest_mandelbrot()) {
        return 2;
    }
    if (!selftest_jit_workers()) {
        return 3;
    }

    LOG("=== MATOKI M6_1 Threads Selftest TAMAMLANDI: TUM TESTLER OK ===");
    return 0;
}
