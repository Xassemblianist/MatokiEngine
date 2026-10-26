/* mbin/mbin.c — WebAssembly ikili (binary) modül üretici implementasyonu.
 * Bayt tamponu, LEB128 kodlama/çözme ve WebAssembly Core Spec §5'e uygun
 * bölüm (section) serileştiricisini içerir.
 * libc ve malloc kullanılmaz; tüm bellek çağıran tarafından sağlanır.
 */
#include "mbin/mbin.h"

static u32 mb_cstrlen(const char* s) {
    u32 n = 0;
    if (!s) return 0;
    while (s[n] != '\0') n++;
    return n;
}

static void mb_memzero(void* dst, usize len) {
    u8* p = (u8*)dst;
    for (usize i = 0; i < len; i++) {
        p[i] = 0;
    }
}

static void mb_memcpy(void* dst, const void* src, usize len) {
    u8* d = (u8*)dst;
    const u8* s = (const u8*)src;
    for (usize i = 0; i < len; i++) {
        d[i] = s[i];
    }
}

void mb_buf_init(MbBuf* b, u8* mem, u32 cap) {
    ASSERT(b != NULL);
    ASSERT(mem != NULL || cap == 0);
    b->p = mem;
    b->len = 0;
    b->cap = cap;
}

void mb_u8(MbBuf* b, u8 v) {
    if (b->len >= b->cap) {
        PANIC("mb_u8: buffer overflow (len=%u, cap=%u)", b->len, b->cap);
    }
    b->p[b->len++] = v;
}

void mb_bytes(MbBuf* b, const void* src, u32 len) {
    if (len == 0) return;
    if (b->len > b->cap || len > b->cap - b->len) {
        PANIC("mb_bytes: buffer overflow (len=%u, add=%u, cap=%u)", b->len, len, b->cap);
    }
    mb_memcpy(b->p + b->len, src, len);
    b->len += len;
}

void mb_uleb(MbBuf* b, u64 v) {
    do {
        u8 byte = (u8)(v & 0x7Fu);
        v >>= 7;
        if (v != 0) {
            byte |= 0x80u;
        }
        mb_u8(b, byte);
    } while (v != 0);
}

void mb_sleb(MbBuf* b, i64 v) {
    b32 more = TRUE;
    while (more) {
        u8 byte = (u8)((u64)v & 0x7Fu);
        v >>= 7;
        if ((v == 0 && (byte & 0x40u) == 0) ||
            (v == -1 && (byte & 0x40u) != 0)) {
            more = FALSE;
        } else {
            byte |= 0x80u;
        }
        mb_u8(b, byte);
    }
}

void mb_f32(MbBuf* b, f32 v) {
    union { f32 f; u32 u; } conv;
    conv.f = v;
    u8 raw[4];
    raw[0] = (u8)(conv.u & 0xFFu);
    raw[1] = (u8)((conv.u >> 8) & 0xFFu);
    raw[2] = (u8)((conv.u >> 16) & 0xFFu);
    raw[3] = (u8)((conv.u >> 24) & 0xFFu);
    mb_bytes(b, raw, 4);
}

void mb_f64(MbBuf* b, f64 v) {
    union { f64 f; u64 u; } conv;
    conv.f = v;
    u8 raw[8];
    for (u32 i = 0; i < 8; i++) {
        raw[i] = (u8)((conv.u >> (i * 8)) & 0xFFu);
    }
    mb_bytes(b, raw, 8);
}

void mb_name(MbBuf* b, const char* str, u32 len) {
    mb_uleb(b, (u64)len);
    mb_bytes(b, str, len);
}

u64 mb_decode_uleb(const u8* p, u32 max_len, u32* out_bytes_read) {
    u64 result = 0;
    u32 shift = 0;
    u32 idx = 0;
    while (TRUE) {
        if (idx >= max_len || shift >= 64) {
            PANIC("mb_decode_uleb: invalid or truncated ULEB128");
        }
        u8 byte = p[idx++];
        result |= (u64)(byte & 0x7Fu) << shift;
        if ((byte & 0x80u) == 0) {
            break;
        }
        shift += 7;
    }
    if (out_bytes_read) {
        *out_bytes_read = idx;
    }
    return result;
}

