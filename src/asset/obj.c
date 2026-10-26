/* asset/obj.c — OBJ iki geçişte okunur: (1) kayıt/köşe sayımı, (2) ayrıştırma + tekilleştirme.
 * Tekilleştirme anahtarı (konum, uv, normal) üçlüsüdür; açık adresli hash tablosu scratch arena'da.
 * Sayılar json_make_f64 ile (aynı dönüşüm yolu) okunur; hatalı satırda satır numarası raporlanır.
 */
#include "asset/obj.h"
#include "asset/json.h"

typedef struct { const char* p; const char* end; } Cur;

static void set_err(ObjInfo* info, u32 line, const char* msg) {
    if (!info || info->err[0]) return;
    info->err_line = line;
    u32 i = 0;
    while (msg[i] && i + 1 < sizeof(info->err)) { info->err[i] = msg[i]; i++; }
    info->err[i] = 0;
}

static void skip_sp(Cur* c) {
    while (c->p < c->end && (*c->p == ' ' || *c->p == '\t')) c->p++;
}

static b32 at_eol(const Cur* c) {
    return c->p >= c->end || *c->p == '\n' || *c->p == '\r' || *c->p == '#';
}

static b32 parse_f32(Cur* c, f32* out) {
    skip_sp(c);
    b32 neg = FALSE;
    if (c->p < c->end && (*c->p == '-' || *c->p == '+')) { neg = (*c->p == '-'); c->p++; }
    u64 mant = 0;
    u32 digits = 0;
    i32 e10 = 0;
    b32 any = FALSE;
    while (c->p < c->end && *c->p >= '0' && *c->p <= '9') {
        if (digits < 19) { mant = mant * 10u + (u64)(*c->p - '0'); if (mant) digits++; } else e10++;
        c->p++; any = TRUE;
    }
    if (c->p < c->end && *c->p == '.') {
        c->p++;
        while (c->p < c->end && *c->p >= '0' && *c->p <= '9') {
            if (digits < 19) { mant = mant * 10u + (u64)(*c->p - '0'); if (mant) digits++; e10--; }
            c->p++; any = TRUE;
        }
    }
    if (!any) return FALSE;
    if (c->p < c->end && (*c->p == 'e' || *c->p == 'E')) {
        c->p++;
        b32 en = FALSE;
        if (c->p < c->end && (*c->p == '-' || *c->p == '+')) { en = (*c->p == '-'); c->p++; }
        i32 ev = 0;
        while (c->p < c->end && *c->p >= '0' && *c->p <= '9') { if (ev < 100000) ev = ev * 10 + (*c->p - '0'); c->p++; }
        e10 += en ? -ev : ev;
    }
    f64 v = json_make_f64(mant, e10);
    *out = (f32)(neg ? -v : v);
    return TRUE;
}

static b32 parse_int(Cur* c, i32* out) {
    b32 neg = FALSE;
    if (c->p < c->end && (*c->p == '-' || *c->p == '+')) { neg = (*c->p == '-'); c->p++; }
    if (c->p >= c->end || *c->p < '0' || *c->p > '9') return FALSE;
    i64 v = 0;
    while (c->p < c->end && *c->p >= '0' && *c->p <= '9') { if (v < 1000000000) v = v * 10 + (*c->p - '0'); c->p++; }
    *out = (i32)(neg ? -v : v);
    return TRUE;
}

static void next_line(Cur* c) {
    while (c->p < c->end && *c->p != '\n') c->p++;
    if (c->p < c->end) c->p++;
}

/* Satır başındaki anahtar kelime: "v", "vt", "vn", "f", ... */
static u32 keyword(Cur* c, char kw[8]) {
    skip_sp(c);
    u32 n = 0;
    while (c->p < c->end && *c->p != ' ' && *c->p != '\t' && *c->p != '\n' && *c->p != '\r' && n < 7) kw[n++] = *c->p++;
    kw[n] = 0;
    while (c->p < c->end && *c->p != ' ' && *c->p != '\t' && *c->p != '\n' && *c->p != '\r') c->p++;
    return n;
}

static b32 kw_is(const char* kw, const char* s) {
    u32 i = 0;
    while (s[i] && kw[i] == s[i]) i++;
    return s[i] == 0 && kw[i] == 0;
}

