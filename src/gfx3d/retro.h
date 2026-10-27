/* gfx3d/retro.h — Eski konsol (retro) görüntü modu: tamsayı büyütme, bit derinliği azaltma
 * (ör. 15-bit 5:5:5), sıralı Bayer 4x4 titreşim (dither), 32^3 LUT ile palet eşleme (<=256 renk),
 * tarama çizgisi (scanline) karartma, hafif yatay CRT bulanıklığı ve 3D köşe yakalama (vertex snap).
 * Tüm işlemler deterministik tamsayı aritmetiğiyle çalışır; wasm SIMD ve skaler yollar bit-bit aynıdır.
 */
#ifndef GFX3D_RETRO_H
#define GFX3D_RETRO_H

#include "core/types.h"
#include "raster/framebuffer.h"

#define RETRO_PALETTE_MAX  256u
#define RETRO_LUT_DIM      32u
#define RETRO_LUT_SIZE     (RETRO_LUT_DIM * RETRO_LUT_DIM * RETRO_LUT_DIM) /* 32768 */
#define RETRO_MAX_DIM      16384u
#define RETRO_MAX_SUBPIXEL 256u

typedef struct {
    u32 colors[RETRO_PALETTE_MAX]; /* RGBA8 palet renkleri (0xAABBGGRR, A=0xFF) */
    u32 count;                     /* Geçerli renk sayısı (0..256) */
    u8  lut[RETRO_LUT_SIZE];       /* (r5, g5, b5) -> en yakın palet indeksi */
} RetroPalette;

typedef struct {
    u32                 inner_w;    /* İç genişlik (0 ise src->w kullanılır) */
    u32                 inner_h;    /* İç yükseklik (0 ise src->h kullanılır) */
    u32                 scale;      /* Tamsayı büyütme çarpanı (0 ise hedefe sığan en büyük tamsayı) */
    u32                 color_bits; /* Kanal başına bit (1..8) veya toplam RGB bit (15=5:5:5, 12=4:4:4, 24=8:8:8) */
    b32                 dither;     /* Sıralı Bayer 4x4 titreşim açık/kapalı */
    const RetroPalette* palette;    /* İsteğe bağlı palet (NULL veya count==0 ise devre dışı) */
    f32                 scanline;   /* Tarama çizgisi karartma gücü [0..1] (0 = kapalı, 1 = tam karartma) */
    b32                 crt_blur;   /* İsteğe bağlı hafif yatay CRT bulanıklığı ([1, 6, 1]/8) */
} RetroParams;

/* 5-bit (0..31) R, G, B bileşenlerinden 32^3 LUT indeksini hesaplar */
static inline u32 retro_lut_index(u32 r5, u32 g5, u32 b5) {
    return ((r5 & 31u) << 10) | ((g5 & 31u) << 5) | (b5 & 31u);
}

/* Varsayılan retro parametrelerini döner (5:5:5, Bayer açık, scanline 0.25) */
RetroParams retro_default_params(void);

/* Verilen palet renkleri (<=256) için 32^3 (32768) en yakın renk arama tablosunu (LUT) doldurur */
void retro_build_palette_lut(RetroPalette* pal, const u32* colors, u32 count);

/* Kaba kuvvet (brute-force) Öklid uzaklığı ile en yakın palet indeksini bulur (referans ve test için) */
u8 retro_palette_nearest_brute(const u32* colors, u32 count, u8 r, u8 g, u8 b);

/* Tek pikseli verilen koordinat ve parametrelerle nicemler (bit derinliği + Bayer + palet) */
u32 retro_quantize_pixel(u32 rgba, u32 x, u32 y, u32 color_bits, b32 dither,
                         const RetroPalette* palette);

/* 3D ekran koordinatlarını piksel ızgarasına (subpixel <= 1) veya 1/subpixel alt ızgaraya yuvarlar */
void retro_snap_xy(f32* x, f32* y, u32 subpixel);

/* Düşük çözünürlüklü src kare tamponunu retro efektleriyle dst tamponuna tamsayı ölçekleyerek basar */
void retro_present(const Framebuffer* src, Framebuffer* dst, const RetroParams* params);

/* İç satır işleme çekirdekleri (retro.c skaler ve retro_wasm.c SIMD tarafından ortak kullanılır) */
void retro_quantize_row(const u32* src, u32* dst, u32 w, u32 sy, u32 bpc, b32 dither,
                        const RetroPalette* palette);
void retro_blur_row(const u32* src, u32* dst, u32 w);
void retro_darken_row(const u32* src, u32* dst, u32 w, u32 keep);
void retro_expand_row(const u32* src, u32* dst, u32 count, u32 scale);

#endif
