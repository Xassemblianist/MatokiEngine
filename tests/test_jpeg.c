/* tests/test_jpeg.c — JPEG çözücü (baseline + progressive) birim, referans, bozuk dosya, fuzz ve SIMD testleri.
 * tests/assets/ altındaki 9 test JPEG'ini (gri, 4:4:4, 4:2:2, 4:2:0, restart, progressive, 33x17, 1x1, 257x129)
 * ffmpeg referans PPM çıktılarıyla karşılaştırır, EXIF yön okumayı, bozuk dosya reddini, 50.000 mutasyonluk
 * ASan/UBSan fuzz testini ve wasm SIMD ile skaler sürümün bit-bit eşdeğerliğini doğrular.
 */
#include "test.h"
#include "core/arena.h"
#include "core/rng.h"
#include "core/log.h"
#include "plat/platform.h"
#include "raster/framebuffer.h"
#include "img/jpeg.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static u8 g_arena_mem[MB(32)];
static u8 g_file_buf[MB(2)];
static u8 g_ppm_buf[MB(2)];

static usize load_bin_file(const char* path, u8* dst, usize cap) {
    FILE* f = fopen(path, "rb");
    if (!f) return 0;
    usize n = fread(dst, 1, cap, f);
    fclose(f);
    return n;
}

static const u8* parse_ppm_header(const u8* buf, usize len, u32* w, u32* h) {
    if (len < 7 || buf[0] != 'P' || buf[1] != '6') return NULL;
    usize p = 2;
    u32 vals[3] = {0, 0, 0};
    for (u32 k = 0; k < 3; k++) {
        while (p < len && (buf[p] == ' ' || buf[p] == '\n' || buf[p] == '\r' || buf[p] == '\t' || buf[p] == '#')) {
            if (buf[p] == '#') {
                while (p < len && buf[p] != '\n') p++;
            } else {
                p++;
            }
        }
        if (p >= len || buf[p] < '0' || buf[p] > '9') return NULL;
        u32 v = 0;
        while (p < len && buf[p] >= '0' && buf[p] <= '9') {
            v = v * 10u + (u32)(buf[p] - '0');
            p++;
        }
        vals[k] = v;
    }
    if (p >= len || vals[2] != 255u) return NULL;
    p++; /* tek beyaz boşluk baytı */
    if (p + (usize)vals[0] * (usize)vals[1] * 3u > len) return NULL;
    *w = vals[0];
    *h = vals[1];
    return buf + p;
}

typedef struct {
    const char* name;
    b32         is_gray;
    u32         exp_w;
    u32         exp_h;
} JpegTestCase;

static const JpegTestCase k_cases[] = {
    { "gray",        TRUE,   48,  48 },
    { "color_444",   FALSE,  48,  48 },
    { "color_422",   FALSE,  48,  48 },
    { "color_420",   FALSE,  48,  48 },
    { "restart_420", FALSE,  48,  48 },
    { "prog_420",    FALSE,  48,  48 },
    { "odd_33x17",   FALSE,  33,  17 },
    { "odd_1x1",     FALSE,   1,   1 },
    { "odd_257x129", FALSE, 257, 129 },
};

