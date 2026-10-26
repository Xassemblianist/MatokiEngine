/* tests/test_math.c — f32 matematik fonksiyonları ve matris testleri.
 * libm kullanmaz; referans fonksiyonlar yüksek hassasiyetli f64 Taylor serileriyle
 * bu dosyada gerçeklenmiştir.
 */
#include "test.h"
#include "core/math.h"
#include "core/rng.h"
#include "core/log.h"

#define REF_PI      3.14159265358979323846264338327950288
#define REF_HALF_PI 1.57079632679489661923132169163975144
#define REF_LN2     0.693147180559945309417232121458176568

static f64 ref_abs(f64 x) {
    return x < 0.0 ? -x : x;
}

static f64 ref_floor(f64 x) {
    i64 i = (i64)x;
    return (f64)(i - (x < (f64)i));
}

static f64 ref_sqrt(f64 x) {
    if (x <= 0.0) return 0.0;
    f64 g = (f64)__builtin_sqrtf((f32)x);
    for (i32 i = 0; i < 4; i++) {
        g = 0.5 * (g + x / g);
    }
    return g;
}

/* [-pi/4, pi/4] üzerinde tam f64 Taylor serisi ile sin/cos */
static f64 ref_sin_kernel(f64 r) {
    f64 r2 = r * r;
    f64 term = r;
    f64 sum = r;
    for (i32 k = 1; k <= 12; k++) {
        term *= -r2 / (f64)((2 * k) * (2 * k + 1));
        sum += term;
    }
    return sum;
}

static f64 ref_cos_kernel(f64 r) {
    f64 r2 = r * r;
    f64 term = 1.0;
    f64 sum = 1.0;
    for (i32 k = 1; k <= 12; k++) {
        term *= -r2 / (f64)((2 * k - 1) * (2 * k));
        sum += term;
    }
    return sum;
}

static f64 ref_sin(f64 x) {
    f64 kf = ref_floor(x / REF_HALF_PI + 0.5);
    i64 k = (i64)kf;
    f64 r = x - kf * REF_HALF_PI;
    switch (k & 3) {
        case 0:  return  ref_sin_kernel(r);
        case 1:  return  ref_cos_kernel(r);
        case 2:  return -ref_sin_kernel(r);
        default: return -ref_cos_kernel(r);
    }
}

static f64 ref_cos(f64 x) {
    f64 kf = ref_floor(x / REF_HALF_PI + 0.5);
    i64 k = (i64)kf;
    f64 r = x - kf * REF_HALF_PI;
    switch ((k + 1) & 3) {
        case 0:  return  ref_sin_kernel(r);
        case 1:  return  ref_cos_kernel(r);
        case 2:  return -ref_sin_kernel(r);
        default: return -ref_cos_kernel(r);
    }
}

static f64 ref_tan(f64 x) {
    return ref_sin(x) / ref_cos(x);
}

static f64 ref_atan_scalar(f64 x) {
    /* İki kademeli yarım açı indirgeme: x -> x / (1 + sqrt(1 + x^2)) */
    x = x / (1.0 + ref_sqrt(1.0 + x * x));
    x = x / (1.0 + ref_sqrt(1.0 + x * x));
    f64 x2 = x * x;
    f64 term = x;
    f64 sum = x;
    for (i32 k = 1; k <= 16; k++) {
        term *= -x2;
        sum += term / (f64)(2 * k + 1);
    }
    return 4.0 * sum;
}

static f64 ref_atan2(f64 y, f64 x) {
    if (x == 0.0 && y == 0.0) return 0.0;
    f64 ax = ref_abs(x);
    f64 ay = ref_abs(y);
    f64 a = (ax >= ay) ? ref_atan_scalar(ay / ax) : (REF_HALF_PI - ref_atan_scalar(ax / ay));
    if (x < 0.0) a = REF_PI - a;
    return (y < 0.0) ? -a : a;
}

static f64 ref_exp2(f64 x) {
    f64 kf = ref_floor(x + 0.5);
    i64 k = (i64)kf;
    f64 r = (x - kf) * REF_LN2;
    f64 term = 1.0;
    f64 sum = 1.0;
    for (i32 i = 1; i <= 18; i++) {
        term *= r / (f64)i;
        sum += term;
    }
    /* 2^k ile ölçekle */
    u64 bits = (u64)(k + 1023) << 52;
    f64 scale;
    __builtin_memcpy(&scale, &bits, sizeof(scale));
    return sum * scale;
}

