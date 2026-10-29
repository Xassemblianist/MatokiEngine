/* bench/bench_memory.c — L0 bellek modeli performans ve karakterizasyon ölçümleri.
 * 1–64 MB kopyalama (memory.copy vs SIMD128 vs __builtin_memcpy), 16 MB durum anlık
 * görüntüsü (snapshot H4 hipotezi), 8 worker ile paralel kopya, XXH64 durum özeti
 * hızı ve memory.grow maliyetini ölçüp hem LOG'a hem rapor tamponuna yazar.
 */
#include "core/types.h"
#include "plat/platform.h"
#include "thread/job.h"
#include "thread/thread.h"

#if defined(__wasm_simd128__)
#include <wasm_simd128.h>
#endif

#define BENCH_MEM_REPORT_CAP 8192
#define BENCH_MEM_MAX_RUNS   15

/* Rapor metin tamponu */
static char g_report_buf[BENCH_MEM_REPORT_CAP];
static u32  g_report_len = 0;

/* Test tamponları için dinamik alan işaretçileri */
static u8*   g_test_src = NULL;
static u8*   g_test_dst = NULL;
static usize g_test_cap = 0;

#if !defined(__wasm__)
/* Host ortamı için 64 MB statik test tamponu */
static _Alignas(64) u8 g_host_src_buf[64 * 1024 * 1024];
static _Alignas(64) u8 g_host_dst_buf[64 * 1024 * 1024];
#endif

/* --------------------------------------------------------------------------
 * Dahili Rapor Biçimlendirici (libc printf/sprintf olmadan)
 * -------------------------------------------------------------------------- */
static void rpt_clear(void) {
    g_report_len = 0;
    g_report_buf[0] = '\0';
}

static void rpt_str(const char* s) {
    if (!s) return;
    while (*s && g_report_len + 1 < BENCH_MEM_REPORT_CAP) {
        g_report_buf[g_report_len++] = *s++;
    }
    g_report_buf[g_report_len] = '\0';
}

static void rpt_u64(u64 val) {
    char tmp[24];
    u32 i = 0;
    if (val == 0) {
        rpt_str("0");
        return;
    }
    while (val > 0) {
        tmp[i++] = (char)('0' + (val % 10));
        val /= 10;
    }
    while (i > 0) {
        if (g_report_len + 1 < BENCH_MEM_REPORT_CAP) {
            g_report_buf[g_report_len++] = tmp[--i];
        } else {
            break;
        }
    }
    g_report_buf[g_report_len] = '\0';
}

static void rpt_f64(f64 val, u32 decimals) {
    if (val < 0.0) {
        rpt_str("-");
        val = -val;
    }
    u64 int_part = (u64)val;
    rpt_u64(int_part);
    if (decimals > 0) {
        rpt_str(".");
        f64 frac = val - (f64)int_part;
        for (u32 d = 0; d < decimals; d++) {
            frac *= 10.0;
            u32 digit = (u32)frac;
            if (digit > 9) digit = 9;
            if (g_report_len + 1 < BENCH_MEM_REPORT_CAP) {
                g_report_buf[g_report_len++] = (char)('0' + digit);
            }
            frac -= (f64)digit;
        }
        g_report_buf[g_report_len] = '\0';
    }
}

/* --------------------------------------------------------------------------
 * Kopyalama Çekirdekleri
 * -------------------------------------------------------------------------- */

/* 1. Clang derleyici yerleşiği (__builtin_memcpy) */
void bench_mem_copy_builtin(void* restrict dst, const void* restrict src, usize size) {
    __builtin_memcpy(dst, src, size);
}

/* 2. WebAssembly bulk memory opcode (memory.copy) */
void bench_mem_copy_wasm(void* dst, const void* src, usize size) {
#if defined(__wasm__)
    __asm__ volatile(
        "local.get %0\n"
        "local.get %1\n"
        "local.get %2\n"
        "memory.copy 0, 0\n"
        :
        : "r"(dst), "r"(src), "r"(size)
    );
#else
    __builtin_memcpy(dst, src, size);
#endif
}

