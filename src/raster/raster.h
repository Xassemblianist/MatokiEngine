/* raster/raster.h — 24.8 sabit nokta kurulumlu, 64x64 karo binlemeli SIMD yazılım rasterizer'ı.
 * 8x8 bloklarda trivial-reject / trivial-accept uygular ve 2x2 quad'lar halinde ShadeFn çağırır.
 * 2D afin ve 3D perspektif-doğru (inv_w) varying interpolasyonunu, early-z derinlik testini
 * ve Matoki blend füzyonunu (ABI v1) destekler.
 */
#ifndef RASTER_RASTER_H
#define RASTER_RASTER_H

#include "core/types.h"
#include "core/arena.h"
#include "raster/framebuffer.h"
#include "raster/shade_abi.h"

#define RASTER_TILE_SIZE  64
#define RASTER_BLOCK_SIZE 8
#define RASTER_TRI_BATCH  1024

typedef enum {
    BLEND_NONE  = 0,
    BLEND_ALPHA = 1,
    BLEND_ADD   = 2
} BlendMode;

typedef enum {
    DEPTH_NEVER    = 0,
    DEPTH_LESS     = 1,
    DEPTH_EQUAL    = 2,
    DEPTH_LEQUAL   = 3,
    DEPTH_GREATER  = 4,
    DEPTH_NOTEQUAL = 5,
    DEPTH_GEQUAL   = 6,
    DEPTH_ALWAYS   = 7
} DepthFunc;

typedef struct {
    f32 x, y;
    f32 z;
    f32 inv_w;
    f32 var[SHADE_MAX_VARYINGS];
} RasterVert;

typedef struct RasterShaderState {
    ShadeFn        shade_fn;
    const void*    uniforms;
    const Texture* textures;
    u32            varying_count;
    BlendMode      blend;
    b32            fused;
    b32            depth_test;
    b32            depth_write;
    DepthFunc      depth_func;
    b32            depth_late;   /* TRUE: derinlik shade SONRASI yazılır (discard'a saygılı, üçgen başına flush) */
} RasterShaderState;

typedef struct BinnedTri {
    const RasterShaderState* state;
    i16 min_x, min_y;
    i16 max_x, max_y;
    i32 a0, b0, c0;
    i32 a1, b1, c1;
    i32 a2, b2, c2;
    f32 x0, y0;
    f32 z0, dz_dx, dz_dy;
    f32 dl1_dx, dl1_dy;
    f32 dl2_dx, dl2_dy;
    f32 inv_w0, inv_w1, inv_w2;
    b32 is_affine;
    f32* var_coeffs; /* [varying_count * 3] */
} BinnedTri;

typedef struct TileBinChunk TileBinChunk;

typedef struct {
    TileBinChunk* head;
    TileBinChunk* tail;
} TileBin;

typedef struct {
    RasterVert v0, v1, v2;
} RawTri;

typedef struct {
    Framebuffer*       fb;
    Arena*             frame;
    b32                cull_back;
    RasterShaderState  cur_state;
    RasterShaderState* pinned_state;

    u32      tiles_x;
    u32      tiles_y;
    TileBin* tiles;

    u32     tri_buf_count;
    RawTri* tri_buf;

    u32        batch_count;
    _Alignas(16) QuadHeader quads[SHADE_MAX_QUADS];
    _Alignas(16) f32        varyings[SHADE_MAX_VARYINGS * SHADE_MAX_QUADS * 4];
    _Alignas(16) f32        out_rgba[4 * SHADE_MAX_QUADS * 4];
    _Alignas(16) f32        quad_z[SHADE_MAX_QUADS * 4];

    /* İstatistikler (benchmark / Mpiksel/s hesabı için) */
    u64 stat_tris_in;
    u64 stat_tris_binned;
    u64 stat_quads;
    u64 stat_pixels;
} Rasterizer;

typedef void (*RasterWatEdgeBlockFn)(const i32* coeffs, i32 bx, i32 by, u8* out_masks);
typedef void (*RasterWatBlendAlphaFn)(u32* dst, const u32* src, u32 count);

void raster_begin(Rasterizer* r, Framebuffer* fb, Arena* frame);
void raster_set_cull(Rasterizer* r, b32 cull_back);
/* Derinlik yazımı varsayılan olarak early-Z'dir: test geçen lane'ler quad üretilirken yazılır
 * (discard edilen lane'ler de yazılır). late_write=TRUE: yazım shade sonrasında, yalnız hayatta kalan
 * lane'lere; doğruluk için her üçgenden sonra batch boşaltılır (alfa testli materyaller için). */
void raster_set_depth(Rasterizer* r, b32 test, b32 write, DepthFunc func);
void raster_set_depth_ex(Rasterizer* r, b32 test, b32 write, DepthFunc func, b32 late_write);
void raster_set_shader(Rasterizer* r, ShadeFn shade_fn, const void* uniforms,
                       const Texture* textures, u32 varying_count, BlendMode blend);
void raster_set_shader_ex(Rasterizer* r, ShadeFn shade_fn, const void* uniforms,
                          const Texture* textures, u32 varying_count, BlendMode blend, b32 fused);
void raster_triangle(Rasterizer* r, RasterVert v0, RasterVert v1, RasterVert v2);
void raster_flush(Rasterizer* r);

/* Ertelenmiş/paralel ön uç için (gfx3d g3d_flush):
 * raster_pin_state: geçerli durumun frame arena'daki sabit kopyası (çizim başına bir kez, ana iş parçacığı).
 * raster_setup_tri: tek üçgen kurulumu; saf, iş parçacığı güvenli; vc en az varying_count*3 f32.
 *   Sıfır alan / ayıklanan / ekran dışı → FALSE (out yazılmış olabilir ama kullanılmaz).
 * raster_bin_tris: kurulmuş üçgenleri sırayla karolara dağıtır (ana iş parçacığı; önce tri_buf boşaltılır).
 *   submitted: kuruluma gönderilen üçgen sayısı (istatistik; raster_triangle sayımıyla aynı anlam). */
const RasterShaderState* raster_pin_state(Rasterizer* r);
b32  raster_setup_tri(const RasterVert* v0, const RasterVert* v1, const RasterVert* v2,
                      const RasterShaderState* state, b32 cull_back, i32 fb_w, i32 fb_h,
                      BinnedTri* out, f32* vc);
void raster_bin_tris(Rasterizer* r, BinnedTri* tris, u32 count, u32 submitted);

/* Paralel karo dağıtım modu: TRUE = dinamik (atomik sayaç, grain=1), FALSE = statik bölme */
void raster_set_parallel_dynamic(b32 dynamic_sched);
b32  raster_get_parallel_dynamic(void);

/* Opsiyonel elle yazılmış WAT çekirdek kancaları (NULL verilirse C SIMD yoluna düşer) */
void raster_set_wat_kernels(RasterWatEdgeBlockFn edge_fn, RasterWatBlendAlphaFn blend_fn);
RasterWatEdgeBlockFn  raster_get_wat_edge_fn(void);
RasterWatBlendAlphaFn raster_get_wat_blend_fn(void);

/* İç çekirdek arayüzü (wasm SIMD: raster_simd_wasm.c, host skaler yedek: raster.c) */
void raster_kern_tri_scan(Rasterizer* r, const BinnedTri* tri,
                          const RasterShaderState* state, i32 tile_x0, i32 tile_y0,
                          i32 tile_x1, i32 tile_y1);
u64  raster_kern_blend_batch(Rasterizer* r, const RasterShaderState* state, u32 count);
void raster_flush_full_batch(Rasterizer* r, const RasterShaderState* state);

#endif