TEST(jpeg_reference_comparison) {
    Arena a;
    arena_init(&a, g_arena_mem, sizeof(g_arena_mem));

    for (u32 ci = 0; ci < ARRAY_LEN(k_cases); ci++) {
        char jpg_path[128], ppm_path[128];
        snprintf(jpg_path, sizeof(jpg_path), "tests/assets/%s.jpg", k_cases[ci].name);
        snprintf(ppm_path, sizeof(ppm_path), "tests/assets/%s_ref.ppm", k_cases[ci].name);

        usize jpg_len = load_bin_file(jpg_path, g_file_buf, sizeof(g_file_buf));
        usize ppm_len = load_bin_file(ppm_path, g_ppm_buf, sizeof(g_ppm_buf));
        CHECK(jpg_len > 0 && ppm_len > 0);
        if (!jpg_len || !ppm_len) continue;

        u32 ref_w = 0, ref_h = 0;
        const u8* ref_rgb = parse_ppm_header(g_ppm_buf, ppm_len, &ref_w, &ref_h);
        CHECK(ref_rgb != NULL);
        CHECK(ref_w == k_cases[ci].exp_w && ref_h == k_cases[ci].exp_h);

        usize mark = arena_mark(&a);
        u32 w = 0, h = 0;
        u32* px = jpeg_decode(g_file_buf, jpg_len, &w, &h, &a);
        if (!px) {
            fprintf(stderr, "jpeg_decode(%s) hata: %s\n", k_cases[ci].name, jpeg_last_error());
        }
        CHECK(px != NULL);
        if (!px) continue;
        CHECK(w == ref_w && h == ref_h);
        CHECK(jpeg_last_error()[0] == '\0');
        CHECK(jpeg_orientation() == 1u);

        /* Geçici bellek sızıntısı olmamalı: arena'da yalnız w*h*4 bayt kalmalı */
        CHECK(arena_mark(&a) - mark == (usize)w * (usize)h * sizeof(u32));

        u64 sum_diff = 0;
        u32 max_diff = 0;
        u32 num_px = w * h;
        for (u32 i = 0; i < num_px; i++) {
            u32 c = px[i];
            i32 r = (i32)((c >>  0) & 0xFFu);
            i32 g = (i32)((c >>  8) & 0xFFu);
            i32 b = (i32)((c >> 16) & 0xFFu);
            i32 A = (i32)((c >> 24) & 0xFFu);
            CHECK(A == 255);

            i32 rr = (i32)ref_rgb[i * 3u + 0u];
            i32 rg = (i32)ref_rgb[i * 3u + 1u];
            i32 rb = (i32)ref_rgb[i * 3u + 2u];

            u32 dr = (u32)(r > rr ? r - rr : rr - r);
            u32 dg = (u32)(g > rg ? g - rg : rg - g);
            u32 db = (u32)(b > rb ? b - rb : rb - b);

            sum_diff += dr + dg + db;
            if (dr > max_diff) max_diff = dr;
            if (dg > max_diff) max_diff = dg;
            if (db > max_diff) max_diff = db;
        }

        f64 mean_diff = (f64)sum_diff / (f64)(num_px * 3u);
        u32 max_limit = k_cases[ci].is_gray ? 3u : 8u;
        if (mean_diff > 1.0 || max_diff > max_limit) {
            fprintf(stderr, "%s: mean_diff=%.4f, max_diff=%u (limit %u)\n",
                    k_cases[ci].name, mean_diff, max_diff, max_limit);
        }
        CHECK(mean_diff <= 1.0);
        CHECK(max_diff <= max_limit);

        arena_pop_to(&a, mark);
    }

    /* Progressive (prog_420) ve Baseline (color_420) aynı katsayıları kodladığından çıktıları birebir aynı olmalı */
    usize len_base = load_bin_file("tests/assets/color_420.jpg", g_file_buf, MB(1));
    usize len_prog = load_bin_file("tests/assets/prog_420.jpg", g_file_buf + MB(1), MB(1));
    u32 wb = 0, hb = 0, wp = 0, hp = 0;
    u32* px_base = jpeg_decode(g_file_buf, len_base, &wb, &hb, &a);
    u32* px_prog = jpeg_decode(g_file_buf + MB(1), len_prog, &wp, &hp, &a);
    CHECK(px_base != NULL && px_prog != NULL && wb == wp && hb == hp);
    if (px_base && px_prog) {
        CHECK(memcmp(px_base, px_prog, (usize)wb * (usize)hb * sizeof(u32)) == 0);
    }

    /* Kutu (nearest) kroma yükseltme seçeneği de çalışmalı */
    u32 wn = 0, hn = 0;
    u32* px_near = jpeg_decode_ex(g_file_buf, len_base, &wn, &hn, &a, FALSE);
    CHECK(px_near != NULL && wn == wb && hn == hb);
}