/* 3. SIMD128 döngüsü (4x unrolled, 64 bayt/iterasyon) */
void bench_mem_copy_simd(void* restrict dst, const void* restrict src, usize size) {
#if defined(__wasm_simd128__)
    u8* d = (u8*)dst;
    const u8* s = (const u8*)src;
    usize chunks = size / 64u;
    for (usize i = 0; i < chunks; i++) {
        v128_t v0 = wasm_v128_load((const v128_t*)(s));
        v128_t v1 = wasm_v128_load((const v128_t*)(s + 16));
        v128_t v2 = wasm_v128_load((const v128_t*)(s + 32));
        v128_t v3 = wasm_v128_load((const v128_t*)(s + 48));
        wasm_v128_store((v128_t*)(d), v0);
        wasm_v128_store((v128_t*)(d + 16), v1);
        wasm_v128_store((v128_t*)(d + 32), v2);
        wasm_v128_store((v128_t*)(d + 48), v3);
        s += 64;
        d += 64;
    }
    usize rem = size & 63u;
    if (rem > 0) {
        __builtin_memcpy(d, s, rem);
    }
#else
    /* Non-wasm / fallback: 64-bit unrolled döngü */
    u64* d = (u64*)dst;
    const u64* s = (const u64*)src;
    usize chunks = size / 32u;
    for (usize i = 0; i < chunks; i++) {
        u64 a = s[0];
        u64 b = s[1];
        u64 c = s[2];
        u64 e = s[3];
        d[0] = a;
        d[1] = b;
        d[2] = c;
        d[3] = e;
        s += 4;
        d += 4;
    }
    usize rem = size & 31u;
    if (rem > 0) {
        __builtin_memcpy(d, s, rem);
    }
#endif
}

/* --------------------------------------------------------------------------
 * Durum Özeti (Hash) — XXH64 Standart Algoritması (freestanding)
 * -------------------------------------------------------------------------- */
#define HASH_PRIME64_1 0x9E3779B185EBCA87ULL
#define HASH_PRIME64_2 0xC2B2AE3D27D4EB4FULL
#define HASH_PRIME64_3 0x165667B19E3779F9ULL
#define HASH_PRIME64_4 0x85EBCA77C2B2AE63ULL
#define HASH_PRIME64_5 0x27D4EB2F165667C5ULL

static inline u64 hash_rotl64(u64 x, u32 r) {
    return (x << r) | (x >> (64 - r));
}

static inline u64 hash_round64(u64 acc, u64 val) {
    acc += val * HASH_PRIME64_2;
    acc = hash_rotl64(acc, 31);
    acc *= HASH_PRIME64_1;
    return acc;
}

u64 bench_mem_hash64(const void* src, usize len) {
    const u8* p = (const u8*)src;
    const u8* const bEnd = p + len;
    u64 h64;

    if (len >= 32) {
        const u8* const limit = bEnd - 32;
        u64 v1 = HASH_PRIME64_1 + HASH_PRIME64_2;
        u64 v2 = HASH_PRIME64_2;
        u64 v3 = 0;
        u64 v4 = 0 - HASH_PRIME64_1;

        do {
            u64 k1, k2, k3, k4;
            __builtin_memcpy(&k1, p, 8);
            __builtin_memcpy(&k2, p + 8, 8);
            __builtin_memcpy(&k3, p + 16, 8);
            __builtin_memcpy(&k4, p + 24, 8);

            v1 = hash_round64(v1, k1);
            v2 = hash_round64(v2, k2);
            v3 = hash_round64(v3, k3);
            v4 = hash_round64(v4, k4);

            p += 32;
        } while (p <= limit);

        h64 = hash_rotl64(v1, 1) + hash_rotl64(v2, 7) + hash_rotl64(v3, 12) + hash_rotl64(v4, 18);

        v1 *= HASH_PRIME64_2; v1 = hash_rotl64(v1, 31); v1 *= HASH_PRIME64_1; h64 ^= v1;
        h64 = h64 * HASH_PRIME64_1 + HASH_PRIME64_4;

        v2 *= HASH_PRIME64_2; v2 = hash_rotl64(v2, 31); v2 *= HASH_PRIME64_1; h64 ^= v2;
        h64 = h64 * HASH_PRIME64_1 + HASH_PRIME64_4;

        v3 *= HASH_PRIME64_2; v3 = hash_rotl64(v3, 31); v3 *= HASH_PRIME64_1; h64 ^= v3;
        h64 = h64 * HASH_PRIME64_1 + HASH_PRIME64_4;

        v4 *= HASH_PRIME64_2; v4 = hash_rotl64(v4, 31); v4 *= HASH_PRIME64_1; h64 ^= v4;
        h64 = h64 * HASH_PRIME64_1 + HASH_PRIME64_4;
    } else {
        h64 = HASH_PRIME64_5;
    }

    h64 += (u64)len;

    while (p + 8 <= bEnd) {
        u64 k1;
        __builtin_memcpy(&k1, p, 8);
        k1 *= HASH_PRIME64_2;
        k1 = hash_rotl64(k1, 31);
        k1 *= HASH_PRIME64_1;
        h64 ^= k1;
        h64 = hash_rotl64(h64, 27) * HASH_PRIME64_1 + HASH_PRIME64_4;
        p += 8;
    }

    if (p + 4 <= bEnd) {
        u32 k1;
        __builtin_memcpy(&k1, p, 4);
        h64 ^= (u64)k1 * HASH_PRIME64_1;
        h64 = hash_rotl64(h64, 23) * HASH_PRIME64_2 + HASH_PRIME64_3;
        p += 4;
    }

    while (p < bEnd) {
        h64 ^= ((u64)(*p)) * HASH_PRIME64_5;
        h64 = hash_rotl64(h64, 11) * HASH_PRIME64_1;
        p++;
    }

    h64 ^= h64 >> 33;
    h64 *= HASH_PRIME64_2;
    h64 ^= h64 >> 29;
    h64 *= HASH_PRIME64_3;
    h64 ^= h64 >> 32;

    return h64;
}