i64 mb_decode_sleb(const u8* p, u32 max_len, u32* out_bytes_read) {
    u64 result = 0;
    u32 shift = 0;
    u32 idx = 0;
    u8 byte = 0;
    while (TRUE) {
        if (idx >= max_len || shift >= 64) {
            PANIC("mb_decode_sleb: invalid or truncated SLEB128");
        }
        byte = p[idx++];
        result |= (u64)(byte & 0x7Fu) << shift;
        shift += 7;
        if ((byte & 0x80u) == 0) {
            break;
        }
    }
    if (shift < 64 && (byte & 0x40u) != 0) {
        result |= (~(u64)0) << shift;
    }
    if (out_bytes_read) {
        *out_bytes_read = idx;
    }
    return (i64)result;
}

void mb_module_init(MbModule* m, u8* work_mem, u32 work_cap) {
    ASSERT(m != NULL);
    ASSERT(work_mem != NULL && work_cap > 0);
    mb_memzero(m, sizeof(MbModule));
    m->work_mem = work_mem;
    m->work_cap = work_cap;
    m->work_used = 0;
}

u32 mb_type_add(MbModule* m, const u8* params, u32 np, const u8* results, u32 nr) {
    ASSERT(m != NULL);
    if (np > MB_MAX_PARAMS || nr > MB_MAX_RESULTS) {
        PANIC("mb_type_add: signature too large (np=%u, nr=%u)", np, nr);
    }
    if (m->type_count >= MB_MAX_TYPES) {
        PANIC("mb_type_add: too many types (max=%u)", MB_MAX_TYPES);
    }
    u32 idx = m->type_count++;
    MbFuncType* t = &m->types[idx];
    t->param_count = np;
    t->result_count = nr;
    if (np > 0) mb_memcpy(t->params, params, np);
    if (nr > 0) mb_memcpy(t->results, results, nr);
    return idx;
}

u32 mb_type(MbModule* m, const u8* params, u32 np, const u8* results, u32 nr) {
    ASSERT(m != NULL);
    if (np > MB_MAX_PARAMS || nr > MB_MAX_RESULTS) {
        PANIC("mb_type: signature too large (np=%u, nr=%u)", np, nr);
    }

    /* Tekrar eden imzaları tekilleştir */
    for (u32 i = 0; i < m->type_count; i++) {
        const MbFuncType* t = &m->types[i];
        if (t->param_count != np || t->result_count != nr) continue;
        b32 match = TRUE;
        for (u32 j = 0; j < np; j++) {
            if (t->params[j] != params[j]) { match = FALSE; break; }
        }
        if (!match) continue;
        for (u32 j = 0; j < nr; j++) {
            if (t->results[j] != results[j]) { match = FALSE; break; }
        }
        if (match) return i;
    }

    return mb_type_add(m, params, np, results, nr);
}

static MbImport* mb_add_import(MbModule* m, const char* mod, const char* name, u8 kind) {
    ASSERT(m != NULL);
    if (m->import_count >= MB_MAX_IMPORTS) {
        PANIC("mb_add_import: too many imports (max=%u)", MB_MAX_IMPORTS);
    }
    u32 mod_len = mb_cstrlen(mod);
    u32 name_len = mb_cstrlen(name);
    if (mod_len >= MB_MAX_NAME_LEN || name_len >= MB_MAX_NAME_LEN) {
        PANIC("mb_add_import: import name too long");
    }
    MbImport* imp = &m->imports[m->import_count++];
    mb_memzero(imp, sizeof(MbImport));
    mb_memcpy(imp->mod_name, mod, mod_len);
    imp->mod_len = mod_len;
    mb_memcpy(imp->field_name, name, name_len);
    imp->field_len = name_len;
    imp->kind = kind;
    return imp;
}

