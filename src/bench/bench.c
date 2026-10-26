/* bench/bench.c — Mikro-benchmark ölçüm, istatistik (medyan + min) ve tablo biçimlendirme.
 * plat_time_ms() ile zamanlama yapar; libc kullanmadan sabit noktalı metin formatlayıcıyla
 * sonuç tablosunu hem LOG'a hem de JS arayüzünün okuyabileceği report tamponuna yazar.
 */
#include "bench/bench.h"
#include "plat/platform.h"
#include "core/log.h"

void bench_suite_init(BenchSuite* suite) {
    suite->count = 0;
    suite->all_bit_exact = TRUE;
    suite->report[0] = '\0';
    suite->report_len = 0;
}

void bench_measure(BenchBodyFn fn, void* ctx, u32 warmup_iters, u32 bench_iters, u32 runs,
                   f64* out_med_ms, f64* out_min_ms) {
    f64 samples[BENCH_MAX_RUNS];
    if (runs == 0) runs = 1;
    if (runs > BENCH_MAX_RUNS) runs = BENCH_MAX_RUNS;

    /* Isınma: V8 Liftoff -> TurboFan tier-up geçişini tamamla */
    if (warmup_iters > 0) {
        fn(ctx, warmup_iters);
    }

    for (u32 r = 0; r < runs; r++) {
        f64 t0 = plat_time_ms();
        fn(ctx, bench_iters);
        f64 t1 = plat_time_ms();
        f64 dt = t1 - t0;
        if (dt < 0.0) dt = 0.0;
        samples[r] = dt;
    }

    /* Küçük dizi sıralaması (insertion sort) */
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

void bench_suite_add(BenchSuite* suite, const char* name, b32 bit_exact,
                     BenchBodyFn c_fn, BenchBodyFn wat_fn, void* ctx,
                     u32 warmup_iters, u32 bench_iters, u32 runs) {
    if (suite->count >= BENCH_MAX_ENTRIES) return;
    BenchEntry* e = &suite->entries[suite->count++];
    e->name = name;
    e->bit_exact = bit_exact;
    if (!bit_exact) {
        suite->all_bit_exact = FALSE;
    }

    /* Karşılıklı ısınma sonrasında C ve WAT ölçümleri */
    bench_measure(c_fn, ctx, warmup_iters, bench_iters, runs, &e->c_med_ms, &e->c_min_ms);
    bench_measure(wat_fn, ctx, warmup_iters, bench_iters, runs, &e->wat_med_ms, &e->wat_min_ms);

    if (e->wat_med_ms > 1e-6) {
        e->ratio = e->c_med_ms / e->wat_med_ms;
    } else {
        e->ratio = 1.0;
    }
    /* Kural (ARCHITECTURE.md §0.5): yalnızca >%15 (ratio > 1.15) kazançta MOTORA GİR */
    e->enter_engine = (bit_exact && e->ratio > 1.15) ? TRUE : FALSE;
}

static void rpt_putc(BenchSuite* s, char c) {
    if (s->report_len + 1u < BENCH_REPORT_CAP) {
        s->report[s->report_len++] = c;
        s->report[s->report_len] = '\0';
    }
}

static void rpt_puts(BenchSuite* s, const char* str) {
    if (!str) return;
    while (*str) {
        rpt_putc(s, *str++);
    }
}

static void rpt_pad(BenchSuite* s, const char* str, u32 width) {
    u32 len = 0;
    while (str[len] != '\0') {
        rpt_putc(s, str[len]);
        len++;
    }
    while (len < width) {
        rpt_putc(s, ' ');
        len++;
    }
}

static void rpt_u32(BenchSuite* s, u32 val) {
    char tmp[16];
    u32 n = 0;
    if (val == 0) {
        rpt_putc(s, '0');
        return;
    }
    while (val > 0) {
        tmp[n++] = (char)('0' + (val % 10u));
        val /= 10u;
    }
    while (n > 0) {
        rpt_putc(s, tmp[--n]);
    }
}

static void rpt_f64_2dec(BenchSuite* s, f64 val, u32 pad_width) {
    char buf[32];
    u32 pos = 0;
    if (val < 0.0) val = 0.0;
    u32 scaled = (u32)(val * 100.0 + 0.5);
    u32 whole = scaled / 100u;
    u32 frac  = scaled % 100u;

    char wtmp[16];
    u32 wn = 0;
    if (whole == 0) {
        wtmp[wn++] = '0';
    } else {
        while (whole > 0) {
            wtmp[wn++] = (char)('0' + (whole % 10u));
            whole /= 10u;
        }
    }
    while (wn > 0) {
        buf[pos++] = wtmp[--wn];
    }
    buf[pos++] = '.';
    buf[pos++] = (char)('0' + (frac / 10u));
    buf[pos++] = (char)('0' + (frac % 10u));
    buf[pos] = '\0';

    while (pos < pad_width) {
        rpt_putc(s, ' ');
        pos++;
    }
    rpt_puts(s, buf);
}

void bench_suite_format_and_log(BenchSuite* suite) {
    suite->report_len = 0;
    suite->report[0] = '\0';

    rpt_puts(suite, "çekirdek             | bit-bit | C med (min) ms  | WAT med (min) ms | oran   | karar\n");
    rpt_puts(suite, "---------------------+---------+-----------------+------------------+--------+------------\n");

    LOG("=== MATOKI M5_3: Elle WAT vs clang (-O2 -msimd128) Benchmark ===");
    LOG("cekirdek             | bit-bit | C med (min) ms  | WAT med (min) ms | oran   | karar");
    LOG("---------------------+---------+-----------------+------------------+--------+------------");

    for (u32 i = 0; i < suite->count; i++) {
        const BenchEntry* e = &suite->entries[i];
        u32 line_start = suite->report_len;

        rpt_pad(suite, e->name, 20);
        rpt_puts(suite, " | ");
        rpt_puts(suite, e->bit_exact ? "  EVET " : " HAYIR!");
        rpt_puts(suite, " | ");
        rpt_f64_2dec(suite, e->c_med_ms, 6);
        rpt_puts(suite, " (");
        rpt_f64_2dec(suite, e->c_min_ms, 5);
        rpt_puts(suite, ") | ");
        rpt_f64_2dec(suite, e->wat_med_ms, 6);
        rpt_puts(suite, " (");
        rpt_f64_2dec(suite, e->wat_min_ms, 5);
        rpt_puts(suite, ")  | ");
        rpt_f64_2dec(suite, e->ratio, 5);
        rpt_puts(suite, "x | ");
        rpt_puts(suite, e->enter_engine ? "MOTORA GİR" : "REDDET");

        /* Bu satırı LOG'a da bas */
        LOG("%s", suite->report + line_start);

        rpt_putc(suite, '\n');
    }

    /* Ayrıca makine tarafından ayrıştırılabilir (TSV) özet satırları ekle */
    rpt_puts(suite, "\n#TSV\n");
    for (u32 i = 0; i < suite->count; i++) {
        const BenchEntry* e = &suite->entries[i];
        rpt_puts(suite, e->name);
        rpt_putc(suite, '\t');
        rpt_u32(suite, e->bit_exact ? 1u : 0u);
        rpt_putc(suite, '\t');
        rpt_f64_2dec(suite, e->c_med_ms, 0);
        rpt_putc(suite, '\t');
        rpt_f64_2dec(suite, e->c_min_ms, 0);
        rpt_putc(suite, '\t');
        rpt_f64_2dec(suite, e->wat_med_ms, 0);
        rpt_putc(suite, '\t');
        rpt_f64_2dec(suite, e->wat_min_ms, 0);
        rpt_putc(suite, '\t');
        rpt_f64_2dec(suite, e->ratio, 0);
        rpt_putc(suite, '\t');
        rpt_puts(suite, e->enter_engine ? "MOTORA GİR" : "REDDET");
        rpt_putc(suite, '\n');
    }
}