/* --------------------------------------------------------------------------
 * Paralel Kopya (job_parallel_for kullanarak)
 * -------------------------------------------------------------------------- */
typedef struct {
    u8*       dst;
    const u8* src;
    usize     total_size;
    u32       num_parts;
} ParallelCopyCtx;

static void parallel_copy_worker(u32 start, u32 end, void* user_ctx) {
    ParallelCopyCtx* ctx = (ParallelCopyCtx*)user_ctx;
    usize chunk = ctx->total_size / (usize)ctx->num_parts;
    for (u32 p = start; p < end; p++) {
        usize offset = (usize)p * chunk;
        usize size = (p == ctx->num_parts - 1u) ? (ctx->total_size - offset) : chunk;
        bench_mem_copy_builtin(ctx->dst + offset, ctx->src + offset, size);
    }
}

void bench_mem_parallel_copy(void* dst, const void* src, usize size, u32 threads) {
    if (threads <= 1u) {
        bench_mem_copy_builtin(dst, src, size);
        return;
    }
    ParallelCopyCtx ctx;
    ctx.dst = (u8*)dst;
    ctx.src = (const u8*)src;
    ctx.total_size = size;
    ctx.num_parts = threads;

    /* job_parallel_for count=threads, grain=1 */
    job_parallel_for(threads, 1u, parallel_copy_worker, &ctx);
}

/* --------------------------------------------------------------------------
 * memory.grow ve memory.size işlemleri
 * -------------------------------------------------------------------------- */
i32 bench_mem_grow(u32 pages) {
#if defined(__wasm__)
    return __builtin_wasm_memory_grow(0, pages);
#else
    (void)pages;
    return 0;
#endif
}

u32 bench_mem_size(void) {
#if defined(__wasm__)
    return __builtin_wasm_memory_size(0);
#else
    return 0;
#endif
}

/* --------------------------------------------------------------------------
 * Test Tamponları Hazırlığı
 * -------------------------------------------------------------------------- */
b32 bench_mem_prepare_buffers(usize size_bytes) {
    if (g_test_cap >= size_bytes && g_test_src && g_test_dst) {
        return TRUE;
    }
#if defined(__wasm__)
    /* 2x size_bytes bellek gerekir (src + dst) */
    usize total_need = size_bytes * 2u;
    u32 pages_needed = (u32)((total_need + 65535u) / 65536u);
    i32 prev_pages = __builtin_wasm_memory_grow(0, pages_needed);
    if (prev_pages < 0) {
        return FALSE;
    }
    g_test_src = (u8*)((usize)prev_pages * 65536u);
    g_test_dst = g_test_src + size_bytes;
    g_test_cap = size_bytes;
#else
    if (size_bytes <= sizeof(g_host_src_buf)) {
        g_test_src = g_host_src_buf;
        g_test_dst = g_host_dst_buf;
        g_test_cap = size_bytes;
    } else {
        return FALSE;
    }
#endif

    /* Belirli bir desenle doldur (doğrulama ve sayfa ayrılması için) */
    for (usize i = 0; i < size_bytes; i += 4096) {
        g_test_src[i] = (u8)(i ^ 0x5A);
        g_test_dst[i] = 0;
    }
    return TRUE;
}

