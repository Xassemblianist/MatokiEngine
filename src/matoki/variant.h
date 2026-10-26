/* matoki/variant.h — Matoki shader varyant önbelleği ve LICM/füzyon yardımcıları (D hattı).
 * (kaynak hash, sabitlenmiş uniform değerleri, fuse_blend, sampler bayrakları) anahtarıyla
 * derlenmiş WebAssembly modüllerini önbellekler; aynı anahtar için yeniden derleme yapmaz.
 * Ayrıca LICM geçişi ve füzyonlu referans yorumlayıcı (ir_eval_ex) arayüzlerini sunar.
 */
#ifndef MATOKI_VARIANT_H
#define MATOKI_VARIANT_H

#include "core/types.h"
#include "core/arena.h"
#include "matoki/ir.h"
#include "matoki/codegen.h"
#include "matoki/matoki.h"

/* LICM tarafından döngü öncesine (preheader) taşınan IR komutlarını işaretler */
#define IR_FLAG_HOISTED 4u

#define MTK_VARIANT_MAX_SPECS 16
#define MTK_VARIANT_CACHE_CAP 64

typedef struct {
    u32 byte_offset;
    f32 value;
} MtkSpecUniform;

typedef struct {
    const char*    src;
    u32            src_len;
    const char*    filename;
    u64            src_hash;                         /* 0 ise mtk_variant_get otomatik hesaplar */
    MtkSpecUniform specs[MTK_VARIANT_MAX_SPECS];
    u32            spec_count;
    MtkCodegenOpts cg_opts;
    u32            sampler_mask;                     /* bit s = slot s için sampler_flags[s] geçerli */
    u32            sampler_flags[SHADE_MAX_TEXTURES];
} MtkVariantKey;

typedef struct {
    b32            occupied;
    u64            key_hash;
    u64            src_hash;
    u32            src_len;
    MtkSpecUniform specs[MTK_VARIANT_MAX_SPECS];
    u32            spec_count;
    MtkCodegenOpts cg_opts;
    u32            sampler_mask;
    u32            sampler_flags[SHADE_MAX_TEXTURES];
    const u8*      wasm;
    u32            wasm_len;
    u32            ir_inst_count;
    u32            loop_inst_count;
    u32            wasm_loop_ops;
    MtkLayout      layout;
} MtkVariant;

typedef struct {
    Arena*     persist;
    MtkVariant entries[MTK_VARIANT_CACHE_CAP];
    u32        count;
    u32        hits;
    u32        misses;
} MtkVariantCache;

/* Varyant önbelleği işlemleri */
void              mtk_variant_cache_init(MtkVariantCache* cache, Arena* persist);
u64               mtk_hash_source(const char* src, u32 len);
const MtkVariant* mtk_variant_get(MtkVariantCache* cache, const MtkVariantKey* key,
                                  Arena* scratch, MtkDiag* diag);

/* LICM geçişi ve döngü içi komut ölçüm yardımcıları */
void ir_opt_licm(IrFunc* f);
u32  ir_count_loop_insts(const IrFunc* f);
u32  mtk_wasm_count_loop_ops(const u8* wasm, u32 wasm_len);

/* Blend füzyonu destekli referans IR yorumlayıcısı (oracle) */
void ir_eval_ex(const IrFunc* f, ShadeBatch* batch, u32 fuse_blend);

#endif /* MATOKI_VARIANT_H */