TEST(jpeg_exif_orientation) {
    Arena a;
    arena_init(&a, g_arena_mem, sizeof(g_arena_mem));

    usize base_len = load_bin_file("tests/assets/odd_1x1.jpg", g_file_buf, sizeof(g_file_buf));
    CHECK(base_len > 4);

    /* Hem Little-Endian (II) hem Big-Endian (MM) EXIF APP1 ile yön 1..8 testi */
    for (u32 endian = 0; endian < 2; endian++) {
        for (u32 orient = 1; orient <= 8; orient++) {
            u8 app1[36];
            /* Segment başlığı: FF E1 + uzunluk (34 bayt = 0x0022) */
            app1[0] = 0xFFu; app1[1] = 0xE1u;
            app1[2] = 0x00u; app1[3] = 34u;
            /* "Exif\0\0" */
            memcpy(app1 + 4, "Exif\0\0", 6);
            u8* tiff = app1 + 10;
            memset(tiff, 0, 26);
            if (endian == 0) {
                /* Little-Endian (II) */
                tiff[0] = 'I'; tiff[1] = 'I';
                tiff[2] = 42u; tiff[3] = 0u;
                tiff[4] = 8u;  /* ifd0 offset = 8 */
                tiff[8] = 1u;  /* 1 IFD girdisi */
                tiff[10] = 0x12u; tiff[11] = 0x01u; /* Tag 0x0112 (Orientation) */
                tiff[12] = 3u;    tiff[13] = 0u;    /* Type 3 (SHORT) */
                tiff[14] = 1u;                      /* Count = 1 */
                tiff[18] = (u8)orient;              /* Value */
            } else {
                /* Big-Endian (MM) */
                tiff[0] = 'M'; tiff[1] = 'M';
                tiff[2] = 0u;  tiff[3] = 42u;
                tiff[7] = 8u;  /* ifd0 offset = 8 */
                tiff[9] = 1u;  /* 1 IFD girdisi */
                tiff[10] = 0x01u; tiff[11] = 0x12u; /* Tag 0x0112 */
                tiff[12] = 0u;    tiff[13] = 3u;    /* Type 3 (SHORT) */
                tiff[17] = 1u;                      /* Count = 1 */
                tiff[19] = (u8)orient;              /* Value */
            }

            usize mark = arena_mark(&a);
            u8* mod = ARENA_PUSH_ARRAY(&a, u8, base_len + sizeof(app1));
            mod[0] = 0xFFu; mod[1] = 0xD8u;
            memcpy(mod + 2, app1, sizeof(app1));
            memcpy(mod + 2 + sizeof(app1), g_file_buf + 2, base_len - 2);

            u32 w = 0, h = 0;
            u32* px = jpeg_decode(mod, base_len + sizeof(app1), &w, &h, &a);
            CHECK(px != NULL && w == 1u && h == 1u);
            CHECK(jpeg_orientation() == orient);
            arena_pop_to(&a, mark);
        }
    }
}

