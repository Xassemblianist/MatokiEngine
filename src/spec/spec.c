/* spec/spec.c — Çalışma anı özelleştirici (ARCH_V2 İlke 3 / H1) implementasyonu.
 * EcsWorld arketiplerini hizalı SoA sütunlarına dönüştürür; arketip sütun ofsetlerini ve
 * simülasyon sabitlerini (dt, g, sürtünme, sınır kutusu) doğrudan mbin ile üretilen
 * dalsız SIMD128 WebAssembly modülüne gömer, varyant önbelleğinde saklar ve JIT ile yürütür.
 */
#include "spec/spec.h"
#include "mbin/mbin.h"
#include "mbin/mbin_ops.h"
#include "jit/jit.h"
#include "plat/platform.h"
#include "core/rng.h"
#include "core/log.h"

static char g_spec_report[SPEC_REPORT_CAP];
static u32  g_spec_report_len = 0u;

static inline u32 spec_f32_bits(f32 x) {
    u32 u;
    __builtin_memcpy(&u, &x, sizeof(u32));
    return u;
}

static inline b32 spec_f32_bit_eq(f32 a, f32 b) {
    return spec_f32_bits(a) == spec_f32_bits(b);
}

SpecParams spec_default_params(void) {
    SpecParams p;
    p.dt          = 1.0f / 60.0f;
    p.gravity     = -9.81f;
    p.drag        = 0.995f;
    p.bmin[0]     = -50.0f;
    p.bmin[1]     = -25.0f;
    p.bmin[2]     = -50.0f;
    p.bmax[0]     =  50.0f;
    p.bmax[1]     =  25.0f;
    p.bmax[2]     =  50.0f;
    p.restitution =  0.85f;
    return p;
}

void spec_arch_layout_init(SpecArchLayout* layout, u32 mask, u32 count, u32 extra_prefix_cols) {
    ASSERT(layout != NULL);
    u32 cap = (count == 0u) ? 16u : ALIGN_UP(count, 16u);
    u32 col_bytes = cap * 4u;
    u32 base_off  = extra_prefix_cols * col_bytes;

    layout->mask        = mask;
    layout->count       = count;
    layout->cap         = cap;
    layout->off_p[0]    = base_off + 0u * col_bytes;
    layout->off_p[1]    = base_off + 1u * col_bytes;
    layout->off_p[2]    = base_off + 2u * col_bytes;
    layout->off_v[0]    = base_off + 3u * col_bytes;
    layout->off_v[1]    = base_off + 4u * col_bytes;
    layout->off_v[2]    = base_off + 5u * col_bytes;
    layout->total_bytes = base_off + 6u * col_bytes;
}

const char* spec_kernel_export_name(SpecKernelKind kind) {
    switch (kind) {
    case SPEC_KERNEL_MOVE:    return "spec_move";
    case SPEC_KERNEL_GRAVITY: return "spec_gravity";
    case SPEC_KERNEL_BOUNCE:  return "spec_bounce";
    case SPEC_KERNEL_FUSED:   return "spec_fused";
    default:                  return "spec_unknown";
    }
}

/* ============================================================================
 * 1. Çalışma Anı WebAssembly SIMD128 Kod Üreticisi (mbin arka ucu)
 * ============================================================================ */

typedef struct {
    MbFunc*               mf;
    SpecKernelKind        kind;
    const SpecArchLayout* layout;
    const SpecParams*     params;
    f32                   dt;
    f32                   g_dt;
    f32                   drag;
    f32                   neg_rest;

    /* i32 local'leri */
    u32 loc_base;
    u32 loc_count;
    u32 loc_ptr;
    u32 loc_end;

    /* Skaler kuyruk f32 local'leri */
    u32 loc_fp;
    u32 loc_fv;

    /* SIMD v128 geçici ve sabit local'leri */
    u32 loc_vp;
    u32 loc_vv;
    u32 loc_v_dt;
    u32 loc_v_g_dt;
    u32 loc_v_drag;
    u32 loc_v_bmin[3];
    u32 loc_v_bmax[3];
    u32 loc_v_neg_rest;
} SpecCgCtx;

/* Tek bir eksen (a = 0:X, 1:Y, 2:Z) ve unroll bayt ofseti (u_off = 0, 16, 32, 48) için
 * dalsız SIMD128 komutlarını yayar. Sütun ofsetleri doğrudan v128.load/store memarg'ına gömülür. */
static void emit_simd_axis(SpecCgCtx* cg, u32 a, u32 u_off) {
    MbFunc* mf = cg->mf;
    u32 off_p  = cg->layout->off_p[a] + u_off;
    u32 off_v  = cg->layout->off_v[a] + u_off;

    switch (cg->kind) {
    case SPEC_KERNEL_MOVE:
        /* p[a] = p[a] + v[a] * dt */
        mb_local_get(mf, cg->loc_ptr);
        mb_local_get(mf, cg->loc_ptr);
        mb_v128_load(mf, 4u, off_p);
        mb_local_get(mf, cg->loc_ptr);
        mb_v128_load(mf, 4u, off_v);
        mb_local_get(mf, cg->loc_v_dt);
        mb_f32x4_mul(mf);
        mb_f32x4_add(mf);
        mb_v128_store(mf, 4u, off_p);
        break;

    case SPEC_KERNEL_GRAVITY:
        /* v[a] = (v[a] [+ g_dt]) * drag */
        mb_local_get(mf, cg->loc_ptr);
        mb_local_get(mf, cg->loc_ptr);
        mb_v128_load(mf, 4u, off_v);
        if (a == 1u) {
            mb_local_get(mf, cg->loc_v_g_dt);
            mb_f32x4_add(mf);
        }
        mb_local_get(mf, cg->loc_v_drag);
        mb_f32x4_mul(mf);
        mb_v128_store(mf, 4u, off_v);
        break;

    case SPEC_KERNEL_BOUNCE:
        /* p_yeni = pmin(pmax(p, bmin), bmax) */
        mb_local_get(mf, cg->loc_ptr);
        mb_local_get(mf, cg->loc_ptr);
        mb_v128_load(mf, 4u, off_p);
        mb_local_tee(mf, cg->loc_vp);
        mb_local_get(mf, cg->loc_v_bmin[a]);
        mb_f32x4_pmax(mf);
        mb_local_get(mf, cg->loc_v_bmax[a]);
        mb_f32x4_pmin(mf);
        mb_v128_store(mf, 4u, off_p);

        /* v_yeni = bitselect(v * neg_rest, v, (p < bmin) | (p > bmax)) */
        mb_local_get(mf, cg->loc_ptr);
        mb_local_get(mf, cg->loc_ptr);
        mb_v128_load(mf, 4u, off_v);
        mb_local_tee(mf, cg->loc_vv);
        mb_local_get(mf, cg->loc_v_neg_rest);
        mb_f32x4_mul(mf);
        mb_local_get(mf, cg->loc_vv);
        mb_local_get(mf, cg->loc_vp);
        mb_local_get(mf, cg->loc_v_bmin[a]);
        mb_f32x4_lt(mf);
        mb_local_get(mf, cg->loc_vp);
        mb_local_get(mf, cg->loc_v_bmax[a]);
        mb_f32x4_gt(mf);
        mb_v128_or(mf);
        mb_v128_bitselect(mf);
        mb_v128_store(mf, 4u, off_v);
        break;

    case SPEC_KERNEL_FUSED:
    default:
        /* 1. p1 = p0 + v0 * dt (loc_vp), v0 -> loc_vv; p2 = pmin(pmax(p1, bmin), bmax) -> store */
        mb_local_get(mf, cg->loc_ptr);
        mb_local_get(mf, cg->loc_ptr);
        mb_v128_load(mf, 4u, off_p);
        mb_local_get(mf, cg->loc_ptr);
        mb_v128_load(mf, 4u, off_v);
        mb_local_tee(mf, cg->loc_vv);
        mb_local_get(mf, cg->loc_v_dt);
        mb_f32x4_mul(mf);
        mb_f32x4_add(mf);
        mb_local_tee(mf, cg->loc_vp);
        mb_local_get(mf, cg->loc_v_bmin[a]);
        mb_f32x4_pmax(mf);
        mb_local_get(mf, cg->loc_v_bmax[a]);
        mb_f32x4_pmin(mf);
        mb_v128_store(mf, 4u, off_p);

        /* 2. v1 = (v0 [+ g_dt]) * drag (loc_vv); v2 = bitselect(v1 * neg_rest, v1, mask) -> store */
        mb_local_get(mf, cg->loc_ptr);
        mb_local_get(mf, cg->loc_vv);
        if (a == 1u) {
            mb_local_get(mf, cg->loc_v_g_dt);
            mb_f32x4_add(mf);
        }
        mb_local_get(mf, cg->loc_v_drag);
        mb_f32x4_mul(mf);
        mb_local_tee(mf, cg->loc_vv);
        mb_local_get(mf, cg->loc_v_neg_rest);
        mb_f32x4_mul(mf);
        mb_local_get(mf, cg->loc_vv);
        mb_local_get(mf, cg->loc_vp);
        mb_local_get(mf, cg->loc_v_bmin[a]);
        mb_f32x4_lt(mf);
        mb_local_get(mf, cg->loc_vp);
        mb_local_get(mf, cg->loc_v_bmax[a]);
        mb_f32x4_gt(mf);
        mb_v128_or(mf);
        mb_v128_bitselect(mf);
        mb_v128_store(mf, 4u, off_v);
        break;
    }
}