u32 mb_import_func(MbModule* m, const char* mod, const char* name, u32 type_idx) {
    ASSERT(m != NULL);
    if (m->func_count > 0) {
        PANIC("mb_import_func: function imports must be added before defined functions");
    }
    if (type_idx >= m->type_count) {
        PANIC("mb_import_func: invalid type_idx %u", type_idx);
    }
    MbImport* imp = mb_add_import(m, mod, name, MB_EXT_FUNC);
    imp->type_idx = type_idx;
    return m->imported_func_count++;
}

void mb_import_memory(MbModule* m, const char* mod, const char* name,
                      u32 min_pages, u32 max_pages, b32 shared) {
    MbImport* imp = mb_add_import(m, mod, name, MB_EXT_MEMORY);
    imp->min_pages = min_pages;
    imp->max_pages = max_pages;
    imp->has_max = (max_pages > 0 || shared != 0) ? TRUE : FALSE;
    imp->shared = shared ? TRUE : FALSE;
}

void mb_import_table(MbModule* m, const char* mod, const char* name, u32 min_elems) {
    mb_import_table_limits(m, mod, name, min_elems, 0, FALSE);
}

void mb_import_table_limits(MbModule* m, const char* mod, const char* name,
                            u32 min_elems, u32 max_elems, b32 has_max) {
    MbImport* imp = mb_add_import(m, mod, name, MB_EXT_TABLE);
    imp->min_pages = min_elems;
    imp->max_pages = max_elems;
    imp->has_max = has_max ? TRUE : FALSE;
    imp->shared = FALSE;
}

u32 mb_import_global(MbModule* m, const char* mod, const char* name,
                     u8 valtype, b32 mutable_) {
    ASSERT(m != NULL);
    if (m->global_count > 0) {
        PANIC("mb_import_global: global imports must be added before defined globals");
    }
    MbImport* imp = mb_add_import(m, mod, name, MB_EXT_GLOBAL);
    imp->valtype = valtype;
    imp->mutable_ = mutable_ ? TRUE : FALSE;
    return m->imported_global_count++;
}

void mb_table(MbModule* m, u32 min_elems, u32 max_elems, b32 has_max) {
    ASSERT(m != NULL);
    m->has_defined_table = TRUE;
    m->table_min_elems = min_elems;
    m->table_max_elems = max_elems;
    m->table_has_max = has_max ? TRUE : FALSE;
}

void mb_memory(MbModule* m, u32 min_pages, u32 max_pages, b32 shared) {
    ASSERT(m != NULL);
    m->has_defined_memory = TRUE;
    m->mem_min_pages = min_pages;
    m->mem_max_pages = max_pages;
    m->mem_has_max = (max_pages > 0 || shared != 0) ? TRUE : FALSE;
    m->mem_shared = shared ? TRUE : FALSE;
}

static MbGlobal* mb_add_global(MbModule* m, u8 valtype, b32 mutable_) {
    ASSERT(m != NULL);
    if (m->global_count >= MB_MAX_GLOBALS) {
        PANIC("mb_add_global: too many globals (max=%u)", MB_MAX_GLOBALS);
    }
    MbGlobal* g = &m->globals[m->global_count++];
    mb_memzero(g, sizeof(MbGlobal));
    g->valtype = valtype;
    g->mutable_ = mutable_ ? TRUE : FALSE;
    g->init_type = valtype;
    return g;
}

u32 mb_global_i32(MbModule* m, b32 mutable_, i32 init_val) {
    MbGlobal* g = mb_add_global(m, MB_VAL_I32, mutable_);
    g->init_i64 = (i64)init_val;
    return m->imported_global_count + m->global_count - 1;
}

u32 mb_global_i64(MbModule* m, b32 mutable_, i64 init_val) {
    MbGlobal* g = mb_add_global(m, MB_VAL_I64, mutable_);
    g->init_i64 = init_val;
    return m->imported_global_count + m->global_count - 1;
}

