/* core/types.h — MATOKI temel tipleri ve yardımcı makrolar.
 * libc yok: tüm tipler derleyicinin önceden tanımlı makrolarından gelir.
 * Hem wasm32 hem native (host test) derlemesinde aynı şekilde çalışır.
 */
#ifndef CORE_TYPES_H
#define CORE_TYPES_H

typedef __UINT8_TYPE__   u8;
typedef __UINT16_TYPE__  u16;
typedef __UINT32_TYPE__  u32;
typedef __UINT64_TYPE__  u64;
typedef __INT8_TYPE__    i8;
typedef __INT16_TYPE__   i16;
typedef __INT32_TYPE__   i32;
typedef __INT64_TYPE__   i64;
typedef float            f32;
typedef double           f64;
typedef u32              b32;
typedef __SIZE_TYPE__    usize;
typedef __PTRDIFF_TYPE__ isize;
typedef __UINTPTR_TYPE__ uptr;

_Static_assert(sizeof(u8)  == 1, "u8");
_Static_assert(sizeof(u16) == 2, "u16");
_Static_assert(sizeof(u32) == 4, "u32");
_Static_assert(sizeof(u64) == 8, "u64");
_Static_assert(sizeof(f32) == 4, "f32");
_Static_assert(sizeof(f64) == 8, "f64");

#ifndef NULL
#define NULL ((void*)0)
#endif
#define TRUE  1
#define FALSE 0

#define ARRAY_LEN(a)      (sizeof(a) / sizeof((a)[0]))
#define MIN(a, b)         ((a) < (b) ? (a) : (b))
#define MAX(a, b)         ((a) > (b) ? (a) : (b))
#define CLAMP(x, lo, hi)  ((x) < (lo) ? (lo) : (x) > (hi) ? (hi) : (x))
#define ALIGN_UP(x, a)    (((x) + ((a) - 1)) & ~((a) - 1))
#define IS_POW2(x)        ((x) != 0 && (((x) & ((x) - 1)) == 0))
#define UNUSED(x)         ((void)(x))
#define KB(n)             ((usize)(n) << 10)
#define MB(n)             ((usize)(n) << 20)

#define U32_MAX 0xFFFFFFFFu
#define I32_MAX 0x7FFFFFFF
#define I32_MIN (-I32_MAX - 1)

#endif