/* Kalan (count & 3) varlık için dalsız skaler (select tabanlı) kuyruk komutlarını yayar. */
static void emit_scalar_axis(SpecCgCtx* cg, u32 a) {
    MbFunc* mf = cg->mf;
    u32 off_p  = cg->layout->off_p[a];
    u32 off_v  = cg->layout->off_v[a];
    f32 bmin   = cg->params->bmin[a];
    f32 bmax   = cg->params->bmax[a];

    switch (cg->kind) {
    case SPEC_KERNEL_MOVE:
        mb_local_get(mf, cg->loc_ptr);
        mb_local_get(mf, cg->loc_ptr);
        mb_f32_load(mf, 2u, off_p);
        mb_local_get(mf, cg->loc_ptr);
        mb_f32_load(mf, 2u, off_v);
        mb_f32_const(mf, cg->dt);
        mb_f32_mul(mf);
        mb_f32_add(mf);
        mb_f32_store(mf, 2u, off_p);
        break;

    case SPEC_KERNEL_GRAVITY:
        mb_local_get(mf, cg->loc_ptr);
        mb_local_get(mf, cg->loc_ptr);
        mb_f32_load(mf, 2u, off_v);
        if (a == 1u) {
            mb_f32_const(mf, cg->g_dt);
            mb_f32_add(mf);
        }
        mb_f32_const(mf, cg->drag);
        mb_f32_mul(mf);
        mb_f32_store(mf, 2u, off_v);
        break;

    case SPEC_KERNEL_BOUNCE:
        mb_local_get(mf, cg->loc_ptr);
        mb_f32_const(mf, bmax);
        mb_f32_const(mf, bmin);
        mb_local_get(mf, cg->loc_ptr);
        mb_f32_load(mf, 2u, off_p);
        mb_local_tee(mf, cg->loc_fp);
        mb_local_get(mf, cg->loc_fp);
        mb_f32_const(mf, bmin);
        mb_f32_lt(mf);
        mb_select(mf);
        mb_local_get(mf, cg->loc_fp);
        mb_f32_const(mf, bmax);
        mb_f32_gt(mf);
        mb_select(mf);
        mb_f32_store(mf, 2u, off_p);

        mb_local_get(mf, cg->loc_ptr);
        mb_local_get(mf, cg->loc_ptr);
        mb_f32_load(mf, 2u, off_v);
        mb_local_tee(mf, cg->loc_fv);
        mb_f32_const(mf, cg->neg_rest);
        mb_f32_mul(mf);
        mb_local_get(mf, cg->loc_fv);
        mb_local_get(mf, cg->loc_fp);
        mb_f32_const(mf, bmin);
        mb_f32_lt(mf);
        mb_local_get(mf, cg->loc_fp);
        mb_f32_const(mf, bmax);
        mb_f32_gt(mf);
        mb_i32_or(mf);
        mb_select(mf);
        mb_f32_store(mf, 2u, off_v);
        break;

    case SPEC_KERNEL_FUSED:
    default:
        mb_local_get(mf, cg->loc_ptr);
        mb_f32_const(mf, bmax);
        mb_f32_const(mf, bmin);
        mb_local_get(mf, cg->loc_ptr);
        mb_f32_load(mf, 2u, off_p);
        mb_local_get(mf, cg->loc_ptr);
        mb_f32_load(mf, 2u, off_v);
        mb_local_tee(mf, cg->loc_fv);
        mb_f32_const(mf, cg->dt);
        mb_f32_mul(mf);
        mb_f32_add(mf);
        mb_local_tee(mf, cg->loc_fp);
        mb_local_get(mf, cg->loc_fp);
        mb_f32_const(mf, bmin);
        mb_f32_lt(mf);
        mb_select(mf);
        mb_local_get(mf, cg->loc_fp);
        mb_f32_const(mf, bmax);
        mb_f32_gt(mf);
        mb_select(mf);
        mb_f32_store(mf, 2u, off_p);

        mb_local_get(mf, cg->loc_ptr);
        mb_local_get(mf, cg->loc_fv);
        if (a == 1u) {
            mb_f32_const(mf, cg->g_dt);
            mb_f32_add(mf);
        }
        mb_f32_const(mf, cg->drag);
        mb_f32_mul(mf);
        mb_local_tee(mf, cg->loc_fv);
        mb_f32_const(mf, cg->neg_rest);
        mb_f32_mul(mf);
        mb_local_get(mf, cg->loc_fv);
        mb_local_get(mf, cg->loc_fp);
        mb_f32_const(mf, bmin);
        mb_f32_lt(mf);
        mb_local_get(mf, cg->loc_fp);
        mb_f32_const(mf, bmax);
        mb_f32_gt(mf);
        mb_i32_or(mf);
        mb_select(mf);
        mb_f32_store(mf, 2u, off_v);
        break;
    }
}

