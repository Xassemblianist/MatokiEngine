/* matoki/variant.c — Matoki shader varyant önbelleği ve Wasm döngü komutu sayacı (D hattı).
 * Kaynak hash'i, özelleştirilmiş uniform sabitleri, blend füzyon modu ve örnekleyici
 * bayraklarına göre derlenmiş modülleri açık adreslemeli tabloda saklar.
 */
#include "matoki/variant.h"
#include "matoki/parser.h"
#include "matoki/sema.h"
#include "matoki/lower.h"
#include "mbin/mbin.h"
#include "core/log.h"

static inline u32 v_f32_bits(f32 x) {
    u32 u;
    __builtin_memcpy(&u, &x, 4);
    return u;
}

static inline u64 fnv1a_u64(u64 h, const void* data, u32 len) {
    const u8* p = (const u8*)data;
    for (u32 i = 0; i < len; i++) {
        h ^= (u64)p[i];
        h *= 1099511628211ull;
    }
    return h;
}

static inline u64 fnv1a_u32(u64 h, u32 v) {
    return fnv1a_u64(h, &v, 4);
}

u64 mtk_hash_source(const char* src, u32 len) {
    u64 h = 14695981039346656037ull;
    if (src && len > 0) {
        h = fnv1a_u64(h, src, len);
    }
    return h;
}

void mtk_variant_cache_init(MtkVariantCache* cache, Arena* persist) {
    ASSERT(cache != NULL && persist != NULL);
    __builtin_memset(cache, 0, sizeof(*cache));
    cache->persist = persist;
}

static void canonicalize_specs(MtkSpecUniform* out, u32* out_count,
                               const MtkSpecUniform* in, u32 in_count) {
    u32 n = in_count > MTK_VARIANT_MAX_SPECS ? MTK_VARIANT_MAX_SPECS : in_count;
    for (u32 i = 0; i < n; i++) {
        out[i] = in[i];
    }
    /* byte_offset'e göre küçükten büyüğe sırala */
    for (u32 i = 1; i < n; i++) {
        MtkSpecUniform cur = out[i];
        u32 j = i;
        while (j > 0 && out[j - 1].byte_offset > cur.byte_offset) {
            out[j] = out[j - 1];
            j--;
        }
        out[j] = cur;
    }
    *out_count = n;
}

static u64 compute_key_hash(u64 src_hash, u32 src_len,
                            const MtkSpecUniform* specs, u32 spec_count,
                            const MtkCodegenOpts* cg_opts,
                            u32 sampler_mask, const u32 sampler_flags[SHADE_MAX_TEXTURES]) {
    u64 h = 14695981039346656037ull;
    h = fnv1a_u64(h, &src_hash, 8);
    h = fnv1a_u32(h, src_len);
    h = fnv1a_u32(h, spec_count);
    for (u32 i = 0; i < spec_count; i++) {
        h = fnv1a_u32(h, specs[i].byte_offset);
        h = fnv1a_u32(h, v_f32_bits(specs[i].value));
    }
    h = fnv1a_u32(h, cg_opts->shared_memory ? 1u : 0u);
    h = fnv1a_u32(h, cg_opts->own_memory ? 1u : 0u);
    h = fnv1a_u32(h, cg_opts->fuse_blend);
    h = fnv1a_u32(h, sampler_mask);
    for (u32 s = 0; s < SHADE_MAX_TEXTURES; s++) {
        if (sampler_mask & (1u << s)) {
            h = fnv1a_u32(h, sampler_flags[s]);
        }
    }
    return h;
}

static b32 entry_matches(const MtkVariant* e, u64 key_hash, u64 src_hash, u32 src_len,
                         const MtkSpecUniform* specs, u32 spec_count,
                         const MtkCodegenOpts* cg_opts,
                         u32 sampler_mask, const u32 sampler_flags[SHADE_MAX_TEXTURES]) {
    if (!e->occupied) return FALSE;
    if (e->key_hash != key_hash || e->src_hash != src_hash || e->src_len != src_len) return FALSE;
    if (e->spec_count != spec_count) return FALSE;
    if ((e->cg_opts.shared_memory != 0) != (cg_opts->shared_memory != 0)) return FALSE;
    if ((e->cg_opts.own_memory != 0) != (cg_opts->own_memory != 0)) return FALSE;
    if (e->cg_opts.fuse_blend != cg_opts->fuse_blend) return FALSE;
    if (e->sampler_mask != sampler_mask) return FALSE;

    for (u32 i = 0; i < spec_count; i++) {
        if (e->specs[i].byte_offset != specs[i].byte_offset) return FALSE;
        if (v_f32_bits(e->specs[i].value) != v_f32_bits(specs[i].value)) return FALSE;
    }
    for (u32 s = 0; s < SHADE_MAX_TEXTURES; s++) {
        if (sampler_mask & (1u << s)) {
            if (e->sampler_flags[s] != sampler_flags[s]) return FALSE;
        }
    }
    return TRUE;
}