TEST(jpeg_corrupt_and_unsupported_reject) {
    Arena a;
    arena_init(&a, g_arena_mem, sizeof(g_arena_mem));

    usize valid_len = load_bin_file("tests/assets/color_420.jpg", g_file_buf, sizeof(g_file_buf));
    CHECK(valid_len > 100);

    u32 w = 0, h = 0;
    usize mark = arena_mark(&a);

    /* 1) NULL / çok kısa / yanlış SOI */
    CHECK(jpeg_decode(NULL, 100, &w, &h, &a) == NULL && jpeg_last_error()[0] != '\0');
    CHECK(jpeg_decode(g_file_buf, 2, &w, &h, &a) == NULL && jpeg_last_error()[0] != '\0');
    u8 bad_soi[8] = { 0x89, 'P', 'N', 'G', 0, 0, 0, 0 };
    CHECK(jpeg_decode(bad_soi, sizeof(bad_soi), &w, &h, &a) == NULL && jpeg_last_error()[0] != '\0');

    /* 2) Kesik dosyalar (çeşitli noktalardan kesilmiş) */
    const usize cuts[] = { 4, 10, 25, 60, valid_len / 4, valid_len / 2, valid_len - 2 };
    for (u32 i = 0; i < ARRAY_LEN(cuts); i++) {
        CHECK(jpeg_decode(g_file_buf, cuts[i], &w, &h, &a) == NULL);
        CHECK(jpeg_last_error()[0] != '\0');
        CHECK(arena_mark(&a) == mark);
    }

    /* 3) Aşırı boyut (w = 30000, h = 30000) ve sıfır boyut (w = 0, h = 16) */
    const u8 huge_sof[] = {
        0xFF, 0xD8,
        0xFF, 0xC0, 0x00, 0x0B, 8, 0x75, 0x30, 0x75, 0x30, 1, 1, 0x11, 0,
        0xFF, 0xD9
    };
    CHECK(jpeg_decode(huge_sof, sizeof(huge_sof), &w, &h, &a) == NULL && jpeg_last_error()[0] != '\0');

    const u8 zero_sof[] = {
        0xFF, 0xD8,
        0xFF, 0xC0, 0x00, 0x0B, 8, 0x00, 0x00, 0x00, 0x10, 1, 1, 0x11, 0,
        0xFF, 0xD9
    };
    CHECK(jpeg_decode(zero_sof, sizeof(zero_sof), &w, &h, &a) == NULL && jpeg_last_error()[0] != '\0');

    /* 4) 12-bit SOF0, CMYK (4 bileşen), Adobe APP14 ve Aritmetik kodlama (SOF9) */
    const u8 sof_12bit[] = {
        0xFF, 0xD8,
        0xFF, 0xC0, 0x00, 0x0B, 12, 0x00, 0x08, 0x00, 0x08, 1, 1, 0x11, 0,
        0xFF, 0xD9
    };
    CHECK(jpeg_decode(sof_12bit, sizeof(sof_12bit), &w, &h, &a) == NULL && jpeg_last_error()[0] != '\0');

    const u8 sof_cmyk[] = {
        0xFF, 0xD8,
        0xFF, 0xC0, 0x00, 0x14, 8, 0x00, 0x08, 0x00, 0x08, 4,
        1, 0x11, 0, 2, 0x11, 0, 3, 0x11, 0, 4, 0x11, 0,
        0xFF, 0xD9
    };
    CHECK(jpeg_decode(sof_cmyk, sizeof(sof_cmyk), &w, &h, &a) == NULL && jpeg_last_error()[0] != '\0');

    const u8 adobe_app14[] = {
        0xFF, 0xD8,
        0xFF, 0xEE, 0x00, 0x0E, 'A', 'd', 'o', 'b', 'e', 0, 100, 0, 0, 0, 0, 1,
        0xFF, 0xD9
    };
    CHECK(jpeg_decode(adobe_app14, sizeof(adobe_app14), &w, &h, &a) == NULL && jpeg_last_error()[0] != '\0');

    const u8 sof_arith[] = {
        0xFF, 0xD8,
        0xFF, 0xC9, 0x00, 0x0B, 8, 0x00, 0x08, 0x00, 0x08, 1, 1, 0x11, 0,
        0xFF, 0xD9
    };
    CHECK(jpeg_decode(sof_arith, sizeof(sof_arith), &w, &h, &a) == NULL && jpeg_last_error()[0] != '\0');

    /* 5) Sıfır (boş) Huffman tablosu ve aşırı abone (oversubscribed) Huffman tablosu */
    const u8 zero_dht[] = {
        0xFF, 0xD8,
        0xFF, 0xC4, 0x00, 0x13, 0x00,
        0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
        0xFF, 0xD9
    };
    CHECK(jpeg_decode(zero_dht, sizeof(zero_dht), &w, &h, &a) == NULL && jpeg_last_error()[0] != '\0');

    const u8 over_dht[] = {
        0xFF, 0xD8,
        0xFF, 0xC4, 0x00, 0x16, 0x00,
        3, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, /* 1-bit'lik 3 kod imkansız */
        0, 1, 2,
        0xFF, 0xD9
    };
    CHECK(jpeg_decode(over_dht, sizeof(over_dht), &w, &h, &a) == NULL && jpeg_last_error()[0] != '\0');
    CHECK(arena_mark(&a) == mark);
}