u32 spec_codegen_kernel(SpecKernelKind kind,
                        const SpecArchLayout* layout,
                        const SpecParams* params,
                        b32 shared_memory,
                        b32 own_memory,
                        Arena* scratch,
                        u8* out,
                        u32 out_cap) {
    if (!layout || !params || !scratch || !out || out_cap == 0u) return 0u;
    if ((u32)kind >= (u32)SPEC_KERNEL_COUNT) return 0u;

    for (u32 a = 0u; a < 3u; a++) {
        if ((layout->off_p[a] & 15u) != 0u || (layout->off_v[a] & 15u) != 0u) {
            return 0u;
        }
    }

    usize mark = arena_mark(scratch);
    u32 work_cap = 131072u;
    u8* work_mem = ARENA_PUSH_ARRAY(scratch, u8, work_cap);

    MbModule mod;
    mb_module_init(&mod, work_mem, work_cap);

    if (own_memory) {
        mb_memory(&mod, 64u, 64u, FALSE);
    } else {
        u32 max_pages = shared_memory ? 16384u : 0u;
        mb_import_memory(&mod, "env", "memory", 1u, max_pages, shared_memory);
    }

    u8 param_types[2] = { MB_VAL_I32, MB_VAL_I32 };
    u32 type_idx = mb_type(&mod, param_types, 2u, NULL, 0u);
    MbFunc* mf   = mb_func_begin(&mod, type_idx);

    SpecCgCtx cg;
    __builtin_memset(&cg, 0, sizeof(cg));
    cg.mf       = mf;
    cg.kind     = kind;
    cg.layout   = layout;
    cg.params   = params;
    cg.dt       = params->dt;
    cg.g_dt     = params->gravity * params->dt;
    cg.drag     = params->drag;
    cg.neg_rest = -params->restitution;

    cg.loc_base  = 0u;
    cg.loc_count = 1u;
    cg.loc_ptr   = mb_local(mf, MB_VAL_I32, 1u);
    cg.loc_end   = mb_local(mf, MB_VAL_I32, 1u);

    b32 need_move    = (kind == SPEC_KERNEL_MOVE    || kind == SPEC_KERNEL_FUSED);
    b32 need_gravity = (kind == SPEC_KERNEL_GRAVITY || kind == SPEC_KERNEL_FUSED);
    b32 need_bounce  = (kind == SPEC_KERNEL_BOUNCE  || kind == SPEC_KERNEL_FUSED);

    if (need_bounce) {
        cg.loc_fp = mb_local(mf, MB_VAL_F32, 1u);
        cg.loc_fv = mb_local(mf, MB_VAL_F32, 1u);
        cg.loc_vp = mb_local(mf, MB_VAL_V128, 1u);
        cg.loc_vv = mb_local(mf, MB_VAL_V128, 1u);
    }
    if (need_move) {
        cg.loc_v_dt = mb_local(mf, MB_VAL_V128, 1u);
    }
    if (need_gravity) {
        cg.loc_v_g_dt = mb_local(mf, MB_VAL_V128, 1u);
        cg.loc_v_drag = mb_local(mf, MB_VAL_V128, 1u);
    }
    if (need_bounce) {
        for (u32 a = 0u; a < 3u; a++) {
            cg.loc_v_bmin[a] = mb_local(mf, MB_VAL_V128, 1u);
            cg.loc_v_bmax[a] = mb_local(mf, MB_VAL_V128, 1u);
        }
        cg.loc_v_neg_rest = mb_local(mf, MB_VAL_V128, 1u);
    }

    /* count == 0 ise erken dön */
    mb_local_get(mf, cg.loc_count);
    mb_i32_eqz(mf);
    mb_if(mf, MB_BLOCK_VOID);
    mb_return(mf);
    mb_end(mf);

    /* Preheader: sabitleri v128 yazmaçlarına yükle */
    if (need_move) {
        mb_f32x4_const(mf, cg.dt, cg.dt, cg.dt, cg.dt);
        mb_local_set(mf, cg.loc_v_dt);
    }
    if (need_gravity) {
        mb_f32x4_const(mf, cg.g_dt, cg.g_dt, cg.g_dt, cg.g_dt);
        mb_local_set(mf, cg.loc_v_g_dt);
        mb_f32x4_const(mf, cg.drag, cg.drag, cg.drag, cg.drag);
        mb_local_set(mf, cg.loc_v_drag);
    }
    if (need_bounce) {
        for (u32 a = 0u; a < 3u; a++) {
            f32 bmin = params->bmin[a];
            f32 bmax = params->bmax[a];
            mb_f32x4_const(mf, bmin, bmin, bmin, bmin);
            mb_local_set(mf, cg.loc_v_bmin[a]);
            mb_f32x4_const(mf, bmax, bmax, bmax, bmax);
            mb_local_set(mf, cg.loc_v_bmax[a]);
        }
        mb_f32x4_const(mf, cg.neg_rest, cg.neg_rest, cg.neg_rest, cg.neg_rest);
        mb_local_set(mf, cg.loc_v_neg_rest);
    }

    /* ptr = base */
    mb_local_get(mf, cg.loc_base);
    mb_local_set(mf, cg.loc_ptr);

    /* 1. Ana döngü: 4x açılmış (unrolled) SIMD128 döngüsü (16 varlık = 64 bayt / iterasyon) */
    mb_local_get(mf, cg.loc_count);
    mb_i32_const(mf, -16);
    mb_i32_and(mf);
    mb_local_tee(mf, cg.loc_end);
    mb_if(mf, MB_BLOCK_VOID);
        mb_local_get(mf, cg.loc_base);
        mb_local_get(mf, cg.loc_end);
        mb_i32_const(mf, 2);
        mb_i32_shl(mf);
        mb_i32_add(mf);
        mb_local_set(mf, cg.loc_end);

        mb_loop(mf, MB_BLOCK_VOID);
            for (u32 a = 0u; a < 3u; a++) {
                for (u32 u = 0u; u < 4u; u++) {
                    emit_simd_axis(&cg, a, u * 16u);
                }
            }
            mb_local_get(mf, cg.loc_ptr);
            mb_i32_const(mf, 64);
            mb_i32_add(mf);
            mb_local_tee(mf, cg.loc_ptr);
            mb_local_get(mf, cg.loc_end);
            mb_i32_ne(mf);
            mb_br_if(mf, 0u);
        mb_end(mf);
    mb_end(mf);

    /* 2. Kalan 4'lü gruplar için 1x SIMD128 döngüsü (4 varlık = 16 bayt / iterasyon) */
    mb_local_get(mf, cg.loc_base);
    mb_local_get(mf, cg.loc_count);
    mb_i32_const(mf, -4);
    mb_i32_and(mf);
    mb_i32_const(mf, 2);
    mb_i32_shl(mf);
    mb_i32_add(mf);
    mb_local_tee(mf, cg.loc_end);
    mb_local_get(mf, cg.loc_ptr);
    mb_i32_ne(mf);
    mb_if(mf, MB_BLOCK_VOID);
        mb_loop(mf, MB_BLOCK_VOID);
            for (u32 a = 0u; a < 3u; a++) {
                emit_simd_axis(&cg, a, 0u);
            }
            mb_local_get(mf, cg.loc_ptr);
            mb_i32_const(mf, 16);
            mb_i32_add(mf);
            mb_local_tee(mf, cg.loc_ptr);
            mb_local_get(mf, cg.loc_end);
            mb_i32_ne(mf);
            mb_br_if(mf, 0u);
        mb_end(mf);
    mb_end(mf);

    /* 3. Kalan (count & 3) varlık için skaler kuyruk döngüsü (1 varlık = 4 bayt / iterasyon) */
    mb_local_get(mf, cg.loc_base);
    mb_local_get(mf, cg.loc_count);
    mb_i32_const(mf, 2);
    mb_i32_shl(mf);
    mb_i32_add(mf);
    mb_local_tee(mf, cg.loc_end);
    mb_local_get(mf, cg.loc_ptr);
    mb_i32_ne(mf);
    mb_if(mf, MB_BLOCK_VOID);
        mb_loop(mf, MB_BLOCK_VOID);
            for (u32 a = 0u; a < 3u; a++) {
                emit_scalar_axis(&cg, a);
            }
            mb_local_get(mf, cg.loc_ptr);
            mb_i32_const(mf, 4);
            mb_i32_add(mf);
            mb_local_tee(mf, cg.loc_ptr);
            mb_local_get(mf, cg.loc_end);
            mb_i32_ne(mf);
            mb_br_if(mf, 0u);
        mb_end(mf);
    mb_end(mf);

    mb_func_end(mf);
    mb_export_func(&mod, spec_kernel_export_name(kind), mf->func_idx);

    if (own_memory) {
        mb_export(&mod, "memory", MB_EXT_MEMORY, 0u);
    }

    MbBuf out_buf;
    mb_buf_init(&out_buf, out, out_cap);
    u32 wasm_len = mb_module_finish(&mod, &out_buf);

    arena_pop_to(scratch, mark);
    return wasm_len;
}

/* ============================================================================
 * 2. Varyant Önbelleği (SpecCache)
 * ============================================================================ */

static u64 spec_hash_u32(u64 h, u32 v) {
    for (u32 b = 0u; b < 4u; b++) {
        h ^= (u64)((v >> (b * 8u)) & 0xFFu);
        h *= 1099511628211ULL;
    }
    return h;
}

static u64 spec_hash_key(const SpecKey* k) {
    u64 h = 1469598103934665603ULL;
    h = spec_hash_u32(h, k->kind);
    for (u32 a = 0u; a < 3u; a++) {
        h = spec_hash_u32(h, k->off_p[a]);
        h = spec_hash_u32(h, k->off_v[a]);
    }
    h = spec_hash_u32(h, spec_f32_bits(k->params.dt));
    h = spec_hash_u32(h, spec_f32_bits(k->params.gravity));
    h = spec_hash_u32(h, spec_f32_bits(k->params.drag));
    for (u32 a = 0u; a < 3u; a++) {
        h = spec_hash_u32(h, spec_f32_bits(k->params.bmin[a]));
        h = spec_hash_u32(h, spec_f32_bits(k->params.bmax[a]));
    }
    h = spec_hash_u32(h, spec_f32_bits(k->params.restitution));
    return h;
}

static b32 spec_keys_equal(const SpecKey* a, const SpecKey* b) {
    if (a->kind != b->kind) return FALSE;
    for (u32 i = 0u; i < 3u; i++) {
        if (a->off_p[i] != b->off_p[i] || a->off_v[i] != b->off_v[i]) return FALSE;
        if (!spec_f32_bit_eq(a->params.bmin[i], b->params.bmin[i])) return FALSE;
        if (!spec_f32_bit_eq(a->params.bmax[i], b->params.bmax[i])) return FALSE;
    }
    if (!spec_f32_bit_eq(a->params.dt, b->params.dt)) return FALSE;
    if (!spec_f32_bit_eq(a->params.gravity, b->params.gravity)) return FALSE;
    if (!spec_f32_bit_eq(a->params.drag, b->params.drag)) return FALSE;
    if (!spec_f32_bit_eq(a->params.restitution, b->params.restitution)) return FALSE;
    return TRUE;
}

void spec_cache_init(SpecCache* cache, Arena* perm_arena) {
    ASSERT(cache != NULL && perm_arena != NULL);
    __builtin_memset(cache, 0, sizeof(*cache));
    cache->arena = perm_arena;
}

SpecKey spec_make_key(SpecKernelKind kind, const SpecArchLayout* layout, const SpecParams* params) {
    SpecKey k;
    __builtin_memset(&k, 0, sizeof(k));
    k.kind = (u32)kind;
    for (u32 a = 0u; a < 3u; a++) {
        k.off_p[a] = layout->off_p[a];
        k.off_v[a] = layout->off_v[a];
    }
    k.params = *params;
    return k;
}

