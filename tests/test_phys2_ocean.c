/* tests/test_phys2_ocean.c — istatistiksel okyanus (phys2/ocean.c), M11 Faz 2.
 * FFT doğrudan DFT ile; genlikler Hermitsel (yükseklik gerçek); reverse = 0'da rüzgâra karşı genlik yok;
 * alan rüzgâr yönünde ilerler (zaman kaydırmalı çapraz ilinti); rms hedefi (Parseval); belirleyicilik.
 * SamurayBalgami/Matoki'den uyarlandı (izinle).
 */
#include "test.h"
#include "test_phys2_common.h"
#include "core/math.h"
#include "phys2/pmath.h"
#include "phys2/ocean.h"

static Phys2OceanDesc ocean_desc(u32 n, f32 reverse) {
    Phys2OceanDesc d;
    d.n = n;
    d.size = 40.0f;
    d.wind_x = 1.0f;
    d.wind_z = 0.0f;
    d.wind_speed = 5.0f;
    d.rms_height = 0.2f;
    d.reverse = reverse;
    d.choppy = 0.8f;
    d.seed = 1234;
    return d;
}

TEST(phys2_ocean_fft_matches_dft) {
    test_arena_reset();
    Phys2Ocean* o = ARENA_PUSH_STRUCT(test_arena(), Phys2Ocean);
    Phys2OceanDesc d = ocean_desc(16, 0.0f);
    phys2_ocean_init(o, test_arena(), &d);
    f32 re[16], im[16];
    f64 rr[16], ri[16];
    Phys2Rng r = phys2_rng_make(7);
    for (u32 i = 0; i < 16; i++) { re[i] = phys2_rng_range(&r, -1, 1); im[i] = phys2_rng_range(&r, -1, 1); }
    for (u32 p = 0; p < 16; p++) {
        rr[p] = ri[p] = 0.0;
        for (u32 k = 0; k < 16; k++) {
            f64 a = 6.283185307179586 * (f64)(k * p) / 16.0;
            f64 c = m_cos((f32)a), s = m_sin((f32)a);
            rr[p] += re[k] * c - im[k] * s;
            ri[p] += re[k] * s + im[k] * c;
        }
    }
    phys2_ocean_fft(o, re, im, 1);
    for (u32 p = 0; p < 16; p++) {
        CHECK_NEAR(re[p], rr[p], 1e-4);
        CHECK_NEAR(im[p], ri[p], 1e-4);
    }
}

TEST(phys2_ocean_amplitudes_hermitian_height_real) {
    test_arena_reset();
    Phys2Ocean* o = ARENA_PUSH_STRUCT(test_arena(), Phys2Ocean);
    Phys2OceanDesc d = ocean_desc(32, 0.3f);
    phys2_ocean_init(o, test_arena(), &d);
    u32 n = 32, nn = n * n;
    f32* re = ARENA_PUSH_ARRAY(test_arena(), f32, nn);
    f32* im = ARENA_PUSH_ARRAY(test_arena(), f32, nn);
    f64 t = 3.7;
    f64 worst = 0.0;
    for (u32 idx = 0; idx < nn; idx++) {
        u32 i = idx % n, j = idx / n;
        u32 neg = ((n - j) % n) * n + (n - i) % n;
        f32 ar, ai, br, bi;
        phys2_ocean_amplitude(o, idx, t, &ar, &ai);
        phys2_ocean_amplitude(o, neg, t, &br, &bi);
        f64 e = m_abs(ar - br) + m_abs(ai + bi);
        if (e > worst) worst = e;
        re[idx] = ar;
        im[idx] = ai;
    }
    CHECK(worst < 1e-6);
    for (u32 j = 0; j < n; j++) phys2_ocean_fft(o, re + j * n, im + j * n, 1);
    for (u32 i = 0; i < n; i++) phys2_ocean_fft(o, re + i, im + i, n);
    f64 max_im = 0.0, max_re = 0.0;
    for (u32 idx = 0; idx < nn; idx++) {
        if (m_abs(im[idx]) > max_im) max_im = m_abs(im[idx]);
        if (m_abs(re[idx]) > max_re) max_re = m_abs(re[idx]);
    }
    test_note("yukseklik max |gercek|", max_re, 4);
    test_note("yukseklik max |sanal|", max_im, 7);
    CHECK(max_re > 0.1);
    CHECK(max_im < 1e-4 * max_re);
}

