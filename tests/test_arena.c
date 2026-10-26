/* tests/test_arena.c — Arena bellek ayırıcı birim testleri.
 * Hizalama (1..256), sıfırlama, mark/pop_to, alt arena ve taşma sınırlarını doğrular.
 */
#include "test.h"
#include "core/arena.h"

static _Alignas(256) u8 g_test_buf[KB(64)];

TEST(arena_align_and_zero) {
    Arena a;
    for (usize i = 0; i < sizeof(g_test_buf); i++) {
        g_test_buf[i] = 0xAA;
    }
    arena_init(&a, g_test_buf, sizeof(g_test_buf));

    for (usize align = 1; align <= 256; align <<= 1) {
        /* Hizalamayı bozmak için önce 1 byte ayır */
        u8* dummy = (u8*)arena_push(&a, 1, 1);
        CHECK(*dummy == 0);
        *dummy = 0xFF;

        u8* p = (u8*)arena_push(&a, 37, align);
        CHECK(((uptr)p & (align - 1)) == 0);
        for (usize j = 0; j < 37; j++) {
            CHECK(p[j] == 0);
            p[j] = 0x55;
        }
    }
}

typedef struct {
    u64 a;
    u32 b;
    u8  c;
} TestStruct;

TEST(arena_mark_pop_and_reset) {
    Arena a;
    arena_init(&a, g_test_buf, sizeof(g_test_buf));

    TestStruct* s1 = ARENA_PUSH_STRUCT(&a, TestStruct);
    CHECK(s1 != NULL);
    s1->a = 42;

    usize m = arena_mark(&a);
    u32* arr = ARENA_PUSH_ARRAY(&a, u32, 100);
    for (u32 i = 0; i < 100; i++) arr[i] = i + 1;
    CHECK(a.used >= m + 400);

    arena_pop_to(&a, m);
    CHECK(a.used == m);
    CHECK(s1->a == 42);

    /* Yeniden ayrılan bellek sıfırlanmış olmalı */
    u32* arr2 = ARENA_PUSH_ARRAY(&a, u32, 100);
    CHECK(arr2 == arr);
    for (u32 i = 0; i < 100; i++) CHECK(arr2[i] == 0);

    arena_reset(&a);
    CHECK(a.used == 0);
}

TEST(arena_sub_and_bounds) {
    Arena a;
    arena_init(&a, g_test_buf, 1024);

    Arena sub = arena_sub(&a, 256);
    CHECK(sub.cap == 256);
    CHECK(sub.used == 0);
    CHECK(((uptr)sub.base & 15) == 0);

    u8* p = ARENA_PUSH_ARRAY(&sub, u8, 256);
    CHECK(p != NULL);
    CHECK(sub.used == 256);
    /* Kalan kapasite 0: PANIC çağırmadan taşma sınırını doğrula */
    CHECK(sub.cap - sub.used == 0);

    /* Ana arenada kalan kapasiteyi tam doldur */
    usize rem = a.cap - a.used;
    u8* rest = (u8*)arena_push(&a, rem, 1);
    CHECK(rest != NULL);
    CHECK(a.used == a.cap);
    CHECK(a.cap - a.used == 0);
}