const SpecVariant* spec_cache_get(SpecCache* cache, const SpecKey* key, Arena* scratch) {
    if (!cache || !key || !scratch) return NULL;

    u64 h = spec_hash_key(key);
    for (u32 i = 0u; i < cache->count; i++) {
        const SpecVariant* v = &cache->entries[i];
        if (v->used && v->hash == h && spec_keys_equal(&v->key, key)) {
            cache->hits++;
            return v;
        }
    }

    cache->misses++;

    SpecArchLayout layout;
    __builtin_memset(&layout, 0, sizeof(layout));
    for (u32 a = 0u; a < 3u; a++) {
        layout.off_p[a] = key->off_p[a];
        layout.off_v[a] = key->off_v[a];
    }

    u8 tmp_buf[4096];

#if defined(PLAT_SHARED_MEMORY) && PLAT_SHARED_MEMORY
    b32 shared_mem = TRUE;
#else
    b32 shared_mem = FALSE;
#endif

    f64 t0 = plat_time_ms();
    SpecKernelKind kind = (SpecKernelKind)key->kind;
    u32 wasm_len = spec_codegen_kernel(kind, &layout, &key->params,
                                       shared_mem, FALSE, scratch, tmp_buf, (u32)sizeof(tmp_buf));
    if (wasm_len == 0u) {
        return NULL;
    }

    u8* perm_wasm = ARENA_PUSH_ARRAY(cache->arena, u8, wasm_len);
    __builtin_memcpy(perm_wasm, tmp_buf, wasm_len);

    SpecKernelFn fn = NULL;
#ifdef __wasm__
    const char* exp_name = spec_kernel_export_name(kind);
    fn = (SpecKernelFn)jit_load(perm_wasm, wasm_len, exp_name);
#endif
    f64 t1 = plat_time_ms();

    u32 slot = (cache->count < SPEC_CACHE_CAP) ? cache->count++ : (u32)(h % SPEC_CACHE_CAP);
    SpecVariant* dst = &cache->entries[slot];
    dst->used       = TRUE;
    dst->hash       = h;
    dst->key        = *key;
    dst->wasm       = perm_wasm;
    dst->wasm_len   = wasm_len;
    dst->compile_ms = t1 - t0;
    dst->fn         = fn;
    return dst;
}

/* ============================================================================
 * 3. EcsWorld <-> SpecWorld (Arketip SoA Dönüştürücü)
 * ============================================================================ */

static u32 spec_popcnt32(u32 x) {
    u32 c = 0u;
    while (x != 0u) {
        x &= (x - 1u);
        c++;
    }
    return c;
}

void spec_world_build(SpecWorld* sw, EcsWorld* w, EcsCompId cid_pos, EcsCompId cid_vel, Arena* arena) {
    ASSERT(sw != NULL && w != NULL && arena != NULL);
    __builtin_memset(sw, 0, sizeof(*sw));
    sw->cid_pos = cid_pos;
    sw->cid_vel = cid_vel;

    u32 req_mask = (1u << cid_pos) | (1u << cid_vel);
    u32 masks[SPEC_MAX_ARCHETYPES];
    u32 counts[SPEC_MAX_ARCHETYPES];
    u32 arch_n = 0u;

    /* 1. Geçiş: EcsQuery ile eşleşen varlıkların arketiplerini ve eleman sayılarını bul */
    EcsQuery q = ecs_query(w, req_mask);
    while (ecs_query_next(&q)) {
        u32 idx   = ecs_entity_index(q.entity);
        u32 emask = w->ent_mask[idx];
        u32 found = SPEC_MAX_ARCHETYPES;
        for (u32 a = 0u; a < arch_n; a++) {
            if (masks[a] == emask) {
                found = a;
                break;
            }
        }
        if (found == SPEC_MAX_ARCHETYPES) {
            ASSERT(arch_n < SPEC_MAX_ARCHETYPES);
            found = arch_n++;
            masks[found]  = emask;
            counts[found] = 0u;
        }
        counts[found]++;
    }

    sw->arch_count = arch_n;
    u32 cursors[SPEC_MAX_ARCHETYPES];

    for (u32 a = 0u; a < arch_n; a++) {
        SpecArchetype* arch = &sw->archetypes[a];
        u32 extra_cols = spec_popcnt32(masks[a] & ~req_mask);
        spec_arch_layout_init(&arch->layout, masks[a], counts[a], extra_cols);
        arch->data     = (u8*)arena_push(arena, arch->layout.total_bytes, 16u);
        arch->entities = ARENA_PUSH_ARRAY(arena, EcsEntity, counts[a] > 0u ? counts[a] : 1u);
        cursors[a]     = 0u;
        sw->total_entities += counts[a];
    }

    /* 2. Geçiş: Varlık tutamaklarını kaydet ve başlangıç bileşenlerini SoA sütunlarına kopyala */
    q = ecs_query(w, req_mask);
    while (ecs_query_next(&q)) {
        u32 idx   = ecs_entity_index(q.entity);
        u32 emask = w->ent_mask[idx];
        for (u32 a = 0u; a < arch_n; a++) {
            if (masks[a] == emask) {
                u32 pos_i = cursors[a]++;
                sw->archetypes[a].entities[pos_i] = q.entity;
                break;
            }
        }
    }

    spec_world_sync_from_ecs(sw, w);
}

void spec_world_sync_from_ecs(SpecWorld* sw, const EcsWorld* w) {
    ASSERT(sw != NULL && w != NULL);
    const EcsStore* sp = &w->stores[sw->cid_pos];
    const EcsStore* sv = &w->stores[sw->cid_vel];

    for (u32 a = 0u; a < sw->arch_count; a++) {
        SpecArchetype* arch = &sw->archetypes[a];
        u32 n = arch->layout.count;
        f32* px = (f32*)(arch->data + arch->layout.off_p[0]);
        f32* py = (f32*)(arch->data + arch->layout.off_p[1]);
        f32* pz = (f32*)(arch->data + arch->layout.off_p[2]);
        f32* vx = (f32*)(arch->data + arch->layout.off_v[0]);
        f32* vy = (f32*)(arch->data + arch->layout.off_v[1]);
        f32* vz = (f32*)(arch->data + arch->layout.off_v[2]);

        for (u32 i = 0u; i < n; i++) {
            u32 idx  = ecs_entity_index(arch->entities[i]);
            u32 dp   = sp->sparse[idx];
            u32 dv   = sv->sparse[idx];
            const SpecPos* p = (const SpecPos*)(sp->data + (usize)dp * sp->stride);
            const SpecVel* v = (const SpecVel*)(sv->data + (usize)dv * sv->stride);
            px[i] = p->x;
            py[i] = p->y;
            pz[i] = p->z;
            vx[i] = v->vx;
            vy[i] = v->vy;
            vz[i] = v->vz;
        }
    }
}

void spec_world_sync_to_ecs(const SpecWorld* sw, EcsWorld* w) {
    ASSERT(sw != NULL && w != NULL);
    EcsStore* sp = &w->stores[sw->cid_pos];
    EcsStore* sv = &w->stores[sw->cid_vel];

    for (u32 a = 0u; a < sw->arch_count; a++) {
        const SpecArchetype* arch = &sw->archetypes[a];
        u32 n = arch->layout.count;
        const f32* px = (const f32*)(arch->data + arch->layout.off_p[0]);
        const f32* py = (const f32*)(arch->data + arch->layout.off_p[1]);
        const f32* pz = (const f32*)(arch->data + arch->layout.off_p[2]);
        const f32* vx = (const f32*)(arch->data + arch->layout.off_v[0]);
        const f32* vy = (const f32*)(arch->data + arch->layout.off_v[1]);
        const f32* vz = (const f32*)(arch->data + arch->layout.off_v[2]);

        for (u32 i = 0u; i < n; i++) {
            u32 idx    = ecs_entity_index(arch->entities[i]);
            u32 dp     = sp->sparse[idx];
            u32 dv     = sv->sparse[idx];
            SpecPos* p = (SpecPos*)(sp->data + (usize)dp * sp->stride);
            SpecVel* v = (SpecVel*)(sv->data + (usize)dv * sv->stride);
            p->x  = px[i];
            p->y  = py[i];
            p->z  = pz[i];
            v->vx = vx[i];
            v->vy = vy[i];
            v->vz = vz[i];
        }
    }
}

/* ============================================================================
 * 4. (a) Genel C Döngüsü — Mevcut EcsQuery Sorgu API'si
 * ============================================================================ */

void spec_ecs_step_move(EcsWorld* w, EcsCompId cid_pos, EcsCompId cid_vel, const SpecParams* params) {
    u32 mask = (1u << cid_pos) | (1u << cid_vel);
    f32 dt   = params->dt;
    EcsQuery q = ecs_query(w, mask);
    while (ecs_query_next(&q)) {
        SpecPos* p = (SpecPos*)ecs_query_get(&q, cid_pos);
        SpecVel* v = (SpecVel*)ecs_query_get(&q, cid_vel);
        f32 dx = v->vx * dt;
        f32 dy = v->vy * dt;
        f32 dz = v->vz * dt;
        p->x = p->x + dx;
        p->y = p->y + dy;
        p->z = p->z + dz;
    }
}

void spec_ecs_step_gravity(EcsWorld* w, EcsCompId cid_pos, EcsCompId cid_vel, const SpecParams* params) {
    u32 mask = (1u << cid_pos) | (1u << cid_vel);
    f32 g_dt = params->gravity * params->dt;
    f32 drag = params->drag;
    EcsQuery q = ecs_query(w, mask);
    while (ecs_query_next(&q)) {
        SpecVel* v = (SpecVel*)ecs_query_get(&q, cid_vel);
        v->vx = v->vx * drag;
        v->vy = (v->vy + g_dt) * drag;
        v->vz = v->vz * drag;
    }
}