static f64 ref_log2(f64 x) {
    u64 bits;
    __builtin_memcpy(&bits, &x, sizeof(bits));
    i64 e = (i64)((bits >> 52) & 0x7ffULL) - 1023;
    u64 m_bits = (bits & 0x000fffffffffffffULL) | (1023ULL << 52);
    f64 m;
    __builtin_memcpy(&m, &m_bits, sizeof(m));
    if (m > 1.4142135623730950488) {
        m *= 0.5;
        e += 1;
    }
    f64 t = (m - 1.0) / (m + 1.0);
    f64 t2 = t * t;
    f64 term = t;
    f64 sum = t;
    for (i32 k = 1; k <= 20; k++) {
        term *= t2;
        sum += term / (f64)(2 * k + 1);
    }
    return (f64)e + (2.0 / REF_LN2) * sum;
}

TEST(math_builtins) {
    CHECK_NEAR(m_sqrt(9.0f), 3.0f, 1e-6);
    CHECK_NEAR(m_floor(2.7f), 2.0f, 1e-6);
    CHECK_NEAR(m_floor(-2.3f), -3.0f, 1e-6);
    CHECK_NEAR(m_ceil(2.3f), 3.0f, 1e-6);
    CHECK_NEAR(m_ceil(-2.7f), -2.0f, 1e-6);
    CHECK_NEAR(m_trunc(-2.7f), -2.0f, 1e-6);
    CHECK_NEAR(m_abs(-5.5f), 5.5f, 1e-6);
    CHECK_NEAR(m_min(3.0f, -1.0f), -1.0f, 1e-6);
    CHECK_NEAR(m_max(3.0f, -1.0f), 3.0f, 1e-6);
}

TEST(math_sin_cos_accuracy) {
    f64 max_sin_err = 0.0;
    f64 max_cos_err = 0.0;
    const i32 N = 40000;
    for (i32 i = 0; i <= N; i++) {
        f32 x = -100.0f + 200.0f * ((f32)i / (f32)N);
        f64 s_err = ref_abs((f64)m_sin(x) - ref_sin((f64)x));
        f64 c_err = ref_abs((f64)m_cos(x) - ref_cos((f64)x));
        if (s_err > max_sin_err) max_sin_err = s_err;
        if (c_err > max_cos_err) max_cos_err = c_err;
    }
    CHECK(max_sin_err < 2e-6);
    CHECK(max_cos_err < 2e-6);
}

TEST(math_tan_atan2_accuracy) {
    f64 max_tan_err = 0.0;
    /* [-1.3, 1.3] aralığında tan mutlak hatası */
    for (i32 i = 0; i <= 4000; i++) {
        f32 x = -1.3f + 2.6f * ((f32)i / 4000.0f);
        f64 err = ref_abs((f64)m_tan(x) - ref_tan((f64)x));
        if (err > max_tan_err) max_tan_err = err;
    }
    CHECK(max_tan_err < 2e-6);

    f64 max_atan2_err = 0.0;
    for (i32 iy = -50; iy <= 50; iy++) {
        for (i32 ix = -50; ix <= 50; ix++) {
            if (ix == 0 && iy == 0) continue;
            f32 y = (f32)iy * 0.37f;
            f32 x = (f32)ix * 0.41f;
            f64 err = ref_abs((f64)m_atan2(y, x) - ref_atan2((f64)y, (f64)x));
            if (err > max_atan2_err) max_atan2_err = err;
        }
    }
    CHECK(max_atan2_err < 1e-6);
}

