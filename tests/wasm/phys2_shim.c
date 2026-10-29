/* tests/wasm/phys2_shim.c — phys2 testlerini wasm'da (SIMD + gerçek thread) koşturan düzenek (test_phys2_wasm.mjs).
 * tests/test.h kayıtlarını toplar, test_phys2_common'ın wasm karşılığını verir, run_tests ile çalıştırır.
 * Host test koşucusuna girmez (Makefile yalnızca tests/ üst dizinindeki .c dosyalarını toplar); libc yok.
 * SamurayBalgami/Matoki'den uyarlandı (izinle). */
#include "core/types.h"
#include "core/arena.h"
#include "core/log.h"
#include "plat/platform.h"
#include "test.h"
#include "test_phys2_common.h"

typedef struct { const char* name; TestFn fn; } Entry;
static Entry g_tests[512];
static u32 g_count;
static u32 g_fails;
static u32 g_cur_fail;
static const char* g_cur_name;

void test_register(const char* name, const char* file, TestFn fn) {
    (void)file;
    if (g_count < 512) { g_tests[g_count].name = name; g_tests[g_count].fn = fn; g_count++; }
}
void test_fail(const char* file, i32 line, const char* expr) {
    g_cur_fail++;
    LOG("    %s:%d: CHECK(%s)", file, line, expr);
}
void test_fail_near(const char* file, i32 line, const char* expr, f64 a, f64 b, f64 eps) {
    g_cur_fail++;
    LOG("    %s:%d: CHECK_NEAR(%s): %f vs %f (eps %f)", file, line, expr, a, b, eps);
}

static _Alignas(64) u8 g_mem[MB(200)];   /* test_arena() */
static Arena g_arena;
Arena* test_arena(void) { if (!g_arena.base) arena_init(&g_arena, g_mem, sizeof(g_mem)); return &g_arena; }
void test_arena_reset(void) { arena_reset(test_arena()); }
void test_note(const char* label, f64 value, u32 decimals) { (void)decimals; LOG("  %s: %f", label, value); }
void p2t_sb_init(P2tBuf* s, char* mem, u32 cap) { s->p = mem; s->len = 0; s->cap = cap; if (cap) mem[0] = 0; }
void p2t_sb_char(P2tBuf* s, char c) { if (s->len + 1 < s->cap) { s->p[s->len++] = c; s->p[s->len] = 0; } }
void p2t_sb_cstr(P2tBuf* s, const char* str) { for (; *str; str++) p2t_sb_char(s, *str); }
void p2t_sb_u64(P2tBuf* s, u64 v) { char t[24]; u32 n = 0; do { t[n++] = (char)('0' + v % 10); v /= 10; } while (v); while (n) p2t_sb_char(s, t[--n]); }
void p2t_sb_i64(P2tBuf* s, i64 v) { if (v < 0) { p2t_sb_char(s, '-'); v = -v; } p2t_sb_u64(s, (u64)v); }
void p2t_sb_f64(P2tBuf* s, f64 v, u32 d) { (void)d; p2t_sb_i64(s, (i64)v); }
void p2t_sb_hex32(P2tBuf* s, u32 v) { p2t_sb_u64(s, v); }
void p2t_log(const P2tBuf* s) { (void)s; }

extern void __wasm_call_ctors(void);
static u32 str_has(const char* s, const char* sub) {
    if (!*sub) return 1;
    for (; *s; s++) { const char *a = s, *b = sub; while (*a && *b && *a == *b) { a++; b++; } if (!*b) return 1; }
    return 0;
}
static char g_filter[64];
EXPORT(filter_buf) char* filter_buf(void) { return g_filter; }
EXPORT(test_count) u32 test_count(void) { return g_count; }
EXPORT(run_tests) u32 run_tests(void) {
    u32 ran = 0;
    g_fails = 0;
    for (u32 i = 0; i < g_count; i++) {
        if (!str_has(g_tests[i].name, g_filter)) continue;
        g_cur_fail = 0;
        g_cur_name = g_tests[i].name;
        f64 t0 = plat_time_ms();
        g_tests[i].fn();
        f64 dt = plat_time_ms() - t0;
        if (g_cur_fail) { LOG("KALDI  %s", g_tests[i].name); g_fails++; }
        else LOG("ok     %s  (%d ms)", g_tests[i].name, (i32)dt);
        ran++;
    }
    LOG("%u koştu, %u kaldı", ran, g_fails);
    return g_fails;
}
