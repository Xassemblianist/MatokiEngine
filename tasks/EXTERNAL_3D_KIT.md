# MATOKI 3D-KIT — Bağımsız Geliştirme Görevi (dış ajan için)

> **Bu belge tek başına yeterlidir.** Ana projeye (MATOKI) erişimin yok ve olmayacak.
> Görevin: ana projeye sonradan **değiştirilmeden takılacak**, kendi başına derlenen ve test edilen bir C kütüphanesi yazmak.
> Ana projenin ortak başlıklarının birebir kopyaları bu belgede (§3). Onlara uyarsan kodun doğrudan çalışır.
> Belge uzundur; önce tamamını oku, sonra §4'teki iskeleti kur, sonra modüllere geç.

---

## 1. MATOKI nedir (bağlam)

MATOKI, tarayıcıda **GPU kullanmadan** çalışan bir yazılım render motorudur. Her şey sıfırdan yazılır:
- Motor C11, `clang --target=wasm32 -nostdlib` ile derlenir. **libc yok, malloc yok** (tüm bellek arena'lardan), Emscripten yok.
- Kendi rasterizer'ı (sabit nokta, SIMD, karo tabanlı), kendi shader dili **Matoki** (çalışma anında wasm SIMD'e JIT derlenir), kendi PNG kodlayıcı/çözücüsü, kendi wasm assembler'ı, kendi fontu ve UI'ı var.
- 2D tamam; şimdi **3D**'ye geçiliyor. Ana ekip rasterizer entegrasyonu, Matoki'ye türev/mipmap ekleme ve shader'lar üzerinde çalışıyor.
- **Senin işin:** rasterizer'dan ve shader dilinden **bağımsız** olan 3D altyapısı: asset yükleme (sıfırdan JSON + glTF/GLB + OBJ), mesh işleme, geometri boru hattı (dönüşüm + homojen kırpma + culling), mipmap referansı, prosedürel şehir üreteci, yazılım occlusion culling, kamera.

Proje felsefesi: **dış kütüphane yok, kopyala-yapıştır kod yok, her şey anlaşılarak sıfırdan; her optimizasyon iddiası ölçümle.**

---

## 2. Kurallar (kesin)