u32 mb_global_f32(MbModule* m, b32 mutable_, f32 init_val) {
    MbGlobal* g = mb_add_global(m, MB_VAL_F32, mutable_);
    g->init_f32 = init_val;
    return m->imported_global_count + m->global_count - 1;
}

u32 mb_global_f64(MbModule* m, b32 mutable_, f64 init_val) {
    MbGlobal* g = mb_add_global(m, MB_VAL_F64, mutable_);
    g->init_f64 = init_val;
    return m->imported_global_count + m->global_count - 1;
}

u32 mb_global_v128(MbModule* m, b32 mutable_, const u8 init_bytes[16]) {
    MbGlobal* g = mb_add_global(m, MB_VAL_V128, mutable_);
    mb_memcpy(g->init_v128, init_bytes, 16);
    return m->imported_global_count + m->global_count - 1;
}

MbFunc* mb_func_begin(MbModule* m, u32 type_idx) {
    ASSERT(m != NULL);
    if (m->func_active) {
        PANIC("mb_func_begin: previous function was not ended with mb_func_end");
    }
    if (type_idx >= m->type_count) {
        PANIC("mb_func_begin: invalid type_idx %u", type_idx);
    }
    if (m->func_count >= MB_MAX_FUNCS) {
        PANIC("mb_func_begin: too many functions (max=%u)", MB_MAX_FUNCS);
    }
    if (m->work_used >= m->work_cap) {
        PANIC("mb_func_begin: work memory exhausted");
    }

    u32 def_idx = m->func_count++;
    MbFunc* f = &m->funcs[def_idx];
    mb_memzero(f, sizeof(MbFunc));
    f->mod = m;
    f->type_idx = type_idx;
    f->func_idx = m->imported_func_count + def_idx;
    f->next_local_idx = m->types[type_idx].param_count;
    mb_buf_init(&f->code, m->work_mem + m->work_used, m->work_cap - m->work_used);
    f->depth = 0;
    f->ended = FALSE;
    m->func_active = TRUE;
    return f;
}

u32 mb_local(MbFunc* f, u8 valtype, u32 count) {
    ASSERT(f != NULL);
    if (f->ended) {
        PANIC("mb_local: cannot add locals after mb_func_end");
    }
    if (count == 0) {
        return f->next_local_idx;
    }
    u32 first_idx = f->next_local_idx;
    f->next_local_idx += count;

    /* Aynı tipte ardışık local gruplarını birleştir */
    if (f->local_group_count > 0 &&
        f->locals[f->local_group_count - 1].valtype == valtype) {
        f->locals[f->local_group_count - 1].count += count;
        return first_idx;
    }

    if (f->local_group_count >= MB_MAX_LOCAL_GROUPS) {
        PANIC("mb_local: too many local groups (max=%u)", MB_MAX_LOCAL_GROUPS);
    }
    MbLocalGroup* g = &f->locals[f->local_group_count++];
    g->count = count;
    g->valtype = valtype;
    return first_idx;
}

void mb_func_end(MbFunc* f) {
    ASSERT(f != NULL && f->mod != NULL);
    if (f->ended) {
        PANIC("mb_func_end: function already ended");
    }
    if (f->depth != 0) {
        PANIC("mb_func_end: unclosed control block (depth=%u)", f->depth);
    }
    /* Fonksiyon gövdesi sonlandırıcı 'end' (0x0B) opcode'u */
    mb_u8(&f->code, 0x0Bu);
    f->ended = TRUE;
    f->mod->work_used += f->code.len;
    f->mod->func_active = FALSE;
}

