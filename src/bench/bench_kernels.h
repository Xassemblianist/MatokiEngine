/* bench/bench_kernels.h — C (clang -O2 -msimd128) ve elle yazılmış WAT çekirdeklerinin ortak imzaları.
 * Her çekirdek hem C referansı/derleyici çıktısı olarak hem de çalışma anında masm + jit_load
 * ile derlenen WAT fonksiyon işaretçisi olarak aynı ABI ile çağrılır.
 * Sonuçlar ölçümden önce bit-bit karşılaştırılır (ARCHITECTURE.md §0.5).
 */
#ifndef BENCH_KERNELS_H
#define BENCH_KERNELS_H

#include "core/types.h"
#include "raster/shade_abi.h"

/* 1. fill_u32: Framebuffer 32-bit RGBA temizleme */
typedef void (*KernFillU32Fn)(u32* dst, u32 val, u32 count);
void bench_c_fill_u32(u32* __restrict dst, u32 val, u32 count);

/* 2. blend_alpha_rgba8: n piksel tamsayı src-over (src RGBA8 + dst RGBA8 -> dst RGBA8) */
typedef void (*KernBlendAlphaFn)(u32* dst, const u32* src, u32 count);
void bench_c_blend_alpha_rgba8(u32* __restrict dst, const u32* __restrict src, u32 count);

/* 3. rgba_f32_to_u8: SoA 4 kanal f32 [0..1] -> paketli RGBA8 (u32) */
typedef void (*KernRgbaF32ToU8Fn)(u32* dst, const f32* r, const f32* g, const f32* b, const f32* a, u32 count);
void bench_c_rgba_f32_to_u8(u32* __restrict dst,
                            const f32* __restrict r,
                            const f32* __restrict g,
                            const f32* __restrict b,
                            const f32* __restrict a,
                            u32 count);

/* 4. edge_block_8x8: 3 kenar fonksiyonu (a*x + b*y + c), 8x8 blokta 16 adet 2x2 quad maskesi (4-bit) */
typedef void (*KernEdgeBlock8x8Fn)(const i32* coeffs, i32 bx, i32 by, u8* out_masks);
void bench_c_edge_block_8x8(const i32* __restrict coeffs, i32 bx, i32 by, u8* __restrict out_masks);

/* 5. bilinear_gather: 4-lane (quad) çift doğrusal doku örnekleme (SoA u,v -> SoA out_rgba) */
typedef void (*KernBilinearGatherFn)(const Texture* tex, const f32* u, const f32* v, f32* out_rgba, u32 quad_count);
void bench_c_bilinear_gather(const Texture* __restrict tex,
                             const f32* __restrict u,
                             const f32* __restrict v,
                             f32* __restrict out_rgba,
                             u32 quad_count);

/* 6a. sum_f32: yatay f32 dizi toplamı */
typedef f32 (*KernSumF32Fn)(const f32* a, u32 count);
f32 bench_c_sum_f32(const f32* __restrict a, u32 count);

/* 6b. dot_f32: iki f32 dizisinin iç çarpımı (dot product) */
typedef f32 (*KernDotF32Fn)(const f32* a, const f32* b, u32 count);
f32 bench_c_dot_f32(const f32* __restrict a, const f32* __restrict b, u32 count);

/* 7. mandelbrot_row: hesap yoğun, erken çıkışlı (branchy) Mandelbrot satırı */
typedef void (*KernMandelbrotRowFn)(u32* out_iter, u32 width, f32 x0, f32 y0, f32 dx, u32 max_iter);
void bench_c_mandelbrot_row(u32* __restrict out_iter, u32 width, f32 x0, f32 y0, f32 dx, u32 max_iter);

#endif /* BENCH_KERNELS_H */
