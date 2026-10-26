/* matoki/ir.h — Matoki ara dili (IR): ön uç (lower) ile arka uç (opt/codegen) arasındaki sözleşme.
 * Tamamen SKALER, SSA, tek fonksiyon. vec3 gibi tipler ön uçta skalerlere bölünür (SoA).
 * Her değerin bir "tekdüzelik"i (uniform/varying) vardır; varying = 4 lane (bir 2x2 quad).
 * Kontrol akışı yapısaldır: IF_BEGIN/ELSE/IF_END işaretçileri; dallardan çıkan değerler SELECT ile birleşir.
 * Bu dosyayı değiştirmek MİMARİ KARARDIR (DECISIONS.md'ye öneri yaz).
 */
#ifndef MATOKI_IR_H
#define MATOKI_IR_H

#include "core/types.h"
#include "core/arena.h"

/* ---------- Tipler ---------- */
typedef enum {
    IR_T_VOID = 0,   /* işaretçiler, OUTPUT, DISCARD */
    IR_T_F32,
    IR_T_I32,
    IR_T_BOOL,       /* codegen: uniform → i32 (0/1), varying → i32x4 maske (0 / 0xFFFFFFFF) */
} IrType;

/* ---------- Opcode'lar ----------
 * a, b, c = operand değer kimlikleri (komut indeksleri). imm = anlık değer.
 * Sonuç tekdüzeliği: operandlardan biri varying ise varying (aksi belirtilmedikçe). */
typedef enum {
    IR_NOP = 0,          /* silinmiş komut (DCE bırakır, ir_compact temizler) */

    /* Kaynaklar */
    IR_CONST_F32,        /* imm.f           → uniform f32 */
    IR_CONST_I32,        /* imm.i           → uniform i32 */
    IR_CONST_BOOL,       /* imm.i (0/1)     → uniform bool */
    IR_UNIFORM_F32,      /* imm.u = uniform bloğu byte ofseti → uniform f32 */
    IR_UNIFORM_I32,      /* imm.u = byte ofseti → uniform i32 */
    IR_VARYING,          /* imm.u = skaler varying indeksi → varying f32 */
    IR_FRAG_X,           /* piksel merkezi x (quad.x + lane%2 + 0.5) → varying f32 */
    IR_FRAG_Y,           /* piksel merkezi y → varying f32 */

    /* f32 aritmetik */
    IR_FADD, IR_FSUB, IR_FMUL, IR_FDIV, IR_FMIN, IR_FMAX,  /* (a, b) */
    IR_FNEG, IR_FABS, IR_FSQRT, IR_FFLOOR, IR_FCEIL, IR_FTRUNC, IR_FNEAREST, /* (a) */

    /* i32 aritmetik */
    IR_IADD, IR_ISUB, IR_IMUL, IR_IAND, IR_IOR, IR_IXOR,
    IR_ISHL, IR_ISHR_S, IR_ISHR_U, IR_IMIN_S, IR_IMAX_S,    /* (a, b) */

    /* Karşılaştırma → bool */
    IR_FEQ, IR_FNE, IR_FLT, IR_FLE, IR_FGT, IR_FGE,
    IR_IEQ, IR_INE, IR_ILT_S, IR_ILE_S, IR_IGT_S, IR_IGE_S,

    /* bool */
    IR_BAND, IR_BOR, IR_BNOT,         /* BNOT: (a) */
    IR_SELECT,                        /* (cond:bool, if_true, if_false), tip = b'nin tipi */

    /* Dönüşüm */
    IR_F2I,              /* trunc_sat f32→i32 */
    IR_I2F,              /* i32→f32 (signed) */
    IR_BITCAST_F2I,
    IR_BITCAST_I2F,

    /* Doku (imm.u = doku slotu, 0..SHADE_MAX_TEXTURES-1; her zaman uniform slot) */
    IR_TEX_W,            /* → uniform i32 genişlik */
    IR_TEX_H,            /* → uniform i32 yükseklik */
    IR_TEX_FLAGS,        /* → uniform i32 TEX_FLAG_* */
    IR_TEXEL_LOAD,       /* (a: i32 texel indeksi y*w+x) → i32 paketli RGBA8 (varying ise 4 lane gather) */

    /* Kontrol (değer üretmez) */
    IR_IF_BEGIN,         /* (a: bool koşul). Koşul uniform ise gerçek dal; varying ise "herhangi bir lane" atlama optimizasyonu */
    IR_ELSE,
    IR_IF_END,
    IR_DISCARD,          /* (a: bool maske) — o lane'ler yazılmaz. Ön uç a'ya aktif bölge maskesini dahil eder */
    IR_OUTPUT,           /* (a: f32) imm.u = kanal 0..3 (r,g,b,a). Her kanal tam olarak bir kez, fonksiyonun sonunda */

    /* --- M8_B eklemeleri (numaralar kaymasın diye sonda) --- */
    IR_DERIV_X,          /* (a: varying f32) kaba türev: 4 lane'e a[lane1] - a[lane0]. Maskeden bağımsız */
    IR_DERIV_Y,          /* (a: varying f32) 4 lane'e a[lane2] - a[lane0] */
    IR_TEX_LEVELS,       /* imm.u = slot → uniform i32 mip seviye sayısı (0 okunursa 1 kabul edilir) */

    IR_OP_COUNT
} IrOp;

#define IR_NONE 0xFFFFFFFFu   /* kullanılmayan operand */

#define IR_FLAG_VARYING 1u    /* sonuç varying */
#define IR_FLAG_LIVE    2u    /* DCE işareti (geçici) */

