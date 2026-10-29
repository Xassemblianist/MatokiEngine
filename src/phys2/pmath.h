/* phys2/pmath.h — fizik matematiği: quaternion, rotasyon matrisi, ortonormal taban.
 * Phys2Quat (x, y, z, w), birim; v' = q v q*. Mat3 sütun-öncelikli (core/math.h): sütun k =
 * gövdenin yerel k ekseninin dünya yönü. Tüm fonksiyonlar satır içi ve belirleyici.
 * SamurayBalgami/Matoki'den uyarlandı (izinle).
 */
#ifndef PHYS2_PMATH_H
#define PHYS2_PMATH_H
#include "core/math.h"
#include "core/arena.h"
#include "core/log.h"
#include "plat/platform.h"

/* ---- core/ API farkları için phys2 içi yardımcılar (bizim core'a dokunulmaz) ---- */

static inline void phys2_mem_copy(void* dst, const void* src, usize n) { __builtin_memcpy(dst, src, n); }
static inline void phys2_mem_zero(void* dst, usize n) { __builtin_memset(dst, 0, n); }

/* Bileşen bileşen çarpım / doğrusal ara değer; birim vektör (uzunluk 0 ise girdiyi döndürür) */
static inline Vec3 phys2_v3_mul(Vec3 a, Vec3 b) { return vec3(a.x * b.x, a.y * b.y, a.z * b.z); }
static inline Vec3 phys2_v3_lerp(Vec3 a, Vec3 b, f32 t) {
    return vec3(m_lerp(a.x, b.x, t), m_lerp(a.y, b.y, t), m_lerp(a.z, b.z, t));
}
static inline Vec3 phys2_v3_norm(Vec3 a) {
    f32 l = vec3_len(a);
    return l > 0.0f ? vec3_scale(a, 1.0f / l) : a;
}

/* e^x, ln x: core/math.h'nin exp2/log2'si üzerinden */
static inline f32 phys2_expf(f32 x) { return m_exp2(x * 1.44269504f); }
static inline f32 phys2_logf(f32 x) { return m_log2(x) * 0.693147181f; }

static inline u32 phys2_f32_bits(f32 x) { union { f32 f; u32 u; } c; c.f = x; return c.u; }

/* Mat3 (sütun-öncelikli) × Vec3 */
static inline Vec3 phys2_mat3_mul_v3(const Mat3* m, Vec3 v) {
    return vec3(m->m[0] * v.x + m->m[3] * v.y + m->m[6] * v.z,
                m->m[1] * v.x + m->m[4] * v.y + m->m[7] * v.z,
                m->m[2] * v.x + m->m[5] * v.y + m->m[8] * v.z);
}

/* Belirleyici xorshift64* (tohum → dizi platformdan bağımsız); sahne/okyanus üretimi ve testler için */
typedef struct Phys2Rng { u64 s; } Phys2Rng;
static inline Phys2Rng phys2_rng_make(u64 seed) {
    Phys2Rng r;
    r.s = seed ? seed : 0x9E3779B97F4A7C15ull;
    return r;
}
static inline u32 phys2_rng_u32(Phys2Rng* r) {
    u64 x = r->s;
    x ^= x >> 12;
    x ^= x << 25;
    x ^= x >> 27;
    r->s = x;
    return (u32)((x * 0x2545F4914F6CDD1Dull) >> 32);
}
static inline f32 phys2_rng_f32(Phys2Rng* r) { return (f32)(phys2_rng_u32(r) >> 8) * (1.0f / 16777216.0f); }
static inline f32 phys2_rng_range(Phys2Rng* r, f32 lo, f32 hi) { return lo + (hi - lo) * phys2_rng_f32(r); }

typedef struct Phys2Quat {
    f32 x, y, z, w;
} Phys2Quat;

static inline Phys2Quat phys2_quat_identity(void) { Phys2Quat q = { 0, 0, 0, 1 }; return q; }

static inline Phys2Quat phys2_quat_mul(Phys2Quat a, Phys2Quat b) {
    Phys2Quat r;
    r.w = a.w * b.w - a.x * b.x - a.y * b.y - a.z * b.z;
    r.x = a.w * b.x + a.x * b.w + a.y * b.z - a.z * b.y;
    r.y = a.w * b.y - a.x * b.z + a.y * b.w + a.z * b.x;
    r.z = a.w * b.z + a.x * b.y - a.y * b.x + a.z * b.w;
    return r;
}

static inline Phys2Quat phys2_quat_normalize(Phys2Quat q) {
    f32 l = m_sqrt(q.x * q.x + q.y * q.y + q.z * q.z + q.w * q.w);
    if (!(l > 0.0f)) return phys2_quat_identity();
    f32 s = 1.0f / l;
    Phys2Quat r = { q.x * s, q.y * s, q.z * s, q.w * s };
    return r;
}