void spec_ecs_step_bounce(EcsWorld* w, EcsCompId cid_pos, EcsCompId cid_vel, const SpecParams* params) {
    u32 mask     = (1u << cid_pos) | (1u << cid_vel);
    f32 min_x    = params->bmin[0], max_x = params->bmax[0];
    f32 min_y    = params->bmin[1], max_y = params->bmax[1];
    f32 min_z    = params->bmin[2], max_z = params->bmax[2];
    f32 neg_rest = -params->restitution;

    EcsQuery q = ecs_query(w, mask);
    while (ecs_query_next(&q)) {
        SpecPos* p = (SpecPos*)ecs_query_get(&q, cid_pos);
        SpecVel* v = (SpecVel*)ecs_query_get(&q, cid_vel);

        f32 px = p->x;
        if (px < min_x) {
            p->x  = min_x;
            v->vx = v->vx * neg_rest;
        } else if (px > max_x) {
            p->x  = max_x;
            v->vx = v->vx * neg_rest;
        }

        f32 py = p->y;
        if (py < min_y) {
            p->y  = min_y;
            v->vy = v->vy * neg_rest;
        } else if (py > max_y) {
            p->y  = max_y;
            v->vy = v->vy * neg_rest;
        }

        f32 pz = p->z;
        if (pz < min_z) {
            p->z  = min_z;
            v->vz = v->vz * neg_rest;
        } else if (pz > max_z) {
            p->z  = max_z;
            v->vz = v->vz * neg_rest;
        }
    }
}

void spec_ecs_step_all(EcsWorld* w, EcsCompId cid_pos, EcsCompId cid_vel, const SpecParams* params) {
    spec_ecs_step_move(w, cid_pos, cid_vel, params);
    spec_ecs_step_gravity(w, cid_pos, cid_vel, params);
    spec_ecs_step_bounce(w, cid_pos, cid_vel, params);
}

/* ============================================================================
 * 5. (a-soa) Genel C SoA Döngüsü — Çalışma Anı İşaretçili & Parametreli C
 * ============================================================================ */

void spec_c_soa_move_arch(SpecArchetype* arch, const SpecParams* params) {
    u32 n  = arch->layout.count;
    f32 dt = params->dt;
    for (u32 a = 0u; a < 3u; a++) {
        f32* __restrict p       = (f32*)(arch->data + arch->layout.off_p[a]);
        const f32* __restrict v = (const f32*)(arch->data + arch->layout.off_v[a]);
        for (u32 i = 0u; i < n; i++) {
            p[i] = p[i] + v[i] * dt;
        }
    }
}

void spec_c_soa_gravity_arch(SpecArchetype* arch, const SpecParams* params) {
    u32 n    = arch->layout.count;
    f32 g_dt = params->gravity * params->dt;
    f32 drag = params->drag;

    f32* __restrict vx = (f32*)(arch->data + arch->layout.off_v[0]);
    f32* __restrict vy = (f32*)(arch->data + arch->layout.off_v[1]);
    f32* __restrict vz = (f32*)(arch->data + arch->layout.off_v[2]);

    for (u32 i = 0u; i < n; i++) vx[i] = vx[i] * drag;
    for (u32 i = 0u; i < n; i++) vy[i] = (vy[i] + g_dt) * drag;
    for (u32 i = 0u; i < n; i++) vz[i] = vz[i] * drag;
}

void spec_c_soa_bounce_arch(SpecArchetype* arch, const SpecParams* params) {
    u32 n        = arch->layout.count;
    f32 neg_rest = -params->restitution;
    for (u32 a = 0u; a < 3u; a++) {
        f32 bmin          = params->bmin[a];
        f32 bmax          = params->bmax[a];
        f32* __restrict p = (f32*)(arch->data + arch->layout.off_p[a]);
        f32* __restrict v = (f32*)(arch->data + arch->layout.off_v[a]);
        for (u32 i = 0u; i < n; i++) {
            f32 pi = p[i];
            if (pi < bmin) {
                p[i] = bmin;
                v[i] = v[i] * neg_rest;
            } else if (pi > bmax) {
                p[i] = bmax;
                v[i] = v[i] * neg_rest;
            }
        }
    }
}

void spec_c_soa_fused_arch(SpecArchetype* arch, const SpecParams* params) {
    u32 n        = arch->layout.count;
    f32 dt       = params->dt;
    f32 g_dt     = params->gravity * params->dt;
    f32 drag     = params->drag;
    f32 neg_rest = -params->restitution;

    for (u32 a = 0u; a < 3u; a++) {
        f32 bmin          = params->bmin[a];
        f32 bmax          = params->bmax[a];
        f32* __restrict p = (f32*)(arch->data + arch->layout.off_p[a]);
        f32* __restrict v = (f32*)(arch->data + arch->layout.off_v[a]);
        for (u32 i = 0u; i < n; i++) {
            f32 v0 = v[i];
            f32 p1 = p[i] + v0 * dt;
            f32 v1 = (a == 1u) ? ((v0 + g_dt) * drag) : (v0 * drag);
            if (p1 < bmin) {
                p[i] = bmin;
                v[i] = v1 * neg_rest;
            } else if (p1 > bmax) {
                p[i] = bmax;
                v[i] = v1 * neg_rest;
            } else {
                p[i] = p1;
                v[i] = v1;
            }
        }
    }
}

void spec_c_soa_move(SpecWorld* sw, const SpecParams* params) {
    for (u32 a = 0u; a < sw->arch_count; a++) {
        spec_c_soa_move_arch(&sw->archetypes[a], params);
    }
}

void spec_c_soa_gravity(SpecWorld* sw, const SpecParams* params) {
    for (u32 a = 0u; a < sw->arch_count; a++) {
        spec_c_soa_gravity_arch(&sw->archetypes[a], params);
    }
}

void spec_c_soa_bounce(SpecWorld* sw, const SpecParams* params) {
    for (u32 a = 0u; a < sw->arch_count; a++) {
        spec_c_soa_bounce_arch(&sw->archetypes[a], params);
    }
}

void spec_c_soa_step_all(SpecWorld* sw, const SpecParams* params) {
    spec_c_soa_move(sw, params);
    spec_c_soa_gravity(sw, params);
    spec_c_soa_bounce(sw, params);
}

void spec_c_soa_step_fused(SpecWorld* sw, const SpecParams* params) {
    for (u32 a = 0u; a < sw->arch_count; a++) {
        spec_c_soa_fused_arch(&sw->archetypes[a], params);
    }
}

/* ============================================================================
 * 6. (b) Özelleştirilmiş JIT Yürütücüleri
 * ============================================================================ */

b32 spec_jit_run_kernel(SpecWorld* sw, SpecCache* cache, SpecKernelKind kind,
                        const SpecParams* params, Arena* scratch) {
    if (!sw || !cache || !params || !scratch) return FALSE;
    for (u32 a = 0u; a < sw->arch_count; a++) {
        SpecArchetype* arch = &sw->archetypes[a];
        if (arch->layout.count == 0u) continue;
        SpecKey key = spec_make_key(kind, &arch->layout, params);
        const SpecVariant* var = spec_cache_get(cache, &key, scratch);
        if (!var || !var->fn) return FALSE;
        var->fn(arch->data, arch->layout.count);
    }
    return TRUE;
}

b32 spec_jit_step_3sys(SpecWorld* sw, SpecCache* cache, const SpecParams* params, Arena* scratch) {
    if (!spec_jit_run_kernel(sw, cache, SPEC_KERNEL_MOVE,    params, scratch)) return FALSE;
    if (!spec_jit_run_kernel(sw, cache, SPEC_KERNEL_GRAVITY, params, scratch)) return FALSE;
    if (!spec_jit_run_kernel(sw, cache, SPEC_KERNEL_BOUNCE,  params, scratch)) return FALSE;
    return TRUE;
}

b32 spec_jit_step_fused(SpecWorld* sw, SpecCache* cache, const SpecParams* params, Arena* scratch) {
    return spec_jit_run_kernel(sw, cache, SPEC_KERNEL_FUSED, params, scratch);
}

/* ============================================================================
 * 7. Doğrulama ve 100k Varlık Benchmark Koşucusu
 * ============================================================================ */

static void spec_rpt_putc(char c) {
    if (g_spec_report_len + 1u < SPEC_REPORT_CAP) {
        g_spec_report[g_spec_report_len++] = c;
        g_spec_report[g_spec_report_len] = '\0';
    }
}

static void spec_rpt_puts(const char* s) {
    if (!s) return;
    while (*s) {
        spec_rpt_putc(*s++);
    }
}

static void spec_rpt_pad(const char* s, u32 width) {
    u32 n = 0u;
    while (s && s[n]) {
        spec_rpt_putc(s[n]);
        n++;
    }
    while (n < width) {
        spec_rpt_putc(' ');
        n++;
    }
}

static void spec_rpt_u32(u32 v) {
    char tmp[16];
    u32 n = 0u;
    if (v == 0u) {
        spec_rpt_putc('0');
        return;
    }
    while (v > 0u) {
        tmp[n++] = (char)('0' + (v % 10u));
        v /= 10u;
    }
    while (n > 0u) {
        spec_rpt_putc(tmp[--n]);
    }
}

