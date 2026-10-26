/* tests/test_rng.c — PCG32 ve profiler birim testleri. */
#include "test.h"
#include "core/rng.h"
#include "core/profiler.h"

TEST(pcg32_official_sequence) {
    /* Resmi PCG32 referans çıktısı: pcg32_srandom_r(&rng, 42u, 54u) */
    static const u32 expected[6] = {
        0xa15c02b7u,
        0x7b47f409u,
        0xba1d3330u,
        0x83d2f293u,
        0xbfa4784bu,
        0xcbed606eu
    };

    Rng r;
    rng_seed_seq(&r, 42ULL, 54ULL);
    for (u32 i = 0; i < 6; i++) {
        CHECK(rng_u32(&r) == expected[i]);
    }

    /* rng_seed varsayılan akış olarak 54ULL kullanır */
    Rng r2;
    rng_seed(&r2, 42ULL);
    for (u32 i = 0; i < 6; i++) {
        CHECK(rng_u32(&r2) == expected[i]);
    }
}

TEST(rng_f32_and_range) {
    Rng r;
    rng_seed(&r, 123456ULL);

    f32 min_f = 1.0f;
    f32 max_f = 0.0f;
    for (u32 i = 0; i < 10000; i++) {
        f32 v = rng_f32(&r);
        CHECK(v >= 0.0f && v < 1.0f);
        if (v < min_f) min_f = v;
        if (v > max_f) max_f = v;
    }
    CHECK(min_f < 0.01f);
    CHECK(max_f > 0.99f);

    u32 buckets[10] = {0};
    for (u32 i = 0; i < 10000; i++) {
        i32 v = rng_range(&r, -5, 5);
        CHECK(v >= -5 && v < 5);
        buckets[v + 5]++;
    }
    for (u32 i = 0; i < 10; i++) {
        CHECK(buckets[i] > 800 && buckets[i] < 1200);
    }

    CHECK(rng_range(&r, 7, 7) == 7);
}

TEST(profiler_nested_zones) {
    prof_reset();
    CHECK(prof_zone_count() == 0);

    for (u32 f = 0; f < 3; f++) {
        PROF_BEGIN("frame_outer");
        for (u32 i = 0; i < 4; i++) {
            PROF_BEGIN("inner_step");
            volatile u32 acc = 0;
            for (u32 k = 0; k < 200; k++) acc += k;
            (void)acc;
            PROF_END();
        }
        PROF_END();
        prof_frame_end();
    }

    CHECK(prof_frame_count() == 3);
    CHECK(prof_zone_count() == 2);

    const ProfZone* z0 = prof_zone_get(0);
    const ProfZone* z1 = prof_zone_get(1);
    CHECK(z0 != NULL && z1 != NULL);
    CHECK(z0->last_calls == 1);
    CHECK(z0->total_calls == 3);
    CHECK(z1->last_calls == 4);
    CHECK(z1->total_calls == 12);
    CHECK(z0->total_ms >= 0.0);
    CHECK(z1->total_ms >= 0.0);
}
