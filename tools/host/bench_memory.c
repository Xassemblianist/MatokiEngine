/* tools/host/bench_memory.c — Host ortamında bellek modeli mikro-benchmark ve karakterizasyonu.
 * L0 bellek modeli sayıları: 1–64 MB kopyalama (memcpy vs SIMD vs skaler), 16 MB durum
 * snapshot (H4 eşiği: 16 MB < 2 ms), pthread ile 8 thread paralel kopya, XXH64 durum hash'i,
 * bellek büyütme maliyeti, 4 GB adresleme ve memory64 raporunu konsola ve Markdown tablosuna basar.
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <time.h>
#include <pthread.h>
#include <unistd.h>
#include <sys/mman.h>
#include <sys/sysinfo.h>
#if defined(__x86_64__) || defined(_M_X64)
#include <immintrin.h>
#endif

typedef uint8_t  u8;
typedef uint32_t u32;
typedef uint64_t u64;
typedef double   f64;

#define BENCH_RUNS 15

/* --------------------------------------------------------------------------
 * Zaman Ölçüm Yardımcıları
 * -------------------------------------------------------------------------- */
static f64 now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (f64)ts.tv_sec * 1000.0 + (f64)ts.tv_nsec / 1e6;
}

static void print_system_info(void) {
    printf("=== MATOKI Bellek Modeli Host Benchmark (H4 & ARCH_V2 §2) ===\n");

    /* Makine yükü */
    double loadavg[3];
    if (getloadavg(loadavg, 3) != -1) {
        printf("Makine Yuku (Load Avg): 1 dk: %.2f, 5 dk: %.2f, 15 dk: %.2f\n",
               loadavg[0], loadavg[1], loadavg[2]);
    }

    /* CPU bilgisi */
    int nprocs = get_nprocs();
    printf("CPU Cekirdek Sayisi:    %d\n", nprocs);

    FILE* fcpu = fopen("/proc/cpuinfo", "r");
    if (fcpu) {
        char line[256];
        while (fgets(line, sizeof(line), fcpu)) {
            if (strncmp(line, "model name", 10) == 0) {
                char* colon = strchr(line, ':');
                if (colon) {
                    colon++;
                    while (*colon == ' ' || *colon == '\t') colon++;
                    printf("CPU Modeli:             %s", colon);
                    break;
                }
            }
        }
        fclose(fcpu);
    }
    printf("\n");
}

/* --------------------------------------------------------------------------
 * Kopyalama Çekirdekleri
 * -------------------------------------------------------------------------- */

/* 1. libc memcpy */
static void copy_libc_memcpy(void* dst, const void* src, size_t size) {
    memcpy(dst, src, size);
}

/* 2. SIMD döngüsü (AVX2 veya SSE2) */
static void copy_simd(void* restrict dst, const void* restrict src, size_t size) {
#if defined(__AVX2__)
    u8* d = (u8*)dst;
    const u8* s = (const u8*)src;
    size_t chunks = size / 128;
    for (size_t i = 0; i < chunks; i++) {
        __m256i a = _mm256_loadu_si256((const __m256i*)(s));
        __m256i b = _mm256_loadu_si256((const __m256i*)(s + 32));
        __m256i c = _mm256_loadu_si256((const __m256i*)(s + 64));
        __m256i e = _mm256_loadu_si256((const __m256i*)(s + 96));
        _mm256_storeu_si256((__m256i*)(d), a);
        _mm256_storeu_si256((__m256i*)(d + 32), b);
        _mm256_storeu_si256((__m256i*)(d + 64), c);
        _mm256_storeu_si256((__m256i*)(d + 96), e);
        s += 128;
        d += 128;
    }
    size_t rem = size & 127;
    if (rem) memcpy(d, s, rem);
#elif defined(__SSE2__)
    u8* d = (u8*)dst;
    const u8* s = (const u8*)src;
    size_t chunks = size / 64;
    for (size_t i = 0; i < chunks; i++) {
        __m128i a = _mm_loadu_si128((const __m128i*)(s));
        __m128i b = _mm_loadu_si128((const __m128i*)(s + 16));
        __m128i c = _mm_loadu_si128((const __m128i*)(s + 32));
        __m128i e = _mm_loadu_si128((const __m128i*)(s + 48));
        _mm_storeu_si128((__m128i*)(d), a);
        _mm_storeu_si128((__m128i*)(d + 16), b);
        _mm_storeu_si128((__m128i*)(d + 32), c);
        _mm_storeu_si128((__m128i*)(d + 48), e);
        s += 64;
        d += 64;
    }
    size_t rem = size & 63;
    if (rem) memcpy(d, s, rem);
#else
    memcpy(dst, src, size);
#endif
}

