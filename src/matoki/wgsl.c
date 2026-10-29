/* matoki/wgsl.c — Matoki IR → WebGPU Shading Language (WGSL) kod üreticisi (D-108).
 * Optimizasyon sonrası IrFunc'u girdi alır; skaler SSA komutlarını piksel başına çalışan
 * WGSL fragment shader metnine çevirir. Ayrıca gfx3d varying sözleşmesine (G3dAttr) uyan
 * WGSL vertex shader üreticisini (mtk_wgsl_vert) ve üst seviye mtk_compile_wgsl'i içerir.
 */
#include "matoki/wgsl.h"
#include "core/log.h"
#include "raster/shade_abi.h"
#include "gfx3d/pipeline.h"

_Static_assert((u32)MTK_WGSL_ATTR_UV         == (u32)G3D_ATTR_UV, "wgsl attr uv");
_Static_assert((u32)MTK_WGSL_ATTR_NORMAL     == (u32)G3D_ATTR_NORMAL, "wgsl attr normal");
_Static_assert((u32)MTK_WGSL_ATTR_VIEW_DEPTH == (u32)G3D_ATTR_VIEW_DEPTH, "wgsl attr view_depth");
_Static_assert((u32)MTK_WGSL_ATTR_COLOR      == (u32)G3D_ATTR_COLOR, "wgsl attr color");
_Static_assert((u32)MTK_WGSL_ATTR_WORLD_POS  == (u32)G3D_ATTR_WORLD_POS, "wgsl attr world_pos");
_Static_assert((u32)MTK_WGSL_ATTR_SHADOW_POS == (u32)G3D_ATTR_SHADOW_POS, "wgsl attr shadow_pos");
_Static_assert(MTK_WGSL_MAX_ATTRS == G3D_MAX_ATTRS, "wgsl max attrs");

#define WGSL_MAX_INSTS 4096u
#define WGSL_MAX_IF    64u

typedef struct {
    char* buf;
    u32   cap;
    u32   len;
    b32   overflow;
} WgslBuf;

static void wb_init(WgslBuf* b, char* out, u32 out_cap) {
    b->buf      = out;
    b->cap      = out_cap;
    b->len      = 0;
    b->overflow = FALSE;
    if (out && out_cap > 0) {
        out[0] = '\0';
    } else {
        b->overflow = TRUE;
    }
}

static void wb_putc(WgslBuf* b, char c) {
    if (b->overflow) return;
    if (b->len + 1u < b->cap) {
        b->buf[b->len++] = c;
        b->buf[b->len]   = '\0';
    } else {
        b->overflow = TRUE;
    }
}

static void wb_puts(WgslBuf* b, const char* s) {
    if (!s || b->overflow) return;
    while (*s) {
        wb_putc(b, *s++);
        if (b->overflow) return;
    }
}

static void wb_putf(WgslBuf* b, const char* fmt, ...) {
    if (b->overflow) return;
    char tmp[512];
    VaList ap;
    VA_START(ap, fmt);
    fmt_vformat(tmp, sizeof(tmp), fmt, ap);
    VA_END(ap);
    wb_puts(b, tmp);
}

static void wb_indent(WgslBuf* b, u32 depth) {
    for (u32 i = 0; i < depth; i++) {
        wb_puts(b, "    ");
    }
}

static inline u32 wgsl_f32_bits(f32 x) {
    u32 u;
    __builtin_memcpy(&u, &x, 4);
    return u;
}

static void wb_f32(WgslBuf* b, f32 v) {
    u32 bits = wgsl_f32_bits(v);
    u32 exp_mask = bits & 0x7F800000u;
    if (exp_mask == 0x7F800000u) {
        wb_putf(b, "bitcast<f32>(0x%08xu)", bits);
        return;
    }
    if (bits == 0x00000000u) {
        wb_puts(b, "0.0");
        return;
    }
    if (bits == 0x80000000u) {
        wb_puts(b, "(-0.0)");
        return;
    }

    b32 neg = (bits & 0x80000000u) != 0;
    f64 d = neg ? -(f64)v : (f64)v;
    if (neg) wb_puts(b, "(-");

    i32 exp10 = 0;
    while (d >= 10.0) { d *= 0.1; exp10++; }
    while (d > 0.0 && d < 1.0) { d *= 10.0; exp10--; }

    u64 mant = (u64)(d * 10000000.0 + 0.5);
    if (mant >= 100000000ULL) {
        mant /= 10ULL;
        exp10++;
    }

    char dig[8];
    u64 tmp = mant;
    for (i32 k = 7; k >= 0; k--) {
        dig[k] = (char)('0' + (tmp % 10ULL));
        tmp /= 10ULL;
    }
    i32 sig = 8;
    while (sig > 1 && dig[sig - 1] == '0') sig--;

    if (exp10 >= 0 && exp10 <= 5) {
        for (i32 k = 0; k <= exp10; k++) {
            wb_putc(b, k < sig ? dig[k] : '0');
        }
        wb_putc(b, '.');
        if (sig > exp10 + 1) {
            for (i32 k = exp10 + 1; k < sig; k++) wb_putc(b, dig[k]);
        } else {
            wb_putc(b, '0');
        }
    } else if (exp10 >= -3 && exp10 < 0) {
        wb_puts(b, "0.");
        for (i32 z = 0; z < -exp10 - 1; z++) wb_putc(b, '0');
        for (i32 k = 0; k < sig; k++) wb_putc(b, dig[k]);
    } else {
        wb_putc(b, dig[0]);
        wb_putc(b, '.');
        if (sig > 1) {
            for (i32 k = 1; k < sig; k++) wb_putc(b, dig[k]);
        } else {
            wb_putc(b, '0');
        }
        if (exp10 < 0) {
            wb_putf(b, "e-%u", (u32)(-exp10));
        } else {
            wb_putf(b, "e+%u", (u32)exp10);
        }
    }

    if (neg) wb_putc(b, ')');
}

static void wb_i32(WgslBuf* b, i32 v) {
    if (v == I32_MIN) {
        wb_puts(b, "bitcast<i32>(0x80000000u)");
    } else if (v < 0) {
        u32 mag = (u32)(-(i64)v);
        wb_putf(b, "(-%u)", mag);
    } else {
        wb_putf(b, "%u", (u32)v);
    }
}

static b32 wgsl_fail(char* err, u32 err_cap, const char* fmt, ...) {
    if (err && err_cap > 0) {
        VaList ap;
        VA_START(ap, fmt);
        fmt_vformat(err, err_cap, fmt, ap);
        VA_END(ap);
    }
    return FALSE;
}

static const char* ir_type_wgsl_name(IrType t) {
    switch (t) {
    case IR_T_F32:  return "f32";
    case IR_T_I32:  return "i32";
    case IR_T_BOOL: return "bool";
    default:        return "void";
    }
}

static const char* ir_type_wgsl_zero(IrType t) {
    switch (t) {
    case IR_T_F32:  return "0.0";
    case IR_T_I32:  return "0";
    case IR_T_BOOL: return "false";
    default:        return "0";
    }
}

static const char* comps_wgsl_f32_type(u32 comps) {
    switch (comps) {
    case 1: return "f32";
    case 2: return "vec2<f32>";
    case 3: return "vec3<f32>";
    case 4: return "vec4<f32>";
    default: return "f32";
    }
}

static b32 is_const_inst(const IrInst* in) {
    return in->op == IR_CONST_F32 || in->op == IR_CONST_I32 || in->op == IR_CONST_BOOL;
}

/* ---------- Doku koordinatı örüntü eşleme (ir_emit_sample -> textureLoad) ---------- */

static b32 is_tex_w_for_slot(const IrFunc* f, u32 id, u32 slot, u32* out_level) {
    const IrInst* in = &f->insts[id];
    if (in->op == IR_TEX_W && in->imm.u == slot) {
        *out_level = IR_NONE;
        return TRUE;
    }
    if (in->op == IR_ISHR_S && in->a < id) {
        const IrInst* ia = &f->insts[in->a];
        if (ia->op == IR_TEX_W && ia->imm.u == slot) {
            *out_level = in->b;
            return TRUE;
        }
    }
    return FALSE;
}

static b32 is_row_mul(const IrFunc* f, u32 id, u32 slot, u32* out_wy, u32* out_level) {
    const IrInst* in = &f->insts[id];
    if (in->op != IR_IMUL) return FALSE;
    if (is_tex_w_for_slot(f, in->a, slot, out_level)) {
        *out_wy = in->b;
        return TRUE;
    }
    if (is_tex_w_for_slot(f, in->b, slot, out_level)) {
        *out_wy = in->a;
        return TRUE;
    }
    return FALSE;
}

static b32 is_mip_base_inst(const IrFunc* f, u32 id) {
    const IrInst* in = &f->insts[id];
    if (in->op != IR_IMUL) return FALSE;
    const IrInst* ia = &f->insts[in->a];
    const IrInst* ib = &f->insts[in->b];
    if (ia->op == IR_CONST_I32 && (u32)ia->imm.i == 0xAAAAAAABu) return TRUE;
    if (ib->op == IR_CONST_I32 && (u32)ib->imm.i == 0xAAAAAAABu) return TRUE;
    return FALSE;
}

static b32 is_row_expr(const IrFunc* f, u32 id, u32 slot, u32* out_wy, u32* out_level) {
    if (is_row_mul(f, id, slot, out_wy, out_level)) {
        return TRUE;
    }
    const IrInst* in = &f->insts[id];
    if (in->op == IR_IADD) {
        if (is_mip_base_inst(f, in->a) && is_row_mul(f, in->b, slot, out_wy, out_level)) {
            return TRUE;
        }
        if (is_mip_base_inst(f, in->b) && is_row_mul(f, in->a, slot, out_wy, out_level)) {
            return TRUE;
        }
    }
    return FALSE;
}