void mb_export(MbModule* m, const char* name, u8 kind, u32 index) {
    ASSERT(m != NULL);
    if (m->export_count >= MB_MAX_EXPORTS) {
        PANIC("mb_export: too many exports (max=%u)", MB_MAX_EXPORTS);
    }
    u32 name_len = mb_cstrlen(name);
    if (name_len == 0 || name_len >= MB_MAX_NAME_LEN) {
        PANIC("mb_export: invalid export name length %u", name_len);
    }
    MbExport* exp = &m->exports[m->export_count++];
    mb_memcpy(exp->name, name, name_len);
    exp->name[name_len] = '\0';
    exp->name_len = name_len;
    exp->kind = kind;
    exp->index = index;
}

void mb_export_func(MbModule* m, const char* name, u32 func_idx) {
    mb_export(m, name, MB_EXT_FUNC, func_idx);
}

void mb_elem(MbModule* m, i32 offset, const u32* func_indices, u32 count) {
    ASSERT(m != NULL);
    if (m->elem_count >= MB_MAX_ELEMS) {
        PANIC("mb_elem: too many elem segments (max=%u)", MB_MAX_ELEMS);
    }
    if (count > MB_MAX_ELEM_FUNCS) {
        PANIC("mb_elem: too many functions in elem segment (count=%u, max=%u)", count, MB_MAX_ELEM_FUNCS);
    }
    MbElem* el = &m->elems[m->elem_count++];
    el->offset = offset;
    el->count = count;
    for (u32 i = 0; i < count; i++) {
        el->func_indices[i] = func_indices[i];
    }
}

void mb_data(MbModule* m, i32 offset, const u8* bytes, u32 len) {
    ASSERT(m != NULL);
    if (m->func_active) {
        PANIC("mb_data: cannot add data while a function is active");
    }
    if (m->data_count >= MB_MAX_DATAS) {
        PANIC("mb_data: too many data segments (max=%u)", MB_MAX_DATAS);
    }
    if (len > m->work_cap - m->work_used) {
        PANIC("mb_data: work memory exhausted");
    }
    MbData* d = &m->datas[m->data_count++];
    d->offset = offset;
    d->data = m->work_mem + m->work_used;
    d->len = len;
    if (len > 0) {
        mb_memcpy(d->data, bytes, len);
        m->work_used += len;
    }
}

static void mb_emit_section(MbBuf* out, u8 section_id, const MbBuf* sec) {
    mb_u8(out, section_id);
    mb_uleb(out, (u64)sec->len);
    mb_bytes(out, sec->p, sec->len);
}

static void mb_emit_limits(MbBuf* sec, u32 min_val, u32 max_val, b32 has_max, b32 shared) {
    u8 flags = 0;
    if (has_max) flags |= 0x01u;
    if (shared)  flags |= 0x03u; /* paylaşımlı bellek her zaman üst sınır içerir */
    mb_u8(sec, flags);
    mb_uleb(sec, (u64)min_val);
    if (flags & 0x01u) {
        mb_uleb(sec, (u64)max_val);
    }
}