TEST(jpeg_fuzz_50000) {
    Arena a;
    arena_init(&a, g_arena_mem, sizeof(g_arena_mem));

    static u8 corpus[ARRAY_LEN(k_cases)][16384];
    usize corpus_len[ARRAY_LEN(k_cases)];
    for (u32 i = 0; i < ARRAY_LEN(k_cases); i++) {
        char path[128];
        snprintf(path, sizeof(path), "tests/assets/%s.jpg", k_cases[i].name);
        corpus_len[i] = load_bin_file(path, corpus[i], sizeof(corpus[i]));
        CHECK(corpus_len[i] > 0 && corpus_len[i] <= sizeof(corpus[i]));
    }

    static u8 mut_buf[16384];
    Rng rng;
    rng_seed(&rng, 0x9E3779B97F4A7C15ULL);

    u32 accepted = 0;
    for (u32 it = 0; it < 50000u; it++) {
        u32 ci = it % (u32)ARRAY_LEN(k_cases);
        usize n = corpus_len[ci];
        memcpy(mut_buf, corpus[ci], n);

        /* Her iterasyonda 1..6 rastgele bayt mutasyonu veya uzunluk kırpması */
        u32 muts = 1u + (rng_u32(&rng) % 6u);
        for (u32 m = 0; m < muts; m++) {
            usize idx = (usize)(rng_u32(&rng) % (u32)n);
            mut_buf[idx] = (u8)(rng_u32(&rng) & 0xFFu);
        }
        usize test_len = n;
        if ((rng_u32(&rng) & 7u) == 0u) {
            test_len = 1u + (usize)(rng_u32(&rng) % (u32)n);
        }

        usize mark = arena_mark(&a);
        u32 w = 0, h = 0;
        u32* px = jpeg_decode(mut_buf, test_len, &w, &h, &a);
        if (px) {
            CHECK(w > 0 && h > 0 && w <= 16384u && h <= 16384u);
            /* ASan altında ilk ve son piksele erişimin geçerliliğini doğrula */
            volatile u32 first = px[0];
            volatile u32 last  = px[(usize)w * (usize)h - 1u];
            UNUSED(first);
            UNUSED(last);
            accepted++;
        } else {
            CHECK(jpeg_last_error()[0] != '\0');
        }
        arena_pop_to(&a, mark);
    }
    printf("jpeg fuzz: 50000 mutasyon, %u gecerli kabul edildi, cokme yok\n", accepted);
}