static b32 match_texel_coords(const IrFunc* f, u32 slot, u32 idx_id,
                              u32* out_wx, u32* out_wy, u32* out_level) {
    const IrInst* idx = &f->insts[idx_id];
    if (idx->op != IR_IADD) return FALSE;
    if (is_row_expr(f, idx->a, slot, out_wy, out_level)) {
        *out_wx = idx->b;
        return TRUE;
    }
    if (is_row_expr(f, idx->b, slot, out_wy, out_level)) {
        *out_wx = idx->a;
        return TRUE;
    }
    return FALSE;
}

static b32 region_dominates(u32 r_def, u32 r_use, const u32* reg_parent) {
    u32 cur = r_use;
    while (cur != r_def) {
        if (cur == 0) return FALSE;
        cur = reg_parent[cur];
    }
    return TRUE;
}

static void emit_operand(WgslBuf* b, const IrFunc* f, u32 id) {
    const IrInst* in = &f->insts[id];
    if (in->op == IR_CONST_F32) {
        wb_f32(b, in->imm.f);
    } else if (in->op == IR_CONST_I32) {
        wb_i32(b, in->imm.i);
    } else if (in->op == IR_CONST_BOOL) {
        wb_puts(b, in->imm.i ? "true" : "false");
    } else {
        wb_putf(b, "t%u", id);
    }
}

typedef struct {
    b32 matched;
    u32 wx;
    u32 wy;
    u32 level;
} TexLoadMatch;

typedef struct {
    TexLoadMatch tex_match[WGSL_MAX_INSTS];
    b32          live[WGSL_MAX_INSTS];
    u32          enclosing_if[WGSL_MAX_INSTS];
    u32          inst_reg[WGSL_MAX_INSTS];
    u32          reg_parent[WGSL_MAX_INSTS];
    b32          needs_func_var[WGSL_MAX_INSTS];
    b32          flatten_if[WGSL_MAX_INSTS];
    char         var_expr[SHADE_MAX_VARYINGS][48];
    b32          var_defined[SHADE_MAX_VARYINGS];
} WgslFragCtx;

/* Büyük yığın çerçevesinden kaçınmak için statik çalışma alanı (tek iş parçacıklı derleme) */
static WgslFragCtx g_wgsl_ctx;