### 2.1 Kütüphane kodu (`kit/src/**`)
- **C11**, derleme bayrakları `-std=c11 -Wall -Wextra -Werror` (+ §4'teki wasm bayrakları). Hem **native host** hem **wasm32** derlemesinde **uyarısız** derlenmeli.
- **libc başlığı include etmek YASAK** — `<stdint.h>`, `<string.h>`, `<math.h>`, `<stdlib.h>`, `<stdio.h>` dahil hepsi. Yalnızca §3'teki başlıklar ve kendi başlıkların.
  - `memcpy/memset` gerekiyorsa `__builtin_memcpy` / `__builtin_memset` kullan (ana projede wasm'da bunlar tek opcode'a iner; host'ta libc'ye düşer — sorun değil).
  - Matematik: §3.4 `math.h`'deki fonksiyonlar (`m_sqrt`, `m_sin`, ...). Ek matematik gerekiyorsa kendi modülünde yaz.
- **malloc/free yok.** Tüm bellek parametre olarak verilen `Arena*`'dan (§3.3). Geçici bellek için `arena_mark` / `arena_pop_to`.
- Global değiştirilebilir durum yok (sabit tablolar hariç). Thread-güvenli tasarla: aynı fonksiyon farklı arena'larla eşzamanlı çağrılabilmeli.
- Tipler: `u8 u16 u32 u64 i8 i16 i32 i64 f32 f64 b32 usize isize uptr` (§3.1). `int`, `size_t`, `float` vb. yazma.
- İsimlendirme: `modul_fonksiyon()` (ör. `gltf_load`, `clip_triangle`), tipler `PascalCase`, sabitler/makrolar `UPPER_SNAKE`. Her `.c/.h` dosyasının başında 3-5 satırlık açıklama yorumu (Türkçe tercih edilir, İngilizce de kabul).
- Hata yönetimi: fonksiyonlar `b32` veya hata kodu döner + insan-okur hata mesajı için çağıranın verdiği `char* err, u32 err_cap` tamponu (biçimlendirme için §3.2 `fmt_format`). **Bozuk girdi asla çökertmez** (fuzz ile kanıtlanacak). `PANIC/ASSERT` yalnızca programlama hataları için (geçersiz parametre), girdi hataları için değil.
- `wasm_simd128.h` (clang'ın SIMD başlığı) **yalnızca** adı `_wasm.c` ile biten dosyalarda; her SIMD dosyasının skaler eşdeğeri olmalı (host testleri skaler yolu, wasm testi SIMD yolunu çalıştırır, sonuçlar bit-bit aynı).

### 2.2 Shim (`kit/shim/**`)
§3'teki başlıkların **birebir** kopyaları + onların senin yazacağın minimal implementasyonları (`shim/core/log.c`, `arena.c`, `math.c`, `mem.c`). Ana projeye entegrasyonda `shim/` **tamamen silinir** ve ana projenin gerçek implementasyonları kullanılır. Bu yüzden:
- Shim başlıklarını **değiştirme** (bir eksik görürsen `INTEGRATION.md`'ye yaz, kendi modülünde çöz).
- Davranışın shim implementasyonunun ayrıntısına dayanmasın (ör. arena'nın sıfırlanmış bellek döndürmesi sözleşmenin parçası — dayanabilirsin; ama `m_sin`'in tam bit çıktısına dayanma).

### 2.3 Testler ve araçlar (`kit/tests/**`, `kit/tools/**`)
- Burada libc serbest (`stdio`, `stdlib`, `string`, `math.h`, `popen`, `system`...).
- Test çatısı §3.5 (`test.h`) — birebir kullan, testlerin ana projeye aynen taşınacak.
- Python 3 (yalnızca standart kütüphane) test verisi üretmek ve çapraz doğrulama için serbest. **Dış model/doku dosyası indirme veya repoya koyma yok**; tüm test varlıkları betikle üretilir.

### 2.4 Ölçüm
- Her performans iddiası ölçümle: makine (CPU modeli), derleyici sürümü, bayraklar, tekrar sayısı, medyan. `kit/BENCH.md`'ye yaz. Tahmini sayı yazma.

---

## 3. Ana projenin ortak başlıkları (BİREBİR kopyala → `kit/shim/`)

### 3.1 `shim/core/types.h`
```c
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
```

### 3.2 `shim/core/log.h`
```c
/* core/log.h — mini printf, LOG/ASSERT/PANIC.
 * Desteklenen biçimler: %d %i %u %x %X %c %s %p %% , uzunluk: l ll z ,
 * %f / %.Nf (N <= 9), genişlik + '0' dolgu (ör. %08x, %5d, %-8s).
 */
#ifndef CORE_LOG_H
#define CORE_LOG_H

#include "core/types.h"

typedef __builtin_va_list VaList;
#define VA_START(ap, last) __builtin_va_start(ap, last)
#define VA_END(ap)         __builtin_va_end(ap)
#define VA_ARG(ap, T)      __builtin_va_arg(ap, T)

/* buf her zaman NUL ile biter; dönüş: yazılan karakter sayısı (NUL hariç, kırpılmış) */
u32  fmt_vformat(char* buf, u32 cap, const char* fmt, VaList ap);
u32  fmt_format(char* buf, u32 cap, const char* fmt, ...);

void log_printf(const char* fmt, ...);
_Noreturn void log_panic(const char* file, i32 line, const char* fmt, ...);

#define LOG(...)   log_printf(__VA_ARGS__)
#define PANIC(...) log_panic(__FILE__, __LINE__, __VA_ARGS__)
#define ASSERT(c)  do { if (!(c)) log_panic(__FILE__, __LINE__, "ASSERT: %s", #c); } while (0)

#endif
```
Shim implementasyonu (`shim/core/log.c`): host'ta `vsnprintf`'e devretmek **yasak** (kütüphane kuralı shim için de geçerli değil ama wasm derlemesi de geçmeli) — basit bir libc'siz biçimlendirici yaz veya `#if defined(__wasm__)` ile wasm'da minimal, host'ta libc kullan (shim zaten silinecek; **yalnızca shim'de** bu `#if` serbest). `log_printf`/`log_panic` host'ta stdout/stderr + `abort()`.

### 3.3 `shim/core/arena.h`
```c
/* core/arena.h — Doğrusal bellek ayırıcı (bump allocator).
 * Motor içinde dinamik bellek yalnızca Arena üzerinden ayrılır (malloc yok).
 * Tüm ayırmalar sıfırlanmış döner; kapasite aşımında PANIC çağrılır.
 */
#ifndef CORE_ARENA_H
#define CORE_ARENA_H

#include "core/types.h"

typedef struct {
    u8*   base;
    usize cap;
    usize used;
} Arena;

void  arena_init(Arena* a, void* base, usize cap);
void* arena_push(Arena* a, usize size, usize align);
void  arena_reset(Arena* a);
usize arena_mark(const Arena* a);
void  arena_pop_to(Arena* a, usize mark);
Arena arena_sub(Arena* parent, usize cap);

#define ARENA_PUSH_STRUCT(a, T)    ((T*)arena_push((a), sizeof(T), _Alignof(T)))
#define ARENA_PUSH_ARRAY(a, T, n)  ((T*)arena_push((a), sizeof(T) * (usize)(n), _Alignof(T)))

#endif
```
**Önemli:** `arena_push` taşmada PANIC eder. Bozuk/kötü niyetli girdi (ör. glTF'te `count: 4000000000`) arena'yı taşırmamalı — **ayırmadan önce boyutları doğrula** ve taşma kontrollü çarpım kullan; arena'nın kalan kapasitesini (`a->cap - a->used`) kontrol edip hata döndür. Bunun için yardımcı yaz: `b32 arena_can_push(const Arena*, usize size, usize align)` — kendi modülünde (`kit/src/asset/asset_util.h`), shim'e değil.

### 3.4 `shim/core/math.h` (kısaltılmış — kullanabileceğin her şey bu)
```c
#ifndef CORE_MATH_H
#define CORE_MATH_H
#include "core/types.h"

#define M_PI      3.14159265358979323846f
#define M_TAU     6.28318530717958647692f
#define M_HALF_PI 1.57079632679489661923f
#define M_DEG2RAD (M_PI / 180.0f)
#define M_RAD2DEG (180.0f / M_PI)

static inline f32 m_sqrt(f32 x) { return __builtin_sqrtf(x); }
static inline f32 m_abs(f32 x)  { return __builtin_fabsf(x); }
#if defined(__wasm__)
static inline f32 m_floor(f32 x) { return __builtin_floorf(x); }
static inline f32 m_ceil(f32 x)  { return __builtin_ceilf(x); }
static inline f32 m_trunc(f32 x) { return __builtin_truncf(x); }
#else
static inline f32 m_trunc(f32 x) { if (m_abs(x) >= 8388608.0f) return x; return (f32)(i32)x; }
static inline f32 m_floor(f32 x) { if (m_abs(x) >= 8388608.0f) return x; i32 i = (i32)x; return (f32)(i - (x < (f32)i)); }
static inline f32 m_ceil(f32 x)  { if (m_abs(x) >= 8388608.0f) return x; i32 i = (i32)x; return (f32)(i + (x > (f32)i)); }
#endif
static inline f32 m_min(f32 a, f32 b) { return a < b ? a : b; }
static inline f32 m_max(f32 a, f32 b) { return a > b ? a : b; }
static inline f32 m_clamp(f32 x, f32 lo, f32 hi) { return m_min(m_max(x, lo), hi); }
static inline f32 m_lerp(f32 a, f32 b, f32 t)    { return a + (b - a) * t; }

f32 m_sin(f32 x);  f32 m_cos(f32 x);  f32 m_tan(f32 x);  f32 m_atan2(f32 y, f32 x);
f32 m_exp2(f32 x); f32 m_log2(f32 x); f32 m_pow(f32 x, f32 y);

typedef struct { i32 x, y; }       IVec2;
typedef struct { f32 x, y; }       Vec2;
typedef struct { f32 x, y, z; }    Vec3;
typedef struct { f32 x, y, z, w; } Vec4;
/* Sütun-ana (column-major) yerleşim: m[col * N + row] */
typedef struct { f32 m[9]; }  Mat3;
typedef struct { f32 m[16]; } Mat4;

static inline IVec2 ivec2(i32 x, i32 y) { IVec2 r = { x, y }; return r; }
static inline Vec2  vec2(f32 x, f32 y)  { Vec2 r = { x, y }; return r; }
static inline Vec3  vec3(f32 x, f32 y, f32 z) { Vec3 r = { x, y, z }; return r; }
static inline Vec4  vec4(f32 x, f32 y, f32 z, f32 w) { Vec4 r = { x, y, z, w }; return r; }
static inline Vec2 vec2_add(Vec2 a, Vec2 b) { return vec2(a.x + b.x, a.y + b.y); }
static inline Vec2 vec2_sub(Vec2 a, Vec2 b) { return vec2(a.x - b.x, a.y - b.y); }
static inline Vec2 vec2_scale(Vec2 v, f32 s) { return vec2(v.x * s, v.y * s); }
static inline f32  vec2_dot(Vec2 a, Vec2 b) { return a.x * b.x + a.y * b.y; }
static inline f32  vec2_len(Vec2 v) { return m_sqrt(vec2_dot(v, v)); }
static inline Vec2 vec2_norm(Vec2 v) { f32 l = vec2_len(v); return l > 0.0f ? vec2_scale(v, 1.0f / l) : vec2(0.0f, 0.0f); }
static inline Vec3 vec3_add(Vec3 a, Vec3 b) { return vec3(a.x + b.x, a.y + b.y, a.z + b.z); }
static inline Vec3 vec3_sub(Vec3 a, Vec3 b) { return vec3(a.x - b.x, a.y - b.y, a.z - b.z); }
static inline Vec3 vec3_scale(Vec3 v, f32 s) { return vec3(v.x * s, v.y * s, v.z * s); }
static inline f32  vec3_dot(Vec3 a, Vec3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
static inline Vec3 vec3_cross(Vec3 a, Vec3 b) { return vec3(a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x); }
static inline f32  vec3_len(Vec3 v) { return m_sqrt(vec3_dot(v, v)); }
static inline Vec3 vec3_norm(Vec3 v) { f32 l = vec3_len(v); return l > 0.0f ? vec3_scale(v, 1.0f / l) : vec3(0.0f, 0.0f, 0.0f); }
static inline Vec4 vec4_add(Vec4 a, Vec4 b) { return vec4(a.x + b.x, a.y + b.y, a.z + b.z, a.w + b.w); }
static inline Vec4 vec4_sub(Vec4 a, Vec4 b) { return vec4(a.x - b.x, a.y - b.y, a.z - b.z, a.w - b.w); }
static inline Vec4 vec4_scale(Vec4 v, f32 s) { return vec4(v.x * s, v.y * s, v.z * s, v.w * s); }
static inline f32  vec4_dot(Vec4 a, Vec4 b) { return a.x * b.x + a.y * b.y + a.z * b.z + a.w * b.w; }

Mat3 mat3_identity(void);
Mat3 mat3_mul(Mat3 a, Mat3 b);
Vec2 mat3_mul_point(Mat3 m, Vec2 p);
Vec3 mat3_mul_vec3(Mat3 m, Vec3 v);
Mat3 mat3_2d(f32 tx, f32 ty, f32 rot_rad, f32 sx, f32 sy);
Mat4 mat4_identity(void);
Mat4 mat4_mul(Mat4 a, Mat4 b);
Vec4 mat4_mul_vec4(Mat4 m, Vec4 v);
/* Sağ-el, kamera -Z'ye bakar, klip derinliği 0..1 (WebGPU/D3D tarzı): near → z_ndc=0, far → z_ndc=1 */
Mat4 mat4_perspective(f32 fov_y_rad, f32 aspect, f32 near_z, f32 far_z);
Mat4 mat4_look_at(Vec3 eye, Vec3 target, Vec3 up);
Mat4 mat4_inverse(Mat4 m);
#endif
```
Shim `math.c`: bu fonksiyonları yaz (perspektif konvansiyonu yorumdaki gibi: `x_clip = f/aspect * x`, `y_clip = f * y`, `z_clip = far/(near-far) * z + near*far/(near-far)`, `w_clip = -z`; `f = 1/tan(fov/2)`). Senin kodun bu **konvansiyona** dayanabilir, shim'in sin/cos hassasiyetine dayanmamalı. Ek matris yardımcıları (TRS'den matris, quaternion, normal matrisi, ortografik) gerekiyorsa `kit/src/gfx3d/xform.h/.c`'de yaz (shim'e değil).

### 3.5 `shim/tests/test.h` (test çatısı — birebir)
```c
/* tests/test.h — mini test çatısı (sadece native host derlemesi).
 * Kullanım:  TEST(isim) { CHECK(x == 1); CHECK_NEAR(a, b, 1e-5); }
 * Testler constructor ile otomatik kaydolur; koşucu: tools/host/test_main.c
 */
#ifndef TESTS_TEST_H
#define TESTS_TEST_H

#include "core/types.h"

typedef void (*TestFn)(void);

void test_register(const char* name, const char* file, TestFn fn);
void test_fail(const char* file, i32 line, const char* expr);
void test_fail_near(const char* file, i32 line, const char* expr, f64 a, f64 b, f64 eps);

#define TEST(name)                                                        \
    static void test_fn_##name(void);                                     \
    __attribute__((constructor)) static void test_reg_##name(void) {      \
        test_register(#name, __FILE__, test_fn_##name);                   \
    }                                                                     \
    static void test_fn_##name(void)

#define CHECK(c) \
    do { if (!(c)) test_fail(__FILE__, __LINE__, #c); } while (0)

#define CHECK_NEAR(a, b, eps)                                             \
    do {                                                                  \
        f64 t_a_ = (f64)(a), t_b_ = (f64)(b), t_e_ = (f64)(eps);          \
        f64 t_d_ = t_a_ - t_b_; if (t_d_ < 0) t_d_ = -t_d_;               \
        if (!(t_d_ <= t_e_))                                              \
            test_fail_near(__FILE__, __LINE__, #a " ~ " #b, t_a_, t_b_, t_e_); \
    } while (0)

#endif
```
Test koşucusunu (`shim/tools/test_main.c`: kayıtlı testleri çalıştırır, `N geçti, M kaldı` basar, isim filtresi argümanı, kalan varsa çıkış kodu 1) kendin yaz.

### 3.6 Ana projedeki doku yapısı (Modül C için — birebir)
```c
/* Doku: genişlik/yükseklik 2'nin kuvveti (wrap = AND maskesi) */
typedef struct Texture {
    u32* px;           /* RGBA8, satır öncelikli; u32 little-endian = bellekte R,G,B,A (px & 0xFF = R) */
    u32  w, h;
    u32  w_mask, h_mask;   /* w-1, h-1 */
    u32  log2_w;
    u32  flags;        /* TEX_FLAG_* */
    u32  levels;       /* mip seviye sayısı >= 1 (0 görülürse 1 kabul et) */
} Texture;             /* sizeof == 32 (wasm32) */
#define TEX_FLAG_CLAMP     1u   /* yoksa wrap */
#define TEX_FLAG_BILINEAR  2u   /* yoksa en yakın komşu */
#define TEX_FLAG_MIPMAP    4u
```
Bunu `kit/src/mip/texture_abi.h` olarak koy (entegrasyonda ana projenin `raster/shade_abi.h`'ı ile değiştirilecek; bu yüzden **struct'a alan ekleme**).

---

## 4. İskelet ve build

```
kit/
  Makefile
  README.md            — ne var, nasıl derlenir/test edilir
  INTEGRATION.md       — ana projeye takma kılavuzu (§8)
  BENCH.md             — tüm ölçümler
  DECISIONS.md         — aldığın tasarım kararları ve gerekçeleri
  shim/core/{types.h,log.h,log.c,arena.h,arena.c,math.h,math.c,mem.c}
  shim/tests/test.h
  shim/tools/test_main.c
  src/asset/   src/gfx3d/   src/mip/   src/city/   src/occl/
  tests/       (test_*.c, assets/make_assets.py, fuzz)
  tools/       (tool_*.c native araçlar)
```

**Makefile gereksinimleri:**
- `make` → host test koşucusu + araçlar; `make test` → testleri çalıştır; `make wasm` → `src/**/*.c` + shim'i wasm32'ye **derle ve linkle** (yalnızca derlenebilirlik + libc import'u olmadığının kanıtı: `wasm-objdump -x` veya `wasm2wat` ile import listesinde yalnızca `env.*` shim sembolleri; `make wasm-check` hedefi bunu otomatik doğrulasın); `make fuzz`; `make bench`.
- Host: `clang -std=c11 -O1 -g -Wall -Wextra -Werror -fsanitize=address,undefined -fno-omit-frame-pointer -Ishim -Isrc`.
- wasm: `clang --target=wasm32 -std=c11 -O2 -ffreestanding -fno-builtin -nostdlib -msimd128 -mbulk-memory -mnontrapping-fptoint -msign-ext -Wall -Wextra -Werror -Ishim -Isrc`, link: `-Wl,--no-entry -Wl,--export-all -Wl,--allow-undefined` (yalnızca kontrol amaçlı).
  - wasm'da `memcpy/memset` sembolleri için `shim/core/mem.c`: `__builtin_memcpy` ile tanımla (`-mbulk-memory` ile tek opcode'a iner).
- Gerekli araçlar: `clang` (≥ 17), `wasm-ld`, `wabt` (`wasm-objdump`/`wasm2wat`), `python3`. Node gerekmez (SIMD yolunu wasm'da çalıştırmak istersen `node` ile test edebilirsin — opsiyonel ama SIMD dosyası yazarsan bit-bit eşitliği node ile doğrula).

---

## 5. Modüller

Sıra önerisi: A → B → C → D → E → F. Her modül sonunda `make test` ve `make wasm-check` uyarısız geçmeli; `BENCH.md` ve `DECISIONS.md` güncellenmeli.

---

### Modül A — Asset hattı (`src/asset/`) — EN BÜYÜK PARÇA

#### A1. JSON (`json.h/.c`)
- RFC 8259. API önerisi (değiştirebilirsin, `DECISIONS.md`'ye yaz):
  ```c
  typedef enum { JSON_NULL, JSON_FALSE, JSON_TRUE, JSON_NUMBER, JSON_STRING, JSON_ARRAY, JSON_OBJECT } JsonType;
  typedef struct JsonValue JsonValue;   /* arena'da DOM */
  b32 json_parse(const char* src, usize len, Arena* a, JsonValue** out, char* err, u32 err_cap);  /* err: "satır:sütun: mesaj" */
  JsonType json_type(const JsonValue*);
  const JsonValue* json_get(const JsonValue* obj, const char* key);     /* yoksa NULL */
  const JsonValue* json_at(const JsonValue* arr, u32 i);
  u32 json_len(const JsonValue*);  f64 json_num(const JsonValue*); /* + u32/i32 güvenli dönüşümler: aralık dışı → FALSE */
  const char* json_str(const JsonValue*, u32* len);                    /* UTF-8, kaçışlar çözülmüş */
  ```
- UTF-8 doğrulama (aşırı uzun kodlamalar, vekil kod noktaları reddedilir), `\uXXXX` + vekil çiftleri → UTF-8, kontrol karakterleri reddi.
- **Sayılar:** f64'e dönüşüm doğru yuvarlanmış olmalı (Eisel–Lemire + yedek yol, veya Clinger + büyük sayı yedeği). En az: tüm test vakalarında Python `float()` ile **bit-bit aynı**. Tamsayılar 2^53'e kadar tam.
- Derinlik sınırı (ör. 512), yinelenen anahtarlar: son kazanır (kararını yaz), boş girdi/fazladan içerik hata.
- Performans: büyük glTF'ler için (ör. 50 MB JSON) tek geçiş, düşük ayırma. Hızı ölç (MB/s).

#### A2. Base64 (`base64.h/.c`)
- `data:application/octet-stream;base64,...` ve `data:image/png;base64,...` URI'leri. Geçersiz karakter/padding → hata.

#### A3. glTF 2.0 + GLB (`gltf.h/.c`)
- Girdi: `.gltf` (JSON, gömülü data URI'li buffer'lar) veya `.glb` (12 byte başlık + JSON chunk + BIN chunk). Harici dosya referanslı buffer/image (`"uri": "x.bin"`) için **çözümleyici geri çağırma**: `typedef b32 (*GltfFileResolver)(void* user, const char* uri, u32 uri_len, const u8** data, usize* len);` — NULL ise hata.
- Çıktı (sade, motor dostu, SoA):
  ```c
  typedef struct {
      u32  vertex_count, index_count;
      f32 *px, *py, *pz;          /* zorunlu */
      f32 *nx, *ny, *nz;          /* yoksa hesaplanır (A5) */
      f32 *u, *v;                 /* TEXCOORD_0, yoksa NULL */
      f32 *tx, *ty, *tz, *tw;     /* TANGENT (w = bitangent işareti), yoksa NULL veya hesaplanmış */
      u32 *color;                 /* COLOR_0 → RGBA8, yoksa NULL */
      u32 *indices;               /* her zaman u32'ye genişletilmiş; indekssiz primitive → 0..n-1 */
      u32  material;              /* GltfScene.materials indeksi veya U32_MAX */
      Vec3 aabb_min, aabb_max;
  } GltfPrimitive;
  typedef struct { f32 base_color[4]; i32 base_color_tex; f32 metallic, roughness; i32 metal_rough_tex;
                   i32 normal_tex; f32 normal_scale; i32 occlusion_tex; f32 emissive[3]; i32 emissive_tex;
                   u32 alpha_mode; /* 0 OPAQUE 1 MASK 2 BLEND */ f32 alpha_cutoff; b32 double_sided; } GltfMaterial;
  typedef struct { const u8* data; usize len; u32 mime; /* 0 png 1 jpeg 2 diğer */ u32 sampler_wrap_s, sampler_wrap_t, min_filter, mag_filter; } GltfImage;
  typedef struct { u32 primitive_first, primitive_count; Mat4 world; i32 parent; } GltfNodeInstance;
  typedef struct { GltfPrimitive* prims; u32 prim_count; GltfMaterial* materials; u32 material_count;
                   GltfImage* images; u32 image_count; /* textures[] → images[] çözülmüş */
                   GltfNodeInstance* instances; u32 instance_count; Vec3 aabb_min, aabb_max; } GltfScene;
  b32 gltf_load(const u8* data, usize len, GltfFileResolver res, void* user, Arena* a, GltfScene* out, char* err, u32 err_cap);
  ```
  (Yapıyı geliştirebilirsin; son halini `INTEGRATION.md`'de belgele.)
- Destek: `scene` (varsayılan sahne; yoksa 0; o da yoksa tüm kök node'lar), node hiyerarşisi (TRS — quaternion → matris; veya `matrix`), dünya matrisleri (döngü tespiti!), mesh başına çok primitive, `mode` yalnızca TRIANGLES (4) — STRIP/FAN üçgenlere çevrilir, diğerleri uyarıyla atlanır.
- Accessor'lar: tüm `componentType`'lar (5120..5126), `normalized`, `byteStride` (interleaved), `byteOffset` hizalama kuralları, **sparse accessor** (spec'te var — destekle), `min/max`. Her erişimde sınır kontrolü (bufferView/buffer dışına taşma → hata).
- Görüntüler **çözülmez** — ham bayt aralığı + mime döner (ana projede kendi PNG çözücümüz var). JPEG → mime=1 (ana proje desteklemiyor; uyarı).
- Desteklenmeyenler (skin, morph targets, animations, cameras, KHR_* uzantıları): çökmeden yoksay + `warnings` listesi (arena'da string'ler). `extensionsRequired` içinde bilinmeyen uzantı → hata.

#### A4. OBJ + MTL (`obj.h/.c`)
- `v vt vn f` (çokgen → fan üçgenleme; içbükey çokgenler için kulak kırpma — opsiyonel, kararını yaz), negatif indeksler, `o/g/usemtl`, `mtllib` (çözümleyici geri çağırma ile), `Kd/Ka/Ks/Ns/d/map_Kd`.
- Çıktı: A3 ile aynı `GltfScene`-benzeri yapı (ortak `Scene3D` tipine birleştirmen önerilir — `asset/scene3d.h`, kararını yaz).
- Vertex tekilleştirme: (pos, uv, normal) üçlüsü → hash tablosu (arena'da, açık adresleme).

#### A5. Mesh işleme (`mesh_proc.h/.c`)
- Yüz/köşe normalleri (alan ağırlıklı, açı ağırlıklı opsiyonel), sert kenar eşiği (açı) ile normal ayırma.
- Tangent hesabı (UV'li mesh'ler): MikkTSpace ile **uyumlu** sonuç hedefle (algoritmayı makaleden/spec'ten anlayarak kendin yaz; kodu kopyalama). En azından tutarlı tangent + bitangent işareti.
- Vertex kaynaştırma (epsilon), dejenere üçgen silme.
- **Vertex cache optimizasyonu:** Tom Forsyth'in "Linear-Speed Vertex Cache Optimisation" algoritmasını anlayıp yaz. **ACMR** (ortalama önbellek ıskası oranı, FIFO 16/32 ve LRU 32 modelleri) önce/sonra ölç.
- Overdraw için kaba sıralama (opsiyonel), vertex fetch optimizasyonu (indeks sırasına göre vertex yeniden sıralama).
- Basit mesh sadeleştirme (LOD için): quadric error metrics (Garland–Heckbert), hedef üçgen oranı; kenar çökertmede topoloji korumalı. Ölç: hata vs üçgen sayısı.

#### A6. Kendi binary formatımız `.mmsh` (`mmsh.h/.c`)
- Amaç: tarayıcıda **sıfır kopya** yükleme (dosya baytları belleğe gelir, işaretçiler ofsetten çözülür).
- Başlık: sihirli sayı `"MMSH"`, sürüm, endian işareti, bölüm tablosu (tip, ofset, boyut — 16 byte hizalı), CRC32 (kendi implementasyonun; polinom 0xEDB88320). İçerik: Scene3D'nin tamamı (primitive'ler SoA, materyaller, görüntü baytları (sıkıştırılmamış PNG baytları olduğu gibi), instance'lar, LOD zincirleri).
- `mmsh_write(const Scene3D*, Arena*, u8** out, usize* len)`, `mmsh_load_inplace(u8* data, usize len, Scene3D* out, char* err, u32 cap)` — doğrulama: tüm ofset/boyutlar sınır içinde, hizalama, CRC.
- Araç: `tools/tool_meshc.c` → `meshc girdi.(glb|gltf|obj) -o çıktı.mmsh [--optimize] [--lod 0.5,0.25] [--stats]`.

#### A7. Test varlıkları ve testler
- `tests/assets/make_assets.py`: küp, UV küre, torus, 2 seviyeli node hiyerarşisi; gömülü base64 glTF, harici .bin'li glTF, GLB, interleaved (byteStride) buffer, u8/u16/u32 indeksler, normalized u8/u16 UV, sparse accessor, STRIP primitive, KHR uzantılı (yoksayılacak) ve `extensionsRequired`'lı (hata verecek) dosyalar; OBJ+MTL (negatif indeksler, çokgenler). Beklenen değerleri (vertex sayısı, AABB, dünya matrisleri) bir JSON manifest'e de yazsın; C testleri manifest'le karşılaştırsın.
- JSON: **en az 120 vaka** (geçerli/geçersiz/uygulamaya bağlı), sayı dönüşümü için 10.000 rastgele sayı dizesi → Python `float()` sonucuyla bit-bit (popen ile toplu).
- **Fuzz** (`make fuzz`): JSON, GLB, OBJ, .mmsh parser'larına en az 200.000'er mutasyon (bit çevirme, bayt ekleme/silme, sayı sınır değerleri, chunk uzunluk bozma) — host ASan/UBSan açık; 0 çökme, 0 sızıntı (arena kullandığın için sızıntı = arena taşması/PANIC olmamalı → hata dönmeli). Kendi basit mutasyon motorunu yaz (libFuzzer kullanma — clang'da varsa *ek olarak* kullanabilirsin).
- Performans (`BENCH.md`): JSON MB/s (1 MB, 10 MB, 50 MB üretilmiş glTF JSON), GLB yükleme süresi (10 MB, 1M üçgen), OBJ MB/s, tekilleştirme, Forsyth optimizasyonu süresi + ACMR tablosu, QEM sadeleştirme süresi.

---

### Modül B — Geometri boru hattı (`src/gfx3d/`)

Rasterizer ana projede. Senin boru hattın **ekran uzayı üçgenleri** üretir; formatı:
```c
#define G3D_MAX_VARYINGS 16
typedef struct {
    f32 x, y;        /* piksel uzayı, (0,0) sol üst köşe, y aşağı; piksel merkezleri +0.5'te */
    f32 z;           /* 0..1 derinlik (NDC z) */
    f32 inv_w;       /* 1 / w_clip */
    f32 var[G3D_MAX_VARYINGS];  /* ham varyingler (perspektif bölmesi YAPILMAMIŞ) — rasterizer inv_w ile perspektif-doğru interpolasyon yapar */
} G3dScreenVert;
typedef void (*G3dEmitTri)(void* user, const G3dScreenVert* v0, const G3dScreenVert* v1, const G3dScreenVert* v2);
```
(Not: ana projenin rasterizer'ının varying'leri `var` mı yoksa `var*inv_w` olarak mı beklediği entegrasyonda kontrol edilecek; sen **ham** ver ve `INTEGRATION.md`'de belirt. Tek satırlık dönüşüm.)

#### B1. Dönüşüm ve vertex aşaması (`xform.*`, `vertex.*`)
- TRS → Mat4, quaternion (normalize, slerp, eulerden), normal matrisi (ters-devrik 3×3), ortografik projeksiyon.
- Vertex aşaması: SoA girdi (Mesh) → klip uzayı `Vec4` + varying'ler (dünya konumu, dünya normali, UV, tangent — hangi varyingler üretilecek bir maske ile seçilir). **SIMD sürüm** (`vertex_simd_wasm.c`, 4 vertex/iterasyon, `f32x4`) + skaler eşdeğer, bit-bit aynı (FMA kullanma; işlem sırası aynı olmalı).
- Vertex önbelleği: indeksli çizimde dönüşüm sonuçlarını vertex başına bir kez hesapla (post-transform cache = tüm vertex'leri önce dönüştür, sonra indeksle — basit ve SIMD dostu).

#### B2. Homojen kırpma (`clip.*`)
- Yakın düzlem `z_clip >= 0` (konvansiyon §3.4): **gerçek kırpma**, Sutherland–Hodgman, homojen uzayda, varyingler doğrusal interpolasyon (klip uzayında — perspektif doğru).
- Uzak düzlem `z_clip <= w_clip`: kırp (veya opsiyonel: depth clamp modu — bayrak).
- Sol/sağ/üst/alt: **guard band**. Ana projenin rasterizer'ı 24.8 sabit nokta kullanıyor ve kenar fonksiyonlarını i32'de hesaplıyor; güvenli ekran koordinat aralığını parametre yap: `G3dClipConfig { f32 guard_min_x, guard_max_x, guard_min_y, guard_max_y; }` (varsayılan ±4096 piksel öneri — `DECISIONS.md`'de 24.8 + i32 kenar fonksiyonu çarpımı taşma analizini yap ve güvenli sınırı türet). Yalnızca guard band'i aşan üçgenleri bu düzlemlere karşı kırp.
- Tamamen dışarıda olanları erken ele (her düzlem için tüm köşeler dışarıdaysa), tamamen içeridekileri kırpmadan geçir (hızlı yol).
- `w` çok küçük / NaN / Inf güvenliği.
- Arka yüz ayıklama (ekran uzayında işaretli alan; CW/CCW ayarlanabilir, çift yüzlü materyal bayrağı) ve sıfır alanlı / hiçbir piksel merkezini kapsamayan çok küçük üçgen eleme (opsiyonel, ölç).
- Viewport dönüşümü: NDC → piksel (y çevrilir).

#### B3. Culling (`cull.*`)
- Frustum düzlemleri view-projection'dan (Gribb–Hartmann), normalize; küre ve AABB testleri (AABB için p-vertex/n-vertex), sonuç: DIŞARIDA / KESİŞİYOR / İÇERİDE (İÇERİDE ise kırpma atlanabilir — boru hattına bayrak).
- Nesne başına + chunk başına. İsteğe bağlı: küme (cluster/meshlet) culling — mesh'i ~64-128 üçgenlik kümelere böl, küme başına sınır küre + normal konisi (backface cone culling). Ölç.

#### B4. Boru hattı API'si (`pipeline.h/.c`)
```c
typedef struct { const Mesh* mesh; Mat4 model; u32 varying_mask; b32 double_sided; } G3dDraw;
typedef struct { Mat4 view, proj; u32 vp_w, vp_h; G3dClipConfig clip; b32 cull_ccw; } G3dView;
typedef struct { u32 submitted, frustum_culled_objects, backface_culled, clipped, clip_generated, emitted; } G3dStats;
void g3d_draw(const G3dView*, const G3dDraw*, Arena* scratch, G3dEmitTri emit, void* user, G3dStats* stats);
```
`Mesh` tipini A ile ortak tasarla (Scene3D primitive'inden sıfır kopya oluşturulabilmeli).

#### B5. Testler
- Kırpma: 100.000 rastgele üçgen (yakın düzlemi kesen, tamamen önde/arkada, w≈0'a yakın) → çıkan her köşe `z>=-eps`; köşe varyingleri analitik (doğru üzerindeki gerçek kesişim) ile 1e-5; **alan korunumu** (kırpılmış poligonların klip-uzayı alanı = analitik).
- **Çatlak yok testi:** test klasörüne kendi basit referans rasterizer'ını yaz (`tests/ref_raster.c` — kayan noktalı, top-left kuralı, yalnızca test için). Ortak kenarlı rastgele üçgen ağları (grid mesh, kamera içinden geçiyor) → boru hattı + referans raster → her piksel tam bir kez boyanır (mesh'in ekran izdüşümü içinde).
- Perspektif-doğru interpolasyon: eğik düzlem, piksel merkezlerinde UV analitik değere 1e-4.
- Guard band: dev üçgen (ekranın 100× büyük) — kırpma sonrası koordinatlar sınırda, raster sonucu doğru.
- Culling: bilinen konumlu kutular ve küreler için doğru sınıflandırma; 1M rastgele test, muhafazakârlık (asla görünür bir şeyi DIŞARIDA deme).
- SIMD vertex yolu vs skaler: bit-bit (wasm'da node ile çalıştır — opsiyonel ama SIMD yazarsan zorunlu).
- Performans (`BENCH.md`): vertex dönüşümü Mvert/s (skaler/SIMD, host ve wasm), kırpma Mtri/s (kırpma yok / %10 kırpılan / %50 kırpılan), culling Mtest/s.

---

### Modül C — Mipmap referansı (`src/mip/`)

Ana proje, Matoki shader diline mipmap'li örnekleme ekliyor. JIT'lenmiş kod bu **C referansıyla bit-bit** karşılaştırılacak. Yani bu modül **kâhin (oracle)**: semantiğe harfiyen uy.

#### C1. Mip zinciri yerleşimi ve üretimi (`mip.h/.c`)
- **Yerleşim (sözleşme):** tüm seviyeler `px`'ten başlayarak bitişik. Seviye k: `wk = max(1, w >> k)`, `hk = max(1, h >> k)`, başlangıç `px + Σ_{i<k} wi*hi`. `levels <= 1 + floor(log2(max(w,h)))`. Kare olmayan dokular desteklenir (ör. 256×16).
- `u32 mip_chain_texel_count(u32 w, u32 h, u32 levels)`, `u32* mip_level_ptr(const Texture*, u32 k, u32* wk, u32* hk)`.
- Üretim: `b32 mip_build(Texture* t, Arena* a, u32 max_levels, u32 filter)` — kutu filtresi (2×2 ortalama; tek boyut 1 olunca 2×1 / 1×2), **lineer** ve **sRGB-doğru** (RGB kanallarını sRGB→lineer→ortalama→sRGB; alfa lineer) iki mod. sRGB dönüşümü için kendi tablon/polinomun (256 girişli sRGB→lineer tablo + lineer→sRGB için doğru yuvarlanmış ikili arama veya 4096 girişli tablo — doğruluğu ölç). Yuvarlama kuralını yaz ve tutarlı ol: ortalama = `(a+b+c+d+2) >> 2`.
- Alfa test edilen dokular için alfa kapsama koruma (opsiyonel: "alpha-to-coverage preserving mips" — her seviyede alfa eşiğini geçen piksel oranını seviye 0 ile eşleyecek şekilde ölçekle). Ölç.

#### C2. Seviye seçimi ve örnekleme (`mip_sample.h/.c`) — SEMANTİK (harfiyen)
Bir 2×2 quad için (lane sırası: lane0=(x,y) lane1=(x+1,y) lane2=(x,y+1) lane3=(x+1,y+1)), 4 lane'in `u[4], v[4]` (f32) değerleri verilir:
1. **Kaba türevler** (tüm lane'ler için aynı): `dudx = u[1]-u[0]`, `dvdx = v[1]-v[0]`, `dudy = u[2]-u[0]`, `dvdy = v[2]-v[0]`. Hepsi f32 çıkarma.
2. Ölçekle (seviye 0 boyutları, `(f32)w`, `(f32)h`): `ax = dudx*W`, `bx = dvdx*H`, `ay = dudy*W`, `by = dvdy*H`.
3. `px2 = ax*ax + bx*bx`, `py2 = ay*ay + by*by` (bu sırayla, f32, FMA YOK: önce iki çarpım, sonra toplam).
4. `rho2 = (px2 > py2) ? px2 : py2`. Herhangi biri NaN ise → `level = 0`.
5. **Tamsayı seviye (karekök/log yok):** `bits = bitcast_u32(rho2)`; `e = (i32)((bits >> 23) & 0xFF) - 127`; `e < 0` (yani rho2 < 1, denormal ve 0 dahil) → `level = 0`; aksi halde `level = e >> 1` (= floor(log2(sqrt(rho2))) = floor(log2(rho))). `rho2 == +Inf` (e = 128) → `level = levels-1`.
6. `level = min(level, levels - 1)` (u32).
7. Seviye k'da örnekle (lane başına kendi `u, v`'si ile) — aşağıdaki **seviye-yerel** kurallarla (`W' = wk`, `H' = hk`, f32):
   - **En yakın:** `tx = trunc_sat_i32(floor(u * W'))`, `ty = trunc_sat_i32(floor(v * H'))`. wrap: `tx &= wk-1`, `ty &= hk-1`; clamp: `tx = clamp(tx, 0, wk-1)` (ty aynı).
   - **Bilinear:** `fu = u * W' - 0.5f`, `fv = v * H' - 0.5f`; `x0 = floor(fu)`, `y0 = floor(fv)` (f32); `ax = fu - x0`, `ay = fv - y0`; `ix0 = trunc_sat_i32(x0)`, `ix1 = ix0 + 1` (y aynı); her biri wrap/clamp; 4 texel; kanal başına `lerp(lerp(t00, t10, ax), lerp(t01, t11, ax), ay)`, `lerp(a,b,t) = a + (b - a) * t` (bu ifade sırasıyla, FMA yok).
   - Texel → f32: kanal `/ 255.0f` (bölme, çarpma değil), sıra R,G,B,A (`px & 0xFF` = R).
   - `trunc_sat_i32`: NaN → 0, `>= 2^31` → `I32_MAX`, `< -2^31` → `I32_MIN`, aksi halde sıfıra doğru kesme (wasm `i32.trunc_sat_f32_s` semantiği).
8. Mipmap'siz doku (`flags & TEX_FLAG_MIPMAP == 0` veya `levels <= 1`) → her zaman seviye 0 (adım 1-6 atlanır).
- API: `void mip_sample_quad(const Texture* t, const f32 u[4], const f32 v[4], f32 out_rgba[4][4], u32* out_level)` + tek lane'li `mip_sample_level(...)` (açık seviye).
- **Wasm-host eşitliği:** f32 işlemleri `-ffp-contract=off` ile derlenmeli (host'ta x86 FMA kaynaşmasını engelle — Makefile'a ekle ve `DECISIONS.md`'de belirt). `floor`/`trunc` için §3.4 `m_floor`/`m_trunc` kullan.

#### C3. Testler
- Yerleşim: ofsetler, kare olmayan, `levels` sınırı.
- Seviye seçimi: bilinen ölçekler (1×, 2×, 4×, 8×, 1/2×) → beklenen seviye; sınır değerleri (rho2 tam 4.0, 3.9999998, denormal, Inf, NaN).
- Referans çapraz kontrolü: aynı semantiği Python'da (`struct` ile f32 yuvarlama — her işlemden sonra `struct.unpack('f', struct.pack('f', x))`) yaz, 100.000 rastgele quad × rastgele doku → C sonucu ile **bit-bit** (popen ile toplu).
- sRGB mip üretimi: tablo doğruluğu (256 değerin hepsi için gidiş-dönüş), bilinen desenlerde beklenen ortalamalar.

---

### Modül D — Prosedürel şehir üreteci (`src/city/`) — "GTA'ya selam"

Saf geometri + veri üretimi (render yok). Seed'den **tamamen deterministik** (aynı seed → bit-bit aynı çıktı, host ve wasm'da). Rastgelelik: kendi PCG32'n (`city_rng.h` — PCG32 resmi algoritması, seed+seq) — global durum yok.

#### D1. Yerleşim (`city_layout.*`)
- Dünya chunk'lara bölünür (varsayılan 128 m × 128 m), sonsuz/sınırlı mod. Chunk (cx, cz) içeriği **yalnızca seed + (cx, cz)'den** türetilmeli (komşu chunk'ları üretmeden; sınırlarda yollar eşleşmeli — sınır koşullarını hash ile belirle).
- Sokak ağı: ana bulvarlar (düzenli), ara sokaklar (hafif bozulmuş ızgara), kavşaklar; yol genişlikleri, kaldırımlar.
- Bloklar → parseller → bina yerleşimi. Mahalle türleri (gürültü alanıyla: merkez = gökdelen, çevre = alçak, park alanları).
- Çıktı veri yapıları: yol grafiği (düğüm/kenar, şerit sayısı), parseller, bina tanımları, sokak lambası konumları, park/ağaç konumları.

#### D2. Bina geometrisi (`city_mesh.*`)
- Bina türleri: kutu, kademeli (setback) kule, L/U plan, çatı varyasyonları (düz + çatı üstü kutular, eğimli, kuleli), zemin kat farklı yükseklik. Pencere ızgarası için UV'ler (cephe dokusu atlas koordinatları — doku ana projede üretilecek; sen UV + `facade_style` id ver).
- Yollar, kaldırımlar, şerit çizgileri, kavşak yüzeyleri, sokak lambası direkleri (düşük poligon), basit ağaçlar.
- Chunk başına **birleştirilmiş statik mesh** (Mesh/Scene3D formatında), materyal grupları (draw call azaltma).
- **LOD zinciri:** LOD0 (tam), LOD1 (pencere/çatı detayı yok), LOD2 (bina = tek kutu, chunk = birleşik silüet). A5'teki QEM'i değil, **yapısal** basitleştirmeyi kullan (daha ucuz, daha temiz). Üçgen sayılarını raporla.

#### D3. Araç trafiği (`city_traffic.*`)
- Yol grafiği üzerinde şeritte giden araçlar: konum/yön, hız, kavşakta rastgele dönüş (deterministik), basit takip mesafesi, kavşak öncelik kuralı (basit). `city_traffic_step(state, dt)` — sabit adım, deterministik.
- Araç = kutu gövde + kabin (tek mesh, instancing için dönüşüm matrisi listesi).

#### D4. Testler + araç
- Determinizm: aynı seed → aynı çıktı hash'i (iki kez üret; ayrıca farklı chunk üretim sırasında aynı chunk aynı hash).
- Chunk sınırı sürekliliği: komşu chunk'ların yol uçları eşleşir (konum toleransı 1e-4).
- Geometri geçerliliği: dejenere üçgen yok, normaller dışa bakıyor, binalar yola taşmıyor, binalar birbirine girmiyor.
- Trafik: 10.000 adım sonra araçlar yol üzerinde, çarpışma (aynı şeritte iç içe geçme) yok.
- Araç: `tools/tool_citygen.c` → `citygen --seed 42 --chunks 8x8 -o city.obj` (OBJ çıktısı ile herhangi bir görüntüleyicide kontrol edilebilir) ve `-o city.mmsh`; istatistik: chunk başına üçgen (LOD0/1/2), bina sayısı, lamba sayısı, üretim süresi (ms/chunk).
- Performans: chunk üretim süresi (ana projede çalışma anında chunk streaming yapılacak — hedef: orta yoğunlukta chunk başına < 5 ms wasm'da; host'ta ölç ve wasm tahmini YAPMA, wasm'da node ile ölçebiliyorsan ölç).

---

### Modül E — Yazılım occlusion culling (`src/occl/`)

Ana projede çizim tamamen CPU'da olduğu için **çizilmeyen her bina saf kazanç**. Şehirde binalar birbirini kapatır.

- Düşük çözünürlüklü (ör. 256×128 veya 320×192) **yalnızca derinlik** tamponu; büyük örtücüler (binaların basitleştirilmiş kutuları — LOD2) önden arkaya rasterize edilir; sonra her nesnenin ekran-uzayı AABB'si (en yakın derinlikle) tampona karşı test edilir.
- **Hiyerarşik Z (Hi-Z)** veya Intel'in "Masked Software Occlusion Culling" (Hasselgren, Andersson, Akenine-Möller, 2016) yaklaşımı — makaleyi anlayıp kendin uygula; kod kopyalama. Seçimini ölçümle `DECISIONS.md`'de gerekçelendir. En azından: 8×4 veya 32×1 piksel blok başına maske + 2 derinlik katmanı.
- Kırpma: örtücü üçgenler için yakın düzlem kırpması (B2'yi yeniden kullan).
- **Muhafazakârlık zorunlu:** görünür bir nesneyi asla gizli ilan etme (yanlış negatif = görsel hata). Yanlış pozitif (gizliyi görünür demek) yalnızca performans kaybı.
- SIMD (`*_wasm.c`) + skaler, bit-bit aynı.
- API: `occl_begin(view)`, `occl_add_occluder(tris...)`, `occl_test_aabb(min, max) -> b32 visible`.
- Testler: muhafazakârlık — referans: tam çözünürlüklü kayan noktalı derinlik tamponu (test kodunda) ile 100.000 rastgele sahne/AABB; occl "gizli" dediği her şey referansta da tamamen gizli olmalı (0 ihlal). Verimlilik: referansta gizli olanların yüzde kaçını yakalıyor (şehir sahnesinde, D'nin ürettiği geometriyle).
- Performans: şehir sahnesi (8×8 chunk, kamera sokak seviyesinde) → örtücü raster ms, test ms, culled nesne oranı. Bu sayılar ana projenin "occlusion culling'e değer mi" kararını verecek.

---

### Modül F — Kamera ve yürüme (`src/gfx3d/camera.*`)

- Girdi bağımsız: `CameraInput { f32 move_x, move_y, move_z; f32 look_dx, look_dy; b32 jump, run; f32 dt; }`.
- Modlar: serbest uçuş (FPS), yörünge (orbit, hedef etrafında + zoom), **yürüme**: yerçekimi, zıplama, AABB oyuncu vs. şehir çarpışması (binalar = kutular; D'nin bina AABB'lerini kullanan basit uzamsal ızgara), basamak çıkma (küçük yükseklik farkları), kayma (duvar boyunca).
- Yumuşatma (fare ivmesi yok, isteğe bağlı yumuşatma), pitch sınırı.
- Çıktı: `view` matrisi + `proj` (FOV, aspect, near, far).
- Testler: çarpışmada duvardan geçmeme (yüksek hızda tünelleme yok — sweep testi), yerçekimi/zıplama fiziği sabit adımda deterministik.

---

## 6. Genel kabul kriterleri

- `make`, `make test`, `make wasm-check`, `make fuzz` **uyarısız ve hatasız**. Tüm testler geçer (ASan/UBSan temiz).
- `src/**` libc başlığı içermiyor (otomatik kontrol: `make lint` → `grep -rn '#include <' src/` yalnızca `wasm_simd128.h` dönebilir).
- `src/**` içinde `malloc/free/realloc/calloc` geçmiyor (otomatik kontrol).
- wasm derlemesinde import listesinde libc sembolü yok.
- Hiçbir dış varlık dosyası yok; test varlıkları betikle üretiliyor.
- `README.md`, `INTEGRATION.md`, `BENCH.md`, `DECISIONS.md` dolu.

## 7. Öncelik (zaman yetmezse)
1. **A1-A3 (JSON + glTF/GLB)** + A7 testleri/fuzz — ana proje en çok buna ihtiyaç duyuyor.
2. **C (mipmap referansı)** — küçük ama kritik; ana projenin JIT doğrulaması buna bağlı.
3. **B (boru hattı + kırpma + culling)**.
4. D (şehir), A4-A6, E (occlusion), F (kamera).
Her tamamlanan modül tek başına değerli olmalı — yarım modül bırakma; bitmeyeni `README.md`'de "yapılmadı" olarak işaretle.

## 8. INTEGRATION.md içeriği (zorunlu)
Ana projenin ajanları bu belgeyle entegre edecek. İçermesi gerekenler:
- Dosya ağacı ve her modülün public API özeti (başlık dosyası + kısa kullanım örneği).
- **Shim varsayımları:** shim başlıklarında beklediğin her davranış (arena sıfırlama, PANIC davranışı, perspektif konvansiyonu).
- Ana projenin build sistemine takma: `kit/src/*` → ana repoda `src/` altına aynı yollarla kopyalanacak (`src/asset/`, `src/gfx3d/`, `src/mip/`, `src/city/`, `src/occl/`); `kit/tests/*` → `tests/`; `kit/tools/tool_*.c` → `tools/host/`. Ana projenin Makefile'ı `src/**/*.c`'yi otomatik toplar, `*_wasm.c` yalnızca wasm'a girer, `tests/*.c` host test koşucusuna girer, `tools/host/tool_X.c` → `build/host/X`. Testlerin varlıklara erişim yolu (çalışma dizini = repo kökü) buna uygun olmalı: `tests/assets/...`.
- `texture_abi.h` → ana projenin `raster/shade_abi.h`'ı ile değiştirileceği; include yolu değişikliği (`#include "mip/texture_abi.h"` → `#include "raster/shade_abi.h"`) — bunu tek bir yerde topla (ör. `mip/mip.h` içinde) ki entegrasyon tek satır olsun.
- `G3dScreenVert` → ana projenin `RasterVert`'üne dönüşüm notu (alanlar aynı sırada: x, y, z, inv_w, var[16]).
- Bilinen sınırlamalar, desteklenmeyen glTF özellikleri, performans özetinin kısa hali.

## 9. Teslim
- `kit/` klasörünün tamamı (zip/tar). Derleme çıktıları (`build/`) hariç.
- Kısa bir özet: hangi modüller tam, hangileri kısmi; en önemli 5 ölçüm; ana proje için öneriler (ör. "occlusion culling şehirde nesnelerin %X'ini eliyor, maliyeti Y ms → değer/değmez").
