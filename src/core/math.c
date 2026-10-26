/* core/math.c — f32 minimax aşkın fonksiyonlar ve Mat3/Mat4 işlemleri.
 * Cody-Waite 3 sabitli aralık indirgeme ile [-pi/4, pi/4] aralığına indirgenir.
 * Polinom katsayıları Chebyshev/Remez minimax yaklaşımıyla üretilmiştir.
 */
#include "core/math.h"

static inline u32 f32_to_bits(f32 x) {
    u32 u;
    __builtin_memcpy(&u, &x, sizeof(u));
    return u;
}

static inline f32 bits_to_f32(u32 u) {
    f32 x;
    __builtin_memcpy(&x, &u, sizeof(x));
    return x;
}

/* Cody-Waite pi/2 sabitleri: c1 ve c2'nin alt 12 mantissa biti sıfırdır,
 * böylece |k| <= 4096 için kf * c1 ve kf * c2 çarpımları bit-tam (exact) olur. */
#define PIO2_1   1.5703125f                  /* 0x3fc90000 */
#define PIO2_2   4.837512969970703125e-04f   /* 0x39fda000 */
#define PIO2_3   7.549789954891882163e-08f   /* 0x33a22168 */
#define INV_PIO2 0.63661977236758134308f     /* 2 / pi     */

static inline f32 reduce_pio2(f32 x, i32* out_k) {
    f32 kf = m_floor(x * INV_PIO2 + 0.5f);
    i32 k = (i32)kf;
    f32 r = (x - kf * PIO2_1) - kf * PIO2_2;
    r -= kf * PIO2_3;
    *out_k = k;
    return r;
}

/* [-pi/4, pi/4] üzerinde sin(r) ve cos(r) minimax polinomları */
static inline f32 sin_poly(f32 r) {
    f32 r2 = r * r;
    f32 p = 2.7557314297e-06f;
    p = p * r2 - 1.9841270114e-04f;
    p = p * r2 + 8.3333337680e-03f;
    p = p * r2 - 1.6666667163e-01f;
    return r + r * (r2 * p);
}

static inline f32 cos_poly(f32 r) {
    f32 r2 = r * r;
    f32 p = -2.7557314297e-07f;
    p = p * r2 + 2.4801587642e-05f;
    p = p * r2 - 1.3888889225e-03f;
    p = p * r2 + 4.1666667908e-02f;
    return (1.0f - 0.5f * r2) + r2 * (r2 * p);
}

f32 m_sin(f32 x) {
    i32 k;
    f32 r = reduce_pio2(x, &k);
    switch (k & 3) {
        case 0:  return  sin_poly(r);
        case 1:  return  cos_poly(r);
        case 2:  return -sin_poly(r);
        default: return -cos_poly(r);
    }
}

f32 m_cos(f32 x) {
    i32 k;
    f32 r = reduce_pio2(x, &k);
    switch ((k + 1) & 3) {
        case 0:  return  sin_poly(r);
        case 1:  return  cos_poly(r);
        case 2:  return -sin_poly(r);
        default: return -cos_poly(r);
    }
}

/* [-pi/4, pi/4] üzerinde tan(r) minimax polinomu; tek k için -1/tan(r) */
f32 m_tan(f32 x) {
    i32 k;
    f32 r = reduce_pio2(x, &k);
    f32 r2 = r * r;
    f32 p = 9.848303162e-03f;
    p = p * r2 + 2.235466382e-03f;
    p = p * r2 + 2.506787330e-02f;
    p = p * r2 + 5.319792777e-02f;
    p = p * r2 + 1.334202886e-01f;
    p = p * r2 + 3.333298564e-01f;
    f32 t = r + r * (r2 * p);
    if ((k & 1) == 0) {
        return t;
    }
    return -1.0f / t;
}

/* [0, 1] aralığını tan(pi/8) = sqrt(2)-1 ile [0, 0.41421356] aralığına indirger */
static inline f32 atan_scalar(f32 x) {
    f32 bias = 0.0f;
    if (x > 0.41421356237f) {
        x = (x - 1.0f) / (x + 1.0f);
        bias = 0.78539816339744830962f; /* pi / 4 */
    }
    f32 x2 = x * x;
    f32 p = 7.905684412e-02f;
    p = p * x2 - 1.382558942e-01f;
    p = p * x2 + 1.997200996e-01f;
    p = p * x2 - 3.333276212e-01f;
    return (x + x * (x2 * p)) + bias;
}