TEST(phys2_ocean_no_reverse_waves) {
    test_arena_reset();
    Phys2Ocean* o = ARENA_PUSH_STRUCT(test_arena(), Phys2Ocean);
    Phys2OceanDesc d = ocean_desc(32, 0.0f);
    phys2_ocean_init(o, test_arena(), &d);
    f64 down = 0.0, up = 0.0;
    for (u32 idx = 0; idx < 32 * 32; idx++) {
        f64 e = (f64)o->h0r[idx] * o->h0r[idx] + (f64)o->h0i[idx] * o->h0i[idx];
        if (o->kx[idx] < 0.0f) up += e;
        else down += e;
    }
    test_note("enerji ruzgar yonunde", down, 6);
    test_note("enerji ruzgara karsi", up, 6);
    CHECK(down > 0.0);
    CHECK(up == 0.0);
    /* reverse = 1: iki yön de dolu */
    Phys2Ocean* o2 = ARENA_PUSH_STRUCT(test_arena(), Phys2Ocean);
    Phys2OceanDesc d2 = ocean_desc(32, 1.0f);
    phys2_ocean_init(o2, test_arena(), &d2);
    f64 up2 = 0.0;
    for (u32 idx = 0; idx < 32 * 32; idx++) {
        if (o2->kx[idx] < 0.0f) up2 += (f64)o2->h0r[idx] * o2->h0r[idx] + (f64)o2->h0i[idx] * o2->h0i[idx];
    }
    CHECK(up2 > 0.0);
}

/* C(s) = Σ h(x, t)·h(x + s, t + Δ): alan +x yönünde ilerliyorsa s > 0'da büyük */
static f64 shifted_corr(const f32* a, const f32* b, u32 n, i32 s) {
    f64 c = 0.0;
    for (u32 j = 0; j < n; j++) {
        for (u32 i = 0; i < n; i++) c += (f64)a[j * n + i] * b[j * n + (u32)((i32)i + s + (i32)n) % n];
    }
    return c;
}

TEST(phys2_ocean_waves_travel_downwind) {
    test_arena_reset();
    const u32 n = 64;
    f32 asym[2];
    for (u32 v = 0; v < 2; v++) {
        Phys2Ocean* o = ARENA_PUSH_STRUCT(test_arena(), Phys2Ocean);
        Phys2OceanDesc d = ocean_desc(n, v == 0 ? 0.0f : 1.0f);
        phys2_ocean_init(o, test_arena(), &d);
        f32* h0 = ARENA_PUSH_ARRAY(test_arena(), f32, n * n);
        phys2_ocean_update(o, 10.0);
        for (u32 k = 0; k < n * n; k++) h0[k] = o->height[k];
        phys2_ocean_update(o, 10.3);
        f64 fwd = shifted_corr(h0, o->height, n, 3), back = shifted_corr(h0, o->height, n, -3);
        asym[v] = (f32)((fwd - back) / (m_abs((f32)fwd) + m_abs((f32)back) + 1e-12f));
    }
    test_note("ilinti asimetrisi (reverse 0)", asym[0], 3);
    test_note("ilinti asimetrisi (reverse 1)", asym[1], 3);
    CHECK(asym[0] > 0.5f);
    CHECK(asym[0] > asym[1]);
}

TEST(phys2_ocean_rms_height_and_determinism) {
    test_arena_reset();
    const u32 n = 64;
    Phys2Ocean* a = ARENA_PUSH_STRUCT(test_arena(), Phys2Ocean);
    Phys2Ocean* b = ARENA_PUSH_STRUCT(test_arena(), Phys2Ocean);
    Phys2OceanDesc d = ocean_desc(n, 0.0f);
    phys2_ocean_init(a, test_arena(), &d);
    phys2_ocean_init(b, test_arena(), &d);
    f64 sum2 = 0.0;
    u32 cnt = 0;
    b32 same = TRUE;
    for (u32 s = 0; s < 8; s++) {
        f64 t = 2.0 + s * 3.1;
        phys2_ocean_update(a, t);
        phys2_ocean_update(b, t);
        for (u32 k = 0; k < n * n; k++) {
            sum2 += (f64)a->height[k] * a->height[k];
            cnt++;
            if (a->height[k] != b->height[k] || a->disp_x[k] != b->disp_x[k]) same = FALSE;
        }
    }
    f64 rms = __builtin_sqrt(sum2 / cnt);
    test_note("rms yukseklik (hedef 0.2)", rms, 4);
    CHECK_NEAR(rms, 0.2, 0.04);
    CHECK(same);
}
