/* img/image.h — Görüntü işleme algoritmaları: bulanıklık, boyutlandırma, mipmap, normal map.
 * Doğrudan Framebuffer veya Texture pikselleri üzerinde çalışır; geçici bellek Arena'dan ayrılır.
 * Kutu/Gauss bulanıklık yatay+dikey ayrılabilir (separable) çekirdeklerle O(W*H*R) sürede çalışır.
 * Mipmap zinciri üretimi, Sobel tabanlı normal haritası, renk düzeltme ve alfa ön-çarpma içerir.
 */
#ifndef IMG_IMAGE_H
#define IMG_IMAGE_H

#include "core/types.h"
#include "core/arena.h"
#include "raster/framebuffer.h"
#include "raster/shade_abi.h"

/* Yardımcı: 2'nin kuvveti boyutlu Texture yapısını Arena üzerinde ilklendirir */
void img_tex_init(Texture* tex, Arena* arena, u32 w, u32 h, u32 flags);

/* Ayrılabilir kutu (box) ve Gauss (gaussian) bulanıklık (in-place, scratch için arena kullanır) */
void img_blur_box(u32* px, u32 w, u32 h, i32 radius, Arena* scratch);
void img_blur_gaussian(u32* px, u32 w, u32 h, i32 radius, f32 sigma, Arena* scratch);

/* Yeniden boyutlandırma: Bilinear ve Kutu (box / alan ortalaması) filtreleme */
void img_resize_bilinear(const u32* src, u32 src_w, u32 src_h,
                         u32* dst, u32 dst_w, u32 dst_h);
void img_resize_box(const u32* src, u32 src_w, u32 src_h,
                    u32* dst, u32 dst_w, u32 dst_h);

/* Mipmap zinciri üretimi: base (seviye 0) dokudan 1x1'e kadar tüm mip seviyelerini üretir.
 * out_mips dizisi en az (log2(max(w,h)) + 1) elemanlı olmalıdır. Üretilen seviye sayısını döner. */
u32  img_mipmap_build(const Texture* base, Texture* out_mips, u32 max_levels, Arena* arena);

/* Yükseklik (parlaklık / R kanalı) haritasından 3x3 Sobel filtresiyle teğet uzayı normal map üretir.
 * wrap != 0 ise kenarlar dairesel sarılır (tileable dokular için). */
void img_normal_from_height(const u32* src_rgba, u32* dst_rgba,
                            u32 w, u32 h, f32 strength, b32 wrap);

/* Renk düzeltme: parlaklık [-1..1], kontrast [0..4], doygunluk [0..4], gama (ör. 1.0 veya 2.2) */
void img_color_grade(u32* px, u32 w, u32 h,
                     f32 brightness, f32 contrast, f32 saturation, f32 gamma);

/* Alfa ön-çarpma: R, G, B kanallarını A/255 ile çarpar */
void img_premultiply_alpha(u32* px, u32 w, u32 h);

#endif