f32 m_atan2(f32 y, f32 x) {
    if (x == 0.0f && y == 0.0f) return 0.0f;
    f32 ax = m_abs(x);
    f32 ay = m_abs(y);
    f32 a;
    if (ax >= ay) {
        a = atan_scalar(ay / ax);
    } else {
        a = M_HALF_PI - atan_scalar(ax / ay);
    }
    if (x < 0.0f) a = M_PI - a;
    return (y < 0.0f) ? -a : a;
}

/* x = k + r, r in [-0.5, 0.5]; 2^x = 2^k * 2^r */
f32 m_exp2(f32 x) {
    if (x >= 128.0f) return bits_to_f32(0x7f800000u); /* +inf */
    if (x < -126.0f) return 0.0f;
    f32 kf = m_floor(x + 0.5f);
    i32 k = (i32)kf;
    f32 r = x - kf;
    f32 p = 1.546145504e-04f;
    p = p * r + 1.339087728e-03f;
    p = p * r + 9.618056938e-03f;
    p = p * r + 5.550356954e-02f;
    p = p * r + 2.402265072e-01f;
    p = p * r + 6.931471825e-01f;
    f32 mant = 1.0f + r * p;
    f32 scale = bits_to_f32((u32)(k + 127) << 23);
    return mant * scale;
}

/* x = 2^e * m, m in [sqrt(2)/2, sqrt(2)]; t = (m-1)/(m+1) üzerinden minimax */
f32 m_log2(f32 x) {
    if (x <= 0.0f) return bits_to_f32(0xff800000u); /* -inf */
    u32 bits = f32_to_bits(x);
    i32 e = (i32)((bits >> 23) & 0xffu) - 127;
    /* Denormal sayılar için yeniden ölçekleme */
    if (e == -127) {
        x *= 16777216.0f; /* 2^24 */
        bits = f32_to_bits(x);
        e = (i32)((bits >> 23) & 0xffu) - 127 - 24;
    }
    f32 m = bits_to_f32((bits & 0x007fffffu) | 0x3f800000u);
    if (m > 1.41421356237f) {
        m *= 0.5f;
        e += 1;
    }
    f32 t = (m - 1.0f) / (m + 1.0f);
    f32 t2 = t * t;
    f32 p = 4.317310750e-01f;
    p = p * t2 + 5.767146349e-01f;
    p = p * t2 + 9.617988467e-01f;
    p = p * t2 + 2.885390043e+00f;
    return (f32)e + t * p;
}

f32 m_pow(f32 x, f32 y) {
    if (y == 0.0f) return 1.0f;
    if (x == 0.0f) return 0.0f;
    return m_exp2(y * m_log2(x));
}

/* --- Mat3 İşlemleri (Sütun-ana: m[col * 3 + row]) --- */

Mat3 mat3_identity(void) {
    Mat3 r = {{
        1.0f, 0.0f, 0.0f,
        0.0f, 1.0f, 0.0f,
        0.0f, 0.0f, 1.0f
    }};
    return r;
}

Mat3 mat3_mul(Mat3 a, Mat3 b) {
    Mat3 r;
    for (i32 c = 0; c < 3; c++) {
        for (i32 row = 0; row < 3; row++) {
            r.m[c * 3 + row] =
                a.m[0 * 3 + row] * b.m[c * 3 + 0] +
                a.m[1 * 3 + row] * b.m[c * 3 + 1] +
                a.m[2 * 3 + row] * b.m[c * 3 + 2];
        }
    }
    return r;
}

Vec2 mat3_mul_point(Mat3 m, Vec2 p) {
    return vec2(
        m.m[0] * p.x + m.m[3] * p.y + m.m[6],
        m.m[1] * p.x + m.m[4] * p.y + m.m[7]
    );
}

Vec3 mat3_mul_vec3(Mat3 m, Vec3 v) {
    return vec3(
        m.m[0] * v.x + m.m[3] * v.y + m.m[6] * v.z,
        m.m[1] * v.x + m.m[4] * v.y + m.m[7] * v.z,
        m.m[2] * v.x + m.m[5] * v.y + m.m[8] * v.z
    );
}