static void spec_rpt_f64(f64 val, u32 decimals, u32 pad_width) {
    char buf[32];
    u32 pos = 0u;
    if (val < 0.0) val = 0.0;
    f64 scale = 1.0;
    for (u32 d = 0u; d < decimals; d++) scale *= 10.0;
    u64 scaled = (u64)(val * scale + 0.5);
    u64 iscale = (u64)scale;
    u64 whole  = (iscale > 0u) ? (scaled / iscale) : scaled;
    u64 frac   = (iscale > 0u) ? (scaled % iscale) : 0u;

    char wtmp[20];
    u32 wn = 0u;
    if (whole == 0u) {
        wtmp[wn++] = '0';
    } else {
        while (whole > 0u) {
            wtmp[wn++] = (char)('0' + (whole % 10u));
            whole /= 10u;
        }
    }
    while (wn > 0u) {
        buf[pos++] = wtmp[--wn];
    }
    if (decimals > 0u) {
        buf[pos++] = '.';
        char ftmp[8];
        for (u32 d = 0u; d < decimals; d++) {
            ftmp[d] = (char)('0' + (frac % 10u));
            frac /= 10u;
        }
        for (u32 d = decimals; d > 0u; d--) {
            buf[pos++] = ftmp[d - 1u];
        }
    }
    buf[pos] = '\0';

    while (pos < pad_width) {
        spec_rpt_putc(' ');
        pos++;
    }
    spec_rpt_puts(buf);
}

/* EcsWorld'deki aktif Pos/Vel bileşenlerini SpecWorld SoA durumu ile bit-bit karşılaştırır */
static b32 spec_compare_ecs_and_soa(const EcsWorld* w, const SpecWorld* sw) {
    const EcsStore* sp = &w->stores[sw->cid_pos];
    const EcsStore* sv = &w->stores[sw->cid_vel];

    for (u32 a = 0u; a < sw->arch_count; a++) {
        const SpecArchetype* arch = &sw->archetypes[a];
        u32 n = arch->layout.count;
        const f32* px = (const f32*)(arch->data + arch->layout.off_p[0]);
        const f32* py = (const f32*)(arch->data + arch->layout.off_p[1]);
        const f32* pz = (const f32*)(arch->data + arch->layout.off_p[2]);
        const f32* vx = (const f32*)(arch->data + arch->layout.off_v[0]);
        const f32* vy = (const f32*)(arch->data + arch->layout.off_v[1]);
        const f32* vz = (const f32*)(arch->data + arch->layout.off_v[2]);

        for (u32 i = 0u; i < n; i++) {
            u32 idx = ecs_entity_index(arch->entities[i]);
            const SpecPos* p = (const SpecPos*)(sp->data + (usize)sp->sparse[idx] * sp->stride);
            const SpecVel* v = (const SpecVel*)(sv->data + (usize)sv->sparse[idx] * sv->stride);
            if (!spec_f32_bit_eq(p->x, px[i])  || !spec_f32_bit_eq(p->y, py[i])  || !spec_f32_bit_eq(p->z, pz[i]) ||
                !spec_f32_bit_eq(v->vx, vx[i]) || !spec_f32_bit_eq(v->vy, vy[i]) || !spec_f32_bit_eq(v->vz, vz[i])) {
                return FALSE;
            }
        }
    }
    return TRUE;
}

static void spec_populate_test_world(EcsWorld* w, u32 n_active, u32 n_pos_only,
                                      EcsCompId* out_pos, EcsCompId* out_vel) {
    EcsCompId cid_pos  = ecs_register(w, "Pos",  sizeof(SpecPos), _Alignof(SpecPos), NULL);
    EcsCompId cid_vel  = ecs_register(w, "Vel",  sizeof(SpecVel), _Alignof(SpecVel), NULL);
    EcsCompId cid_tag1 = ecs_register(w, "TagA", sizeof(u32),     _Alignof(u32),     NULL);
    EcsCompId cid_tag2 = ecs_register(w, "TagB", sizeof(u32),     _Alignof(u32),     NULL);

    *out_pos = cid_pos;
    *out_vel = cid_vel;

    Rng rng;
    rng_seed(&rng, 0xF0B20261029ULL);

    /* 3 farklı arketip (%50 Pos|Vel, %30 Pos|Vel|TagA, %20 Pos|Vel|TagB) + sınır taşan başlangıç konumları */
    u32 n0 = (n_active * 5u) / 10u;
    u32 n1 = (n_active * 3u) / 10u;

    for (u32 i = 0u; i < n_active; i++) {
        EcsEntity e = ecs_spawn(w);
        SpecPos p;
        SpecVel v;
        /* Bazı varlıklar sınır kutusu ([-50,50], [-25,25], [-50,50]) dışında başlasın ki yansıma hemen tetiklensin */
        p.x  = (rng_f32(&rng) * 2.0f - 1.0f) * 65.0f;
        p.y  = (rng_f32(&rng) * 2.0f - 1.0f) * 35.0f;
        p.z  = (rng_f32(&rng) * 2.0f - 1.0f) * 65.0f;
        v.vx = (rng_f32(&rng) * 2.0f - 1.0f) * 40.0f;
        v.vy = (rng_f32(&rng) * 2.0f - 1.0f) * 40.0f;
        v.vz = (rng_f32(&rng) * 2.0f - 1.0f) * 40.0f;
        if (i == 0u) {
            p.x = -0.0f;
            p.y =  0.0f;
        }
        ecs_set(w, e, cid_pos, &p);
        ecs_set(w, e, cid_vel, &v);

        u32 tag = i;
        if (i >= n0 && i < n0 + n1) {
            ecs_set(w, e, cid_tag1, &tag);
        } else if (i >= n0 + n1) {
            ecs_set(w, e, cid_tag2, &tag);
            ecs_set(w, e, cid_tag1, &tag);
        }
    }

    /* Sorguya girmeyen yalnız-Pos varlıkları (ecs_query filtrelemesini gerçekçi kılar) */
    for (u32 i = 0u; i < n_pos_only; i++) {
        EcsEntity e = ecs_spawn(w);
        SpecPos p = { (f32)i, 0.0f, 0.0f };
        ecs_set(w, e, cid_pos, &p);
    }
}

i32 spec_bench_verify(Arena* arena) {
    if (!arena) return -1;
    usize mark = arena_mark(arena);

    SpecCache cache;
    spec_cache_init(&cache, arena);
    SpecParams params = spec_default_params();

    /* 1. Farklı varlık sayıları (16x unroll, 4x SIMD ve skaler kuyruk sınırları) */
    static const u32 k_counts[] = { 1u, 3u, 4u, 7u, 15u, 16u, 17u, 19u, 67u, 523u };
    for (u32 c = 0u; c < ARRAY_LEN(k_counts); c++) {
        usize iter_mark = arena_mark(arena);
        u32 n = k_counts[c];

        EcsWorld w_ecs;
        ecs_world_init(&w_ecs, arena, n + 32u);
        EcsCompId cid_pos = 0u, cid_vel = 0u;
        spec_populate_test_world(&w_ecs, n, 4u, &cid_pos, &cid_vel);

        SpecWorld sw_3sys;
        SpecWorld sw_fused;
        SpecWorld sw_c_soa;
        spec_world_build(&sw_3sys,  &w_ecs, cid_pos, cid_vel, arena);
        spec_world_build(&sw_fused, &w_ecs, cid_pos, cid_vel, arena);
        spec_world_build(&sw_c_soa, &w_ecs, cid_pos, cid_vel, arena);

        /* 5 ardışık simülasyon adımı boyunca 4 yolun da bit-bit aynı kaldığını doğrula */
        for (u32 step = 0u; step < 5u; step++) {
            spec_ecs_step_all(&w_ecs, cid_pos, cid_vel, &params);
            spec_c_soa_step_all(&sw_c_soa, &params);
            if (!spec_jit_step_3sys(&sw_3sys, &cache, &params, arena)) {
                arena_pop_to(arena, mark);
                return -10 - (i32)c;
            }
            if (!spec_jit_step_fused(&sw_fused, &cache, &params, arena)) {
                arena_pop_to(arena, mark);
                return -20 - (i32)c;
            }

            if (!spec_compare_ecs_and_soa(&w_ecs, &sw_c_soa) ||
                !spec_compare_ecs_and_soa(&w_ecs, &sw_3sys)  ||
                !spec_compare_ecs_and_soa(&w_ecs, &sw_fused)) {
                arena_pop_to(arena, mark);
                return -30 - (i32)c;
            }
        }

        arena_pop_to(arena, iter_mark);
    }

    /* 2. Sabit değiştiğinde yeniden özelleştirme (cache miss -> yeni varyant) */
    u32 prev_misses = cache.misses;
    SpecParams params2 = params;
    params2.dt = 1.0f / 120.0f;
    SpecArchLayout dummy_layout;
    spec_arch_layout_init(&dummy_layout, 3u, 64u, 0u);
    SpecKey k2 = spec_make_key(SPEC_KERNEL_FUSED, &dummy_layout, &params2);
    const SpecVariant* v2 = spec_cache_get(&cache, &k2, arena);
    if (!v2 || !v2->fn || cache.misses != prev_misses + 1u) {
        arena_pop_to(arena, mark);
        return -50;
    }

    arena_pop_to(arena, mark);
    return 0;
}