/* --------------------------------------------------------------------------
 * İstatistik ve Ölçüm Yardımcısı
 * -------------------------------------------------------------------------- */
typedef void (*MemOpFn)(void* dst, const void* src, usize size);

static void measure_op(MemOpFn fn, void* dst, const void* src, usize size, u32 runs,
                       f64* out_med_ms, f64* out_min_ms) {
    f64 samples[BENCH_MEM_MAX_RUNS];
    if (runs == 0) runs = 1;
    if (runs > BENCH_MEM_MAX_RUNS) runs = BENCH_MEM_MAX_RUNS;

    /* Isınma (warmup) */
    fn(dst, src, size);

    for (u32 r = 0; r < runs; r++) {
        f64 t0 = plat_time_ms();
        fn(dst, src, size);
        f64 t1 = plat_time_ms();
        f64 dt = t1 - t0;
        if (dt < 0.0) dt = 0.0;
        samples[r] = dt;
    }

    /* Sıralama (medyan hesabı için) */
    for (u32 i = 1; i < runs; i++) {
        f64 key = samples[i];
        u32 j = i;
        while (j > 0 && samples[j - 1] > key) {
            samples[j] = samples[j - 1];
            j--;
        }
        samples[j] = key;
    }

    *out_min_ms = samples[0];
    *out_med_ms = samples[runs / 2u];
}

/* --------------------------------------------------------------------------
 * Wasm / Host Dışa Aktarılan Arayüz (EXPORT)
 * -------------------------------------------------------------------------- */

