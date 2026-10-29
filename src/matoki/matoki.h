/* matoki/matoki.h — Matoki shader derleyicisi genel arayüzü.
 * Kaynak kodunu (.mtk) alır; lexer, parser, sema ve lower aşamalarından geçirerek
 * skaler SSA IR (IrFunc) veya doğrudan wasm ikili modülü üretir.
 * Yerleşim tablosu (MtkLayout) ve hata tanılamaları (MtkDiag) çağıran tarafa döndürülür.
 */
#ifndef MATOKI_MATOKI_H
#define MATOKI_MATOKI_H

#include "core/types.h"
#include "core/arena.h"
#include "matoki/ir.h"
#include "matoki/codegen.h"

/* Sembol türleri (MtkSymbol.kind) */
typedef enum {
    MTK_SYM_UNIFORM = 0,
    MTK_SYM_VARYING = 1,
    MTK_SYM_TEXTURE = 2,
} MtkSymKind;

/* Matoki dil tipleri (MtkSymbol.type ve AST/sema tip sistemi) */
typedef enum {
    MTK_TYPE_VOID = 0,
    MTK_TYPE_F32,
    MTK_TYPE_I32,
    MTK_TYPE_U32,
    MTK_TYPE_BOOL,
    MTK_TYPE_VEC2,
    MTK_TYPE_VEC3,
    MTK_TYPE_VEC4,       /* rgba bu tipin takma adıdır */
    MTK_TYPE_IVEC2,
    MTK_TYPE_IVEC3,
    MTK_TYPE_IVEC4,
    MTK_TYPE_MAT3,
    MTK_TYPE_MAT4,
    MTK_TYPE_TEXTURE2D,
    MTK_TYPE_COUNT
} MtkType;

typedef struct {
    char name[32];
    u32  kind;             /* 0 uniform, 1 varying, 2 texture */
    u32  type;             /* MtkType */
    u32  offset_or_index;  /* uniform: byte ofseti, varying: skaler indeks, texture: slot */
    u32  components;       /* skaler bileşen sayısı (f32=1, vec3=3, mat4=16) */
} MtkSymbol;

typedef struct {
    MtkSymbol syms[64];
    u32       sym_count;
    u32       uniform_bytes;
    u32       varying_count;
    u32       texture_count;
} MtkLayout;

#define MTK_MAX_ERRORS 20

typedef struct {
    char text[4096];
    u32  len;
    u32  count;
} MtkDiag;

/* Tanılama tamponuna "dosya:satır:sütun: hata: ..." + kaynak satırı + '^' ekler */
void mtk_diag_init(MtkDiag* diag);
void mtk_diag_error(MtkDiag* diag, const char* filename, const char* src, u32 src_len,
                    u32 line, u32 col, const char* fmt, ...);

/* Tip yardımcıları */
const char* mtk_type_name(MtkType t);
u32         mtk_type_components(MtkType t);
b32         mtk_type_is_float(MtkType t);
b32         mtk_type_is_int(MtkType t);
b32         mtk_type_is_vector(MtkType t);
b32         mtk_type_is_matrix(MtkType t);
MtkType     mtk_type_scalar_base(MtkType t);
MtkType     mtk_type_make_vector(MtkType scalar_base, u32 comps);

/* Kaynaktan IR üretir (optimizasyon çalıştırmaz). Başarıda TRUE döner. */
b32 mtk_compile_ir(const char* src, u32 len, const char* filename, Arena* a,
                   IrFunc* out, MtkLayout* layout, MtkDiag* diag);

/* mtk_compile_ir + ir_optimize + mtk_codegen -> wasm byte'ları. Hata durumunda 0 döner. */
u32 mtk_compile_wasm(const char* src, u32 len, const char* filename, const MtkCodegenOpts* o,
                     Arena* a, u8* out, u32 out_cap, MtkLayout* layout, MtkDiag* diag);

#include "matoki/wgsl.h"

#endif