typedef struct {
    EcsWorld*         w;
    SpecWorld*        sw;
    SpecCache*        cache;
    const SpecParams* params;
    Arena*            scratch;
    EcsCompId         cid_pos;
    EcsCompId         cid_vel;
} SpecBenchCtx;

static void bench_ecs_3sys(void* ctx, u32 iters) {
    SpecBenchCtx* b = (SpecBenchCtx*)ctx;
    for (u32 i = 0u; i < iters; i++) {
        spec_ecs_step_all(b->w, b->cid_pos, b->cid_vel, b->params);
    }
}

static void bench_c_soa_3sys(void* ctx, u32 iters) {
    SpecBenchCtx* b = (SpecBenchCtx*)ctx;
    for (u32 i = 0u; i < iters; i++) {
        spec_c_soa_step_all(b->sw, b->params);
    }
}

static void bench_c_soa_fused(void* ctx, u32 iters) {
    SpecBenchCtx* b = (SpecBenchCtx*)ctx;
    for (u32 i = 0u; i < iters; i++) {
        spec_c_soa_step_fused(b->sw, b->params);
    }
}

static void bench_jit_3sys(void* ctx, u32 iters) {
    SpecBenchCtx* b = (SpecBenchCtx*)ctx;
    for (u32 i = 0u; i < iters; i++) {
        spec_jit_step_3sys(b->sw, b->cache, b->params, b->scratch);
    }
}

static void bench_jit_fused(void* ctx, u32 iters) {
    SpecBenchCtx* b = (SpecBenchCtx*)ctx;
    for (u32 i = 0u; i < iters; i++) {
        spec_jit_step_fused(b->sw, b->cache, b->params, b->scratch);
    }
}

static void bench_ecs_move(void* ctx, u32 iters) {
    SpecBenchCtx* b = (SpecBenchCtx*)ctx;
    for (u32 i = 0u; i < iters; i++) {
        spec_ecs_step_move(b->w, b->cid_pos, b->cid_vel, b->params);
    }
}

static void bench_jit_move(void* ctx, u32 iters) {
    SpecBenchCtx* b = (SpecBenchCtx*)ctx;
    for (u32 i = 0u; i < iters; i++) {
        spec_jit_run_kernel(b->sw, b->cache, SPEC_KERNEL_MOVE, b->params, b->scratch);
    }
}

static void bench_ecs_gravity(void* ctx, u32 iters) {
    SpecBenchCtx* b = (SpecBenchCtx*)ctx;
    for (u32 i = 0u; i < iters; i++) {
        spec_ecs_step_gravity(b->w, b->cid_pos, b->cid_vel, b->params);
    }
}

static void bench_jit_gravity(void* ctx, u32 iters) {
    SpecBenchCtx* b = (SpecBenchCtx*)ctx;
    for (u32 i = 0u; i < iters; i++) {
        spec_jit_run_kernel(b->sw, b->cache, SPEC_KERNEL_GRAVITY, b->params, b->scratch);
    }
}

static void bench_ecs_bounce(void* ctx, u32 iters) {
    SpecBenchCtx* b = (SpecBenchCtx*)ctx;
    for (u32 i = 0u; i < iters; i++) {
        spec_ecs_step_bounce(b->w, b->cid_pos, b->cid_vel, b->params);
    }
}

static void bench_c_soa_bounce(void* ctx, u32 iters) {
    SpecBenchCtx* b = (SpecBenchCtx*)ctx;
    for (u32 i = 0u; i < iters; i++) {
        spec_c_soa_bounce(b->sw, b->params);
    }
}

static void bench_jit_bounce(void* ctx, u32 iters) {
    SpecBenchCtx* b = (SpecBenchCtx*)ctx;
    for (u32 i = 0u; i < iters; i++) {
        spec_jit_run_kernel(b->sw, b->cache, SPEC_KERNEL_BOUNCE, b->params, b->scratch);
    }
}

typedef void (*SpecTimedFn)(void* ctx, u32 iters);

static void spec_measure_fn(SpecTimedFn fn, void* ctx, u32 warmup, u32 iters, u32 runs,
                            f64* out_med_ms, f64* out_min_ms) {
    f64 samples[31];
    if (runs == 0u) runs = 1u;
    if (runs > 31u) runs = 31u;

    if (warmup > 0u) {
        fn(ctx, warmup);
    }
    for (u32 r = 0u; r < runs; r++) {
        f64 t0 = plat_time_ms();
        fn(ctx, iters);
        f64 dt = (plat_time_ms() - t0) / (f64)iters;
        if (dt < 0.0) dt = 0.0;
        samples[r] = dt;
    }
    for (u32 i = 1u; i < runs; i++) {
        f64 key = samples[i];
        u32 j = i;
        while (j > 0u && samples[j - 1u] > key) {
            samples[j] = samples[j - 1u];
            j--;
        }
        samples[j] = key;
    }
    *out_min_ms = samples[0];
    *out_med_ms = samples[runs / 2u];
}

typedef struct {
    const char* name;
    b32         bit_exact;
    f64         base_med_ms;
    f64         base_min_ms;
    f64         spec_med_ms;
    f64         spec_min_ms;
    f64         ratio;
} SpecRow;