EXPORT(engine_bench_mem_run) i32 engine_bench_mem_run(void) {
    rpt_clear();
    rpt_str("=== MATOKI Bellek Modeli Olcum Raporu (H4 & ARCH_V2 §2) ===\n\n");

    const usize sizes[] = { 1024 * 1024, 4 * 1024 * 1024, 16 * 1024 * 1024, 64 * 1024 * 1024 };
    const char* size_names[] = { "1 MB", "4 MB", "16 MB", "64 MB" };

    /* 1. Kopyalama Kıyaslaması: memory.copy vs SIMD vs __builtin_memcpy */
    rpt_str("--- 1. Kopyalama Kıyaslaması (Tek İş Parçacığı) ---\n");
    rpt_str("Boyut\tYöntem\tMedyan (ms)\tMin (ms)\tBant Genişliği (GB/s)\n");

    for (u32 s = 0; s < 4; s++) {
        usize sz = sizes[s];
        if (!bench_mem_prepare_buffers(sz)) {
            rpt_str("Bellek tahsisi basarisiz: ");
            rpt_str(size_names[s]);
            rpt_str("\n");
            continue;
        }

        f64 med_builtin = 0, min_builtin = 0;
        f64 med_wasm = 0, min_wasm = 0;
        f64 med_simd = 0, min_simd = 0;

        measure_op(bench_mem_copy_builtin, g_test_dst, g_test_src, sz, 9, &med_builtin, &min_builtin);
        measure_op(bench_mem_copy_wasm, g_test_dst, g_test_src, sz, 9, &med_wasm, &min_wasm);
        measure_op(bench_mem_copy_simd, g_test_dst, g_test_src, sz, 9, &med_simd, &min_simd);

        f64 sz_gb = (f64)sz / (1024.0 * 1024.0 * 1024.0);

        /* builtin */
        rpt_str(size_names[s]); rpt_str("\t__builtin_memcpy\t");
        rpt_f64(med_builtin, 3); rpt_str("\t");
        rpt_f64(min_builtin, 3); rpt_str("\t");
        rpt_f64(min_builtin > 0 ? (sz_gb / (min_builtin / 1000.0)) : 0.0, 2);
        rpt_str("\n");

        /* memory.copy (wasm) */
        rpt_str(size_names[s]); rpt_str("\tmemory.copy\t");
        rpt_f64(med_wasm, 3); rpt_str("\t");
        rpt_f64(min_wasm, 3); rpt_str("\t");
        rpt_f64(min_wasm > 0 ? (sz_gb / (min_wasm / 1000.0)) : 0.0, 2);
        rpt_str("\n");

        /* SIMD128 */
        rpt_str(size_names[s]); rpt_str("\tSIMD128 loop\t");
        rpt_f64(med_simd, 3); rpt_str("\t");
        rpt_f64(min_simd, 3); rpt_str("\t");
        rpt_f64(min_simd > 0 ? (sz_gb / (min_simd / 1000.0)) : 0.0, 2);
        rpt_str("\n");
    }

    /* 2. 16 MB Durum Bölgesi Snapshot & H4 Eşiği */
    rpt_str("\n--- 2. 16 MB Durum Bölgesi Snapshot & H4 Değerlendirmesi ---\n");
    usize state_sz = 16 * 1024 * 1024;
    bench_mem_prepare_buffers(state_sz);
    f64 snap_med = 0, snap_min = 0;
    measure_op(bench_mem_copy_builtin, g_test_dst, g_test_src, state_sz, 15, &snap_med, &snap_min);
    f64 snap_gb = (f64)state_sz / (1024.0 * 1024.0 * 1024.0);
    f64 snap_bw = snap_min > 0 ? (snap_gb / (snap_min / 1000.0)) : 0.0;

    rpt_str("16 MB Snapshot Medyan: "); rpt_f64(snap_med, 3); rpt_str(" ms\n");
    rpt_str("16 MB Snapshot Min:    "); rpt_f64(snap_min, 3); rpt_str(" ms\n");
    rpt_str("16 MB Bant Genisligi:  "); rpt_f64(snap_bw, 2);  rpt_str(" GB/s\n");
    rpt_str("H4 Esigi (< 2.0 ms):   ");
    if (snap_med < 2.0) {
        rpt_str("GECERLI (H4 DOGRULANDI)\n");
    } else {
        rpt_str("ESIK ASILDI (Optimizasyon gerekli)\n");
    }

    /* 3. Paralel Kopya (1T vs 8T) */
    rpt_str("\n--- 3. Paralel Kopya (16 MB & 64 MB) ---\n");
    const u32 p_threads[] = { 1, 2, 4, 8 };
    for (u32 s = 2; s <= 3; s++) {
        usize sz = sizes[s];
        bench_mem_prepare_buffers(sz);
        f64 sz_gb = (f64)sz / (1024.0 * 1024.0 * 1024.0);
        f64 t1_time = 0.0;

        rpt_str("Hedef: "); rpt_str(size_names[s]); rpt_str("\n");
        rpt_str("Thread\tSure (ms)\tBant Genisligi (GB/s)\tHizlanma\n");

        for (u32 ti = 0; ti < 4; ti++) {
            u32 th = p_threads[ti];
            /* 5 tekrar */
            f64 best_t = 1e9;
            for (u32 r = 0; r < 5; r++) {
                f64 t0 = plat_time_ms();
                bench_mem_parallel_copy(g_test_dst, g_test_src, sz, th);
                f64 t1 = plat_time_ms();
                f64 dt = t1 - t0;
                if (dt < best_t) best_t = dt;
            }
            if (th == 1) t1_time = best_t;
            f64 bw = best_t > 0 ? (sz_gb / (best_t / 1000.0)) : 0.0;
            f64 speedup = (t1_time > 0 && best_t > 0) ? (t1_time / best_t) : 1.0;

            rpt_u64(th); rpt_str("T\t");
            rpt_f64(best_t, 3); rpt_str("\t");
            rpt_f64(bw, 2); rpt_str("\t");
            rpt_f64(speedup, 2); rpt_str("x\n");
        }
    }

    /* 4. Durum Özeti (Hash - XXH64) Hızı */
    rpt_str("\n--- 4. Durum Özeti (XXH64 Hash) Hızı ---\n");
    rpt_str("Boyut\tSure (ms)\tBant Genisligi (GB/s)\tHash Özeti\n");
    for (u32 s = 0; s < 4; s++) {
        usize sz = sizes[s];
        bench_mem_prepare_buffers(sz);
        f64 sz_gb = (f64)sz / (1024.0 * 1024.0 * 1024.0);

        u64 h = 0;
        f64 best_dt = 1e9;
        for (u32 r = 0; r < 5; r++) {
            f64 t0 = plat_time_ms();
            h = bench_mem_hash64(g_test_src, sz);
            f64 t1 = plat_time_ms();
            f64 dt = t1 - t0;
            if (dt < best_dt) best_dt = dt;
        }
        f64 bw = best_dt > 0 ? (sz_gb / (best_dt / 1000.0)) : 0.0;

        rpt_str(size_names[s]); rpt_str("\t");
        rpt_f64(best_dt, 3); rpt_str("\t");
        rpt_f64(bw, 2); rpt_str("\t0x");
        rpt_u64(h); rpt_str("\n");
    }

    /* 5. memory.grow Maliyeti */
    rpt_str("\n--- 5. memory.grow Maliyeti ---\n");
    u32 cur_pages = bench_mem_size();
    rpt_str("Mevcut Sayfa Sayisi: "); rpt_u64(cur_pages);
    rpt_str(" ("); rpt_u64((u64)cur_pages * 64 / 1024); rpt_str(" MB)\n");

    const u32 grow_pages[] = { 1, 16, 256 };
    const char* grow_names[] = { "1 sayfa (64 KB)", "16 sayfa (1 MB)", "256 sayfa (16 MB)" };
    for (u32 g = 0; g < 3; g++) {
        u32 p = grow_pages[g];
        f64 t0 = plat_time_ms();
        i32 prev = bench_mem_grow(p);
        f64 t1 = plat_time_ms();
        f64 dt = t1 - t0;

        rpt_str(grow_names[g]); rpt_str(": ");
        if (prev >= 0) {
            rpt_f64(dt * 1000.0, 1); rpt_str(" µs (onceki sayfa: ");
            rpt_u64((u32)prev); rpt_str(")\n");
        } else {
            rpt_str("BASARISIZ (-1, bellek limiti)\n");
        }
    }

    /* LOG çıktısı */
    plat_log(g_report_buf, g_report_len);
    return 0;
}

