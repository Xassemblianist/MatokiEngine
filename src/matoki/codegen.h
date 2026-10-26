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

/* Referans yorumlayıcı (ir_eval.c): IR'ı skaler C ile, lane lane çalıştırır.
 * JIT çıktısının ve ön ucun doğruluk kâhini. Davranışı codegen ile BİREBİR aynı olmalı
 * (maskeli IF, discard → quads[q].mask bitini temizler, OUTPUT → out_rgba). */
void ir_eval(const IrFunc* f, ShadeBatch* batch);

#endif