u32 mb_module_finish(MbModule* m, MbBuf* out_buf) {
    ASSERT(m != NULL && out_buf != NULL);
    if (m->func_active) {
        PANIC("mb_module_finish: active function was not ended");
    }
    if (m->work_used >= m->work_cap) {
        PANIC("mb_module_finish: no scratch space remaining in work_mem");
    }

    u32 start_len = out_buf->len;

    /* 1. WebAssembly sihirli başlığı (\0asm) ve sürüm (1) */
    static const u8 wasm_header[8] = {
        0x00u, 0x61u, 0x73u, 0x6Du,
        0x01u, 0x00u, 0x00u, 0x00u
    };
    mb_bytes(out_buf, wasm_header, 8);

    /* Bölüm içerikleri için kalan çalışma belleğini geçici tampon olarak kullan */
    MbBuf sec;
    mb_buf_init(&sec, m->work_mem + m->work_used, m->work_cap - m->work_used);

    /* Section 1: Type */
    if (m->type_count > 0) {
        sec.len = 0;
        mb_uleb(&sec, (u64)m->type_count);
        for (u32 i = 0; i < m->type_count; i++) {
            const MbFuncType* t = &m->types[i];
            mb_u8(&sec, 0x60u);
            mb_uleb(&sec, (u64)t->param_count);
            mb_bytes(&sec, t->params, t->param_count);
            mb_uleb(&sec, (u64)t->result_count);
            mb_bytes(&sec, t->results, t->result_count);
        }
        mb_emit_section(out_buf, MB_SEC_TYPE, &sec);
    }

    /* Section 2: Import */
    if (m->import_count > 0) {
        sec.len = 0;
        mb_uleb(&sec, (u64)m->import_count);
        for (u32 i = 0; i < m->import_count; i++) {
            const MbImport* imp = &m->imports[i];
            mb_name(&sec, imp->mod_name, imp->mod_len);
            mb_name(&sec, imp->field_name, imp->field_len);
            mb_u8(&sec, imp->kind);
            if (imp->kind == MB_EXT_FUNC) {
                mb_uleb(&sec, (u64)imp->type_idx);
            } else if (imp->kind == MB_EXT_TABLE) {
                mb_u8(&sec, MB_FUNCREF);
                mb_emit_limits(&sec, imp->min_pages, imp->max_pages, imp->has_max, FALSE);
            } else if (imp->kind == MB_EXT_MEMORY) {
                mb_emit_limits(&sec, imp->min_pages, imp->max_pages, imp->has_max, imp->shared);
            } else if (imp->kind == MB_EXT_GLOBAL) {
                mb_u8(&sec, imp->valtype);
                mb_u8(&sec, imp->mutable_ ? 0x01u : 0x00u);
            } else {
                PANIC("mb_module_finish: unsupported import kind %u", imp->kind);
            }
        }
        mb_emit_section(out_buf, MB_SEC_IMPORT, &sec);
    }

    /* Section 3: Function */
    if (m->func_count > 0) {
        sec.len = 0;
        mb_uleb(&sec, (u64)m->func_count);
        for (u32 i = 0; i < m->func_count; i++) {
            mb_uleb(&sec, (u64)m->funcs[i].type_idx);
        }
        mb_emit_section(out_buf, MB_SEC_FUNC, &sec);
    }

    /* Section 4: Table */
    if (m->has_defined_table) {
        sec.len = 0;
        mb_uleb(&sec, 1u);
        mb_u8(&sec, MB_FUNCREF);
        mb_emit_limits(&sec, m->table_min_elems, m->table_max_elems, m->table_has_max, FALSE);
        mb_emit_section(out_buf, MB_SEC_TABLE, &sec);
    }

    /* Section 5: Memory (eğer modül kendi belleğini tanımlıyorsa) */
    if (m->has_defined_memory) {
        sec.len = 0;
        mb_uleb(&sec, 1u);
        mb_emit_limits(&sec, m->mem_min_pages, m->mem_max_pages, m->mem_has_max, m->mem_shared);
        mb_emit_section(out_buf, MB_SEC_MEMORY, &sec);
    }

    /* Section 6: Global */
    if (m->global_count > 0) {
        sec.len = 0;
        mb_uleb(&sec, (u64)m->global_count);
        for (u32 i = 0; i < m->global_count; i++) {
            const MbGlobal* g = &m->globals[i];
            mb_u8(&sec, g->valtype);
            mb_u8(&sec, g->mutable_ ? 0x01u : 0x00u);
            if (g->init_type == MB_VAL_I32) {
                mb_u8(&sec, 0x41u);
                mb_sleb(&sec, (i64)(i32)g->init_i64);
            } else if (g->init_type == MB_VAL_I64) {
                mb_u8(&sec, 0x42u);
                mb_sleb(&sec, g->init_i64);
            } else if (g->init_type == MB_VAL_F32) {
                mb_u8(&sec, 0x43u);
                mb_f32(&sec, g->init_f32);
            } else if (g->init_type == MB_VAL_F64) {
                mb_u8(&sec, 0x44u);
                mb_f64(&sec, g->init_f64);
            } else if (g->init_type == MB_VAL_V128) {
                mb_u8(&sec, 0xFDu);
                mb_uleb(&sec, 0x0Cu);
                mb_bytes(&sec, g->init_v128, 16);
            } else if (g->init_type == 0x23u) {
                mb_u8(&sec, 0x23u);
                mb_uleb(&sec, (u64)g->init_i64);
            } else {
                PANIC("mb_module_finish: unsupported global init_type %u", g->init_type);
            }
            mb_u8(&sec, 0x0Bu);
        }
        mb_emit_section(out_buf, MB_SEC_GLOBAL, &sec);
    }

    /* Section 7: Export */
    if (m->export_count > 0) {
        sec.len = 0;
        mb_uleb(&sec, (u64)m->export_count);
        for (u32 i = 0; i < m->export_count; i++) {
            const MbExport* exp = &m->exports[i];
            mb_name(&sec, exp->name, exp->name_len);
            mb_u8(&sec, exp->kind);
            mb_uleb(&sec, (u64)exp->index);
        }
        mb_emit_section(out_buf, MB_SEC_EXPORT, &sec);
    }

    /* Section 9: Element */
    if (m->elem_count > 0) {
        sec.len = 0;
        mb_uleb(&sec, (u64)m->elem_count);
        for (u32 i = 0; i < m->elem_count; i++) {
            const MbElem* el = &m->elems[i];
            mb_u8(&sec, 0x00u); /* active segment, table 0 */
            mb_u8(&sec, 0x41u); /* i32.const */
            mb_sleb(&sec, (i64)el->offset);
            mb_u8(&sec, 0x0Bu); /* end */
            mb_uleb(&sec, (u64)el->count);
            for (u32 j = 0; j < el->count; j++) {
                mb_uleb(&sec, (u64)el->func_indices[j]);
            }
        }
        mb_emit_section(out_buf, MB_SEC_ELEM, &sec);
    }

    /* Section 10: Code */
    if (m->func_count > 0) {
        sec.len = 0;
        mb_uleb(&sec, (u64)m->func_count);
        for (u32 i = 0; i < m->func_count; i++) {
            const MbFunc* f = &m->funcs[i];
            if (!f->ended) {
                PANIC("mb_module_finish: function %u was not ended", i);
            }
            /* Yerel değişken başlığını küçük bir tampona kodla */
            u8 local_hdr_mem[MB_MAX_LOCAL_GROUPS * 6 + 8];
            MbBuf local_hdr;
            mb_buf_init(&local_hdr, local_hdr_mem, sizeof(local_hdr_mem));
            mb_uleb(&local_hdr, (u64)f->local_group_count);
            for (u32 g = 0; g < f->local_group_count; g++) {
                mb_uleb(&local_hdr, (u64)f->locals[g].count);
                mb_u8(&local_hdr, f->locals[g].valtype);
            }

            u32 body_size = local_hdr.len + f->code.len;
            mb_uleb(&sec, (u64)body_size);
            mb_bytes(&sec, local_hdr.p, local_hdr.len);
            mb_bytes(&sec, f->code.p, f->code.len);
        }
        mb_emit_section(out_buf, MB_SEC_CODE, &sec);
    }

    /* Section 11: Data */
    if (m->data_count > 0) {
        sec.len = 0;
        mb_uleb(&sec, (u64)m->data_count);
        for (u32 i = 0; i < m->data_count; i++) {
            const MbData* d = &m->datas[i];
            mb_u8(&sec, 0x00u); /* active segment, memory 0 */
            mb_u8(&sec, 0x41u); /* i32.const */
            mb_sleb(&sec, (i64)d->offset);
            mb_u8(&sec, 0x0Bu); /* end */
            mb_uleb(&sec, (u64)d->len);
            mb_bytes(&sec, d->data, d->len);
        }
        mb_emit_section(out_buf, MB_SEC_DATA, &sec);
    }

    return out_buf->len - start_len;
}