EXPORT(engine_bench_mem_report_ptr) const char* engine_bench_mem_report_ptr(void) {
    return g_report_buf;
}

EXPORT(engine_bench_mem_report_len) u32 engine_bench_mem_report_len(void) {
    return g_report_len;
}

EXPORT(engine_bench_mem_copy_builtin) void engine_bench_mem_copy_builtin(u32 dst, u32 src, u32 size) {
    bench_mem_copy_builtin((void*)(uptr)dst, (const void*)(uptr)src, (usize)size);
}

EXPORT(engine_bench_mem_copy_wasm) void engine_bench_mem_copy_wasm(u32 dst, u32 src, u32 size) {
    bench_mem_copy_wasm((void*)(uptr)dst, (const void*)(uptr)src, (usize)size);
}

EXPORT(engine_bench_mem_copy_simd) void engine_bench_mem_copy_simd(u32 dst, u32 src, u32 size) {
    bench_mem_copy_simd((void*)(uptr)dst, (const void*)(uptr)src, (usize)size);
}

EXPORT(engine_bench_mem_hash64) u64 engine_bench_mem_hash64(u32 src, u32 size) {
    return bench_mem_hash64((const void*)(uptr)src, (usize)size);
}

EXPORT(engine_bench_mem_grow) i32 engine_bench_mem_grow(u32 pages) {
    return bench_mem_grow(pages);
}

EXPORT(engine_bench_mem_size) u32 engine_bench_mem_size(void) {
    return bench_mem_size();
}

EXPORT(engine_bench_mem_parallel_copy) void engine_bench_mem_parallel_copy(u32 dst, u32 src, u32 size, u32 threads) {
    bench_mem_parallel_copy((void*)(uptr)dst, (const void*)(uptr)src, (usize)size, threads);
}

EXPORT(engine_bench_mem_prepare) u32 engine_bench_mem_prepare(u32 size_mb) {
    usize bytes = (usize)size_mb * 1024u * 1024u;
    if (bench_mem_prepare_buffers(bytes)) {
        return (u32)(uptr)g_test_src;
    }
    return 0;
}

EXPORT(engine_bench_mem_dst_ptr) u32 engine_bench_mem_dst_ptr(void) {
    return (u32)(uptr)g_test_dst;
}