/* 2D öteleme (tx, ty) + dönme (rot_rad) + ölçek (sx, sy) matrisi: T * R * S */
Mat3 mat3_2d(f32 tx, f32 ty, f32 rot_rad, f32 sx, f32 sy) {
    f32 c = m_cos(rot_rad);
    f32 s = m_sin(rot_rad);
    Mat3 r = {{
        c * sx,  s * sx, 0.0f,
       -s * sy,  c * sy, 0.0f,
        tx,      ty,     1.0f
    }};
    return r;
}

/* --- Mat4 İşlemleri (Sütun-ana: m[col * 4 + row]) --- */

Mat4 mat4_identity(void) {
    Mat4 r = {{
        1.0f, 0.0f, 0.0f, 0.0f,
        0.0f, 1.0f, 0.0f, 0.0f,
        0.0f, 0.0f, 1.0f, 0.0f,
        0.0f, 0.0f, 0.0f, 1.0f
    }};
    return r;
}

Mat4 mat4_mul(Mat4 a, Mat4 b) {
    Mat4 r;
    for (i32 c = 0; c < 4; c++) {
        for (i32 row = 0; row < 4; row++) {
            r.m[c * 4 + row] =
                a.m[0 * 4 + row] * b.m[c * 4 + 0] +
                a.m[1 * 4 + row] * b.m[c * 4 + 1] +
                a.m[2 * 4 + row] * b.m[c * 4 + 2] +
                a.m[3 * 4 + row] * b.m[c * 4 + 3];
        }
    }
    return r;
}

Vec4 mat4_mul_vec4(Mat4 m, Vec4 v) {
    return vec4(
        m.m[0] * v.x + m.m[4] * v.y + m.m[ 8] * v.z + m.m[12] * v.w,
        m.m[1] * v.x + m.m[5] * v.y + m.m[ 9] * v.z + m.m[13] * v.w,
        m.m[2] * v.x + m.m[6] * v.y + m.m[10] * v.z + m.m[14] * v.w,
        m.m[3] * v.x + m.m[7] * v.y + m.m[11] * v.z + m.m[15] * v.w
    );
}

/* Sağ-el koordinat sistemi, [-z_near, -z_far] -> [0, 1] derinlik aralığı */
Mat4 mat4_perspective(f32 fov_y_rad, f32 aspect, f32 near_z, f32 far_z) {
    f32 f = 1.0f / m_tan(0.5f * fov_y_rad);
    f32 inv_nf = 1.0f / (near_z - far_z);
    Mat4 r = {{
        f / aspect, 0.0f, 0.0f,                   0.0f,
        0.0f,       f,    0.0f,                   0.0f,
        0.0f,       0.0f, far_z * inv_nf,        -1.0f,
        0.0f,       0.0f, near_z * far_z * inv_nf, 0.0f
    }};
    return r;
}

Mat4 mat4_look_at(Vec3 eye, Vec3 target, Vec3 up) {
    Vec3 f = vec3_norm(vec3_sub(target, eye));
    Vec3 s = vec3_norm(vec3_cross(f, up));
    Vec3 u = vec3_cross(s, f);

    Mat4 r = {{
         s.x,  u.x, -f.x, 0.0f,
         s.y,  u.y, -f.y, 0.0f,
         s.z,  u.z, -f.z, 0.0f,
        -vec3_dot(s, eye), -vec3_dot(u, eye), vec3_dot(f, eye), 1.0f
    }};
    return r;
}