TEST(math_exp2_log2_pow_accuracy) {
    f64 max_exp2_rel = 0.0;
    for (i32 i = 0; i <= 20000; i++) {
        f32 x = -100.0f + 200.0f * ((f32)i / 20000.0f);
        f64 ref = ref_exp2((f64)x);
        f64 rel = ref_abs((f64)m_exp2(x) - ref) / ref;
        if (rel > max_exp2_rel) max_exp2_rel = rel;
    }
    CHECK(max_exp2_rel < 1e-6);

    f64 max_log2_rel = 0.0;
    for (i32 i = 0; i <= 20000; i++) {
        f32 e = -80.0f + 160.0f * ((f32)i / 20000.0f);
        if (m_abs(e) < 1e-4f) continue;
        f32 x = (f32)ref_exp2((f64)e);
        f64 ref = ref_log2((f64)x);
        f64 rel = ref_abs((f64)m_log2(x) - ref) / ref_abs(ref);
        if (rel > max_log2_rel) max_log2_rel = rel;
    }
    CHECK(max_log2_rel < 1e-6);

    CHECK_NEAR(m_pow(2.0f, 10.0f), 1024.0f, 1e-3);
    CHECK_NEAR(m_pow(9.0f, 0.5f), 3.0f, 1e-5);
    CHECK_NEAR(m_pow(27.0f, 1.0f / 3.0f), 3.0f, 1e-5);
}

TEST(mat3_and_mat4_transforms) {
    /* 2D: (1, 0) noktasını (2, 3) ile ölçekle, 90 derece döndür, (10, 20) ötele -> (10, 22) */
    Mat3 m2 = mat3_2d(10.0f, 20.0f, M_HALF_PI, 2.0f, 3.0f);
    Vec2 p = mat3_mul_point(m2, vec2(1.0f, 0.0f));
    CHECK_NEAR(p.x, 10.0f, 1e-5);
    CHECK_NEAR(p.y, 22.0f, 1e-5);

    /* Perspektif derinlik eşlemesi: z = -near -> ndc.z = 0, z = -far -> ndc.z = 1 */
    f32 near_z = 0.5f;
    f32 far_z = 100.0f;
    Mat4 proj = mat4_perspective(60.0f * M_DEG2RAD, 16.0f / 9.0f, near_z, far_z);
    Vec4 cn = mat4_mul_vec4(proj, vec4(0.0f, 0.0f, -near_z, 1.0f));
    Vec4 cf = mat4_mul_vec4(proj, vec4(0.0f, 0.0f, -far_z, 1.0f));
    CHECK_NEAR(cn.z / cn.w, 0.0f, 1e-5);
    CHECK_NEAR(cf.z / cf.w, 1.0f, 1e-5);

    /* LookAt: kamera (0, 0, 5)'te orijine bakarken (0, 0, 0) noktası (0, 0, -5) olur */
    Mat4 view = mat4_look_at(vec3(0.0f, 0.0f, 5.0f), vec3(0.0f, 0.0f, 0.0f), vec3(0.0f, 1.0f, 0.0f));
    Vec4 vorig = mat4_mul_vec4(view, vec4(0.0f, 0.0f, 0.0f, 1.0f));
    CHECK_NEAR(vorig.x, 0.0f, 1e-5);
    CHECK_NEAR(vorig.y, 0.0f, 1e-5);
    CHECK_NEAR(vorig.z, -5.0f, 1e-5);
    CHECK_NEAR(vorig.w, 1.0f, 1e-5);
}

TEST(mat4_inverse_random_100) {
    Rng r;
    rng_seed(&r, 42ULL);
    f64 max_inv_err = 0.0;

    for (u32 n = 0; n < 100; n++) {
        /* Köşegen baskın + rastgele bileşenli iyi koşullu 4x4 matris */
        Mat4 m;
        for (i32 i = 0; i < 16; i++) {
            m.m[i] = rng_f32(&r) * 2.0f - 1.0f;
        }
        m.m[0]  += 3.0f;
        m.m[5]  += 3.0f;
        m.m[10] += 3.0f;
        m.m[15] += 3.0f;

        Mat4 inv = mat4_inverse(m);
        Mat4 prod = mat4_mul(inv, m);

        for (i32 c = 0; c < 4; c++) {
            for (i32 row = 0; row < 4; row++) {
                f32 expected = (c == row) ? 1.0f : 0.0f;
                f64 err = ref_abs((f64)prod.m[c * 4 + row] - (f64)expected);
                if (err > max_inv_err) max_inv_err = err;
                CHECK_NEAR(prod.m[c * 4 + row], expected, 1e-4);
            }
        }
    }
    CHECK(max_inv_err < 1e-4);
}