typedef struct {
    u16 op;          /* IrOp */
    u8  type;        /* IrType */
    u8  flags;       /* IR_FLAG_* */
    u32 a, b, c;
    union { f32 f; i32 i; u32 u; } imm;
} IrInst;

typedef struct {
    IrInst* insts;
    u32     count;
    u32     cap;
    u32     uniform_bytes;   /* uniform bloğu toplam boyutu (ön uç doldurur) */
    u32     varying_count;   /* skaler varying sayısı (ön uç doldurur) */
    u32     texture_count;
} IrFunc;

/* ---------- Kurucu (ir.c — D hattı) ----------
 * Tüm kurucular tip ve tekdüzeliği operandlardan çıkarır, uyumsuzlukta PANIC (ön uç önce tip denetler). */
void ir_init(IrFunc* f, Arena* arena, u32 cap);
u32  ir_const_f32(IrFunc* f, f32 v);
u32  ir_const_i32(IrFunc* f, i32 v);
u32  ir_const_bool(IrFunc* f, b32 v);
u32  ir_uniform(IrFunc* f, IrType t, u32 byte_offset);
u32  ir_varying(IrFunc* f, u32 index);
u32  ir_frag(IrFunc* f, b32 y);
u32  ir_op1(IrFunc* f, IrOp op, u32 a);
u32  ir_op2(IrFunc* f, IrOp op, u32 a, u32 b);
u32  ir_select(IrFunc* f, u32 cond, u32 t, u32 e);
u32  ir_tex_info(IrFunc* f, IrOp op, u32 slot);
u32  ir_texel_load(IrFunc* f, u32 slot, u32 index);
void ir_if_begin(IrFunc* f, u32 cond);
void ir_else(IrFunc* f);
void ir_if_end(IrFunc* f);
void ir_discard(IrFunc* f, u32 mask);
void ir_output(IrFunc* f, u32 channel, u32 value);

/* sample(tex, u, v) genişletmesi: wrap/clamp + en yakın komşu veya bilinear,
 * RGBA8 → 4 adet f32 (0..1). flags = TEX_FLAG_* (derleme zamanında bilinen örnekleyici durumu).
 * TEX_FLAG_MIPMAP: seviye u,v'nin quad türevlerinden seçilir (shade_abi.h MIPMAP SEMANTİĞİ). */
void ir_emit_sample(IrFunc* f, u32 slot, u32 u, u32 v, u32 flags, u32 out_rgba[4]);
/* Açık seviyeli örnekleme (level: i32, [0, levels-1]'e kırpılır). flags'teki MIPMAP yok sayılır. */
void ir_emit_sample_level(IrFunc* f, u32 slot, u32 u, u32 v, u32 level, u32 flags, u32 out_rgba[4]);
/* sample() için mip seviyesi (i32) — C referansı ref_tex_mip_level ile bit-bit aynı */
u32  ir_emit_mip_level(IrFunc* f, u32 slot, u32 u, u32 v);

/* ---------- Doğrulama ve yazdırma ---------- */
/* Her operand tanımdan önce mi, tipler tutarlı mı, IF dengeli mi, OUTPUT 4 kanal mı.
 * Hata varsa mesajı err'e yazar ve FALSE döner. */
b32  ir_verify(const IrFunc* f, char* err, u32 err_cap);
/* İnsan-okur metin:  "%12 = fmul.v %3, %11" (".u" uniform, ".v" varying) */
u32  ir_print(const IrFunc* f, char* buf, u32 cap);

/* ---------- Optimizasyon geçişleri (D hattı) ---------- */
/* uniform özelleştirme: byte_offset'teki uniform'u sabitle değiştir (sonra constfold çalıştır) */
void ir_specialize_uniform_f32(IrFunc* f, u32 byte_offset, f32 value);
void ir_opt_constfold(IrFunc* f);  /* sabit katlama + cebirsel sadeleştirme (x*1, x+0, x*0 → 0 YOK: NaN) */
void ir_opt_cse(IrFunc* f);        /* BÖLGE FARKINDA: bir değer yalnızca onu kapsayan bölgedeki kullanımlara yeniden kullanılabilir */
void ir_opt_dce(IrFunc* f);        /* kökler: OUTPUT, DISCARD; boş IF bölgeleri silinir */
void ir_compact(IrFunc* f);        /* NOP'ları sil, kimlikleri yeniden numarala */
void ir_optimize(IrFunc* f);       /* constfold → cse → dce → compact (sabit noktaya kadar, en fazla 4 tur) */

/* ---------- Anlambilim notları ----------
 * - DERIV_X/Y: ön uç uniform operand için doğrudan 0 sabiti üretir; ir_op1 de aynısını yapar.
 *   Varying IF içinde de 4 lane'in hesaplanmış değerlerinden alınır (SPMD: tüm lane'ler hesaplanır).
 * - Atlanmış bir IF bölgesinde tanımlı değerler ÇÖPTÜR. Bölge dışında yalnızca o bölgenin
 *   koşuluna bağlı bir SELECT üzerinden kullanılabilir. Ön uç bunu garanti eder.
 * - Varying IF: codegen "then" bölgesini yalnızca any_true(cond & aktif) ise, "else"i yalnızca
 *   any_true(~cond & aktif) ise çalıştırır. Uniform IF: düz wasm if/else.
 * - DISCARD sonrası kod çalışmaya devam eder (SPMD); lane yalnızca yazımdan çıkar.
 */

#endif