const MtkVariant* mtk_variant_get(MtkVariantCache* cache, const MtkVariantKey* key,
                                  Arena* scratch, MtkDiag* diag) {
    ASSERT(cache != NULL && key != NULL && scratch != NULL);
    if (diag) mtk_diag_init(diag);

    u64 src_hash = key->src_hash ? key->src_hash : mtk_hash_source(key->src, key->src_len);
    MtkSpecUniform specs[MTK_VARIANT_MAX_SPECS];
    u32 spec_count = 0;
    canonicalize_specs(specs, &spec_count, key->specs, key->spec_count);

    u64 key_hash = compute_key_hash(src_hash, key->src_len, specs, spec_count,
                                    &key->cg_opts, key->sampler_mask, key->sampler_flags);

    u32 mask = MTK_VARIANT_CACHE_CAP - 1u;
    u32 start = (u32)key_hash & mask;
    u32 free_slot = MTK_VARIANT_CACHE_CAP;

    for (u32 probe = 0; probe < MTK_VARIANT_CACHE_CAP; probe++) {
        u32 idx = (start + probe) & mask;
        const MtkVariant* e = &cache->entries[idx];
        if (!e->occupied) {
            if (free_slot == MTK_VARIANT_CACHE_CAP) free_slot = idx;
            break;
        }
        if (entry_matches(e, key_hash, src_hash, key->src_len, specs, spec_count,
                          &key->cg_opts, key->sampler_mask, key->sampler_flags)) {
            cache->hits++;
            return e;
        }
    }

    cache->misses++;
    if (free_slot == MTK_VARIANT_CACHE_CAP) {
        /* Tablo doluysa başlangıç slotunun üzerine yaz (eviction) */
        free_slot = start;
    } else {
        cache->count++;
    }

    usize mark = arena_mark(scratch);
    const char* fname = key->filename ? key->filename : "<variant>";

    MtkLayout layout;
    __builtin_memset(&layout, 0, sizeof(layout));
    AstProgram prog;
    __builtin_memset(&prog, 0, sizeof(prog));

    if (!parser_parse(key->src, key->src_len, fname, scratch, &prog, diag)) {
        arena_pop_to(scratch, mark);
        return NULL;
    }
    if (!sema_check(&prog, key->src, key->src_len, fname, scratch, &layout, diag)) {
        arena_pop_to(scratch, mark);
        return NULL;
    }

    /* Örnekleyici bayrağı geçersiz kılma (sampler_mask) */
    if (key->sampler_mask != 0) {
        for (u32 i = 0; i < prog.decl_count; i++) {
            AstDecl* d = prog.decls[i];
            if (d->kind == AST_DECL_UNIFORM && mtk_type_is_texture(d->type)) {
                u32 slot = d->as.uniform_decl.offset_or_slot;
                if (slot < SHADE_MAX_TEXTURES && (key->sampler_mask & (1u << slot))) {
                    u32 f32_bit = (d->type == MTK_TYPE_TEXTURE2D_F32) ? TEX_FLAG_F32 : 0u;
                    d->as.uniform_decl.tex_flags = key->sampler_flags[slot] | f32_bit;
                }
            }
        }
    }

    IrFunc ir;
    __builtin_memset(&ir, 0, sizeof(ir));
    if (!lower_ast(&prog, &layout, scratch, &ir)) {
        arena_pop_to(scratch, mark);
        return NULL;
    }

    /* Sabitlenmiş uniform değerlerini IR'a göm */
    for (u32 i = 0; i < spec_count; i++) {
        ir_specialize_uniform_f32(&ir, specs[i].byte_offset, specs[i].value);
    }

    ir_optimize(&ir);

    u32 tmp_cap = 65536u;
    u8* tmp_wasm = ARENA_PUSH_ARRAY(scratch, u8, tmp_cap);
    char err[256];
    err[0] = '\0';
    u32 wasm_len = mtk_codegen(&ir, &key->cg_opts, scratch, tmp_wasm, tmp_cap, err, sizeof(err));
    if (wasm_len == 0) {
        if (diag) {
            mtk_diag_error(diag, fname, key->src, key->src_len, 1, 1, "codegen hatasi: %s", err);
        }
        arena_pop_to(scratch, mark);
        return NULL;
    }

    u32 ir_inst_count   = ir.count;
    u32 loop_inst_count = ir_count_loop_insts(&ir);
    u32 wasm_loop_ops   = mtk_wasm_count_loop_ops(tmp_wasm, wasm_len);

    u8* persist_wasm = ARENA_PUSH_ARRAY(cache->persist, u8, wasm_len);
    __builtin_memcpy(persist_wasm, tmp_wasm, wasm_len);

    arena_pop_to(scratch, mark);

    MtkVariant* dst = &cache->entries[free_slot];
    __builtin_memset(dst, 0, sizeof(*dst));
    dst->occupied        = TRUE;
    dst->key_hash        = key_hash;
    dst->src_hash        = src_hash;
    dst->src_len         = key->src_len;
    dst->spec_count      = spec_count;
    for (u32 i = 0; i < spec_count; i++) dst->specs[i] = specs[i];
    dst->cg_opts         = key->cg_opts;
    dst->sampler_mask    = key->sampler_mask;
    for (u32 s = 0; s < SHADE_MAX_TEXTURES; s++) dst->sampler_flags[s] = key->sampler_flags[s];
    dst->wasm            = persist_wasm;
    dst->wasm_len        = wasm_len;
    dst->ir_inst_count   = ir_inst_count;
    dst->loop_inst_count = loop_inst_count;
    dst->wasm_loop_ops   = wasm_loop_ops;
    dst->layout          = layout;
    return dst;
}