u32 mtk_wgsl_frag(const IrFunc* f, const MtkLayout* layout, const MtkWgslOpts* opts,
                  char* out, u32 out_cap, char* err, u32 err_cap) {
    if (f && f->is_compute) {
        return mtk_wgsl_compute(f, layout, opts, out, out_cap, err, err_cap);
    }
    if (err && err_cap > 0) err[0] = '\0';
    if (!out || out_cap == 0) {
        wgsl_fail(err, err_cap, "cikti tamponu gecersiz");
        return 0;
    }
    out[0] = '\0';

    if (!f || f->count == 0 || !f->insts) {
        wgsl_fail(err, err_cap, "bos veya gecersiz IrFunc");
        return 0;
    }
    if (f->count > WGSL_MAX_INSTS) {
        wgsl_fail(err, err_cap, "IrFunc komut sayisi siniri asildi (%u > %u)", f->count, WGSL_MAX_INSTS);
        return 0;
    }

    /* 1. Tam IR doğrulaması (tüm komutlar, tipler, IF dengesi ve 4 OUTPUT kanalı) */
    if (!ir_verify(f, err, err_cap)) {
        return 0;
    }

    WgslFragCtx* ctx = &g_wgsl_ctx;
    __builtin_memset(ctx, 0, sizeof(*ctx));

    u32 u_bytes   = f->uniform_bytes;
    u32 nvar      = f->varying_count;
    u32 tex_count = f->texture_count;

    if (layout) {
        if (layout->uniform_bytes > u_bytes) u_bytes = layout->uniform_bytes;
        if (layout->varying_count > nvar)    nvar    = layout->varying_count;
        if (layout->texture_count > tex_count) tex_count = layout->texture_count;
    }

    /* 2. Ek sınır, hizalama ve desteklenen opcode denetimi */
    for (u32 i = 0; i < f->count; i++) {
        const IrInst* in = &f->insts[i];
        IrOp op = (IrOp)in->op;
        if (op == IR_NOP) continue;

        switch (op) {
        case IR_UNIFORM_F32:
        case IR_UNIFORM_I32:
            if ((in->imm.u & 3u) != 0 || in->imm.u >= 65536u) {
                wgsl_fail(err, err_cap, "inst %%%u: hizasiz veya gecersiz uniform ofseti %u", i, in->imm.u);
                return 0;
            }
            if (f->uniform_bytes > 0 && in->imm.u + 4u > f->uniform_bytes) {
                wgsl_fail(err, err_cap, "inst %%%u: uniform ofseti %u sinir disi (%u)", i, in->imm.u, f->uniform_bytes);
                return 0;
            }
            if (in->imm.u + 4u > u_bytes) {
                u_bytes = in->imm.u + 4u;
            }
            break;

        case IR_VARYING:
            if (in->imm.u >= SHADE_MAX_VARYINGS) {
                wgsl_fail(err, err_cap, "inst %%%u: varying indeksi %u sinir disi", i, in->imm.u);
                return 0;
            }
            if (f->varying_count > 0 && in->imm.u >= f->varying_count) {
                wgsl_fail(err, err_cap, "inst %%%u: varying indeksi %u >= varying_count %u", i, in->imm.u, f->varying_count);
                return 0;
            }
            if (in->imm.u + 1u > nvar) {
                nvar = in->imm.u + 1u;
            }
            break;

        case IR_TEX_W:
        case IR_TEX_H:
        case IR_TEX_FLAGS:
        case IR_TEX_LEVELS:
        case IR_TEXEL_LOAD:
            if (in->imm.u >= SHADE_MAX_TEXTURES) {
                wgsl_fail(err, err_cap, "inst %%%u: doku slotu %u sinir disi", i, in->imm.u);
                return 0;
            }
            if (f->texture_count > 0 && in->imm.u >= f->texture_count) {
                wgsl_fail(err, err_cap, "inst %%%u: doku slotu %u >= texture_count %u", i, in->imm.u, f->texture_count);
                return 0;
            }
            if (in->imm.u + 1u > tex_count) {
                tex_count = in->imm.u + 1u;
            }
            break;

        case IR_CONST_F32:
        case IR_CONST_I32:
        case IR_CONST_BOOL:
        case IR_FRAG_X:
        case IR_FRAG_Y:
        case IR_FADD: case IR_FSUB: case IR_FMUL: case IR_FDIV: case IR_FMIN: case IR_FMAX:
        case IR_FNEG: case IR_FABS: case IR_FSQRT: case IR_FFLOOR: case IR_FCEIL: case IR_FTRUNC: case IR_FNEAREST:
        case IR_IADD: case IR_ISUB: case IR_IMUL: case IR_IAND: case IR_IOR: case IR_IXOR:
        case IR_ISHL: case IR_ISHR_S: case IR_ISHR_U: case IR_IMIN_S: case IR_IMAX_S:
        case IR_FEQ: case IR_FNE: case IR_FLT: case IR_FLE: case IR_FGT: case IR_FGE:
        case IR_IEQ: case IR_INE: case IR_ILT_S: case IR_ILE_S: case IR_IGT_S: case IR_IGE_S:
        case IR_BAND: case IR_BOR: case IR_BNOT:
        case IR_SELECT:
        case IR_F2I: case IR_I2F: case IR_BITCAST_F2I: case IR_BITCAST_I2F:
        case IR_IF_BEGIN: case IR_ELSE: case IR_IF_END:
        case IR_DISCARD: case IR_OUTPUT:
        case IR_DERIV_X: case IR_DERIV_Y:
            break;

        default:
            wgsl_fail(err, err_cap, "inst %%%u: desteklenmeyen IR komutu %u", i, (u32)op);
            return 0;
        }
    }

    /* 3. Varying erişim ifadelerini hazırla */
    if (layout) {
        for (u32 s = 0; s < layout->sym_count; s++) {
            const MtkSymbol* sym = &layout->syms[s];
            if (sym->kind != MTK_SYM_VARYING) continue;
            u32 base = sym->offset_or_index;
            u32 comps = sym->components;
            if (base + comps > SHADE_MAX_VARYINGS) {
                wgsl_fail(err, err_cap, "varying '%s' sinir disi (%u + %u)", sym->name, base, comps);
                return 0;
            }
            static const char swz[4] = { 'x', 'y', 'z', 'w' };
            for (u32 c = 0; c < comps; c++) {
                if (comps == 1) {
                    fmt_format(ctx->var_expr[base + c], sizeof(ctx->var_expr[0]), "fin.v_%s", sym->name);
                } else {
                    fmt_format(ctx->var_expr[base + c], sizeof(ctx->var_expr[0]), "fin.v_%s.%c", sym->name, swz[c]);
                }
                ctx->var_defined[base + c] = TRUE;
            }
        }
    } else {
        for (u32 k = 0; k < nvar && k < SHADE_MAX_VARYINGS; k++) {
            fmt_format(ctx->var_expr[k], sizeof(ctx->var_expr[0]), "fin.v_%u", k);
            ctx->var_defined[k] = TRUE;
        }
    }

    for (u32 i = 0; i < f->count; i++) {
        const IrInst* in = &f->insts[i];
        if (in->op == IR_VARYING && !ctx->var_defined[in->imm.u]) {
            wgsl_fail(err, err_cap, "inst %%%u: varying indeksi %u layout icinde tanimsiz", i, in->imm.u);
            return 0;
        }
    }

    /* 4. IR_TEXEL_LOAD koordinat eşleştirme ve IF kapsama analizi */
    u32 if_stk[WGSL_MAX_IF];
    u32 if_depth = 0;

    for (u32 i = 0; i < f->count; i++) {
        const IrInst* in = &f->insts[i];
        IrOp op = (IrOp)in->op;
        ctx->enclosing_if[i] = (if_depth > 0) ? if_stk[if_depth - 1] : IR_NONE;

        if (op == IR_IF_BEGIN) {
            if_stk[if_depth++] = i;
        } else if (op == IR_IF_END) {
            if_depth--;
            ctx->enclosing_if[i] = (if_depth > 0) ? if_stk[if_depth - 1] : IR_NONE;
        } else if (op == IR_TEXEL_LOAD) {
            u32 wx = IR_NONE, wy = IR_NONE, lvl = IR_NONE;
            if (match_texel_coords(f, in->imm.u, in->a, &wx, &wy, &lvl)) {
                ctx->tex_match[i].matched = TRUE;
                ctx->tex_match[i].wx      = wx;
                ctx->tex_match[i].wy      = wy;
                ctx->tex_match[i].level   = lvl;
            }
        } else if (op == IR_DERIV_X || op == IR_DERIV_Y) {
            /* WGSL dpdx/dpdy tekdüze olmayan (varying) IF içinde çağrılamaz;
             * kapsayan varying IF bloklarını düzleştirerek SPMD semantiğini koru. */
            for (u32 d = 0; d < if_depth; d++) {
                u32 if_id = if_stk[d];
                if (f->insts[if_id].flags & IR_FLAG_VARYING) {
                    ctx->flatten_if[if_id] = TRUE;
                }
            }
        }
    }

    /* 5. Canlılık analizi (match_texel_coords ile ölü kalan 1B indeks hesabını atlar) */
    u32 out_id[4] = { IR_NONE, IR_NONE, IR_NONE, IR_NONE };
    for (u32 i = 0; i < f->count; i++) {
        const IrInst* in = &f->insts[i];
        if (in->op == IR_OUTPUT) {
            ctx->live[i] = TRUE;
            out_id[in->imm.u] = in->a;
        } else if (in->op == IR_DISCARD) {
            ctx->live[i] = TRUE;
        }
    }

    for (u32 i = f->count; i > 0; i--) {
        u32 idx = i - 1;
        const IrInst* in = &f->insts[idx];
        if (in->op == IR_NOP || !ctx->live[idx]) continue;

        if (in->op == IR_TEXEL_LOAD && ctx->tex_match[idx].matched) {
            ctx->live[ctx->tex_match[idx].wx] = TRUE;
            ctx->live[ctx->tex_match[idx].wy] = TRUE;
            if (ctx->tex_match[idx].level != IR_NONE) {
                ctx->live[ctx->tex_match[idx].level] = TRUE;
            }
        } else {
            if (in->a != IR_NONE) ctx->live[in->a] = TRUE;
            if (in->b != IR_NONE) ctx->live[in->b] = TRUE;
            if (in->c != IR_NONE) ctx->live[in->c] = TRUE;
        }

        u32 p_if = ctx->enclosing_if[idx];
        if (p_if != IR_NONE) {
            ctx->live[p_if] = TRUE;
            ctx->live[f->insts[p_if].a] = TRUE;
        }
    }

    /* 6. Bölge ağacı ve kapsam dışı kullanılan (SELECT ile birleştirilen) değerlerin tespiti */
    u32 cur_reg  = 0;
    u32 next_reg = 1;
    u32 reg_stk[WGSL_MAX_IF];
    u32 r_depth  = 0;
    ctx->reg_parent[0] = 0;

    for (u32 i = 0; i < f->count; i++) {
        const IrInst* in = &f->insts[i];
        IrOp op = (IrOp)in->op;
        if (op == IR_NOP) {
            ctx->inst_reg[i] = cur_reg;
            continue;
        }
        if (op == IR_IF_BEGIN) {
            ctx->inst_reg[i] = cur_reg;
            reg_stk[r_depth++] = cur_reg;
            if (!ctx->flatten_if[i] && ctx->live[i]) {
                u32 nr = next_reg++;
                ctx->reg_parent[nr] = cur_reg;
                cur_reg = nr;
            }
            continue;
        }
        if (op == IR_ELSE) {
            u32 if_id = ctx->enclosing_if[i];
            u32 parent_r = reg_stk[r_depth - 1];
            ctx->inst_reg[i] = parent_r;
            if (if_id != IR_NONE && !ctx->flatten_if[if_id] && ctx->live[if_id]) {
                u32 nr = next_reg++;
                ctx->reg_parent[nr] = parent_r;
                cur_reg = nr;
            }
            continue;
        }
        if (op == IR_IF_END) {
            cur_reg = reg_stk[--r_depth];
            ctx->inst_reg[i] = cur_reg;
            continue;
        }
        ctx->inst_reg[i] = cur_reg;
    }

    for (u32 u = 0; u < f->count; u++) {
        const IrInst* in = &f->insts[u];
        if (in->op == IR_NOP || !ctx->live[u]) continue;

        u32 ops[3] = { in->a, in->b, in->c };
        if (in->op == IR_TEXEL_LOAD && ctx->tex_match[u].matched) {
            ops[0] = ctx->tex_match[u].wx;
            ops[1] = ctx->tex_match[u].wy;
            ops[2] = ctx->tex_match[u].level;
        }
        for (u32 k = 0; k < 3; k++) {
            u32 def = ops[k];
            if (def == IR_NONE || is_const_inst(&f->insts[def])) continue;
            if (!region_dominates(ctx->inst_reg[def], ctx->inst_reg[u], ctx->reg_parent)) {
                ctx->needs_func_var[def] = TRUE;
            }
        }
    }

    /* 7. WGSL metnini üret */
    WgslBuf wb;
    wb_init(&wb, out, out_cap);

    u32 group = opts ? opts->frag_group : MTK_WGSL_FRAG_GROUP;
    const char* entry_name = (opts && opts->entry_name && opts->entry_name[0]) ? opts->entry_name : "frag";
    u32 u_vec4_count = (u_bytes + 15u) / 16u;

    wb_putf(&wb, "// Matoki WGSL fragment shader (uniform_bytes=%u, varyings=%u, textures=%u)\n",
            u_bytes, nvar, tex_count);

    if (layout && layout->sym_count > 0) {
        for (u32 s = 0; s < layout->sym_count; s++) {
            const MtkSymbol* sym = &layout->syms[s];
            if (sym->kind == MTK_SYM_UNIFORM) {
                wb_putf(&wb, "// uniform %-12s : %-6s offset=%u (mtk_u.data[%u])\n",
                        sym->name, mtk_type_name((MtkType)sym->type),
                        sym->offset_or_index, sym->offset_or_index / 16u);
            } else if (sym->kind == MTK_SYM_TEXTURE) {
                wb_putf(&wb, "// texture %-12s : slot=%u (@group(%u) @binding(%u/%u))\n",
                        sym->name, sym->offset_or_index, group,
                        MTK_WGSL_TEX_BINDING(sym->offset_or_index),
                        MTK_WGSL_SAMP_BINDING(sym->offset_or_index));
            }
        }
    }
    wb_putc(&wb, '\n');

    if (u_vec4_count > 0) {
        wb_putf(&wb, "struct MtkUniforms {\n    data: array<vec4<f32>, %u>,\n};\n\n", u_vec4_count);
        wb_putf(&wb, "@group(%u) @binding(%u) var<uniform> mtk_u: MtkUniforms;\n",
                group, MTK_WGSL_UBO_BINDING);
    }

    for (u32 s = 0; s < tex_count; s++) {
        wb_putf(&wb, "@group(%u) @binding(%u) var tex_%u: texture_2d<f32>;\n",
                group, MTK_WGSL_TEX_BINDING(s), s);
        wb_putf(&wb, "@group(%u) @binding(%u) var samp_%u: sampler;\n",
                group, MTK_WGSL_SAMP_BINDING(s), s);
    }
    if (u_vec4_count > 0 || tex_count > 0) {
        wb_putc(&wb, '\n');
    }

    wb_puts(&wb, "struct FragIn {\n    @builtin(position) frag_pos: vec4<f32>,\n");
    if (layout) {
        u32 loc = 0;
        for (u32 s = 0; s < layout->sym_count; s++) {
            const MtkSymbol* sym = &layout->syms[s];
            if (sym->kind != MTK_SYM_VARYING) continue;
            wb_putf(&wb, "    @location(%u) v_%s: %s,\n",
                    loc++, sym->name, comps_wgsl_f32_type(sym->components));
        }
    } else {
        for (u32 k = 0; k < nvar; k++) {
            wb_putf(&wb, "    @location(%u) v_%u: f32,\n", k, k);
        }
    }
    wb_puts(&wb, "};\n\n");

    wb_putf(&wb, "@fragment\nfn %s(fin: FragIn) -> @location(0) vec4<f32> {\n", entry_name);
    wb_puts(&wb, "    _ = fin.frag_pos;\n");
    if (u_vec4_count > 0) {
        wb_puts(&wb, "    _ = mtk_u.data[0];\n");
    }
    for (u32 s = 0; s < tex_count; s++) {
        wb_putf(&wb, "    _ = tex_%u;\n    _ = samp_%u;\n", s, s);
    }

    /* IF dalları dışına taşınan değişkenlerin ön bildirimi */
    for (u32 i = 0; i < f->count; i++) {
        if (!ctx->live[i] || !ctx->needs_func_var[i]) continue;
        const IrInst* in = &f->insts[i];
        wb_putf(&wb, "    var t%u: %s = %s;\n",
                i, ir_type_wgsl_name((IrType)in->type), ir_type_wgsl_zero((IrType)in->type));
    }

    u32 indent = 1;
    u32 active_if_stk[WGSL_MAX_IF];
    u32 active_if_depth = 0;

    for (u32 i = 0; i < f->count; i++) {
        const IrInst* in = &f->insts[i];
        IrOp op = (IrOp)in->op;
        if (op == IR_NOP) continue;

        if (op == IR_IF_BEGIN) {
            active_if_stk[active_if_depth++] = i;
            if (!ctx->flatten_if[i] && ctx->live[i]) {
                wb_indent(&wb, indent);
                wb_puts(&wb, "if (");
                emit_operand(&wb, f, in->a);
                wb_puts(&wb, ") {\n");
                indent++;
            }
            continue;
        }
        if (op == IR_ELSE) {
            u32 if_id = active_if_stk[active_if_depth - 1];
            if (!ctx->flatten_if[if_id] && ctx->live[if_id]) {
                wb_indent(&wb, indent - 1);
                wb_puts(&wb, "} else {\n");
            }
            continue;
        }
        if (op == IR_IF_END) {
            u32 if_id = active_if_stk[--active_if_depth];
            if (!ctx->flatten_if[if_id] && ctx->live[if_id]) {
                indent--;
                wb_indent(&wb, indent);
                wb_puts(&wb, "}\n");
            }
            continue;
        }

        if (!ctx->live[i] || is_const_inst(in)) continue;

        if (op == IR_DISCARD) {
            wb_indent(&wb, indent);
            wb_puts(&wb, "if (");
            emit_operand(&wb, f, in->a);
            wb_puts(&wb, ") {\n");
            wb_indent(&wb, indent + 1);
            wb_puts(&wb, "discard;\n");
            wb_indent(&wb, indent);
            wb_puts(&wb, "}\n");
            continue;
        }

        if (op == IR_OUTPUT) {
            continue;
        }

        wb_indent(&wb, indent);
        if (ctx->needs_func_var[i]) {
            wb_putf(&wb, "t%u = ", i);
        } else {
            wb_putf(&wb, "let t%u: %s = ", i, ir_type_wgsl_name((IrType)in->type));
        }

        switch (op) {
        case IR_UNIFORM_F32: {
            u32 v_idx = in->imm.u / 16u;
            char comp = "xyzw"[(in->imm.u / 4u) & 3u];
            wb_putf(&wb, "mtk_u.data[%u].%c", v_idx, comp);
        } break;

        case IR_UNIFORM_I32: {
            u32 v_idx = in->imm.u / 16u;
            char comp = "xyzw"[(in->imm.u / 4u) & 3u];
            wb_putf(&wb, "bitcast<i32>(mtk_u.data[%u].%c)", v_idx, comp);
        } break;

        case IR_VARYING:
            wb_puts(&wb, ctx->var_expr[in->imm.u]);
            break;

        case IR_FRAG_X:
            wb_puts(&wb, "fin.frag_pos.x");
            break;

        case IR_FRAG_Y:
            wb_puts(&wb, "fin.frag_pos.y");
            break;

        case IR_FADD:
        case IR_IADD:
            emit_operand(&wb, f, in->a);
            wb_puts(&wb, " + ");
            emit_operand(&wb, f, in->b);
            break;

        case IR_FSUB:
        case IR_ISUB:
            emit_operand(&wb, f, in->a);
            wb_puts(&wb, " - ");
            emit_operand(&wb, f, in->b);
            break;

        case IR_FMUL:
        case IR_IMUL:
            emit_operand(&wb, f, in->a);
            wb_puts(&wb, " * ");
            emit_operand(&wb, f, in->b);
            break;

        case IR_FDIV:
            emit_operand(&wb, f, in->a);
            wb_puts(&wb, " / ");
            emit_operand(&wb, f, in->b);
            break;

        case IR_FMIN:
        case IR_IMIN_S:
            wb_puts(&wb, "min(");
            emit_operand(&wb, f, in->a);
            wb_puts(&wb, ", ");
            emit_operand(&wb, f, in->b);
            wb_putc(&wb, ')');
            break;

        case IR_FMAX:
        case IR_IMAX_S:
            wb_puts(&wb, "max(");
            emit_operand(&wb, f, in->a);
            wb_puts(&wb, ", ");
            emit_operand(&wb, f, in->b);
            wb_putc(&wb, ')');
            break;

        case IR_FNEG:
            wb_puts(&wb, "-(");
            emit_operand(&wb, f, in->a);
            wb_putc(&wb, ')');
            break;

        case IR_FABS:
            wb_puts(&wb, "abs(");
            emit_operand(&wb, f, in->a);
            wb_putc(&wb, ')');
            break;

        case IR_FSQRT:
            wb_puts(&wb, "sqrt(");
            emit_operand(&wb, f, in->a);
            wb_putc(&wb, ')');
            break;

        case IR_FFLOOR:
            wb_puts(&wb, "floor(");
            emit_operand(&wb, f, in->a);
            wb_putc(&wb, ')');
            break;

        case IR_FCEIL:
            wb_puts(&wb, "ceil(");
            emit_operand(&wb, f, in->a);
            wb_putc(&wb, ')');
            break;

        case IR_FTRUNC:
            wb_puts(&wb, "trunc(");
            emit_operand(&wb, f, in->a);
            wb_putc(&wb, ')');
            break;

        case IR_FNEAREST:
            wb_puts(&wb, "round(");
            emit_operand(&wb, f, in->a);
            wb_putc(&wb, ')');
            break;

        case IR_IAND:
            emit_operand(&wb, f, in->a);
            wb_puts(&wb, " & ");
            emit_operand(&wb, f, in->b);
            break;

        case IR_IOR:
            emit_operand(&wb, f, in->a);
            wb_puts(&wb, " | ");
            emit_operand(&wb, f, in->b);
            break;

        case IR_IXOR:
            emit_operand(&wb, f, in->a);
            wb_puts(&wb, " ^ ");
            emit_operand(&wb, f, in->b);
            break;

        case IR_ISHL:
            emit_operand(&wb, f, in->a);
            wb_puts(&wb, " << (u32(");
            emit_operand(&wb, f, in->b);
            wb_puts(&wb, ") & 31u)");
            break;

        case IR_ISHR_S:
            emit_operand(&wb, f, in->a);
            wb_puts(&wb, " >> (u32(");
            emit_operand(&wb, f, in->b);
            wb_puts(&wb, ") & 31u)");
            break;

        case IR_ISHR_U:
            wb_puts(&wb, "bitcast<i32>(bitcast<u32>(");
            emit_operand(&wb, f, in->a);
            wb_puts(&wb, ") >> (u32(");
            emit_operand(&wb, f, in->b);
            wb_puts(&wb, ") & 31u))");
            break;

        case IR_FEQ:
        case IR_IEQ:
            emit_operand(&wb, f, in->a);
            wb_puts(&wb, " == ");
            emit_operand(&wb, f, in->b);
            break;

        case IR_FNE:
        case IR_INE:
            emit_operand(&wb, f, in->a);
            wb_puts(&wb, " != ");
            emit_operand(&wb, f, in->b);
            break;

        case IR_FLT:
        case IR_ILT_S:
            emit_operand(&wb, f, in->a);
            wb_puts(&wb, " < ");
            emit_operand(&wb, f, in->b);
            break;

        case IR_FLE:
        case IR_ILE_S:
            emit_operand(&wb, f, in->a);
            wb_puts(&wb, " <= ");
            emit_operand(&wb, f, in->b);
            break;

        case IR_FGT:
        case IR_IGT_S:
            emit_operand(&wb, f, in->a);
            wb_puts(&wb, " > ");
            emit_operand(&wb, f, in->b);
            break;

        case IR_FGE:
        case IR_IGE_S:
            emit_operand(&wb, f, in->a);
            wb_puts(&wb, " >= ");
            emit_operand(&wb, f, in->b);
            break;

        case IR_BAND:
            emit_operand(&wb, f, in->a);
            wb_puts(&wb, " && ");
            emit_operand(&wb, f, in->b);
            break;

        case IR_BOR:
            emit_operand(&wb, f, in->a);
            wb_puts(&wb, " || ");
            emit_operand(&wb, f, in->b);
            break;

        case IR_BNOT:
            wb_puts(&wb, "!(");
            emit_operand(&wb, f, in->a);
            wb_putc(&wb, ')');
            break;

        case IR_SELECT:
            /* WGSL select(false_val, true_val, cond) sırası: c = if_false, b = if_true, a = cond */
            wb_puts(&wb, "select(");
            emit_operand(&wb, f, in->c);
            wb_puts(&wb, ", ");
            emit_operand(&wb, f, in->b);
            wb_puts(&wb, ", ");
            emit_operand(&wb, f, in->a);
            wb_putc(&wb, ')');
            break;

        case IR_F2I:
            wb_puts(&wb, "i32(");
            emit_operand(&wb, f, in->a);
            wb_putc(&wb, ')');
            break;

        case IR_I2F:
            wb_puts(&wb, "f32(");
            emit_operand(&wb, f, in->a);
            wb_putc(&wb, ')');
            break;

        case IR_BITCAST_F2I:
            wb_puts(&wb, "bitcast<i32>(");
            emit_operand(&wb, f, in->a);
            wb_putc(&wb, ')');
            break;

        case IR_BITCAST_I2F:
            wb_puts(&wb, "bitcast<f32>(");
            emit_operand(&wb, f, in->a);
            wb_putc(&wb, ')');
            break;

        case IR_TEX_W:
            wb_putf(&wb, "i32(textureDimensions(tex_%u, 0).x)", in->imm.u);
            break;

        case IR_TEX_H:
            wb_putf(&wb, "i32(textureDimensions(tex_%u, 0).y)", in->imm.u);
            break;

        case IR_TEX_FLAGS:
            wb_putf(&wb, "i32(0)");
            break;

        case IR_TEX_LEVELS:
            wb_putf(&wb, "i32(textureNumLevels(tex_%u))", in->imm.u);
            break;

        case IR_TEXEL_LOAD:
            if (ctx->tex_match[i].matched) {
                wb_putf(&wb, "bitcast<i32>(pack4x8unorm(textureLoad(tex_%u, vec2<i32>(", in->imm.u);
                emit_operand(&wb, f, ctx->tex_match[i].wx);
                wb_puts(&wb, ", ");
                emit_operand(&wb, f, ctx->tex_match[i].wy);
                wb_puts(&wb, "), ");
                if (ctx->tex_match[i].level != IR_NONE) {
                    emit_operand(&wb, f, ctx->tex_match[i].level);
                } else {
                    wb_putc(&wb, '0');
                }
                wb_puts(&wb, ")))");
            } else {
                wb_putf(&wb, "bitcast<i32>(pack4x8unorm(textureLoad(tex_%u, vec2<i32>((", in->imm.u);
                emit_operand(&wb, f, in->a);
                wb_putf(&wb, ") %% i32(textureDimensions(tex_%u, 0).x), (", in->imm.u);
                emit_operand(&wb, f, in->a);
                wb_putf(&wb, ") / i32(textureDimensions(tex_%u, 0).x)), 0)))", in->imm.u);
            }
            break;

        case IR_DERIV_X:
            wb_puts(&wb, "dpdx(");
            emit_operand(&wb, f, in->a);
            wb_putc(&wb, ')');
            break;

        case IR_DERIV_Y:
            wb_puts(&wb, "dpdy(");
            emit_operand(&wb, f, in->a);
            wb_putc(&wb, ')');
            break;

        default:
            wgsl_fail(err, err_cap, "inst %%%u: beklenmeyen komut %u", i, (u32)op);
            return 0;
        }

        wb_puts(&wb, ";\n");
    }

    wb_indent(&wb, 1);
    wb_puts(&wb, "return vec4<f32>(");
    for (u32 c = 0; c < 4; c++) {
        if (c > 0) wb_puts(&wb, ", ");
        emit_operand(&wb, f, out_id[c]);
    }
    wb_puts(&wb, ");\n}\n");

    if (wb.overflow) {
        wgsl_fail(err, err_cap, "WGSL cikti tamponu kapasitesi (%u bayt) yetersiz", out_cap);
        out[0] = '\0';
        return 0;
    }

    return wb.len;
}