TEST(jpeg_wasm_simd_bit_exact) {
    Arena a;
    arena_init(&a, g_arena_mem, sizeof(g_arena_mem));

    /* Wasm SIMD derlemesini ve tüm 9 test JPEG'inde Host skaler ile bit-bit aynılığını doğrula */
    int rc = system(
        "node -e '\n"
        "const fs = require(\"fs\");\n"
        "const os = require(\"os\");\n"
        "const path = require(\"path\");\n"
        "const { execFileSync } = require(\"child_process\");\n"
        "const tmp = fs.mkdtempSync(path.join(os.tmpdir(), \"matoki_jpeg_simd_\"));\n"
        "try {\n"
        "  const cCode = `\n"
        "    #include \"core/types.h\"\n"
        "    #include \"core/arena.h\"\n"
        "    #include \"plat/platform.h\"\n"
        "    #include \"img/jpeg.h\"\n"
        "    static u8 g_buf[16 * 1024 * 1024];\n"
        "    static u8 g_in[262144];\n"
        "    static u32 g_w, g_h;\n"
        "    EXPORT(get_in_ptr) u8* get_in_ptr(void) { return g_in; }\n"
        "    EXPORT(get_w) u32 get_w(void) { return g_w; }\n"
        "    EXPORT(get_h) u32 get_h(void) { return g_h; }\n"
        "    EXPORT(run_decode) u32* run_decode(u32 len) {\n"
        "      Arena a;\n"
        "      arena_init(&a, g_buf, sizeof(g_buf));\n"
        "      return jpeg_decode(g_in, len, &g_w, &g_h, &a);\n"
        "    }\n"
        "  `;\n"
        "  const cPath = path.join(tmp, \"harness.c\");\n"
        "  const wasmPath = path.join(tmp, \"harness.wasm\");\n"
        "  fs.writeFileSync(cPath, cCode);\n"
        "  execFileSync(\"clang\", [\n"
        "    \"--target=wasm32\", \"-std=c11\", \"-O2\", \"-ffreestanding\", \"-fno-builtin\", \"-nostdlib\",\n"
        "    \"-msimd128\", \"-mbulk-memory\", \"-mnontrapping-fptoint\", \"-msign-ext\", \"-Isrc\",\n"
        "    \"-Wl,--no-entry\", \"-Wl,--export-dynamic\", \"-Wl,--initial-memory=33554432\",\n"
        "    \"-Wl,--stack-first\", \"-z\", \"stack-size=1048576\",\n"
        "    cPath, \"src/core/arena.c\", \"src/core/log.c\", \"src/core/mem.c\", \"src/img/jpeg.c\", \"src/img/jpeg_wasm.c\",\n"
        "    \"-o\", wasmPath\n"
        "  ], { stdio: \"inherit\" });\n"
        "  const mod = new WebAssembly.Module(fs.readFileSync(wasmPath));\n"
        "  const inst = new WebAssembly.Instance(mod, {\n"
        "    env: {\n"
        "      plat_log: () => {},\n"
        "      plat_panic: () => { throw new Error(\"plat_panic\"); },\n"
        "      plat_time_ms: () => 0,\n"
        "      plat_jit_load: () => -1\n"
        "    }\n"
        "  });\n"
        "  const names = [\"gray\",\"color_444\",\"color_422\",\"color_420\",\"restart_420\",\"prog_420\",\"odd_33x17\",\"odd_1x1\",\"odd_257x129\"];\n"
        "  const inPtr = inst.exports.get_in_ptr();\n"
        "  for (const name of names) {\n"
        "    const jpg = fs.readFileSync(`tests/assets/${name}.jpg`);\n"
        "    new Uint8Array(inst.exports.memory.buffer, inPtr, jpg.length).set(jpg);\n"
        "    const outPtr = inst.exports.run_decode(jpg.length);\n"
        "    if (!outPtr) throw new Error(`wasm decode failed: ${name}`);\n"
        "    const w = inst.exports.get_w(), h = inst.exports.get_h();\n"
        "    const outBytes = Buffer.from(inst.exports.memory.buffer, outPtr, w * h * 4);\n"
        "    fs.writeFileSync(`build/jpeg_wasm_${name}.bin`, outBytes);\n"
        "  }\n"
        "} finally {\n"
        "  fs.rmSync(tmp, { recursive: true, force: true });\n"
        "}\n"
        "'");
    CHECK(rc == 0);
    if (rc != 0) return;

    for (u32 ci = 0; ci < ARRAY_LEN(k_cases); ci++) {
        char jpg_path[128], bin_path[128];
        snprintf(jpg_path, sizeof(jpg_path), "tests/assets/%s.jpg", k_cases[ci].name);
        snprintf(bin_path, sizeof(bin_path), "build/jpeg_wasm_%s.bin", k_cases[ci].name);

        usize jpg_len = load_bin_file(jpg_path, g_file_buf, sizeof(g_file_buf));
        usize bin_len = load_bin_file(bin_path, g_ppm_buf, sizeof(g_ppm_buf));
        remove(bin_path);

        usize mark = arena_mark(&a);
        u32 w = 0, h = 0;
        u32* host_px = jpeg_decode(g_file_buf, jpg_len, &w, &h, &a);
        CHECK(host_px != NULL);
        CHECK(bin_len == (usize)w * (usize)h * sizeof(u32));
        if (host_px && bin_len == (usize)w * (usize)h * sizeof(u32)) {
            CHECK(memcmp(host_px, g_ppm_buf, bin_len) == 0);
        }
        arena_pop_to(&a, mark);
    }
}

static f64 median_f64(f64* arr, u32 n) {
    for (u32 i = 1; i < n; i++) {
        f64 k = arr[i];
        i32 j = (i32)i - 1;
        while (j >= 0 && arr[j] > k) {
            arr[j + 1] = arr[j];
            j--;
        }
        arr[j + 1] = k;
    }
    return arr[n / 2u];
}