/* Üretilen wasm ikilisinin Code bölümündeki loop..end içi komut sayısını sayar */
u32 mtk_wasm_count_loop_ops(const u8* wasm, u32 wasm_len) {
    if (!wasm || wasm_len < 8) return 0;
    u32 pos = 8;
    while (pos < wasm_len) {
        u8 sec_id = wasm[pos++];
        u32 nread = 0;
        u32 sec_len = (u32)mb_decode_uleb(wasm + pos, wasm_len - pos, &nread);
        pos += nread;
        u32 sec_end = pos + sec_len;
        if (sec_end > wasm_len) return 0;

        if (sec_id == MB_SEC_CODE) {
            u32 func_count = (u32)mb_decode_uleb(wasm + pos, sec_end - pos, &nread);
            pos += nread;
            if (func_count == 0) return 0;
            u32 body_size = (u32)mb_decode_uleb(wasm + pos, sec_end - pos, &nread);
            pos += nread;
            u32 body_end = pos + body_size;
            if (body_end > sec_end) return 0;

            u32 local_groups = (u32)mb_decode_uleb(wasm + pos, body_end - pos, &nread);
            pos += nread;
            for (u32 g = 0; g < local_groups; g++) {
                (void)mb_decode_uleb(wasm + pos, body_end - pos, &nread);
                pos += nread + 1; /* count + valtype */
            }

            u8 is_loop_blk[64];
            u32 blk_depth = 0;
            u32 loop_depth = 0;
            u32 loop_ops = 0;

            while (pos < body_end) {
                u8 op = wasm[pos++];
                if (op == 0x0Bu) { /* end */
                    if (blk_depth == 0) break;
                    blk_depth--;
                    if (is_loop_blk[blk_depth]) {
                        loop_depth--;
                    }
                    continue;
                }
                if (op == 0x05u) { /* else */
                    if (loop_depth > 0) loop_ops++;
                    continue;
                }
                if (op == 0x02u || op == 0x03u || op == 0x04u) {
                    pos++; /* blocktype */
                    if (op == 0x03u) {
                        if (loop_depth > 0) loop_ops++;
                        is_loop_blk[blk_depth++] = 1;
                        loop_depth++;
                    } else {
                        if (loop_depth > 0) loop_ops++;
                        is_loop_blk[blk_depth++] = 0;
                    }
                    continue;
                }

                if (loop_depth > 0) loop_ops++;

                if (op == 0x0Cu || op == 0x0Du || op == 0x10u ||
                    (op >= 0x20u && op <= 0x24u)) {
                    (void)mb_decode_uleb(wasm + pos, body_end - pos, &nread);
                    pos += nread;
                } else if (op == 0x11u) {
                    (void)mb_decode_uleb(wasm + pos, body_end - pos, &nread);
                    pos += nread + 1;
                } else if (op >= 0x28u && op <= 0x3Bu) {
                    (void)mb_decode_uleb(wasm + pos, body_end - pos, &nread);
                    pos += nread;
                    (void)mb_decode_uleb(wasm + pos, body_end - pos, &nread);
                    pos += nread;
                } else if (op == 0x41u || op == 0x42u) {
                    (void)mb_decode_sleb(wasm + pos, body_end - pos, &nread);
                    pos += nread;
                } else if (op == 0x43u) {
                    pos += 4;
                } else if (op == 0x44u) {
                    pos += 8;
                } else if (op == 0xFCu) {
                    (void)mb_decode_uleb(wasm + pos, body_end - pos, &nread);
                    pos += nread;
                } else if (op == 0xFDu) {
                    u32 subop = (u32)mb_decode_uleb(wasm + pos, body_end - pos, &nread);
                    pos += nread;
                    if (subop == 0x00u || subop == 0x0Bu) {
                        (void)mb_decode_uleb(wasm + pos, body_end - pos, &nread);
                        pos += nread;
                        (void)mb_decode_uleb(wasm + pos, body_end - pos, &nread);
                        pos += nread;
                    } else if (subop == 0x0Cu || subop == 0x0Du) {
                        pos += 16;
                    } else if (subop >= 0x15u && subop <= 0x22u) {
                        pos += 1;
                    }
                }
            }
            return loop_ops;
        }
        pos = sec_end;
    }
    return 0;
}
