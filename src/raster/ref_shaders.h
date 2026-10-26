/* raster/ref_shaders.h — C referans piksel shader fonksiyonları (ShadeFn ABI).
 * JIT'lenmiş Matoki shader'larının doğruluğunu test etmek ve M2 rasterizer'ını
 * çalıştırmak için kullanılır. Örnekleme semantiği shade_abi.h ile birebir aynıdır.
 */
#ifndef RASTER_REF_SHADERS_H
#define RASTER_REF_SHADERS_H

#include "core/types.h"
#include "core/math.h"
#include "raster/shade_abi.h"

typedef struct {
    _Alignas(16) Vec4 color;
} RefSolidUniforms;

typedef struct {
    _Alignas(16) Vec4 tint;
} RefSpriteUniforms;

/* shade_abi.h örnekleme semantiğinin birebir C referansı (seviye 0; MIPMAP bayrağı yok sayılır) */
void ref_sample_tex(const Texture* tex, f32 u, f32 v, f32 out_rgba[4]);

/* Mip seviyesi: quad'ın 4 lane'indeki u, v'den (shade_abi.h MIPMAP SEMANTİĞİ, Matoki ile bit-bit) */
i32  ref_tex_mip_level(const Texture* tex, const f32 u[4], const f32 v[4]);
/* Seviye k'yı ayrı bir Texture görünümü olarak döndürür (level [0, levels-1]'e kırpılır) */
Texture ref_tex_level_view(const Texture* tex, i32 level);
/* Açık seviyede örnekleme */
void ref_sample_tex_level(const Texture* tex, f32 u, f32 v, i32 level, f32 out_rgba[4]);

/* C referans ShadeFn'leri */
void ref_shade_solid(ShadeBatch* batch);
void ref_shade_vcolor(ShadeBatch* batch);
void ref_shade_sprite(ShadeBatch* batch);

#if defined(__wasm32__)
/* wasm32'de C fonksiyon adresi almak __indirect_function_table import alt sınırını >1 yapar
 * (web/platform.js ve web/jit_test.js initial:1 ile oluşturur). Yerleşik C referans shader'ları
 * yüksek nöbetçi (sentinel) kimliklerle taşınır, JIT shader'ları (1..N) call_indirect kullanır. */
#define REF_SHADE_SOLID_FN  ((ShadeFn)(uptr)0xFFFFFFF1u)
#define REF_SHADE_VCOLOR_FN ((ShadeFn)(uptr)0xFFFFFFF2u)
#define REF_SHADE_SPRITE_FN ((ShadeFn)(uptr)0xFFFFFFF3u)
#else
#define REF_SHADE_SOLID_FN  ref_shade_solid
#define REF_SHADE_VCOLOR_FN ref_shade_vcolor
#define REF_SHADE_SPRITE_FN ref_shade_sprite
#endif

#endif
