/* raster/texture.h — Doku (Texture) oluşturma ve prosedürel test dokusu üreteçleri.
 * Tüm dokular 2'nin kuvveti (power-of-two) boyutlarda ve 16-byte hizalı RGBA8 dizisi olarak ayrılır.
 * Dama (checker), dört köşe gradyan ve PCG32 tabanlı gürültü (noise) dokularını destekler.
 */
#ifndef RASTER_TEXTURE_H
#define RASTER_TEXTURE_H

#include "core/types.h"
#include "core/arena.h"
#include "raster/shade_abi.h"

void    tex_init(Texture* tex, Arena* arena, u32 w, u32 h, u32 flags);
Texture tex_create(Arena* arena, u32 w, u32 h, u32 flags);

/* Mip zinciri: shade_abi.h yerleşimi (levels = 1 + min(log2 w, log2 h)), TEX_FLAG_MIPMAP eklenir.
 * Seviye 0 doldurulduktan sonra tex_build_mips çağrılır (2x2 kutu filtresi, yuvarlamalı). */
void tex_init_mipmapped(Texture* tex, Arena* arena, u32 w, u32 h, u32 flags);
void tex_build_mips(Texture* tex);
u32  tex_mip_offset(const Texture* tex, u32 level);   /* seviye başlangıcı (texel) */

/* Prosedürel test dokuları (RGBA8 renkler FB_RGBA formatında: 0xAABBGGRR) */
void tex_gen_checker(Texture* tex, u32 cell_size, u32 color_a, u32 color_b);
void tex_gen_gradient(Texture* tex, u32 c_tl, u32 c_tr, u32 c_bl, u32 c_br);
void tex_gen_noise(Texture* tex, u64 seed);

#endif
