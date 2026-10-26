/* bench/bench.h — C (clang -O2 -msimd128) ve elle yazılmış WAT mikro-benchmark çatısı.
 * TurboFan tier-up için ısınma (warmup), çoklu tekrar (runs), sıralı örneklerden
 * medyan ve minimum süre hesabı yapar; sonuçları LOG'a ve metin tamponuna yazar.
 * Kural (ARCHITECTURE.md §0.5): WAT yalnızca clang'ı >%15 (oran > 1.15x) geçerse MOTORA GİR alır.
 */
#ifndef BENCH_BENCH_H
#define BENCH_BENCH_H

#include "core/types.h"

#define BENCH_MAX_ENTRIES   16
#define BENCH_MAX_RUNS      31
#define BENCH_REPORT_CAP    8192

typedef void (*BenchBodyFn)(void* ctx, u32 iters);

typedef struct {
    const char* name;         /* Çekirdek adı */
    b32         bit_exact;    /* C ve WAT çıktıları bit-bit aynı mı? */
    f64         c_med_ms;     /* C (clang) medyan süre (ms) */
    f64         c_min_ms;     /* C (clang) minimum süre (ms) */
    f64         wat_med_ms;   /* WAT (elle) medyan süre (ms) */
    f64         wat_min_ms;   /* WAT (elle) minimum süre (ms) */
    f64         ratio;        /* c_med_ms / wat_med_ms (>1.0 => WAT daha hızlı) */
    b32         enter_engine; /* bit_exact && ratio > 1.15 => MOTORA GİR (TRUE), aksi halde REDDET (FALSE) */
} BenchEntry;

typedef struct {
    BenchEntry entries[BENCH_MAX_ENTRIES];
    u32        count;
    b32        all_bit_exact;
    char       report[BENCH_REPORT_CAP];
    u32        report_len;
} BenchSuite;

void bench_suite_init(BenchSuite* suite);

void bench_measure(BenchBodyFn fn, void* ctx, u32 warmup_iters, u32 bench_iters, u32 runs,
                   f64* out_med_ms, f64* out_min_ms);

void bench_suite_add(BenchSuite* suite, const char* name, b32 bit_exact,
                     BenchBodyFn c_fn, BenchBodyFn wat_fn, void* ctx,
                     u32 warmup_iters, u32 bench_iters, u32 runs);

void bench_suite_format_and_log(BenchSuite* suite);

#endif /* BENCH_BENCH_H */
