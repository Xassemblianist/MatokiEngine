/* img/noise.h — Deterministik (tamsayı hash tabanlı) gürültü (noise) fonksiyonları.
 * Value, Perlin (gradient) 2D/3D, Simplex 2D, Worley (cellular), fBm ve domain warp.
 * Tüm fonksiyonlar hem serbest uzayda hem de tam döşenebilir (tileable) periyodik modda çalışır.
 * Tablo veya durum (state) tutmaz; saf fonksiyonlardır, aynı seed her zaman aynı çıktıyı üretir.
 */
#ifndef IMG_NOISE_H
#define IMG_NOISE_H

#include "core/types.h"
#include "core/math.h"

/* Temel tamsayı karıştırıcılar (hash) */
u32 noise_hash1(u32 x, u32 seed);
u32 noise_hash2(i32 x, i32 y, u32 seed);
u32 noise_hash3(i32 x, i32 y, i32 z, u32 seed);

/* 2D/3D Value Noise: dönüş aralığı [0, 1] */
f32 noise_value2(f32 x, f32 y, u32 seed);
f32 noise_value3(f32 x, f32 y, f32 z, u32 seed);
f32 noise_value2_tile(f32 x, f32 y, i32 period_x, i32 period_y, u32 seed);

/* 2D/3D Gradient (Perlin) Noise: dönüş aralığı yaklaşık [-1, 1] */
f32 noise_perlin2(f32 x, f32 y, u32 seed);
f32 noise_perlin3(f32 x, f32 y, f32 z, u32 seed);
f32 noise_perlin2_tile(f32 x, f32 y, i32 period_x, i32 period_y, u32 seed);
f32 noise_perlin3_tile(f32 x, f32 y, f32 z, i32 px, i32 py, i32 pz, u32 seed);

/* 2D Simplex Noise: dönüş aralığı yaklaşık [-1, 1] */
f32 noise_simplex2(f32 x, f32 y, u32 seed);
/* 2D Döşenebilir Simplex-benzeri / alternatif ya da 4D torus projeksiyonlu ya da periyodik */
f32 noise_simplex2_tile(f32 x, f32 y, f32 period_x, f32 period_y, u32 seed);

/* 2D Worley (Cellular) Noise: dönüş F1 mesafesi [0, 1] (veya out_f2 ile F2 mesafesi) */
f32 noise_worley2(f32 x, f32 y, u32 seed, f32* out_f2);
f32 noise_worley2_tile(f32 x, f32 y, i32 period_x, i32 period_y, u32 seed, f32* out_f2);

/* Fraktal Brownian Motion (fBm) — Perlin tabanlı, dönüş [-1, 1] civarı */
f32 noise_fbm2(f32 x, f32 y, i32 octaves, f32 lacunarity, f32 gain, u32 seed);
f32 noise_fbm2_tile(f32 x, f32 y, i32 period_x, i32 period_y,
                    i32 octaves, f32 lacunarity, f32 gain, u32 seed);

/* Domain Warp — fBm/Perlin ile koordinat bükme */
f32 noise_warp2(f32 x, f32 y, f32 strength, i32 octaves, u32 seed);
f32 noise_warp2_tile(f32 x, f32 y, i32 period_x, i32 period_y,
                     f32 strength, i32 octaves, u32 seed);

#endif
