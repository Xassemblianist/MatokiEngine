/* raster/shade_abi.h — C rasterizer ile JIT'lenmiş Matoki shader'ları arasındaki ikili sözleşme.
 * docs/ARCHITECTURE.md §4. Yerleşimi değiştirmek MİMARİ KARARDIR (DECISIONS.md).
 * JIT kodu alanlara sabit ofsetlerle (i32.load offset=...) erişir; ofsetler aşağıda sabitlenmiştir.
 * Piksel formatı: u32 RGBA8, little-endian → bellekte R,G,B,A (canvas ImageData ile aynı) = 0xAABBGGRR.
 */
#ifndef RASTER_SHADE_ABI_H
#define RASTER_SHADE_ABI_H

#include "core/types.h"

#define SHADE_MAX_QUADS     256
#define SHADE_MAX_VARYINGS  16   /* skaler varying sayısı (vec4 = 4) */
#define SHADE_MAX_TEXTURES  8

/* Quad lane sırası: lane0=(x,y) lane1=(x+1,y) lane2=(x,y+1) lane3=(x+1,y+1) */
typedef struct {
    u32 mask;          /* bit i = lane i kapsanıyor */
    u16 x, y;          /* quad sol üst pikseli (her zaman çift koordinat) */
} QuadHeader;

/* Doku: genişlik/yükseklik 2'nin kuvveti (wrap = AND maskesi) */
typedef struct Texture {
    u32* px;           /* RGBA8, satır öncelikli */
    u32  w, h;
    u32  w_mask, h_mask;   /* w-1, h-1 */
    u32  log2_w;
    u32  flags;        /* TEX_FLAG_* */
    u32  levels;       /* mip seviye sayısı >= 1 (eski _pad; 0 görülürse 1 kabul edilir) */
} Texture;

#define TEX_FLAG_CLAMP     1u   /* yoksa wrap */
#define TEX_FLAG_BILINEAR  2u   /* yoksa en yakın komşu */
#define TEX_FLAG_MIPMAP    4u   /* sample() seviyeyi quad türevlerinden seçer */
#define TEX_FLAG_F32       8u   /* px aslında f32* (tek kanal f32 doku: texture2d_f32 / sample_depth) */

/* MIP ZİNCİRİ YERLEŞİMİ (DECISIONS D-086): tüm seviyeler px'ten başlayarak bitişik.
 *  w = 2^a, h = 2^b; seviye k (0 <= k < levels): wk = w >> k, hk = h >> k;
 *  levels <= 1 + min(a, b)  (küçük kenar 1'e inince zincir biter → wk, hk >= 1 ve 2'nin kuvveti)
 *  base_k = Σ_{i<k} wi*hi = (4wh - (4wh >> 2k)) / 3   (tam bölünür)
 * MIPMAP SEMANTİĞİ (C referansı ve Matoki birebir — bit-bit):
 *  dudx = dFdx(u)*w, dvdx = dFdx(v)*h, dudy = dFdy(u)*w, dvdy = dFdy(v)*h   (w,h seviye 0, f32)
 *    dFdx(a) = a[lane1] - a[lane0], dFdy(a) = a[lane2] - a[lane0] (quad'ın 4 lane'inin hepsine)
 *  rho2 = max(dudx*dudx + dvdx*dvdx, dudy*dudy + dvdy*dvdy)    (f32.max)
 *  rho2 >= 1 değilse (NaN dahil) level = 0; aksi level = ((bits(rho2) >> 23) - 127) >> 1
 *  level = min(level, levels - 1); sonra seçilen seviyede (wk, hk) yukarıdaki en yakın/bilinear kuralı. */

/* ÖRNEKLEME SEMANTİĞİ (C referansı ve Matoki sample() birebir uyar):
 *  En yakın:  tx = trunc_sat(floor(u * w)), ty = trunc_sat(floor(v * h))
 *             wrap: tx &= w_mask, ty &= h_mask   |  clamp: tx = clamp(tx, 0, w-1) (ty aynı)
 *  Bilinear:  fu = u * w - 0.5, fv = v * h - 0.5; x0 = floor(fu), y0 = floor(fv)
 *             ax = fu - x0, ay = fv - y0; 4 texel (x0|x0+1, y0|y0+1) yukarıdaki wrap/clamp ile
 *             sonuç = lerp(lerp(t00, t10, ax), lerp(t01, t11, ax), ay)   (kanal başına, f32)
 *  Texel → f32: kanal / 255.0f, sıra R,G,B,A (px & 0xFF = R). Örnekleme sonrası sRGB dönüşümü YOK.
 *  Tek kanal f32 (texture2d_f32 / sample_depth): px doğrudan f32* (ör. fb.depth) dizisidir;
 *  32-bit texel bit-yeniden-yorumlama (f32.reinterpret_i32) ile doğrudan f32 olarak okunur. */