i32 spec_bench_run(Arena* arena, u32 entity_count) {
    if (!arena) return -1;
    if (entity_count == 0u) entity_count = 100000u;
    if (entity_count > 200000u) entity_count = 200000u;

    g_spec_report_len = 0u;
    g_spec_report[0]  = '\0';

    i32 vrc = spec_bench_verify(arena);
    if (vrc != 0) {
        LOG("[spec] HATA: spec_bench_verify basarisiz (rc=%d)", vrc);
        return vrc;
    }

    usize mark = arena_mark(arena);

    u32 n_pos_only = entity_count / 20u;
    EcsWorld* w = ecs_world_create(arena, entity_count + n_pos_only + 64u);
    EcsCompId cid_pos = 0u, cid_vel = 0u;
    spec_populate_test_world(w, entity_count, n_pos_only, &cid_pos, &cid_vel);

    SpecWorld* sw_jit3  = ARENA_PUSH_STRUCT(arena, SpecWorld);
    SpecWorld* sw_fused = ARENA_PUSH_STRUCT(arena, SpecWorld);
    SpecWorld* sw_csoa  = ARENA_PUSH_STRUCT(arena, SpecWorld);
    spec_world_build(sw_jit3,  w, cid_pos, cid_vel, arena);
    spec_world_build(sw_fused, w, cid_pos, cid_vel, arena);
    spec_world_build(sw_csoa,  w, cid_pos, cid_vel, arena);

    SpecCache* cache = ARENA_PUSH_STRUCT(arena, SpecCache);
    spec_cache_init(cache, arena);
    SpecParams params = spec_default_params();

    /* Tüm arketipler (3 arketip) ve 4 çekirdek türü için JIT derlemesini gerçekleştir ve süreyi ölç */
    f64 compile_total_ms = 0.0;
    u32 total_wasm_bytes = 0u;
    for (u32 a = 0u; a < sw_jit3->arch_count; a++) {
        const SpecArchLayout* lay = &sw_jit3->archetypes[a].layout;
        for (u32 k = 0u; k < (u32)SPEC_KERNEL_COUNT; k++) {
            SpecKey key = spec_make_key((SpecKernelKind)k, lay, &params);
            const SpecVariant* var = spec_cache_get(cache, &key, arena);
            if (!var || !var->fn) {
                arena_pop_to(arena, mark);
                return -100;
            }
            compile_total_ms += var->compile_ms;
            total_wasm_bytes += var->wasm_len;
        }
    }

    /* 100k varlık üzerinde bit-bit doğrulama (ecs_query vs C SoA vs JIT 3-sys vs JIT Fused) */
    spec_ecs_step_all(w, cid_pos, cid_vel, &params);
    spec_c_soa_step_all(sw_csoa, &params);
    spec_jit_step_3sys(sw_jit3, cache, &params, arena);
    spec_jit_step_fused(sw_fused, cache, &params, arena);

    b32 ok_3sys  = spec_compare_ecs_and_soa(w, sw_jit3);
    b32 ok_fused = spec_compare_ecs_and_soa(w, sw_fused);
    b32 ok_csoa  = spec_compare_ecs_and_soa(w, sw_csoa);
    b32 all_ok   = ok_3sys && ok_fused && ok_csoa;

    SpecBenchCtx ctx_ecs   = { w, sw_jit3,  cache, &params, arena, cid_pos, cid_vel };
    SpecBenchCtx ctx_csoa  = { w, sw_csoa,  cache, &params, arena, cid_pos, cid_vel };
    SpecBenchCtx ctx_jit3  = { w, sw_jit3,  cache, &params, arena, cid_pos, cid_vel };
    SpecBenchCtx ctx_fused = { w, sw_fused, cache, &params, arena, cid_pos, cid_vel };

    const u32 warmup = 5u;
    const u32 iters  = 4u;
    const u32 runs   = 15u;

    f64 ecs_all_med = 0.0,   ecs_all_min = 0.0;
    f64 csoa_all_med = 0.0,  csoa_all_min = 0.0;
    f64 csoa_fus_med = 0.0,  csoa_fus_min = 0.0;
    f64 jit3_all_med = 0.0,  jit3_all_min = 0.0;
    f64 jitf_all_med = 0.0,  jitf_all_min = 0.0;

    f64 ecs_mov_med = 0.0,   ecs_mov_min = 0.0;
    f64 jit_mov_med = 0.0,   jit_mov_min = 0.0;
    f64 ecs_grv_med = 0.0,   ecs_grv_min = 0.0;
    f64 jit_grv_med = 0.0,   jit_grv_min = 0.0;
    f64 ecs_bnc_med = 0.0,   ecs_bnc_min = 0.0;
    f64 csoa_bnc_med = 0.0,  csoa_bnc_min = 0.0;
    f64 jit_bnc_med = 0.0,   jit_bnc_min = 0.0;

    spec_measure_fn(bench_ecs_3sys,     &ctx_ecs,   warmup, iters, runs, &ecs_all_med,  &ecs_all_min);
    spec_measure_fn(bench_c_soa_3sys,   &ctx_csoa,  warmup, iters, runs, &csoa_all_med, &csoa_all_min);
    spec_measure_fn(bench_c_soa_fused,  &ctx_csoa,  warmup, iters, runs, &csoa_fus_med, &csoa_fus_min);
    spec_measure_fn(bench_jit_3sys,     &ctx_jit3,  warmup, iters, runs, &jit3_all_med, &jit3_all_min);
    spec_measure_fn(bench_jit_fused,    &ctx_fused, warmup, iters, runs, &jitf_all_med, &jitf_all_min);

    spec_measure_fn(bench_ecs_move,     &ctx_ecs,   warmup, iters, runs, &ecs_mov_med,  &ecs_mov_min);
    spec_measure_fn(bench_jit_move,     &ctx_jit3,  warmup, iters, runs, &jit_mov_med,  &jit_mov_min);
    spec_measure_fn(bench_ecs_gravity,  &ctx_ecs,   warmup, iters, runs, &ecs_grv_med,  &ecs_grv_min);
    spec_measure_fn(bench_jit_gravity,  &ctx_jit3,  warmup, iters, runs, &jit_grv_med,  &jit_grv_min);
    spec_measure_fn(bench_ecs_bounce,   &ctx_ecs,   warmup, iters, runs, &ecs_bnc_med,  &ecs_bnc_min);
    spec_measure_fn(bench_c_soa_bounce, &ctx_csoa,  warmup, iters, runs, &csoa_bnc_med, &csoa_bnc_min);
    spec_measure_fn(bench_jit_bounce,   &ctx_jit3,  warmup, iters, runs, &jit_bnc_med,  &jit_bnc_min);

    SpecRow rows[9] = {
        { "3 sistem: ecs_query vs JIT (3 gecis)",     ok_3sys,  ecs_all_med,  ecs_all_min,  jit3_all_med, jit3_all_min, 0.0 },
        { "3 sistem: ecs_query vs JIT (birlesik)",    ok_fused, ecs_all_med,  ecs_all_min,  jitf_all_med, jitf_all_min, 0.0 },
        { "3 sistem: C-SoA(3) vs JIT (3 gecis)",      ok_3sys,  csoa_all_med, csoa_all_min, jit3_all_med, jit3_all_min, 0.0 },
        { "3 sistem: C-SoA(3) vs JIT (birlesik)",     ok_fused, csoa_all_med, csoa_all_min, jitf_all_med, jitf_all_min, 0.0 },
        { "3 sistem: C-SoA(birlesik) vs JIT(birl.)",  ok_fused, csoa_fus_med, csoa_fus_min, jitf_all_med, jitf_all_min, 0.0 },
        { "Sistem 1: hareket (ecs_query vs JIT)",     ok_3sys,  ecs_mov_med,  ecs_mov_min,  jit_mov_med,  jit_mov_min,  0.0 },
        { "Sistem 2: yercekimi (ecs_query vs JIT)",   ok_3sys,  ecs_grv_med,  ecs_grv_min,  jit_grv_med,  jit_grv_min,  0.0 },
        { "Sistem 3: yansima (ecs_query vs JIT)",     ok_3sys,  ecs_bnc_med,  ecs_bnc_min,  jit_bnc_med,  jit_bnc_min,  0.0 },
        { "Sistem 3: yansima (C-SoA vs JIT)",         ok_3sys,  csoa_bnc_med, csoa_bnc_min, jit_bnc_med,  jit_bnc_min,  0.0 }
    };

    for (u32 i = 0u; i < 9u; i++) {
        rows[i].ratio = (rows[i].spec_med_ms > 1e-6) ? (rows[i].base_med_ms / rows[i].spec_med_ms) : 1.0;
    }

    spec_rpt_puts("=== MATOKI F0_B (H1): Arketip Basina JIT ECS Ozellestirme Benchmark ===\n");
    spec_rpt_puts("varlik: ");
    spec_rpt_u32(entity_count);
    spec_rpt_puts(" (3 arketip: 50k/30k/20k + 5k filtre) | derleme: ");
    spec_rpt_f64(compile_total_ms, 3u, 0u);
    spec_rpt_puts(" ms (12 modul, ");
    spec_rpt_u32(total_wasm_bytes);
    spec_rpt_puts(" B wasm)\n\n");

    spec_rpt_puts("karsilastirma                          | bit-bit | genel med(min) ms | JIT med(min) ms   | hizlanma | H1 (>=2x)\n");
    spec_rpt_puts("---------------------------------------+---------+-------------------+-------------------+----------+----------\n");

    LOG("=== MATOKI F0_B (H1): %u varlik, 3 arketip, 12 JIT modul derleme=%.3f ms (%u B) ===",
        entity_count, compile_total_ms, total_wasm_bytes);

    for (u32 i = 0u; i < 9u; i++) {
        u32 line_start = g_spec_report_len;
        spec_rpt_pad(rows[i].name, 38u);
        spec_rpt_puts(" | ");
        spec_rpt_puts(rows[i].bit_exact ? "  EVET " : " HAYIR!");
        spec_rpt_puts(" | ");
        spec_rpt_f64(rows[i].base_med_ms, 3u, 7u);
        spec_rpt_puts(" (");
        spec_rpt_f64(rows[i].base_min_ms, 3u, 6u);
        spec_rpt_puts(") | ");
        spec_rpt_f64(rows[i].spec_med_ms, 3u, 7u);
        spec_rpt_puts(" (");
        spec_rpt_f64(rows[i].spec_min_ms, 3u, 6u);
        spec_rpt_puts(") | ");
        spec_rpt_f64(rows[i].ratio, 2u, 6u);
        spec_rpt_puts("x  | ");
        spec_rpt_puts(rows[i].ratio >= 2.0 ? "GECTI" : "ANALIZ");

        LOG("%s", g_spec_report + line_start);
        spec_rpt_putc('\n');
    }

    spec_rpt_puts("\n#TSV\n");
    spec_rpt_puts("#META\t");
    spec_rpt_u32(entity_count);
    spec_rpt_putc('\t');
    spec_rpt_u32(sw_jit3->arch_count);
    spec_rpt_putc('\t');
    spec_rpt_f64(compile_total_ms, 3u, 0u);
    spec_rpt_putc('\t');
    spec_rpt_u32(total_wasm_bytes);
    spec_rpt_putc('\n');

    for (u32 i = 0u; i < 9u; i++) {
        spec_rpt_puts(rows[i].name);
        spec_rpt_putc('\t');
        spec_rpt_u32(rows[i].bit_exact ? 1u : 0u);
        spec_rpt_putc('\t');
        spec_rpt_f64(rows[i].base_med_ms, 3u, 0u);
        spec_rpt_putc('\t');
        spec_rpt_f64(rows[i].base_min_ms, 3u, 0u);
        spec_rpt_putc('\t');
        spec_rpt_f64(rows[i].spec_med_ms, 3u, 0u);
        spec_rpt_putc('\t');
        spec_rpt_f64(rows[i].spec_min_ms, 3u, 0u);
        spec_rpt_putc('\t');
        spec_rpt_f64(rows[i].ratio, 2u, 0u);
        spec_rpt_putc('\t');
        spec_rpt_puts(rows[i].ratio >= 2.0 ? "GEÇTİ (≥2x)" : "ANALİZ (<2x)");
        spec_rpt_putc('\n');
    }

    arena_pop_to(arena, mark);
    return all_ok ? 0 : -200;
}

const char* spec_bench_report_ptr(void) {
    return g_spec_report;
}

u32 spec_bench_report_len(void) {
    return g_spec_report_len;
}