u32 mtk_wgsl_compute(const IrFunc* f, const MtkLayout* layout, const MtkWgslOpts* opts,
                     char* out, u32 out_cap, char* err, u32 err_cap) {
    if (err && err_cap > 0) err[0] = '\0';
    if (!out || out_cap == 0) {
        wgsl_fail(err, err_cap, "cikti tamponu gecersiz");
        return 0;
    }
    out[0] = '\0';

    if (!f || f->count == 0 || !f->insts) {
        wgsl_fail(err, err_cap, "bos veya gecersiz IrFunc");
        return 0;
    }
    if (f->count > WGSL_MAX_INSTS) {
        wgsl_fail(err, err_cap, "IrFunc komut sayisi siniri asildi (%u > %u)", f->count, WGSL_MAX_INSTS);
        return 0;
    }

    if (!ir_verify(f, err, err_cap)) {
        return 0;
    }

    WgslFragCtx* ctx = &g_wgsl_ctx;
    __builtin_memset(ctx, 0, sizeof(*ctx));

    u32 u_bytes     = f->uniform_bytes;
    u32 buf_count   = f->buffer_count;
    u32 atomic_mask = f->buffer_atomic_mask;
    u8  elem_types[IR_MAX_BUFFERS];
    for (u32 b = 0; b < IR_MAX_BUFFERS; b++) {
        elem_types[b] = f->buffer_elem_types[b];
    }

    if (layout) {
        if (layout->uniform_bytes > u_bytes)  u_bytes   = layout->uniform_bytes;
        if (layout->buffer_count > buf_count) buf_count = layout->buffer_count;
        for (u32 s = 0; s < layout->sym_count; s++) {
            const MtkSymbol* sym = &layout->syms[s];
            if (sym->kind == MTK_SYM_BUFFER && sym->offset_or_index < IR_MAX_BUFFERS) {
                elem_types[sym->offset_or_index] = (u8)mtk_type_buffer_elem((MtkType)sym->type);
            }
        }
    }

    /* Sınır, hizalama ve desteklenen opcode denetimi */
    for (u32 i = 0; i < f->count; i++) {
        const IrInst* in = &f->insts[i];
        IrOp op = (IrOp)in->op;
        if (op == IR_NOP) continue;

        switch (op) {
        case IR_UNIFORM_F32:
        case IR_UNIFORM_I32:
            if ((in->imm.u & 3u) != 0 || in->imm.u >= 65536u) {
                wgsl_fail(err, err_cap, "inst %%%u: hizasiz veya gecersiz uniform ofseti %u", i, in->imm.u);
                return 0;
            }
            if (f->uniform_bytes > 0 && in->imm.u + 4u > f->uniform_bytes) {
                wgsl_fail(err, err_cap, "inst %%%u: uniform ofseti %u sinir disi (%u)", i, in->imm.u, f->uniform_bytes);
                return 0;
            }
            if (in->imm.u + 4u > u_bytes) {
                u_bytes = in->imm.u + 4u;
            }
            break;

        case IR_BUF_LOAD:
        case IR_BUF_STORE: {
            u32 slot = IR_BUF_SLOT(in->imm.u);
            if (slot >= IR_MAX_BUFFERS) {
                wgsl_fail(err, err_cap, "inst %%%u: buffer slotu %u sinir disi", i, slot);
                return 0;
            }
            if (slot + 1u > buf_count) buf_count = slot + 1u;
            if (elem_types[slot] == 0) {
                if (IR_BUF_IS_VEC4(in->imm.u)) elem_types[slot] = (u8)MTK_TYPE_VEC4;
                else if (op == IR_BUF_LOAD && in->type == IR_T_I32) elem_types[slot] = (u8)MTK_TYPE_U32;
                else if (op == IR_BUF_STORE && f->insts[in->b].type == IR_T_I32) elem_types[slot] = (u8)MTK_TYPE_U32;
                else elem_types[slot] = (u8)MTK_TYPE_F32;
            }
        } break;

        case IR_ATOMIC_ADD: {
            u32 slot = in->imm.u;
            if (slot >= IR_MAX_BUFFERS) {
                wgsl_fail(err, err_cap, "inst %%%u: atomic buffer slotu %u sinir disi", i, slot);
                return 0;
            }
            if (slot + 1u > buf_count) buf_count = slot + 1u;
            atomic_mask |= (1u << slot);
            elem_types[slot] = (u8)MTK_TYPE_U32;
        } break;

        case IR_CONST_F32:
        case IR_CONST_I32:
        case IR_CONST_BOOL:
        case IR_GLOBAL_ID:
        case IR_FADD: case IR_FSUB: case IR_FMUL: case IR_FDIV: case IR_FMIN: case IR_FMAX:
        case IR_FNEG: case IR_FABS: case IR_FSQRT: case IR_FFLOOR: case IR_FCEIL: case IR_FTRUNC: case IR_FNEAREST:
        case IR_IADD: case IR_ISUB: case IR_IMUL: case IR_IAND: case IR_IOR: case IR_IXOR:
        case IR_ISHL: case IR_ISHR_S: case IR_ISHR_U: case IR_IMIN_S: case IR_IMAX_S:
        case IR_FEQ: case IR_FNE: case IR_FLT: case IR_FLE: case IR_FGT: case IR_FGE:
        case IR_IEQ: case IR_INE: case IR_ILT_S: case IR_ILE_S: case IR_IGT_S: case IR_IGE_S:
        case IR_BAND: case IR_BOR: case IR_BNOT:
        case IR_SELECT:
        case IR_F2I: case IR_I2F: case IR_BITCAST_F2I: case IR_BITCAST_I2F:
        case IR_IF_BEGIN: case IR_ELSE: case IR_IF_END:
            break;

        default:
            wgsl_fail(err, err_cap, "inst %%%u: compute icinde desteklenmeyen IR komutu %u", i, (u32)op);
            return 0;
        }
    }

    /* IF kapsama analizi */
    u32 if_stk[WGSL_MAX_IF];
    u32 if_depth = 0;
    for (u32 i = 0; i < f->count; i++) {
        const IrInst* in = &f->insts[i];
        IrOp op = (IrOp)in->op;
        ctx->enclosing_if[i] = (if_depth > 0) ? if_stk[if_depth - 1] : IR_NONE;
        if (op == IR_IF_BEGIN) {
            if_stk[if_depth++] = i;
        } else if (op == IR_IF_END) {
            if_depth--;
            ctx->enclosing_if[i] = (if_depth > 0) ? if_stk[if_depth - 1] : IR_NONE;
        }
    }

    /* Canlılık analizi (kökler: IR_BUF_STORE ve IR_ATOMIC_ADD) */
    for (u32 i = 0; i < f->count; i++) {
        const IrInst* in = &f->insts[i];
        if (in->op == IR_BUF_STORE || in->op == IR_ATOMIC_ADD) {
            ctx->live[i] = TRUE;
        }
    }

    for (u32 i = f->count; i > 0; i--) {
        u32 idx = i - 1;
        const IrInst* in = &f->insts[idx];
        if (in->op == IR_NOP || !ctx->live[idx]) continue;

        if (in->a != IR_NONE) ctx->live[in->a] = TRUE;
        if (in->b != IR_NONE) ctx->live[in->b] = TRUE;
        if (in->c != IR_NONE) ctx->live[in->c] = TRUE;

        u32 p_if = ctx->enclosing_if[idx];
        if (p_if != IR_NONE) {
            ctx->live[p_if] = TRUE;
            ctx->live[f->insts[p_if].a] = TRUE;
        }
    }

    /* Bölge ağacı ve kapsam dışı kullanılan değerlerin tespiti */
    u32 cur_reg  = 0;
    u32 next_reg = 1;
    u32 reg_stk[WGSL_MAX_IF];
    u32 r_depth  = 0;
    ctx->reg_parent[0] = 0;

    for (u32 i = 0; i < f->count; i++) {
        const IrInst* in = &f->insts[i];
        IrOp op = (IrOp)in->op;
        if (op == IR_NOP) {
            ctx->inst_reg[i] = cur_reg;
            continue;
        }
        if (op == IR_IF_BEGIN) {
            ctx->inst_reg[i] = cur_reg;
            reg_stk[r_depth++] = cur_reg;
            if (ctx->live[i]) {
                u32 nr = next_reg++;
                ctx->reg_parent[nr] = cur_reg;
                cur_reg = nr;
            }
            continue;
        }
        if (op == IR_ELSE) {
            u32 if_id = ctx->enclosing_if[i];
            u32 parent_r = reg_stk[r_depth - 1];
            ctx->inst_reg[i] = parent_r;
            if (if_id != IR_NONE && ctx->live[if_id]) {
                u32 nr = next_reg++;
                ctx->reg_parent[nr] = parent_r;
                cur_reg = nr;
            }
            continue;
        }
        if (op == IR_IF_END) {
            cur_reg = reg_stk[--r_depth];
            ctx->inst_reg[i] = cur_reg;
            continue;
        }
        ctx->inst_reg[i] = cur_reg;
    }

    for (u32 u = 0; u < f->count; u++) {
        const IrInst* in = &f->insts[u];
        if (in->op == IR_NOP || !ctx->live[u]) continue;
        if (in->op == IR_ATOMIC_ADD && in->c != IR_NONE) {
            ctx->needs_func_var[u] = TRUE;
        }
        u32 ops[3] = { in->a, in->b, in->c };
        for (u32 k = 0; k < 3; k++) {
            u32 def = ops[k];
            if (def == IR_NONE || is_const_inst(&f->insts[def])) continue;
            if (!region_dominates(ctx->inst_reg[def], ctx->inst_reg[u], ctx->reg_parent)) {
                ctx->needs_func_var[def] = TRUE;
            }
        }
    }

    /* WGSL metnini üret */
    WgslBuf wb;
    wb_init(&wb, out, out_cap);

    u32 group = opts ? opts->frag_group : MTK_WGSL_FRAG_GROUP;
    const char* entry_name = (opts && opts->entry_name && opts->entry_name[0]) ? opts->entry_name : "compute";
    u32 u_vec4_count = (u_bytes + 15u) / 16u;

    wb_putf(&wb, "// Matoki WGSL compute shader (uniform_bytes=%u, buffers=%u)\n",
            u_bytes, buf_count);

    if (layout && layout->sym_count > 0) {
        for (u32 s = 0; s < layout->sym_count; s++) {
            const MtkSymbol* sym = &layout->syms[s];
            if (sym->kind == MTK_SYM_UNIFORM) {
                wb_putf(&wb, "// uniform %-12s : %-6s offset=%u (mtk_u.data[%u])\n",
                        sym->name, mtk_type_name((MtkType)sym->type),
                        sym->offset_or_index, sym->offset_or_index / 16u);
            } else if (sym->kind == MTK_SYM_BUFFER) {
                wb_putf(&wb, "// buffer  %-12s : %-12s slot=%u (@group(%u) @binding(%u))\n",
                        sym->name, mtk_type_name((MtkType)sym->type),
                        sym->offset_or_index, group,
                        MTK_WGSL_BUF_BINDING(sym->offset_or_index));
            }
        }
    }
    wb_putc(&wb, '\n');

    if (u_vec4_count > 0) {
        wb_putf(&wb, "struct MtkUniforms {\n    data: array<vec4<f32>, %u>,\n};\n\n", u_vec4_count);
        wb_putf(&wb, "@group(%u) @binding(%u) var<uniform> mtk_u: MtkUniforms;\n",
                group, MTK_WGSL_UBO_BINDING);
    }

    for (u32 s = 0; s < buf_count; s++) {
        const char* et = "f32";
        if (atomic_mask & (1u << s)) {
            et = "atomic<u32>";
        } else if (elem_types[s] == (u8)MTK_TYPE_VEC4) {
            et = "vec4<f32>";
        } else if (elem_types[s] == (u8)MTK_TYPE_U32) {
            et = "u32";
        }
        wb_putf(&wb, "@group(%u) @binding(%u) var<storage, read_write> buf_%u: array<%s>;\n",
                group, MTK_WGSL_BUF_BINDING(s), s, et);
    }
    if (u_vec4_count > 0 || buf_count > 0) {
        wb_putc(&wb, '\n');
    }

    wb_putf(&wb, "@compute @workgroup_size(64)\nfn %s(@builtin(global_invocation_id) gid: vec3<u32>) {\n",
            entry_name);
    wb_puts(&wb, "    _ = gid;\n");
    if (u_vec4_count > 0) {
        wb_puts(&wb, "    _ = mtk_u.data[0];\n");
    }
    for (u32 s = 0; s < buf_count; s++) {
        wb_putf(&wb, "    _ = &buf_%u;\n", s);
    }

    for (u32 i = 0; i < f->count; i++) {
        if (!ctx->live[i] || !ctx->needs_func_var[i]) continue;
        const IrInst* in = &f->insts[i];
        wb_putf(&wb, "    var t%u: %s = %s;\n",
                i, ir_type_wgsl_name((IrType)in->type), ir_type_wgsl_zero((IrType)in->type));
    }

    u32 indent = 1;
    u32 active_if_stk[WGSL_MAX_IF];
    u32 active_if_depth = 0;

    for (u32 i = 0; i < f->count; i++) {
        const IrInst* in = &f->insts[i];
        IrOp op = (IrOp)in->op;
        if (op == IR_NOP) continue;

        if (op == IR_IF_BEGIN) {
            active_if_stk[active_if_depth++] = i;
            if (ctx->live[i]) {
                wb_indent(&wb, indent);
                wb_puts(&wb, "if (");
                emit_operand(&wb, f, in->a);
                wb_puts(&wb, ") {\n");
                indent++;
            }
            continue;
        }
        if (op == IR_ELSE) {
            u32 if_id = active_if_stk[active_if_depth - 1];
            if (ctx->live[if_id]) {
                wb_indent(&wb, indent - 1);
                wb_puts(&wb, "} else {\n");
            }
            continue;
        }
        if (op == IR_IF_END) {
            u32 if_id = active_if_stk[--active_if_depth];
            if (ctx->live[if_id]) {
                indent--;
                wb_indent(&wb, indent);
                wb_puts(&wb, "}\n");
            }
            continue;
        }

        if (!ctx->live[i] || is_const_inst(in)) continue;

        if (op == IR_BUF_STORE) {
            u32 slot    = IR_BUF_SLOT(in->imm.u);
            u32 comp    = IR_BUF_COMP(in->imm.u);
            b32 is_vec4 = IR_BUF_IS_VEC4(in->imm.u);

            /* Ardışık 4-bileşenli vec4 yazımını tek vec4<f32>(...) atamasında birleştir */
            if (is_vec4 && comp == 0 && i + 3 < f->count) {
                const IrInst* i1 = &f->insts[i + 1];
                const IrInst* i2 = &f->insts[i + 2];
                const IrInst* i3 = &f->insts[i + 3];
                if (i1->op == IR_BUF_STORE && i2->op == IR_BUF_STORE && i3->op == IR_BUF_STORE &&
                    IR_BUF_IS_VEC4(i1->imm.u) && IR_BUF_IS_VEC4(i2->imm.u) && IR_BUF_IS_VEC4(i3->imm.u) &&
                    IR_BUF_SLOT(i1->imm.u) == slot && IR_BUF_SLOT(i2->imm.u) == slot && IR_BUF_SLOT(i3->imm.u) == slot &&
                    IR_BUF_COMP(i1->imm.u) == 1 && IR_BUF_COMP(i2->imm.u) == 2 && IR_BUF_COMP(i3->imm.u) == 3 &&
                    i1->a == in->a && i2->a == in->a && i3->a == in->a &&
                    i1->c == in->c && i2->c == in->c && i3->c == in->c &&
                    ctx->inst_reg[i + 3] == ctx->inst_reg[i]) {
                    u32 store_indent = indent;
                    if (in->c != IR_NONE) {
                        wb_indent(&wb, indent);
                        wb_puts(&wb, "if (");
                        emit_operand(&wb, f, in->c);
                        wb_puts(&wb, ") {\n");
                        store_indent++;
                    }
                    wb_indent(&wb, store_indent);
                    wb_putf(&wb, "buf_%u[u32(", slot);
                    emit_operand(&wb, f, in->a);
                    wb_puts(&wb, ")] = vec4<f32>(");
                    emit_operand(&wb, f, in->b);
                    wb_puts(&wb, ", ");
                    emit_operand(&wb, f, i1->b);
                    wb_puts(&wb, ", ");
                    emit_operand(&wb, f, i2->b);
                    wb_puts(&wb, ", ");
                    emit_operand(&wb, f, i3->b);
                    wb_puts(&wb, ");\n");
                    if (in->c != IR_NONE) {
                        wb_indent(&wb, indent);
                        wb_puts(&wb, "}\n");
                    }
                    i += 3;
                    continue;
                }
            }

            u32 store_indent = indent;
            if (in->c != IR_NONE) {
                wb_indent(&wb, indent);
                wb_puts(&wb, "if (");
                emit_operand(&wb, f, in->c);
                wb_puts(&wb, ") {\n");
                store_indent++;
            }
            wb_indent(&wb, store_indent);
            if (atomic_mask & (1u << slot)) {
                wb_putf(&wb, "atomicStore(&buf_%u[u32(", slot);
                emit_operand(&wb, f, in->a);
                wb_puts(&wb, ")], bitcast<u32>(");
                emit_operand(&wb, f, in->b);
                wb_puts(&wb, "));\n");
            } else if (is_vec4) {
                wb_putf(&wb, "buf_%u[u32(", slot);
                emit_operand(&wb, f, in->a);
                wb_putf(&wb, ")].%c = ", "xyzw"[comp & 3u]);
                emit_operand(&wb, f, in->b);
                wb_puts(&wb, ";\n");
            } else if (elem_types[slot] == (u8)MTK_TYPE_U32) {
                wb_putf(&wb, "buf_%u[u32(", slot);
                emit_operand(&wb, f, in->a);
                wb_puts(&wb, ")] = bitcast<u32>(");
                emit_operand(&wb, f, in->b);
                wb_puts(&wb, ");\n");
            } else {
                wb_putf(&wb, "buf_%u[u32(", slot);
                emit_operand(&wb, f, in->a);
                wb_puts(&wb, ")] = ");
                emit_operand(&wb, f, in->b);
                wb_puts(&wb, ";\n");
            }
            if (in->c != IR_NONE) {
                wb_indent(&wb, indent);
                wb_puts(&wb, "}\n");
            }
            continue;
        }

        if (op == IR_ATOMIC_ADD && in->c != IR_NONE) {
            wb_indent(&wb, indent);
            wb_puts(&wb, "if (");
            emit_operand(&wb, f, in->c);
            wb_puts(&wb, ") {\n");
            wb_indent(&wb, indent + 1);
            wb_putf(&wb, "t%u = bitcast<i32>(atomicAdd(&buf_%u[u32(", i, in->imm.u);
            emit_operand(&wb, f, in->a);
            wb_puts(&wb, ")], bitcast<u32>(");
            emit_operand(&wb, f, in->b);
            wb_puts(&wb, ")));\n");
            wb_indent(&wb, indent);
            wb_puts(&wb, "}\n");
            continue;
        }

        wb_indent(&wb, indent);
        if (ctx->needs_func_var[i]) {
            wb_putf(&wb, "t%u = ", i);
        } else {
            wb_putf(&wb, "let t%u: %s = ", i, ir_type_wgsl_name((IrType)in->type));
        }

        switch (op) {
        case IR_UNIFORM_F32: {
            u32 v_idx = in->imm.u / 16u;
            char comp = "xyzw"[(in->imm.u / 4u) & 3u];
            wb_putf(&wb, "mtk_u.data[%u].%c", v_idx, comp);
        } break;

        case IR_UNIFORM_I32: {
            u32 v_idx = in->imm.u / 16u;
            char comp = "xyzw"[(in->imm.u / 4u) & 3u];
            wb_putf(&wb, "bitcast<i32>(mtk_u.data[%u].%c)", v_idx, comp);
        } break;

        case IR_GLOBAL_ID:
            wb_puts(&wb, "i32(gid.x)");
            break;

        case IR_BUF_LOAD: {
            u32 slot    = IR_BUF_SLOT(in->imm.u);
            u32 comp    = IR_BUF_COMP(in->imm.u);
            b32 is_vec4 = IR_BUF_IS_VEC4(in->imm.u);
            if (atomic_mask & (1u << slot)) {
                wb_putf(&wb, "bitcast<i32>(atomicLoad(&buf_%u[u32(", slot);
                emit_operand(&wb, f, in->a);
                wb_puts(&wb, ")]))");
            } else if (is_vec4) {
                wb_putf(&wb, "buf_%u[u32(", slot);
                emit_operand(&wb, f, in->a);
                wb_putf(&wb, ")].%c", "xyzw"[comp & 3u]);
            } else if (elem_types[slot] == (u8)MTK_TYPE_U32) {
                wb_putf(&wb, "bitcast<i32>(buf_%u[u32(", slot);
                emit_operand(&wb, f, in->a);
                wb_puts(&wb, ")])");
            } else {
                wb_putf(&wb, "buf_%u[u32(", slot);
                emit_operand(&wb, f, in->a);
                wb_puts(&wb, ")]");
            }
        } break;

        case IR_ATOMIC_ADD:
            wb_putf(&wb, "bitcast<i32>(atomicAdd(&buf_%u[u32(", in->imm.u);
            emit_operand(&wb, f, in->a);
            wb_puts(&wb, ")], bitcast<u32>(");
            emit_operand(&wb, f, in->b);
            wb_puts(&wb, ")))");
            break;

        case IR_FADD:
        case IR_IADD:
            emit_operand(&wb, f, in->a);
            wb_puts(&wb, " + ");
            emit_operand(&wb, f, in->b);
            break;

        case IR_FSUB:
        case IR_ISUB:
            emit_operand(&wb, f, in->a);
            wb_puts(&wb, " - ");
            emit_operand(&wb, f, in->b);
            break;

        case IR_FMUL:
        case IR_IMUL:
            emit_operand(&wb, f, in->a);
            wb_puts(&wb, " * ");
            emit_operand(&wb, f, in->b);
            break;

        case IR_FDIV:
            emit_operand(&wb, f, in->a);
            wb_puts(&wb, " / ");
            emit_operand(&wb, f, in->b);
            break;

        case IR_FMIN:
        case IR_IMIN_S:
            wb_puts(&wb, "min(");
            emit_operand(&wb, f, in->a);
            wb_puts(&wb, ", ");
            emit_operand(&wb, f, in->b);
            wb_putc(&wb, ')');
            break;

        case IR_FMAX:
        case IR_IMAX_S:
            wb_puts(&wb, "max(");
            emit_operand(&wb, f, in->a);
            wb_puts(&wb, ", ");
            emit_operand(&wb, f, in->b);
            wb_putc(&wb, ')');
            break;

        case IR_FNEG:
            wb_puts(&wb, "-(");
            emit_operand(&wb, f, in->a);
            wb_putc(&wb, ')');
            break;

        case IR_FABS:
            wb_puts(&wb, "abs(");
            emit_operand(&wb, f, in->a);
            wb_putc(&wb, ')');
            break;

        case IR_FSQRT:
            wb_puts(&wb, "sqrt(");
            emit_operand(&wb, f, in->a);
            wb_putc(&wb, ')');
            break;

        case IR_FFLOOR:
            wb_puts(&wb, "floor(");
            emit_operand(&wb, f, in->a);
            wb_putc(&wb, ')');
            break;

        case IR_FCEIL:
            wb_puts(&wb, "ceil(");
            emit_operand(&wb, f, in->a);
            wb_putc(&wb, ')');
            break;

        case IR_FTRUNC:
            wb_puts(&wb, "trunc(");
            emit_operand(&wb, f, in->a);
            wb_putc(&wb, ')');
            break;

        case IR_FNEAREST:
            wb_puts(&wb, "round(");
            emit_operand(&wb, f, in->a);
            wb_putc(&wb, ')');
            break;

        case IR_IAND:
            emit_operand(&wb, f, in->a);
            wb_puts(&wb, " & ");
            emit_operand(&wb, f, in->b);
            break;

        case IR_IOR:
            emit_operand(&wb, f, in->a);
            wb_puts(&wb, " | ");
            emit_operand(&wb, f, in->b);
            break;

        case IR_IXOR:
            emit_operand(&wb, f, in->a);
            wb_puts(&wb, " ^ ");
            emit_operand(&wb, f, in->b);
            break;

        case IR_ISHL:
            emit_operand(&wb, f, in->a);
            wb_puts(&wb, " << (u32(");
            emit_operand(&wb, f, in->b);
            wb_puts(&wb, ") & 31u)");
            break;

        case IR_ISHR_S:
            emit_operand(&wb, f, in->a);
            wb_puts(&wb, " >> (u32(");
            emit_operand(&wb, f, in->b);
            wb_puts(&wb, ") & 31u)");
            break;

        case IR_ISHR_U:
            wb_puts(&wb, "bitcast<i32>(bitcast<u32>(");
            emit_operand(&wb, f, in->a);
            wb_puts(&wb, ") >> (u32(");
            emit_operand(&wb, f, in->b);
            wb_puts(&wb, ") & 31u))");
            break;

        case IR_FEQ:
        case IR_IEQ:
            emit_operand(&wb, f, in->a);
            wb_puts(&wb, " == ");
            emit_operand(&wb, f, in->b);
            break;

        case IR_FNE:
        case IR_INE:
            emit_operand(&wb, f, in->a);
            wb_puts(&wb, " != ");
            emit_operand(&wb, f, in->b);
            break;

        case IR_FLT:
        case IR_ILT_S:
            emit_operand(&wb, f, in->a);
            wb_puts(&wb, " < ");
            emit_operand(&wb, f, in->b);
            break;

        case IR_FLE:
        case IR_ILE_S:
            emit_operand(&wb, f, in->a);
            wb_puts(&wb, " <= ");
            emit_operand(&wb, f, in->b);
            break;

        case IR_FGT:
        case IR_IGT_S:
            emit_operand(&wb, f, in->a);
            wb_puts(&wb, " > ");
            emit_operand(&wb, f, in->b);
            break;

        case IR_FGE:
        case IR_IGE_S:
            emit_operand(&wb, f, in->a);
            wb_puts(&wb, " >= ");
            emit_operand(&wb, f, in->b);
            break;

        case IR_BAND:
            emit_operand(&wb, f, in->a);
            wb_puts(&wb, " && ");
            emit_operand(&wb, f, in->b);
            break;

        case IR_BOR:
            emit_operand(&wb, f, in->a);
            wb_puts(&wb, " || ");
            emit_operand(&wb, f, in->b);
            break;

        case IR_BNOT:
            wb_puts(&wb, "!(");
            emit_operand(&wb, f, in->a);
            wb_putc(&wb, ')');
            break;

        case IR_SELECT:
            wb_puts(&wb, "select(");
            emit_operand(&wb, f, in->c);
            wb_puts(&wb, ", ");
            emit_operand(&wb, f, in->b);
            wb_puts(&wb, ", ");
            emit_operand(&wb, f, in->a);
            wb_putc(&wb, ')');
            break;

        case IR_F2I:
            wb_puts(&wb, "i32(");
            emit_operand(&wb, f, in->a);
            wb_putc(&wb, ')');
            break;

        case IR_I2F:
            wb_puts(&wb, "f32(");
            emit_operand(&wb, f, in->a);
            wb_putc(&wb, ')');
            break;

        case IR_BITCAST_F2I:
            wb_puts(&wb, "bitcast<i32>(");
            emit_operand(&wb, f, in->a);
            wb_putc(&wb, ')');
            break;

        case IR_BITCAST_I2F:
            wb_puts(&wb, "bitcast<f32>(");
            emit_operand(&wb, f, in->a);
            wb_putc(&wb, ')');
            break;

        default:
            wgsl_fail(err, err_cap, "inst %%%u: beklenmeyen compute komutu %u", i, (u32)op);
            return 0;
        }

        wb_puts(&wb, ";\n");
    }

    wb_puts(&wb, "}\n");

    if (wb.overflow) {
        wgsl_fail(err, err_cap, "WGSL compute cikti tamponu kapasitesi (%u bayt) yetersiz", out_cap);
        out[0] = '\0';
        return 0;
    }

    return wb.len;
}

