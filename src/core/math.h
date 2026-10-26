/* core/math.h — f32 matematik fonksiyonları, vektör ve matris tipleri.
 * libc kullanmaz; temel skaler işlemler clang yerleşiklerine (wasm opcode'larına),
 * aşkın fonksiyonlar ise aralık indirgeme + minimax polinomlarına dayanır.
 */
#ifndef CORE_MATH_H
#define CORE_MATH_H

#include "core/types.h"

#define M_PI      3.14159265358979323846f
#define M_TAU     6.28318530717958647692f
#define M_HALF_PI 1.57079632679489661923f
#define M_DEG2RAD (M_PI / 180.0f)
#define M_RAD2DEG (180.0f / M_PI)

/* --- Yerleşik f32 yardımcıları (wasm opcode'larına iner, host'ta libm gerektirmez) --- */

static inline f32 m_sqrt(f32 x) { return __builtin_sqrtf(x); }
static inline f32 m_abs(f32 x)  { return __builtin_fabsf(x); }

#if defined(__wasm__)
static inline f32 m_floor(f32 x) { return __builtin_floorf(x); }
static inline f32 m_ceil(f32 x)  { return __builtin_ceilf(x); }
static inline f32 m_trunc(f32 x) { return __builtin_truncf(x); }
#else
static inline f32 m_trunc(f32 x) {
    if (m_abs(x) >= 8388608.0f) return x;
    return (f32)(i32)x;
}
static inline f32 m_floor(f32 x) {
    if (m_abs(x) >= 8388608.0f) return x;
    i32 i = (i32)x;
    return (f32)(i - (x < (f32)i));
}
static inline f32 m_ceil(f32 x) {
    if (m_abs(x) >= 8388608.0f) return x;
    i32 i = (i32)x;
    return (f32)(i + (x > (f32)i));
}
#endif

static inline f32 m_min(f32 a, f32 b) { return a < b ? a : b; }
static inline f32 m_max(f32 a, f32 b) { return a > b ? a : b; }
static inline f32 m_clamp(f32 x, f32 lo, f32 hi) { return m_min(m_max(x, lo), hi); }
static inline f32 m_lerp(f32 a, f32 b, f32 t)    { return a + (b - a) * t; }

/* --- Minimax polinom + aralık indirgeme ile f32 aşkın fonksiyonlar --- */

f32 m_sin(f32 x);
f32 m_cos(f32 x);
f32 m_tan(f32 x);
f32 m_atan2(f32 y, f32 x);
f32 m_exp2(f32 x);
f32 m_log2(f32 x);
f32 m_pow(f32 x, f32 y);

/* --- Vektör ve Matris Tipleri --- */

typedef struct { i32 x, y; }       IVec2;
typedef struct { f32 x, y; }       Vec2;
typedef struct { f32 x, y, z; }    Vec3;
typedef struct { f32 x, y, z, w; } Vec4;

/* Sütun-ana (column-major) yerleşim: m[col * N + row] */
typedef struct { f32 m[9]; }  Mat3;
typedef struct { f32 m[16]; } Mat4;

/* --- IVec2 / Vec2 / Vec3 / Vec4 yardımcıları --- */

static inline IVec2 ivec2(i32 x, i32 y) { IVec2 r = { x, y }; return r; }
static inline Vec2  vec2(f32 x, f32 y)  { Vec2 r = { x, y }; return r; }
static inline Vec3  vec3(f32 x, f32 y, f32 z) { Vec3 r = { x, y, z }; return r; }
static inline Vec4  vec4(f32 x, f32 y, f32 z, f32 w) { Vec4 r = { x, y, z, w }; return r; }

static inline Vec2 vec2_add(Vec2 a, Vec2 b) { return vec2(a.x + b.x, a.y + b.y); }
static inline Vec2 vec2_sub(Vec2 a, Vec2 b) { return vec2(a.x - b.x, a.y - b.y); }
static inline Vec2 vec2_scale(Vec2 v, f32 s) { return vec2(v.x * s, v.y * s); }
static inline f32  vec2_dot(Vec2 a, Vec2 b) { return a.x * b.x + a.y * b.y; }
static inline f32  vec2_len(Vec2 v) { return m_sqrt(vec2_dot(v, v)); }
static inline Vec2 vec2_norm(Vec2 v) {
    f32 l = vec2_len(v);
    return l > 0.0f ? vec2_scale(v, 1.0f / l) : vec2(0.0f, 0.0f);
}

static inline Vec3 vec3_add(Vec3 a, Vec3 b) { return vec3(a.x + b.x, a.y + b.y, a.z + b.z); }
static inline Vec3 vec3_sub(Vec3 a, Vec3 b) { return vec3(a.x - b.x, a.y - b.y, a.z - b.z); }
static inline Vec3 vec3_scale(Vec3 v, f32 s) { return vec3(v.x * s, v.y * s, v.z * s); }
static inline f32  vec3_dot(Vec3 a, Vec3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
static inline Vec3 vec3_cross(Vec3 a, Vec3 b) {
    return vec3(
        a.y * b.z - a.z * b.y,
        a.z * b.x - a.x * b.z,
        a.x * b.y - a.y * b.x
    );
}
static inline f32  vec3_len(Vec3 v) { return m_sqrt(vec3_dot(v, v)); }
static inline Vec3 vec3_norm(Vec3 v) {
    f32 l = vec3_len(v);
    return l > 0.0f ? vec3_scale(v, 1.0f / l) : vec3(0.0f, 0.0f, 0.0f);
}

static inline Vec4 vec4_add(Vec4 a, Vec4 b) { return vec4(a.x + b.x, a.y + b.y, a.z + b.z, a.w + b.w); }
static inline Vec4 vec4_sub(Vec4 a, Vec4 b) { return vec4(a.x - b.x, a.y - b.y, a.z - b.z, a.w - b.w); }
static inline Vec4 vec4_scale(Vec4 v, f32 s) { return vec4(v.x * s, v.y * s, v.z * s, v.w * s); }
static inline f32  vec4_dot(Vec4 a, Vec4 b) { return a.x * b.x + a.y * b.y + a.z * b.z + a.w * b.w; }

/* --- Mat3 / Mat4 fonksiyonları --- */

Mat3 mat3_identity(void);
Mat3 mat3_mul(Mat3 a, Mat3 b);
Vec2 mat3_mul_point(Mat3 m, Vec2 p);
Vec3 mat3_mul_vec3(Mat3 m, Vec3 v);
Mat3 mat3_2d(f32 tx, f32 ty, f32 rot_rad, f32 sx, f32 sy);

Mat4 mat4_identity(void);
Mat4 mat4_mul(Mat4 a, Mat4 b);
Vec4 mat4_mul_vec4(Mat4 m, Vec4 v);
Mat4 mat4_perspective(f32 fov_y_rad, f32 aspect, f32 near_z, f32 far_z);
Mat4 mat4_look_at(Vec3 eye, Vec3 target, Vec3 up);
Mat4 mat4_inverse(Mat4 m);

#endif
