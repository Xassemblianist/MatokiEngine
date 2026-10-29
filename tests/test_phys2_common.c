/* tests/test_phys2_common.c — phys2 testlerinin ortak yardımcıları (arena, not satırları, metin tamponu).
 * Not satırları yalnızca PHYS2_TEST_VERBOSE ortam değişkeni ayarlıysa yazdırılır (make test sessiz kalır).
 * SamurayBalgami/Matoki'den uyarlandı (izinle).
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "test_phys2_common.h"

#define TEST_ARENA_BYTES MB(256)

static u8 g_test_mem[TEST_ARENA_BYTES] __attribute__((aligned(64)));
static Arena g_test_arena;

Arena* test_arena(void) {
    if (!g_test_arena.base) arena_init(&g_test_arena, g_test_mem, sizeof(g_test_mem));
    return &g_test_arena;
}

void test_arena_reset(void) { arena_reset(test_arena()); }

static int verbose(void) {
    static int v = -1;
    if (v < 0) v = getenv("PHYS2_TEST_VERBOSE") != NULL;
    return v;
}

void test_note(const char* label, f64 value, u32 decimals) {
    if (verbose()) printf("  %s: %.*f\n", label, (int)decimals, value);
}

void p2t_sb_init(P2tBuf* s, char* mem, u32 cap) { s->p = mem; s->len = 0; s->cap = cap; if (cap) mem[0] = 0; }

void p2t_sb_char(P2tBuf* s, char c) {
    if (s->len + 1 < s->cap) { s->p[s->len++] = c; s->p[s->len] = 0; }
}

void p2t_sb_cstr(P2tBuf* s, const char* str) { for (; *str; str++) p2t_sb_char(s, *str); }

static void sb_fmt(P2tBuf* s, const char* fmt, ...) __attribute__((format(printf, 2, 3)));
static void sb_fmt(P2tBuf* s, const char* fmt, ...) {
    char tmp[64];
    __builtin_va_list ap;
    __builtin_va_start(ap, fmt);
    vsnprintf(tmp, sizeof(tmp), fmt, ap);
    __builtin_va_end(ap);
    p2t_sb_cstr(s, tmp);
}

void p2t_sb_u64(P2tBuf* s, u64 v) { sb_fmt(s, "%llu", (unsigned long long)v); }
void p2t_sb_i64(P2tBuf* s, i64 v) { sb_fmt(s, "%lld", (long long)v); }
void p2t_sb_f64(P2tBuf* s, f64 v, u32 decimals) { sb_fmt(s, "%.*f", (int)decimals, v); }
void p2t_sb_hex32(P2tBuf* s, u32 v) { sb_fmt(s, "%08x", v); }

void p2t_log(const P2tBuf* s) {
    if (verbose()) printf("%s\n", s->p);
}
