/* tests/test_fixed.c — Fx8 (24.8) ve Fx16 (16.16) sabit nokta testleri. */
#include "test.h"
#include "core/fixed.h"

TEST(fx8_conversions_and_rounding) {
    CHECK(fx8_from_i32(0) == 0);
    CHECK(fx8_from_i32(1) == 256);
    CHECK(fx8_from_i32(-3) == -768);
    CHECK(fx8_to_i32(fx8_from_i32(42)) == 42);
    CHECK(fx8_to_i32(fx8_from_i32(-42)) == -42);

    Fx8 a = fx8_from_f32(3.25f);
    CHECK(a == 3 * 256 + 64);
    CHECK_NEAR(fx8_to_f32(a), 3.25f, 1e-6);
    CHECK(fx8_floor_i32(a) == 3);
    CHECK(fx8_ceil_i32(a) == 4);
    CHECK(fx8_round_i32(a) == 3);
    CHECK(fx8_frac(a) == 64);

    Fx8 b = fx8_from_f32(-2.75f);
    CHECK(b == -2 * 256 - 192);
    CHECK_NEAR(fx8_to_f32(b), -2.75f, 1e-6);
    CHECK(fx8_floor_i32(b) == -3);
    CHECK(fx8_ceil_i32(b) == -2);
    CHECK(fx8_round_i32(b) == -3);

    Fx8 exact = fx8_from_i32(7);
    CHECK(fx8_floor(exact) == exact);
    CHECK(fx8_ceil(exact) == exact);
    CHECK(fx8_round(exact) == exact);
    CHECK(fx8_frac(exact) == 0);
}

TEST(fx8_mul_and_div_i64_intermediate) {
    Fx8 a = fx8_from_f32(12.5f);
    Fx8 b = fx8_from_f32(-4.25f);
    Fx8 prod = fx8_mul(a, b);
    CHECK_NEAR(fx8_to_f32(prod), -53.125f, 1.0f / 256.0f);

    Fx8 quot = fx8_div(prod, a);
    CHECK_NEAR(fx8_to_f32(quot), -4.25f, 1.0f / 256.0f);

    /* i32 çarpımında taşacak büyük sayılar (i64 ara çarpım doğrulaması) */
    Fx8 big1 = fx8_from_i32(20000);
    Fx8 big2 = fx8_from_f32(1.5f);
    Fx8 big_prod = fx8_mul(big1, big2);
    CHECK(fx8_to_i32(big_prod) == 30000);
    CHECK(fx8_to_i32(fx8_div(big_prod, big2)) == 20000);
}

TEST(fx16_operations) {
    Fx16 a = fx16_from_f32(1.5f);
    Fx16 b = fx16_from_f32(2.25f);
    CHECK(a == (1 << 16) + (1 << 15));
    CHECK_NEAR(fx16_to_f32(fx16_mul(a, b)), 3.375f, 1.0f / 65536.0f);
    CHECK_NEAR(fx16_to_f32(fx16_div(b, a)), 1.5f, 1.0f / 65536.0f);

    Fx16 neg = fx16_from_f32(-5.625f);
    CHECK(fx16_floor_i32(neg) == -6);
    CHECK(fx16_ceil_i32(neg) == -5);
    CHECK(fx16_round_i32(neg) == -6);

    /* i32'de taşacak 16.16 çarpımı (ör. 1000 * 25.5 = 25500) */
    Fx16 c = fx16_from_i32(1000);
    Fx16 d = fx16_from_f32(25.5f);
    Fx16 cd = fx16_mul(c, d);
    CHECK(fx16_to_i32(cd) == 25500);
    CHECK_NEAR(fx16_to_f32(fx16_div(cd, c)), 25.5f, 1.0f / 65536.0f);

    /* Fx8 <-> Fx16 dönüşümü */
    Fx8 x8 = fx8_from_f32(13.75f);
    Fx16 x16 = fx8_to_fx16(x8);
    CHECK_NEAR(fx16_to_f32(x16), 13.75f, 1e-6);
    CHECK(fx16_to_fx8(x16) == x8);
}
