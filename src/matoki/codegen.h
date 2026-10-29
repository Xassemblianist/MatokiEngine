/* matoki/codegen.h — IR → wasm modülü (D hattı) ve referans yorumlayıcı arayüzü.
 * Üretilen modül: import env.memory (min 1 sayfa, max yok, shared = opts.shared_memory),
 * tek fonksiyon, export adı "shade", tip (i32 batch_ptr) -> ().
 * Fonksiyon batch->count quad üzerinde döngü yapar; ShadeBatch'e shade_abi.h ofsetleriyle erişir.
 */
#ifndef MATOKI_CODEGEN_H
#define MATOKI_CODEGEN_H

#include "core/types.h"
#include "core/arena.h"
#include "matoki/ir.h"
#include "raster/shade_abi.h"

/* Blend füzyonu (M5): shader out_rgba yerine doğrudan ShadeBatch.fb_px'e yazar.
 * Blend formülü ve f32→RGBA8 dönüşümü src/raster/raster.c'deki C blend ile BİT-BİT aynı olmalı
 * (referans = raster.c; farklılık = hata). Füzyonlu shader'da rasterizer kendi blend'ini ATLAR. */
typedef enum {
    MTK_FUSE_NONE    = 0,   /* out_rgba'ya yaz (varsayılan, M4 davranışı) */
    MTK_FUSE_REPLACE = 1,   /* BLEND_NONE */
    MTK_FUSE_ALPHA   = 2,   /* BLEND_ALPHA */
    MTK_FUSE_ADD     = 3,   /* BLEND_ADD */
} MtkFuseBlend;

typedef struct {
    b32 shared_memory;    /* Faz 2 (thread'ler) için TRUE */
    b32 own_memory;       /* SADECE TEST: memory import etmek yerine 64 sayfalık kendi belleğini tanımla ve export et ("memory") */
    u32 fuse_blend;       /* MtkFuseBlend */
} MtkCodegenOpts;

/* Dönüş: üretilen byte sayısı; hata: 0 ve err doldurulur. scratch: mbin çalışma alanı için. */
u32 mtk_codegen(const IrFunc* f, const MtkCodegenOpts* opts, Arena* scratch,
                u8* out, u32 out_cap, char* err, u32 err_cap);

/* ---------- Compute ABI (F0_A) ---------- */
#define COMPUTE_MAX_BUFFERS 16u

typedef struct {
    u32       start;                     /* başlangıç eleman indeksi (global_id tabanı) */
    u32       count;                     /* bu çağrıda işlenecek eleman sayısı */
    u32       total_count;               /* toplam eleman sayısı N (sınır kontrolü için) */
    u32       buffer_count;              /* kullanılan buffer sayısı */
    const u8* uniforms;                  /* 16-byte hizalı uniform bloğu */
    void*     buffers[COMPUTE_MAX_BUFFERS]; /* buffer taban işaretçileri (f32* / u32* / vec4*) */
} MtkComputeBatch;

typedef void (*MtkComputeFn)(MtkComputeBatch* batch);   /* wasm tipi: (i32) -> () */

/* wasm32 ofsetleri — compute JIT bunları kullanır */
#define COMPUTE_OFF_START        0u
#define COMPUTE_OFF_COUNT        4u
#define COMPUTE_OFF_TOTAL_COUNT  8u
#define COMPUTE_OFF_BUFFER_COUNT 12u
#define COMPUTE_OFF_UNIFORMS     16u
#define COMPUTE_OFF_BUFFERS      20u

#if defined(__wasm32__)
_Static_assert(__builtin_offsetof(MtkComputeBatch, start)        == COMPUTE_OFF_START, "compute_abi");
_Static_assert(__builtin_offsetof(MtkComputeBatch, count)        == COMPUTE_OFF_COUNT, "compute_abi");
_Static_assert(__builtin_offsetof(MtkComputeBatch, total_count)  == COMPUTE_OFF_TOTAL_COUNT, "compute_abi");
_Static_assert(__builtin_offsetof(MtkComputeBatch, buffer_count) == COMPUTE_OFF_BUFFER_COUNT, "compute_abi");
_Static_assert(__builtin_offsetof(MtkComputeBatch, uniforms)     == COMPUTE_OFF_UNIFORMS, "compute_abi");
_Static_assert(__builtin_offsetof(MtkComputeBatch, buffers)      == COMPUTE_OFF_BUFFERS, "compute_abi");
#endif

u32 mtk_codegen_compute(const IrFunc* f, const MtkCodegenOpts* opts, Arena* scratch,
                        u8* out, u32 out_cap, char* err, u32 err_cap);

/* Referans yorumlayıcı (ir_eval.c): IR'ı skaler C ile, lane lane çalıştırır.
 * JIT çıktısının ve ön ucun doğruluk kâhini. Davranışı codegen ile BİREBİR aynı olmalı
 * (maskeli IF, discard → quads[q].mask bitini temizler, OUTPUT → out_rgba). */
void ir_eval(const IrFunc* f, ShadeBatch* batch);
void ir_eval_compute(const IrFunc* f, MtkComputeBatch* batch);

#endif