static inline Phys2Quat phys2_quat_from_axis_angle(Vec3 axis, f32 angle) {
    Vec3 a = phys2_v3_norm(axis);
    f32 s = m_sin(angle * 0.5f);
    Phys2Quat q = { a.x * s, a.y * s, a.z * s, m_cos(angle * 0.5f) };
    return q;
}

/* v + 2w(q×v) + 2 q×(q×v) */
static inline Vec3 phys2_quat_rotate(Phys2Quat q, Vec3 v) {
    Vec3 u = vec3(q.x, q.y, q.z);
    Vec3 t = vec3_scale(vec3_cross(u, v), 2.0f);
    return vec3_add(vec3_add(v, vec3_scale(t, q.w)), vec3_cross(u, t));
}

/* q += 0.5 * dt * (w, 0) * q; normalize */
static inline Phys2Quat phys2_quat_integrate(Phys2Quat q, Vec3 w, f32 dt) {
    Phys2Quat wq = { w.x, w.y, w.z, 0.0f };
    Phys2Quat d = phys2_quat_mul(wq, q);
    f32 h = 0.5f * dt;
    Phys2Quat r = { q.x + d.x * h, q.y + d.y * h, q.z + d.z * h, q.w + d.w * h };
    return phys2_quat_normalize(r);
}

static inline Mat3 phys2_mat3_from_quat(Phys2Quat q) {
    f32 xx = q.x * q.x, yy = q.y * q.y, zz = q.z * q.z;
    f32 xy = q.x * q.y, xz = q.x * q.z, yz = q.y * q.z;
    f32 wx = q.w * q.x, wy = q.w * q.y, wz = q.w * q.z;
    Mat3 m;
    m.m[0] = 1.0f - 2.0f * (yy + zz); m.m[1] = 2.0f * (xy + wz);        m.m[2] = 2.0f * (xz - wy);
    m.m[3] = 2.0f * (xy - wz);        m.m[4] = 1.0f - 2.0f * (xx + zz); m.m[5] = 2.0f * (yz + wx);
    m.m[6] = 2.0f * (xz + wy);        m.m[7] = 2.0f * (yz - wx);        m.m[8] = 1.0f - 2.0f * (xx + yy);
    return m;
}

static inline Vec3 phys2_mat3_col(const Mat3* m, u32 c) { return vec3(m->m[c * 3], m->m[c * 3 + 1], m->m[c * 3 + 2]); }

/* R^T v (dünya → yerel) */
static inline Vec3 phys2_mat3_tmul_v3(const Mat3* m, Vec3 v) {
    return vec3(m->m[0] * v.x + m->m[1] * v.y + m->m[2] * v.z,
              m->m[3] * v.x + m->m[4] * v.y + m->m[5] * v.z,
              m->m[6] * v.x + m->m[7] * v.y + m->m[8] * v.z);
}

/* R diag(d) R^T */
static inline Mat3 phys2_mat3_rotate_diag(const Mat3* r, Vec3 d) {
    const f32 dd[3] = { d.x, d.y, d.z };
    Mat3 o;
    for (u32 c = 0; c < 3; c++) {
        for (u32 row = 0; row < 3; row++) {
            f32 s = 0.0f;
            for (u32 k = 0; k < 3; k++) s += r->m[k * 3 + row] * dd[k] * r->m[k * 3 + c];
            o.m[c * 3 + row] = s;
        }
    }
    return o;
}

/* n'ye dik ortonormal t1, t2 (Duff vd. 2017, dalsız) */
static inline void phys2_ortho_basis(Vec3 n, Vec3* t1, Vec3* t2) {
    f32 s = n.z >= 0.0f ? 1.0f : -1.0f;
    f32 a = -1.0f / (s + n.z);
    f32 b = n.x * n.y * a;
    *t1 = vec3(1.0f + s * n.x * n.x * a, s * b, -s * n.x);
    *t2 = vec3(b, s + n.y * n.y * a, -n.y);
}

static inline Mat4 phys2_mat4_from_pos_rot_scale(Vec3 p, Phys2Quat q, Vec3 s) {
    Mat3 r = phys2_mat3_from_quat(q);
    Mat4 m = mat4_identity();
    for (u32 c = 0; c < 3; c++) {
        f32 k = c == 0 ? s.x : (c == 1 ? s.y : s.z);
        m.m[c * 4 + 0] = r.m[c * 3 + 0] * k;
        m.m[c * 4 + 1] = r.m[c * 3 + 1] * k;
        m.m[c * 4 + 2] = r.m[c * 3 + 2] * k;
    }
    m.m[12] = p.x;
    m.m[13] = p.y;
    m.m[14] = p.z;
    return m;
}

#endif