u32 mtk_wgsl_vert(const u8* attrs, u32 attr_count,
                  char* out, u32 out_cap, char* err, u32 err_cap) {
    if (err && err_cap > 0) err[0] = '\0';
    if (!out || out_cap == 0) {
        wgsl_fail(err, err_cap, "cikti tamponu gecersiz");
        return 0;
    }
    out[0] = '\0';

    if (attr_count > MTK_WGSL_MAX_ATTRS) {
        wgsl_fail(err, err_cap, "vertex attr_count (%u) MTK_WGSL_MAX_ATTRS (%u) sinirini asiyor",
                  attr_count, MTK_WGSL_MAX_ATTRS);
        return 0;
    }
    if (attr_count > 0 && !attrs) {
        wgsl_fail(err, err_cap, "attrs dizisi NULL olamaz");
        return 0;
    }

    b32 need_normal = FALSE;
    b32 need_uv     = FALSE;
    b32 need_color  = FALSE;

    for (u32 i = 0; i < attr_count; i++) {
        switch (attrs[i]) {
        case MTK_WGSL_ATTR_UV:         need_uv = TRUE; break;
        case MTK_WGSL_ATTR_NORMAL:     need_normal = TRUE; break;
        case MTK_WGSL_ATTR_VIEW_DEPTH: break;
        case MTK_WGSL_ATTR_COLOR:      need_color = TRUE; break;
        case MTK_WGSL_ATTR_WORLD_POS:  break;
        case MTK_WGSL_ATTR_SHADOW_POS: break;
        default:
            wgsl_fail(err, err_cap, "gecersiz vertex özniteligi attrs[%u] = %u", i, (u32)attrs[i]);
            return 0;
        }
    }

    WgslBuf wb;
    wb_init(&wb, out, out_cap);

    wb_putf(&wb, "// Matoki WGSL vertex shader (gfx3d varying sözleşmesi, attr_count=%u)\n\n", attr_count);
    wb_puts(&wb, "struct VsUniforms {\n"
                 "    mvp: mat4x4<f32>,\n"
                 "    model: mat4x4<f32>,\n"
                 "    normal_mat: mat4x4<f32>,\n"
                 "    shadow_mat: mat4x4<f32>,\n"
                 "};\n\n");
    wb_putf(&wb, "@group(%u) @binding(%u) var<uniform> vs_u: VsUniforms;\n\n",
            MTK_WGSL_VERT_GROUP, MTK_WGSL_VERT_UBO_BINDING);

    wb_puts(&wb, "struct VsIn {\n");
    wb_putf(&wb, "    @location(%u) pos: vec3<f32>,\n", MTK_WGSL_VS_LOC_POS);
    if (need_normal) {
        wb_putf(&wb, "    @location(%u) normal: vec3<f32>,\n", MTK_WGSL_VS_LOC_NORMAL);
    }
    if (need_uv) {
        wb_putf(&wb, "    @location(%u) uv: vec2<f32>,\n", MTK_WGSL_VS_LOC_UV);
    }
    if (need_color) {
        wb_putf(&wb, "    @location(%u) color: vec4<f32>,\n", MTK_WGSL_VS_LOC_COLOR);
    }
    wb_puts(&wb, "};\n\n");

    wb_puts(&wb, "struct VsOut {\n"
                 "    @builtin(position) clip_pos: vec4<f32>,\n");
    for (u32 i = 0; i < attr_count; i++) {
        u32 comps = g3d_attr_components((G3dAttr)attrs[i]);
        wb_putf(&wb, "    @location(%u) v_%u: %s,\n", i, i, comps_wgsl_f32_type(comps));
    }
    wb_puts(&wb, "};\n\n");

    wb_puts(&wb, "@vertex\n"
                 "fn vert(vin: VsIn) -> VsOut {\n"
                 "    var vout: VsOut;\n"
                 "    let pos4: vec4<f32> = vec4<f32>(vin.pos, 1.0);\n"
                 "    let clip: vec4<f32> = vs_u.mvp * pos4;\n"
                 "    vout.clip_pos = clip;\n");

    for (u32 i = 0; i < attr_count; i++) {
        switch (attrs[i]) {
        case MTK_WGSL_ATTR_UV:
            wb_putf(&wb, "    vout.v_%u = vin.uv;\n", i);
            break;
        case MTK_WGSL_ATTR_NORMAL:
            wb_putf(&wb, "    vout.v_%u = (vs_u.normal_mat * vec4<f32>(vin.normal, 0.0)).xyz;\n", i);
            break;
        case MTK_WGSL_ATTR_VIEW_DEPTH:
            wb_putf(&wb, "    vout.v_%u = clip.w;\n", i);
            break;
        case MTK_WGSL_ATTR_COLOR:
            wb_putf(&wb, "    vout.v_%u = vin.color;\n", i);
            break;
        case MTK_WGSL_ATTR_WORLD_POS:
            wb_putf(&wb, "    vout.v_%u = (vs_u.model * pos4).xyz;\n", i);
            break;
        case MTK_WGSL_ATTR_SHADOW_POS:
            wb_putf(&wb, "    vout.v_%u = (vs_u.shadow_mat * pos4).xyz;\n", i);
            break;
        default:
            break;
        }
    }

    wb_puts(&wb, "    return vout;\n}\n");

    if (wb.overflow) {
        wgsl_fail(err, err_cap, "WGSL vertex cikti tamponu kapasitesi (%u bayt) yetersiz", out_cap);
        out[0] = '\0';
        return 0;
    }

    return wb.len;
}

u32 mtk_compile_wgsl(const char* src, u32 len, const char* filename, const MtkWgslOpts* o,
                     Arena* a, char* out, u32 out_cap, MtkLayout* layout, MtkDiag* diag) {
    MtkLayout local_layout;
    if (!layout) layout = &local_layout;
    __builtin_memset(layout, 0, sizeof(*layout));

    IrFunc ir;
    __builtin_memset(&ir, 0, sizeof(ir));

    if (!mtk_compile_ir(src, len, filename, a, &ir, layout, diag)) {
        return 0;
    }

    ir_optimize(&ir);

    char err[256];
    err[0] = '\0';
    u32 n = mtk_wgsl_frag(&ir, layout, o, out, out_cap, err, sizeof(err));
    if (n == 0 && diag) {
        mtk_diag_error(diag, filename, src, len, 1, 1, "WGSL codegen hatasi: %s", err);
    }
    return n;
}