TEST(jpeg_bench_1024_420) {
    Arena a;
    arena_init(&a, g_arena_mem, sizeof(g_arena_mem));

    int rc = system("python3 tests/assets/make_jpeg.py --bench build/jpeg_bench");
    CHECK(rc == 0);
    if (rc != 0) return;

    usize len_base = load_bin_file("build/jpeg_bench/bench_1024_base.jpg", g_file_buf, MB(1));
    usize len_prog = load_bin_file("build/jpeg_bench/bench_1024_prog.jpg", g_ppm_buf, MB(1));
    remove("build/jpeg_bench/bench_1024_base.jpg");
    remove("build/jpeg_bench/bench_1024_prog.jpg");
    CHECK(len_base > 0 && len_prog > 0);
    if (!len_base || !len_prog) return;

    /* 1024x1024 baseline ve progressive çıktılarının birebir aynı olduğunu doğrula */
    usize mark = arena_mark(&a);
    u32 wb = 0, hb = 0, wp = 0, hp = 0;
    u32* px_b = jpeg_decode(g_file_buf, len_base, &wb, &hb, &a);
    u32* px_p = jpeg_decode(g_ppm_buf, len_prog, &wp, &hp, &a);
    CHECK(px_b != NULL && px_p != NULL);
    CHECK(wb == 1024u && hb == 1024u && wp == 1024u && hp == 1024u);
    if (px_b && px_p) {
        CHECK(memcmp(px_b, px_p, 1024u * 1024u * sizeof(u32)) == 0);
    }
    arena_pop_to(&a, mark);

    /* Isınma (3 tur) + Ölçüm (11 tur medyan) */
    const u32 WARMUP = 3u;
    const u32 ITERS  = 11u;
    f64 t_base[11], t_prog[11];

    for (u32 i = 0; i < WARMUP + ITERS; i++) {
        u32 w = 0, h = 0;
        f64 t0 = plat_time_ms();
        u32* px = jpeg_decode(g_file_buf, len_base, &w, &h, &a);
        f64 dt = plat_time_ms() - t0;
        CHECK(px != NULL && w == 1024u && h == 1024u);
        arena_pop_to(&a, mark);
        if (i >= WARMUP) t_base[i - WARMUP] = dt;
    }

    for (u32 i = 0; i < WARMUP + ITERS; i++) {
        u32 w = 0, h = 0;
        f64 t0 = plat_time_ms();
        u32* px = jpeg_decode(g_ppm_buf, len_prog, &w, &h, &a);
        f64 dt = plat_time_ms() - t0;
        CHECK(px != NULL && w == 1024u && h == 1024u);
        arena_pop_to(&a, mark);
        if (i >= WARMUP) t_prog[i - WARMUP] = dt;
    }

    f64 med_base = median_f64(t_base, ITERS);
    f64 med_prog = median_f64(t_prog, ITERS);
    f64 mp = (1024.0 * 1024.0) / 1e6;
    f64 mps_base = mp / (med_base / 1000.0);
    f64 mps_prog = mp / (med_prog / 1000.0);
    LOG("jpeg 1024x1024 4:2:0 bench (ASan/O1): baseline %.2f ms (%.1f MP/s), progressive %.2f ms (%.1f MP/s)",
        med_base, mps_base, med_prog, mps_prog);
}

/* İnceleme (Claude): başlıkta dev boyut yazan küçük bir JPEG arena'yı taşırıp PANIC yerine NULL + hata döndürmeli */
TEST(jpeg_review_huge_header_small_arena) {
    static u8 file[4096];
    FILE* f = fopen("tests/assets/odd_33x17.jpg", "rb");
    CHECK(f != NULL);
    if (!f) return;
    u32 n = (u32)fread(file, 1, sizeof(file), f);
    fclose(f);
    /* SOF0 (FFC0) bul: FF C0 Lh Ll P Hh Hl Wh Wl */
    u32 sof = 0;
    for (u32 i = 2; i + 9 < n; i++) {
        if (file[i] == 0xFF && (file[i + 1] == 0xC0 || file[i + 1] == 0xC1 || file[i + 1] == 0xC2)) { sof = i; break; }
    }
    CHECK(sof != 0);
    file[sof + 5] = 0x1F; file[sof + 6] = 0x40;   /* yükseklik 8000 */
    file[sof + 7] = 0x1F; file[sof + 8] = 0x40;   /* genişlik 8000 → 256 MB çıktı */
    static u8 mem[MB(8)];
    Arena a;
    arena_init(&a, mem, sizeof(mem));
    u32 w = 0, h = 0;
    u32* px = jpeg_decode(file, n, &w, &h, &a);
    CHECK(px == NULL);
    CHECK(jpeg_last_error()[0] != 0);
    CHECK(a.used == 0);
}