typedef struct {
    u32         count;           /* batch'teki quad sayısı (<= SHADE_MAX_QUADS) */
    u32         varying_count;   /* skaler varying sayısı */
    QuadHeader* quads;           /* [count] */
    f32*        varyings;        /* SoA: varyings[(v * count + q) * 4 + lane] */
    const u8*   uniforms;        /* uniform bloğu, 16 byte hizalı alanlar */
    f32*        out_rgba;        /* SoA: out_rgba[(c * count + q) * 4 + lane], c: 0=r 1=g 2=b 3=a, 0..1 */
    const Texture* textures;     /* [SHADE_MAX_TEXTURES] */
    /* --- ABI v1 (M5): füzyonlu shader'lar için. Rasterizer HER ZAMAN doldurur. --- */
    u32*        fb_px;           /* framebuffer başlangıcı; füzyonlu shader (x,y) pikselini fb_px[y*fb_stride+x]'e blend ederek yazar */
    u32         fb_stride;       /* piksel cinsinden satır adımı */
} ShadeBatch;

/* discard edilen lane'ler: shader out_alpha'ya NaN değil, quads[q].mask bitini temizleyerek bildirir */
typedef void (*ShadeFn)(ShadeBatch* batch);   /* wasm tipi: (i32) -> () */

/* wasm32 ofsetleri — JIT bunları kullanır */
#define SHADE_OFF_COUNT          0
#define SHADE_OFF_VARYING_COUNT  4
#define SHADE_OFF_QUADS          8
#define SHADE_OFF_VARYINGS       12
#define SHADE_OFF_UNIFORMS       16
#define SHADE_OFF_OUT_RGBA       20
#define SHADE_OFF_TEXTURES       24
#define SHADE_OFF_FB_PX          28
#define SHADE_OFF_FB_STRIDE      32
#define QUAD_OFF_MASK            0
#define QUAD_OFF_X               4
#define QUAD_OFF_Y               6
#define QUAD_SIZE                8
#define TEX_OFF_PX               0
#define TEX_OFF_W                4
#define TEX_OFF_H                8
#define TEX_OFF_W_MASK           12
#define TEX_OFF_H_MASK           16
#define TEX_OFF_LOG2_W           20
#define TEX_OFF_FLAGS            24
#define TEX_OFF_LEVELS           28
#define TEX_SIZE                 32

#if defined(__wasm32__)
_Static_assert(__builtin_offsetof(ShadeBatch, count)         == SHADE_OFF_COUNT, "abi");
_Static_assert(__builtin_offsetof(ShadeBatch, varying_count) == SHADE_OFF_VARYING_COUNT, "abi");
_Static_assert(__builtin_offsetof(ShadeBatch, quads)         == SHADE_OFF_QUADS, "abi");
_Static_assert(__builtin_offsetof(ShadeBatch, varyings)      == SHADE_OFF_VARYINGS, "abi");
_Static_assert(__builtin_offsetof(ShadeBatch, uniforms)      == SHADE_OFF_UNIFORMS, "abi");
_Static_assert(__builtin_offsetof(ShadeBatch, out_rgba)      == SHADE_OFF_OUT_RGBA, "abi");
_Static_assert(__builtin_offsetof(ShadeBatch, textures)      == SHADE_OFF_TEXTURES, "abi");
_Static_assert(__builtin_offsetof(ShadeBatch, fb_px)         == SHADE_OFF_FB_PX, "abi");
_Static_assert(__builtin_offsetof(ShadeBatch, fb_stride)     == SHADE_OFF_FB_STRIDE, "abi");
_Static_assert(sizeof(QuadHeader) == QUAD_SIZE, "abi");
_Static_assert(__builtin_offsetof(QuadHeader, x) == QUAD_OFF_X, "abi");
_Static_assert(__builtin_offsetof(QuadHeader, y) == QUAD_OFF_Y, "abi");
_Static_assert(sizeof(Texture) == TEX_SIZE, "abi");
_Static_assert(__builtin_offsetof(Texture, flags) == TEX_OFF_FLAGS, "abi");
_Static_assert(__builtin_offsetof(Texture, levels) == TEX_OFF_LEVELS, "abi");
#endif

#endif