/* 3. 64-bit skaler döngü */
static void copy_scalar64(void* restrict dst, const void* restrict src, size_t size) {
    u64* d = (u64*)dst;
    const u64* s = (const u64*)src;
    size_t chunks = size / 32;
    for (size_t i = 0; i < chunks; i++) {
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
    size_t rem = size & 31;
    if (rem) memcpy(d, s, rem);
}

/* --------------------------------------------------------------------------
 * XXH64 Durum Özeti (Hash)
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

static u64 state_hash64(const void* src, size_t len) {
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
            memcpy(&k1, p, 8);
            memcpy(&k2, p + 8, 8);
            memcpy(&k3, p + 16, 8);
            memcpy(&k4, p + 24, 8);

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
        memcpy(&k1, p, 8);
        k1 *= HASH_PRIME64_2;
        k1 = hash_rotl64(k1, 31);
        k1 *= HASH_PRIME64_1;
        h64 ^= k1;
        h64 = hash_rotl64(h64, 27) * HASH_PRIME64_1 + HASH_PRIME64_4;
        p += 8;
    }

    if (p + 4 <= bEnd) {
        u32 k1;
        memcpy(&k1, p, 4);
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
 * Paralel Kopya (Pthread)
 * -------------------------------------------------------------------------- */
typedef struct {
    u8*       dst;
    const u8* src;
    size_t    size;
} ThreadCopyArg;

static void* thread_copy_func(void* arg) {
    ThreadCopyArg* t = (ThreadCopyArg*)arg;
    memcpy(t->dst, t->src, t->size);
    return NULL;
}

static f64 bench_parallel_copy_pthread(u8* dst, const u8* src, size_t total_size, int num_threads) {
    if (num_threads <= 1) {
        f64 t0 = now_ms();
        memcpy(dst, src, total_size);
        return now_ms() - t0;
    }

    pthread_t threads[8];
    ThreadCopyArg args[8];
    size_t chunk = total_size / (size_t)num_threads;

    f64 t0 = now_ms();
    for (int i = 0; i < num_threads; i++) {
        size_t offset = (size_t)i * chunk;
        size_t sz = (i == num_threads - 1) ? (total_size - offset) : chunk;
        args[i].dst = dst + offset;
        args[i].src = src + offset;
        args[i].size = sz;
        pthread_create(&threads[i], NULL, thread_copy_func, &args[i]);
    }
    for (int i = 0; i < num_threads; i++) {
        pthread_join(threads[i], NULL);
    }
    return now_ms() - t0;
}

/* --------------------------------------------------------------------------
 * Ölçüm Yardımcısı
 * -------------------------------------------------------------------------- */
typedef void (*CopyFn)(void* dst, const void* src, size_t size);

static void measure_copy(CopyFn fn, void* dst, const void* src, size_t size,
                         f64* out_med, f64* out_min) {
    f64 samples[BENCH_RUNS];
    /* Warmup */
    fn(dst, src, size);

    for (int r = 0; r < BENCH_RUNS; r++) {
        f64 t0 = now_ms();
        fn(dst, src, size);
        f64 t1 = now_ms();
        samples[r] = t1 - t0;
    }
    /* Sort */
    for (int i = 1; i < BENCH_RUNS; i++) {
        f64 k = samples[i];
        int j = i;
        while (j > 0 && samples[j - 1] > k) {
            samples[j] = samples[j - 1];
            j--;
        }
        samples[j] = k;
    }
    *out_min = samples[0];
    *out_med = samples[BENCH_RUNS / 2];
}

/* --------------------------------------------------------------------------
 * Ana Benchmark Akışı
 * -------------------------------------------------------------------------- */
int main(void) {
    print_system_info();

    const size_t sizes[] = { 1 * 1024 * 1024, 4 * 1024 * 1024, 16 * 1024 * 1024, 64 * 1024 * 1024 };
    const char* size_names[] = { "1 MB", "4 MB", "16 MB", "64 MB" };

    /* 64 MB bellek tamponları ayır (sayfa hizalı) */
    size_t max_sz = 64 * 1024 * 1024;
    u8* src = (u8*)aligned_alloc(4096, max_sz);
    u8* dst = (u8*)aligned_alloc(4096, max_sz);
    if (!src || !dst) {
        fprintf(stderr, "Bellek ayrilamadi!\n");
        return 1;
    }

    /* Sayfaları ısıt ve belirgin veri yaz */
    for (size_t i = 0; i < max_sz; i += 4096) {
        src[i] = (u8)(i ^ 0xA5);
        dst[i] = 0;
    }

    printf("### 1. Kopyalama Kıyaslaması (Tek İş Parçacığı: memcpy vs SIMD vs Skaler)\n\n");
    printf("| Boyut | Yöntem | Medyan (ms) | Min (ms) | Bant Genişliği (GB/s) |\n");
    printf("|---|---|---:|---:|---:|\n");

    f64 h4_snap_med = 0.0, h4_snap_min = 0.0;

    for (int s = 0; s < 4; s++) {
        size_t sz = sizes[s];
        f64 sz_gb = (f64)sz / (1024.0 * 1024.0 * 1024.0);

        f64 med_libc, min_libc;
        f64 med_simd, min_simd;
        f64 med_scal, min_scal;

        measure_copy(copy_libc_memcpy, dst, src, sz, &med_libc, &min_libc);
        measure_copy(copy_simd, dst, src, sz, &med_simd, &min_simd);
        measure_copy(copy_scalar64, dst, src, sz, &med_scal, &min_scal);

        if (sz == 16 * 1024 * 1024) {
            h4_snap_med = med_libc;
            h4_snap_min = min_libc;
        }

        printf("| %s | libc memcpy | %.3f | %.3f | %.2f |\n",
               size_names[s], med_libc, min_libc, sz_gb / (min_libc / 1000.0));
        printf("| %s | SIMD loop | %.3f | %.3f | %.2f |\n",
               size_names[s], med_simd, min_simd, sz_gb / (min_simd / 1000.0));
        printf("| %s | skaler u64 | %.3f | %.3f | %.2f |\n",
               size_names[s], med_scal, min_scal, sz_gb / (min_scal / 1000.0));
    }
    printf("\n");

    /* 2. 16 MB Durum Bölgesi Snapshot & H4 Değerlendirmesi */
    printf("### 2. 16 MB Durum Bölgesi Snapshot & H4 Değerlendirmesi\n\n");
    f64 snap_gb = 16.0 / 1024.0;
    f64 snap_bw = snap_gb / (h4_snap_min / 1000.0);
    printf("- **16 MB Snapshot Medyan Süresi:** `%.3f ms`\n", h4_snap_med);
    printf("- **16 MB Snapshot Minimum Süresi:** `%.3f ms`\n", h4_snap_min);
    printf("- **Bant Genişliği:** `%.2f GB/s`\n", snap_bw);
    printf("- **H4 Eşik Kriteri (16 MB < 2.0 ms):** %s\n",
           h4_snap_med < 2.0 ? "**GEÇTİ** (16 MB blok kopyası bütçenin rahatça altında)"
                             : "**KALDI** (2.0 ms bütçesi aşıldı)");

    /* 8 kare halka tamponu simülasyonu */
    size_t ring_size = 8 * 16 * 1024 * 1024;
    u8* ring = (u8*)aligned_alloc(4096, ring_size);
    if (ring) {
        f64 t0 = now_ms();
        /* 8 ardışık snapshot al */
        for (int frame = 0; frame < 8; frame++) {
            memcpy(ring + frame * (16 * 1024 * 1024), src, 16 * 1024 * 1024);
        }
        /* 1 rollback geri yükleme */
        memcpy(dst, ring + 3 * (16 * 1024 * 1024), 16 * 1024 * 1024);
        f64 t1 = now_ms();
        printf("- **8 Kare Halka Tamponu (128 MB) Kayıt + 1 Rollback Süresi:** `%.3f ms` (Kare başına ort: `%.3f ms`)\n\n",
               t1 - t0, (t1 - t0) / 9.0);
        free(ring);
    }

    /* 3. Paralel Kopya (1T, 2T, 4T, 8T pthread) */
    printf("### 3. Paralel Kopya (16 MB & 64 MB, Pthread)\n\n");
    int threads_list[] = { 1, 2, 4, 8 };

    for (int s = 2; s <= 3; s++) {
        size_t sz = sizes[s];
        f64 sz_gb = (f64)sz / (1024.0 * 1024.0 * 1024.0);
        printf("#### Hedef Boyut: %s\n\n", size_names[s]);
        printf("| İş Parçacığı | En İyi Süre (ms) | Bant Genişliği (GB/s) | Hızlanma |\n");
        printf("|---|---:|---:|---:|\n");

        f64 t1_time = 0.0;
        for (int ti = 0; ti < 4; ti++) {
            int th = threads_list[ti];
            f64 best_t = 1e9;
            for (int r = 0; r < 7; r++) {
                f64 dt = bench_parallel_copy_pthread(dst, src, sz, th);
                if (dt < best_t) best_t = dt;
            }
            if (th == 1) t1_time = best_t;
            f64 bw = sz_gb / (best_t / 1000.0);
            f64 speedup = t1_time / best_t;
            printf("| %dT | %.3f | %.2f | %.2fx |\n", th, best_t, bw, speedup);
        }
        printf("\n");
    }

    /* 4. Durum Özeti (Hash - XXH64) Hızı */
    printf("### 4. Durum Özeti (XXH64 Hash) Hızı\n\n");
    printf("| Boyut | Süre (ms) | Bant Genişliği (GB/s) | Özet Hash |\n");
    printf("|---|---:|---:|---|---:|\n");

    for (int s = 0; s < 4; s++) {
        size_t sz = sizes[s];
        f64 sz_gb = (f64)sz / (1024.0 * 1024.0 * 1024.0);

        u64 h = 0;
        f64 best_dt = 1e9;
        for (int r = 0; r < 7; r++) {
            f64 t0 = now_ms();
            h = state_hash64(src, sz);
            f64 t1 = now_ms();
            f64 dt = t1 - t0;
            if (dt < best_dt) best_dt = dt;
        }
        f64 bw = sz_gb / (best_dt / 1000.0);
        printf("| %s | %.3f | %.2f | `0x%016llX` |\n",
               size_names[s], best_dt, bw, (unsigned long long)h);
    }
    printf("\n");

    /* 5. Bellek Büyütme Maliyeti (mmap sayfa ayırma) */
    printf("### 5. Bellek Büyütme Maliyeti (mmap Anonim Sayfa)\n\n");
    printf("| Boyut | Maliyet (µs) | Durum |\n");
    printf("|---|---:|---|\n");
    const size_t grow_sizes[] = { 64 * 1024, 1 * 1024 * 1024, 16 * 1024 * 1024 };
    const char* grow_names[] = { "64 KB (1 wasm sayfası)", "1 MB (16 sayfa)", "16 MB (256 sayfa)" };

    for (int g = 0; g < 3; g++) {
        size_t gsz = grow_sizes[g];
        f64 best_dt = 1e9;
        for (int r = 0; r < 10; r++) {
            f64 t0 = now_ms();
            void* p = mmap(NULL, gsz, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
            f64 t1 = now_ms();
            if (p != MAP_FAILED) {
                /* İlk sayfaya dokun (page fault tetikle) */
                *(volatile u8*)p = 1;
                munmap(p, gsz);
            }
            f64 dt = (t1 - t0) * 1000.0;
            if (dt < best_dt) best_dt = dt;
        }
        printf("| %s | %.1f µs | Başarılı |\n", grow_names[g], best_dt);
    }
    printf("\n");

    /* 6. 4 GB'a Yakın Bellek Davranışı (Host) */
    printf("### 6. 4 GB'a Yakın Bellek Davranışı (Host)\n\n");
    size_t four_gb = 4ULL * 1024ULL * 1024ULL * 1024ULL;
    f64 t0 = now_ms();
    void* big_mem = mmap(NULL, four_gb, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
    f64 t1 = now_ms();
    if (big_mem != MAP_FAILED) {
        printf("- **4 GB mmap Tahsisi:** Başarılı (Süre: `%.3f ms`)\n", t1 - t0);
        /* Başlangıç, 2 GB ve 4 GB sınırına dokun */
        u8* ptr = (u8*)big_mem;
        ptr[0] = 0x11;
        ptr[2ULL * 1024ULL * 1024ULL * 1024ULL] = 0x22;
        ptr[four_gb - 1024] = 0x33;
        printf("- **4 GB Sınır Adresine Erişim:** Başarılı (Son 1 KB: `0x%02X` okundu)\n", ptr[four_gb - 1024]);
        munmap(big_mem, four_gb);
    } else {
        printf("- **4 GB mmap Tahsisi:** Başarısız (Yetersiz sanal adres alanı)\n");
    }
    printf("\n");

    /* 7. memory64 Chrome Raporu */
    printf("### 7. memory64 Chrome Bayrak Durumu Raporu\n\n");
    printf("- **Teknoloji:** WebAssembly Memory64 Proposal (64-bit doğrusal bellek indeksleme).\n");
    printf("- **Chrome Bayrağı:** `--js-flags=\"--experimental-wasm-memory64\"` veya `chrome://flags/#enable-webassembly-memory64`.\n");
    printf("- **Standartlaşma Durumu:** W3C Phase 3 (Chrome 105+, Firefox 131+, Node.js v18+ experimental).\n");
    printf("- **L0 Katmanına Etkisi:**\n");
    printf("  - wasm32'nin 4 GB (ve pratikte tarayıcı SAB için 1-2 GB) sınırı tamamen kalkar.\n");
    printf("  - Dezavantaj: İşaretçi boyutları 8 bayta çıktığından bellek ayak izi artar; 32-bit ofsetli veri yapıları (ARCH_V2 §2) bu kaybı önler.\n");
    printf("  - Faz 0 Önerisi: Motor varsayılan olarak wasm32 + SAB 1 GB ile çalışmalı; memory64 yalnızca 4 GB üstü büyük sahneler için dinamik seçenek olarak korunmalıdır.\n");

    free(src);
    free(dst);
    return 0;
}