/* Bir satırdaki köşe sayısı (boşlukla ayrılmış simgeler) */
static u32 count_tokens(Cur c) {
    u32 n = 0;
    for (;;) {
        skip_sp(&c);
        if (at_eol(&c)) return n;
        n++;
        while (c.p < c.end && *c.p != ' ' && *c.p != '\t' && *c.p != '\n' && *c.p != '\r') c.p++;
    }
}

static inline u32 hash3(i32 a, i32 b, i32 c) {
    u32 h = (u32)a * 0x9E3779B1u ^ (u32)b * 0x85EBCA77u ^ (u32)c * 0xC2B2AE3Du;
    h ^= h >> 15;
    h *= 0x2C1B3C6Du;
    h ^= h >> 12;
    return h;
}

static void copy_word(Cur* c, char* dst, u32 cap) {
    skip_sp(c);
    u32 n = 0;
    while (c->p < c->end && *c->p != '\n' && *c->p != '\r' && n + 1 < cap) dst[n++] = *c->p++;
    while (n > 0 && (dst[n - 1] == ' ' || dst[n - 1] == '\t')) n--;
    dst[n] = 0;
}

Mesh* obj_load(const char* text, u32 len, Arena* out, Arena* scratch, ObjInfo* info) {
    ObjInfo dummy;
    if (!info) info = &dummy;
    __builtin_memset(info, 0, sizeof(*info));

    /* --- Geçiş 1: sayım --- */
    u32 np = 0, nt = 0, nn = 0, corners = 0, tris = 0, faces = 0;
    Cur c = { text, text + len };
    while (c.p < c.end) {
        char kw[8];
        keyword(&c, kw);
        if (kw_is(kw, "v")) np++;
        else if (kw_is(kw, "vt")) nt++;
        else if (kw_is(kw, "vn")) nn++;
        else if (kw_is(kw, "f")) {
            u32 k = count_tokens(c);
            if (k >= 3) { corners += k; tris += k - 2; faces++; }
        }
        next_line(&c);
    }
    if (tris == 0) { set_err(info, 0, "yuz (f) yok"); return NULL; }

    f32* pos = ARENA_PUSH_ARRAY(scratch, f32, (usize)np * 3u + 3u);
    f32* tc  = ARENA_PUSH_ARRAY(scratch, f32, (usize)nt * 2u + 2u);
    f32* nm  = ARENA_PUSH_ARRAY(scratch, f32, (usize)nn * 3u + 3u);
    u32 cap = 16;
    while (cap < corners * 2u) cap <<= 1;
    i32* hkey = ARENA_PUSH_ARRAY(scratch, i32, (usize)cap * 3u);
    u32* hval = ARENA_PUSH_ARRAY(scratch, u32, cap);
    for (u32 i = 0; i < cap; i++) hval[i] = 0xFFFFFFFFu;
    i32* vkey = ARENA_PUSH_ARRAY(scratch, i32, (usize)corners * 3u);   /* benzersiz köşe → (p, t, n) */
    u32* idx  = ARENA_PUSH_ARRAY(scratch, u32, (usize)tris * 3u);
    u32 nuniq = 0, nidx = 0;
    u32 ip = 0, it = 0, in = 0;
    b32 has_n = FALSE;

    /* --- Geçiş 2: ayrıştırma --- */
    c.p = text;
    u32 line = 0;
    while (c.p < c.end) {
        line++;
        char kw[8];
        keyword(&c, kw);
        if (kw_is(kw, "v")) {
            for (u32 k = 0; k < 3; k++) {
                if (!parse_f32(&c, &pos[ip * 3 + k])) { set_err(info, line, "gecersiz v satiri"); return NULL; }
            }
            ip++;
        } else if (kw_is(kw, "vt")) {
            if (!parse_f32(&c, &tc[it * 2]) || !parse_f32(&c, &tc[it * 2 + 1])) { set_err(info, line, "gecersiz vt satiri"); return NULL; }
            it++;
        } else if (kw_is(kw, "vn")) {
            for (u32 k = 0; k < 3; k++) {
                if (!parse_f32(&c, &nm[in * 3 + k])) { set_err(info, line, "gecersiz vn satiri"); return NULL; }
            }
            in++;
        } else if (kw_is(kw, "mtllib")) {
            copy_word(&c, info->mtllib, sizeof(info->mtllib));
        } else if (kw_is(kw, "f")) {
            u32 k = count_tokens(c);
            if (k < 3) { next_line(&c); continue; }
            u32 first = 0, prev = 0;
            for (u32 j = 0; j < k; j++) {
                skip_sp(&c);
                i32 pi = 0, ti = 0, ni = 0;
                if (!parse_int(&c, &pi)) { set_err(info, line, "gecersiz yuz kosesi"); return NULL; }
                if (c.p < c.end && *c.p == '/') {
                    c.p++;
                    if (c.p < c.end && *c.p != '/') { if (!parse_int(&c, &ti)) { set_err(info, line, "gecersiz vt indeksi"); return NULL; } }
                    if (c.p < c.end && *c.p == '/') { c.p++; if (!parse_int(&c, &ni)) { set_err(info, line, "gecersiz vn indeksi"); return NULL; } }
                }
                /* 1 tabanlı → 0 tabanlı; negatif = sondan göreli; 0 = yok */
                pi = pi > 0 ? pi - 1 : (i32)ip + pi;
                ti = ti > 0 ? ti - 1 : (ti < 0 ? (i32)it + ti : -1);
                ni = ni > 0 ? ni - 1 : (ni < 0 ? (i32)in + ni : -1);
                if (pi < 0 || pi >= (i32)ip || ti >= (i32)it || ni >= (i32)in || ti < -1 || ni < -1) {
                    set_err(info, line, "yuz indeksi aralik disi"); return NULL;
                }
                if (ni >= 0) has_n = TRUE;
                u32 h = hash3(pi, ti, ni) & (cap - 1u);
                u32 vi;
                for (;;) {
                    if (hval[h] == 0xFFFFFFFFu) {
                        vi = nuniq++;
                        hval[h] = vi;
                        hkey[h * 3] = pi; hkey[h * 3 + 1] = ti; hkey[h * 3 + 2] = ni;
                        vkey[vi * 3] = pi; vkey[vi * 3 + 1] = ti; vkey[vi * 3 + 2] = ni;
                        break;
                    }
                    if (hkey[h * 3] == pi && hkey[h * 3 + 1] == ti && hkey[h * 3 + 2] == ni) { vi = hval[h]; break; }
                    h = (h + 1u) & (cap - 1u);
                }
                if (j == 0) first = vi;
                else if (j >= 2) { idx[nidx++] = first; idx[nidx++] = prev; idx[nidx++] = vi; }
                prev = vi;
            }
        }
        next_line(&c);
    }

    Mesh* m = mesh_alloc(out, nuniq, nidx, FALSE);
    for (u32 v = 0; v < nuniq; v++) {
        i32 pi = vkey[v * 3], ti = vkey[v * 3 + 1], ni = vkey[v * 3 + 2];
        m->px[v] = pos[pi * 3]; m->py[v] = pos[pi * 3 + 1]; m->pz[v] = pos[pi * 3 + 2];
        m->u[v] = ti >= 0 ? tc[ti * 2] : 0.0f;
        m->v[v] = ti >= 0 ? 1.0f - tc[ti * 2 + 1] : 0.0f;
        m->nx[v] = ni >= 0 ? nm[ni * 3] : 0.0f;
        m->ny[v] = ni >= 0 ? nm[ni * 3 + 1] : 0.0f;
        m->nz[v] = ni >= 0 ? nm[ni * 3 + 2] : 0.0f;
    }
    for (u32 i = 0; i < nidx; i++) m->indices[i] = idx[i];
    if (!has_n) mesh_compute_normals(m);
    mesh_compute_bounds(m);

    info->positions = ip; info->texcoords = it; info->normals = in;
    info->faces = faces; info->triangles = nidx / 3u; info->unique_vertices = nuniq;
    info->generated_normals = !has_n;
    return m;
}

b32 mtl_load_first(const char* text, u32 len, MtlInfo* out) {
    out->kd[0] = out->kd[1] = out->kd[2] = 1.0f;
    out->map_kd[0] = 0;
    Cur c = { text, text + len };
    u32 materials = 0;
    while (c.p < c.end) {
        char kw[8];
        keyword(&c, kw);
        if (kw_is(kw, "newmtl")) {
            if (++materials > 1) break;
        } else if (kw_is(kw, "Kd")) {
            for (u32 k = 0; k < 3; k++) parse_f32(&c, &out->kd[k]);
        } else if (kw_is(kw, "map_Kd")) {
            copy_word(&c, out->map_kd, sizeof(out->map_kd));
        }
        next_line(&c);
    }
    return materials > 0;
}