Mat4 mat4_inverse(Mat4 m) {
    const f32* a = m.m;
    f32 inv[16];

    inv[0] =  a[5]  * a[10] * a[15] -
              a[5]  * a[11] * a[14] -
              a[9]  * a[6]  * a[15] +
              a[9]  * a[7]  * a[14] +
              a[13] * a[6]  * a[11] -
              a[13] * a[7]  * a[10];

    inv[4] = -a[4]  * a[10] * a[15] +
              a[4]  * a[11] * a[14] +
              a[8]  * a[6]  * a[15] -
              a[8]  * a[7]  * a[14] -
              a[12] * a[6]  * a[11] +
              a[12] * a[7]  * a[10];

    inv[8] =  a[4]  * a[9]  * a[15] -
              a[4]  * a[11] * a[13] -
              a[8]  * a[5]  * a[15] +
              a[8]  * a[7]  * a[13] +
              a[12] * a[5]  * a[11] -
              a[12] * a[7]  * a[9];

    inv[12] = -a[4]  * a[9]  * a[14] +
               a[4]  * a[10] * a[13] +
               a[8]  * a[5]  * a[14] -
               a[8]  * a[6]  * a[13] -
               a[12] * a[5]  * a[10] +
               a[12] * a[6]  * a[9];

    inv[1] = -a[1]  * a[10] * a[15] +
              a[1]  * a[11] * a[14] +
              a[9]  * a[2]  * a[15] -
              a[9]  * a[3]  * a[14] -
              a[13] * a[2]  * a[11] +
              a[13] * a[3]  * a[10];

    inv[5] =  a[0]  * a[10] * a[15] -
              a[0]  * a[11] * a[14] -
              a[8]  * a[2]  * a[15] +
              a[8]  * a[3]  * a[14] +
              a[12] * a[2]  * a[11] -
              a[12] * a[3]  * a[10];

    inv[9] = -a[0]  * a[9]  * a[15] +
              a[0]  * a[11] * a[13] +
              a[8]  * a[1]  * a[15] -
              a[8]  * a[3]  * a[13] -
              a[12] * a[1]  * a[11] +
              a[12] * a[3]  * a[9];

    inv[13] =  a[0]  * a[9]  * a[14] -
               a[0]  * a[10] * a[13] -
               a[8]  * a[1]  * a[14] +
               a[8]  * a[2]  * a[13] +
               a[12] * a[1]  * a[10] -
               a[12] * a[2]  * a[9];

    inv[2] =  a[1]  * a[6] * a[15] -
              a[1]  * a[7] * a[14] -
              a[5]  * a[2] * a[15] +
              a[5]  * a[3] * a[14] +
              a[13] * a[2] * a[7] -
              a[13] * a[3] * a[6];

    inv[6] = -a[0]  * a[6] * a[15] +
              a[0]  * a[7] * a[14] +
              a[4]  * a[2] * a[15] -
              a[4]  * a[3] * a[14] -
              a[12] * a[2] * a[7] +
              a[12] * a[3] * a[6];

    inv[10] =  a[0]  * a[5] * a[15] -
               a[0]  * a[7] * a[13] -
               a[4]  * a[1] * a[15] +
               a[4]  * a[3] * a[13] +
               a[12] * a[1] * a[7] -
               a[12] * a[3] * a[5];

    inv[14] = -a[0]  * a[5] * a[14] +
               a[0]  * a[6] * a[13] +
               a[4]  * a[1] * a[14] -
               a[4]  * a[2] * a[13] -
               a[12] * a[1] * a[6] +
               a[12] * a[2] * a[5];

    inv[3] = -a[1] * a[6] * a[11] +
              a[1] * a[7] * a[10] +
              a[5] * a[2] * a[11] -
              a[5] * a[3] * a[10] -
              a[9] * a[2] * a[7] +
              a[9] * a[3] * a[6];

    inv[7] =  a[0] * a[6] * a[11] -
              a[0] * a[7] * a[10] -
              a[4] * a[2] * a[11] +
              a[4] * a[3] * a[10] +
              a[8] * a[2] * a[7] -
              a[8] * a[3] * a[6];

    inv[11] = -a[0] * a[5] * a[11] +
               a[0] * a[7] * a[9] +
               a[4] * a[1] * a[11] -
               a[4] * a[3] * a[9] -
               a[8] * a[1] * a[7] +
               a[8] * a[3] * a[5];

    inv[15] =  a[0] * a[5] * a[10] -
               a[0] * a[6] * a[9] -
               a[4] * a[1] * a[10] +
               a[4] * a[2] * a[9] +
               a[8] * a[1] * a[6] -
               a[8] * a[2] * a[5];

    f32 det = a[0] * inv[0] + a[1] * inv[4] + a[2] * inv[8] + a[3] * inv[12];
    if (m_abs(det) < 1e-30f) {
        return mat4_identity();
    }
    f32 inv_det = 1.0f / det;
    Mat4 out;
    for (i32 i = 0; i < 16; i++) {
        out.m[i] = inv[i] * inv_det;
    }
    return out;
}
