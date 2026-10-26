/* asset/gltf.c — glTF/GLB ayrıştırma: JSON DOM → tamponlar → accessor okuma → node ağacı gezme.
 * Her accessor erişimi bufferView ve buffer sınırlarına karşı u64 aritmetiğiyle denetlenir
 * (bozuk dosya → hata, asla sınır dışı okuma). Dokular PNG çözülüp 2'nin kuvvetine boyutlanır ve mip'lenir.
 */
#include "asset/gltf.h"
#include "asset/json.h"
#include "asset/base64.h"
#include "img/png.h"
#include "img/image.h"
#include "raster/texture.h"
#include "core/log.h"
#include "plat/platform.h"

typedef struct { const u8* data; u32 len; } GBuf;

typedef struct {
    const JsonValue* root;
    GBuf*  bufs;
    u32    buf_count;
    Arena* out;
    Arena* scratch;
    GltfModel* m;
    GltfResolveFn resolve;
    void*  user;
    /* çizim toplama (bağlı liste → dizi) */
    struct DrawNode { GltfDraw d; struct DrawNode* next; } *head, *tail;
    u32    draws;
} G;

static void g_err(GltfModel* m, const char* msg) {
    if (m->err[0]) return;
    u32 i = 0;
    while (msg[i] && i + 1 < sizeof(m->err)) { m->err[i] = msg[i]; i++; }
    m->err[i] = 0;
}

static void g_warn(GltfModel* m, const char* msg) {
    m->warnings++;
    u32 n = 0;
    while (m->warn[n]) n++;
    if (n + 4 >= sizeof(m->warn)) return;
    if (n) { m->warn[n++] = ';'; m->warn[n++] = ' '; }
    for (u32 i = 0; msg[i] && n + 1 < sizeof(m->warn); i++) m->warn[n++] = msg[i];
    m->warn[n] = 0;
}

static b32 str_starts(const char* s, const char* p) {
    while (*p) { if (*s++ != *p++) return FALSE; }
    return TRUE;
}

static b32 str_eq(const char* a, const char* b) {
    while (*a && *a == *b) { a++; b++; }
    return *a == *b;
}

/* "data:...;base64,XXXX" → baytlar (scratch) */
static b32 decode_data_uri(G* g, const char* uri, GBuf* out) {
    const char* comma = uri;
    while (*comma && *comma != ',') comma++;
    if (!*comma) return FALSE;
    const char* b64 = comma + 1;
    b32 is_b64 = FALSE;
    for (const char* p = uri; p + 7 <= comma; p++) {
        if (str_starts(p, ";base64")) is_b64 = TRUE;
    }
    if (!is_b64) return FALSE;
    u32 tl = 0;
    while (b64[tl]) tl++;
    u32 cap = base64_max_decoded(tl);
    u8* buf = (u8*)arena_push(g->scratch, cap ? cap : 1u, 16);
    u32 n = 0;
    if (!base64_decode(b64, tl, buf, cap, &n)) return FALSE;
    out->data = buf;
    out->len = n;
    return TRUE;
}

static b32 load_uri(G* g, const char* uri, GBuf* out) {
    if (str_starts(uri, "data:")) return decode_data_uri(g, uri, out);
    if (g->resolve) {
        u32 len = 0;
        const u8* d = g->resolve(g->user, uri, &len);
        if (d) { out->data = d; out->len = len; return TRUE; }
    }
    return FALSE;
}

/* bufferView → (ptr, len, stride) */
static b32 view_slice(G* g, i64 vi, const u8** ptr, u32* len, u32* stride) {
    const JsonValue* v = json_at(json_get(g->root, "bufferViews"), (u32)vi);
    if (!v || vi < 0) return FALSE;
    i64 bi = json_int(json_get(v, "buffer"), -1);
    if (bi < 0 || (u64)bi >= g->buf_count || !g->bufs[bi].data) return FALSE;
    i64 off = json_int(json_get(v, "byteOffset"), 0);
    i64 bl  = json_int(json_get(v, "byteLength"), -1);
    if (off < 0 || bl < 0 || (u64)off + (u64)bl > g->bufs[bi].len) return FALSE;
    *ptr = g->bufs[bi].data + off;
    *len = (u32)bl;
    *stride = (u32)json_int(json_get(v, "byteStride"), 0);
    return TRUE;
}

static u32 type_comps(const char* t) {
    if (str_eq(t, "SCALAR")) return 1;
    if (str_eq(t, "VEC2")) return 2;
    if (str_eq(t, "VEC3")) return 3;
    if (str_eq(t, "VEC4")) return 4;
    if (str_eq(t, "MAT4")) return 16;
    return 0;
}

static u32 ctype_size(i64 ct) {
    switch (ct) {
    case 5120: case 5121: return 1;
    case 5122: case 5123: return 2;
    case 5125: case 5126: return 4;
    default: return 0;
    }
}

static f32 read_comp(const u8* p, i64 ct, b32 norm) {
    switch (ct) {
    case 5126: { f32 f; __builtin_memcpy(&f, p, 4); return f; }
    case 5121: return norm ? (f32)p[0] / 255.0f : (f32)p[0];
    case 5120: { i8 v = (i8)p[0]; return norm ? m_max((f32)v / 127.0f, -1.0f) : (f32)v; }
    case 5123: { u16 v; __builtin_memcpy(&v, p, 2); return norm ? (f32)v / 65535.0f : (f32)v; }
    case 5122: { i16 v; __builtin_memcpy(&v, p, 2); return norm ? m_max((f32)v / 32767.0f, -1.0f) : (f32)v; }
    case 5125: { u32 v; __builtin_memcpy(&v, p, 4); return (f32)v; }
    default: return 0.0f;
    }
}

/* accessor → count × comps f32 (comps beklenen bileşen sayısı; fazlası atılır) */
static f32* read_accessor_f32(G* g, i64 ai, u32 want, u32* out_count) {
    const JsonValue* a = json_at(json_get(g->root, "accessors"), (u32)ai);
    if (!a || ai < 0) return NULL;
    if (json_get(a, "sparse")) g_warn(g->m, "sparse accessor yok sayildi");
    u32 comps = type_comps(json_str(json_get(a, "type"), ""));
    i64 ct = json_int(json_get(a, "componentType"), 0);
    u32 csz = ctype_size(ct);
    i64 count = json_int(json_get(a, "count"), -1);
    b32 norm = json_bool(json_get(a, "normalized"), FALSE);
    if (comps < want || csz == 0 || count <= 0 || count > (1 << 24)) return NULL;
    i64 vi = json_int(json_get(a, "bufferView"), -1);
    const u8* base; u32 vlen, stride;
    if (!view_slice(g, vi, &base, &vlen, &stride)) return NULL;
    i64 aoff = json_int(json_get(a, "byteOffset"), 0);
    u32 esz = comps * csz;
    if (stride == 0) stride = esz;
    if (aoff < 0 || stride < esz) return NULL;
    if ((u64)aoff + (u64)(count - 1) * stride + esz > vlen) return NULL;
    f32* out = ARENA_PUSH_ARRAY(g->scratch, f32, (usize)count * want);
    for (i64 i = 0; i < count; i++) {
        const u8* e = base + aoff + (u64)i * stride;
        for (u32 c = 0; c < want; c++) out[i * want + c] = read_comp(e + c * csz, ct, norm);
    }
    *out_count = (u32)count;
    return out;
}

static u32* read_indices(G* g, i64 ai, u32* out_count, u32 vertex_count) {
    const JsonValue* a = json_at(json_get(g->root, "accessors"), (u32)ai);
    if (!a) return NULL;
    i64 ct = json_int(json_get(a, "componentType"), 0);
    if (ct != 5121 && ct != 5123 && ct != 5125) return NULL;
    u32 csz = ctype_size(ct);
    i64 count = json_int(json_get(a, "count"), -1);
    if (count <= 0 || count > (1 << 26)) return NULL;
    const u8* base; u32 vlen, stride;
    if (!view_slice(g, json_int(json_get(a, "bufferView"), -1), &base, &vlen, &stride)) return NULL;
    i64 aoff = json_int(json_get(a, "byteOffset"), 0);
    if (stride == 0) stride = csz;
    if (aoff < 0 || (u64)aoff + (u64)(count - 1) * stride + csz > vlen) return NULL;
    u32* out = ARENA_PUSH_ARRAY(g->scratch, u32, (usize)count);
    for (i64 i = 0; i < count; i++) {
        const u8* e = base + aoff + (u64)i * stride;
        u32 v = (ct == 5121) ? e[0] : (ct == 5123) ? (u32)(e[0] | (e[1] << 8)) :
                (u32)(e[0] | (e[1] << 8) | (e[2] << 16) | ((u32)e[3] << 24));
        if (v >= vertex_count) return NULL;
        out[i] = v;
    }
    *out_count = (u32)count;
    return out;
}

Mat4 mat4_from_quat(f32 x, f32 y, f32 z, f32 w) {
    Mat4 r = mat4_identity();
    r.m[0]  = 1.0f - 2.0f * (y * y + z * z);
    r.m[1]  = 2.0f * (x * y + z * w);
    r.m[2]  = 2.0f * (x * z - y * w);
    r.m[4]  = 2.0f * (x * y - z * w);
    r.m[5]  = 1.0f - 2.0f * (x * x + z * z);
    r.m[6]  = 2.0f * (y * z + x * w);
    r.m[8]  = 2.0f * (x * z + y * w);
    r.m[9]  = 2.0f * (y * z - x * w);
    r.m[10] = 1.0f - 2.0f * (x * x + y * y);
    return r;
}

static Mat4 node_local(const JsonValue* n) {
    const JsonValue* mat = json_get(n, "matrix");
    if (json_len(mat) == 16) {
        Mat4 r;
        for (u32 i = 0; i < 16; i++) r.m[i] = (f32)json_num(json_at(mat, i), 0.0);
        return r;
    }
    const JsonValue* t = json_get(n, "translation");
    const JsonValue* q = json_get(n, "rotation");
    const JsonValue* s = json_get(n, "scale");
    Mat4 T = mat4_identity(), R = mat4_identity(), S = mat4_identity();
    if (json_len(t) == 3) {
        T.m[12] = (f32)json_num(json_at(t, 0), 0); T.m[13] = (f32)json_num(json_at(t, 1), 0);
        T.m[14] = (f32)json_num(json_at(t, 2), 0);
    }
    if (json_len(q) == 4) {
        R = mat4_from_quat((f32)json_num(json_at(q, 0), 0), (f32)json_num(json_at(q, 1), 0),
                           (f32)json_num(json_at(q, 2), 0), (f32)json_num(json_at(q, 3), 1));
    }
    if (json_len(s) == 3) {
        S.m[0] = (f32)json_num(json_at(s, 0), 1); S.m[5] = (f32)json_num(json_at(s, 1), 1);
        S.m[10] = (f32)json_num(json_at(s, 2), 1);
    }
    return mat4_mul(T, mat4_mul(R, S));
}

static Mesh* build_primitive(G* g, const JsonValue* prim) {
    const JsonValue* attrs = json_get(prim, "attributes");
    i64 mode = json_int(json_get(prim, "mode"), 4);
    if (mode != 4) { g_warn(g->m, "ucgen olmayan primitive atlandi"); return NULL; }
    if (json_get(attrs, "JOINTS_0") || json_get(prim, "targets")) g_warn(g->m, "skin/morph yok sayildi");
    u32 vn = 0, nn = 0, tn = 0;
    f32* pos = read_accessor_f32(g, json_int(json_get(attrs, "POSITION"), -1), 3, &vn);
    if (!pos) { g_err(g->m, "POSITION okunamadi"); return NULL; }
    f32* nrm = NULL;
    f32* uv  = NULL;
    if (json_get(attrs, "NORMAL")) {
        nrm = read_accessor_f32(g, json_int(json_get(attrs, "NORMAL"), -1), 3, &nn);
        if (!nrm || nn != vn) { g_err(g->m, "NORMAL okunamadi"); return NULL; }
    }
    if (json_get(attrs, "TEXCOORD_0")) {
        uv = read_accessor_f32(g, json_int(json_get(attrs, "TEXCOORD_0"), -1), 2, &tn);
        if (!uv || tn != vn) { g_err(g->m, "TEXCOORD_0 okunamadi"); return NULL; }
    }
    u32 icount = 0;
    u32* idx = NULL;
    if (json_get(prim, "indices")) {
        idx = read_indices(g, json_int(json_get(prim, "indices"), -1), &icount, vn);
        if (!idx) { g_err(g->m, "indices okunamadi veya aralik disi"); return NULL; }
    } else {
        icount = vn;
    }
    icount -= icount % 3u;
    if (icount == 0) return NULL;
    Mesh* mesh = mesh_alloc(g->out, vn, icount, FALSE);
    for (u32 i = 0; i < vn; i++) {
        mesh->px[i] = pos[i * 3]; mesh->py[i] = pos[i * 3 + 1]; mesh->pz[i] = pos[i * 3 + 2];
        mesh->u[i] = uv ? uv[i * 2] : 0.0f;
        mesh->v[i] = uv ? uv[i * 2 + 1] : 0.0f;    /* glTF UV zaten üstten başlar */
        if (nrm) { mesh->nx[i] = nrm[i * 3]; mesh->ny[i] = nrm[i * 3 + 1]; mesh->nz[i] = nrm[i * 3 + 2]; }
    }
    for (u32 i = 0; i < icount; i++) mesh->indices[i] = idx ? idx[i] : i;
    if (!nrm) mesh_compute_normals(mesh);
    mesh_compute_bounds(mesh);
    return mesh;
}

static void visit_node(G* g, i64 ni, Mat4 parent, u32 depth) {
    const JsonValue* n = json_at(json_get(g->root, "nodes"), (u32)ni);
    if (!n || ni < 0 || depth > 64) { if (depth > 64) g_warn(g->m, "node derinligi/dongusu"); return; }
    Mat4 world = mat4_mul(parent, node_local(n));
    const JsonValue* mesh = json_at(json_get(g->root, "meshes"), (u32)json_int(json_get(n, "mesh"), -1));
    if (json_get(n, "mesh") && mesh) {
        const JsonValue* prims = json_get(mesh, "primitives");
        for (u32 p = 0; p < json_len(prims); p++) {
            const JsonValue* prim = json_at(prims, p);
            Mesh* m = build_primitive(g, prim);
            if (!m) { if (g->m->err[0]) return; continue; }
            struct DrawNode* d = ARENA_PUSH_STRUCT(g->scratch, struct DrawNode);
            d->d.mesh = m;
            d->d.world = world;
            d->d.material = (i32)json_int(json_get(prim, "material"), -1);
            if (d->d.material >= (i32)g->m->material_count) d->d.material = -1;
            if (g->tail) g->tail->next = d; else g->head = d;
            g->tail = d;
            g->draws++;
            g->m->triangles += m->index_count / 3u;
        }
    }
    const JsonValue* ch = json_get(n, "children");
    for (u32 i = 0; i < json_len(ch); i++) visit_node(g, json_int(json_at(ch, i), -1), world, depth + 1);
}

static u32 next_pow2_le(u32 v, u32 cap) {
    u32 p = 1;
    while (p < v && p < cap) p <<= 1;
    return p;
}

static void load_texture(G* g, u32 ti, Texture* out) {
    __builtin_memset(out, 0, sizeof(*out));
    const JsonValue* t = json_at(json_get(g->root, "textures"), ti);
    const JsonValue* img = json_at(json_get(g->root, "images"), (u32)json_int(json_get(t, "source"), -1));
    if (!img) { g_warn(g->m, "doku kaynagi yok"); return; }
    GBuf b = { NULL, 0 };
    const char* mime = json_str(json_get(img, "mimeType"), "");
    const char* uri = json_str(json_get(img, "uri"), NULL);
    if (uri) {
        if (str_starts(uri, "data:image/jpeg")) { g_warn(g->m, "JPEG desteklenmiyor"); return; }
        if (!load_uri(g, uri, &b)) { g_warn(g->m, "goruntu uri cozulemedi"); return; }
    } else {
        u32 stride;
        if (!view_slice(g, json_int(json_get(img, "bufferView"), -1), &b.data, &b.len, &stride)) {
            g_warn(g->m, "goruntu bufferView gecersiz"); return;
        }
        if (str_eq(mime, "image/jpeg")) { g_warn(g->m, "JPEG desteklenmiyor"); return; }
    }
    if (b.len < 8 || b.data[0] != 0x89 || b.data[1] != 'P') { g_warn(g->m, "PNG olmayan goruntu"); return; }
    u32 w = 0, h = 0;
    u32* px = png_decode(b.data, b.len, &w, &h, g->scratch);
    if (!px || w == 0 || h == 0) { g_warn(g->m, "PNG cozulemedi"); return; }
    u32 nw = next_pow2_le(w, 1024), nh = next_pow2_le(h, 1024);
    u32 flags = TEX_FLAG_BILINEAR;
    const JsonValue* smp = json_at(json_get(g->root, "samplers"), (u32)json_int(json_get(t, "sampler"), -1));
    if (json_int(json_get(smp, "wrapS"), 10497) == 33071 || json_int(json_get(smp, "wrapT"), 10497) == 33071) {
        flags |= TEX_FLAG_CLAMP;
    }
    tex_init_mipmapped(out, g->out, nw, nh, flags);
    if (nw == w && nh == h) {
        for (u32 i = 0; i < w * h; i++) out->px[i] = px[i];
    } else {
        img_resize_bilinear(px, w, h, out->px, nw, nh);
    }
    tex_build_mips(out);
}

b32 gltf_load(const u8* data, u32 len, Arena* out, Arena* scratch,
              GltfResolveFn resolve, void* user, GltfModel* model) {
    __builtin_memset(model, 0, sizeof(*model));
    const char* json_text = (const char*)data;
    u32 json_len_b = len;
    GBuf bin = { NULL, 0 };

    if (len >= 12 && data[0] == 'g' && data[1] == 'l' && data[2] == 'T' && data[3] == 'F') {
        u32 ver, total;
        __builtin_memcpy(&ver, data + 4, 4);
        __builtin_memcpy(&total, data + 8, 4);
        if (ver != 2) { g_err(model, "GLB surumu 2 degil"); return FALSE; }
        if (total > len) { g_err(model, "GLB kesik"); return FALSE; }
        u32 off = 12;
        json_text = NULL;
        while (off + 8 <= total) {
            u32 clen, ctype;
            __builtin_memcpy(&clen, data + off, 4);
            __builtin_memcpy(&ctype, data + off + 4, 4);
            if ((u64)off + 8u + clen > total) { g_err(model, "GLB parcasi tasiyor"); return FALSE; }
            if (ctype == 0x4E4F534Au && !json_text) { json_text = (const char*)data + off + 8; json_len_b = clen; }
            else if (ctype == 0x004E4942u && !bin.data) { bin.data = data + off + 8; bin.len = clen; }
            off += 8u + ((clen + 3u) & ~3u);
        }
        if (!json_text) { g_err(model, "GLB JSON parcasi yok"); return FALSE; }
    }

    JsonValue root;
    JsonError je;
    f64 t0 = plat_time_ms();
    b32 jok = json_parse(json_text, json_len_b, scratch, &root, &je);
    model->t_json_ms = plat_time_ms() - t0;
    if (!jok) {
        g_err(model, "JSON hatasi");
        return FALSE;
    }
    const JsonValue* asset = json_get(&root, "asset");
    const char* ver = json_str(json_get(asset, "version"), "");
    if (!(ver[0] == '2' && ver[1] == '.')) { g_err(model, "asset.version 2.x degil"); return FALSE; }
    if (json_len(json_get(&root, "extensionsRequired"))) { g_err(model, "zorunlu uzanti desteklenmiyor"); return FALSE; }
    if (json_len(json_get(&root, "extensionsUsed"))) g_warn(model, "uzantilar yok sayildi");
    if (json_len(json_get(&root, "animations"))) g_warn(model, "animasyon yok sayildi");
    if (json_len(json_get(&root, "skins"))) g_warn(model, "skin yok sayildi");

    G g;
    __builtin_memset(&g, 0, sizeof(g));
    g.root = &root;
    g.out = out;
    g.scratch = scratch;
    g.m = model;
    g.resolve = resolve;
    g.user = user;

    const JsonValue* bufs = json_get(&root, "buffers");
    g.buf_count = json_len(bufs);
    g.bufs = ARENA_PUSH_ARRAY(scratch, GBuf, g.buf_count + 1u);
    for (u32 i = 0; i < g.buf_count; i++) {
        const JsonValue* b = json_at(bufs, i);
        const char* uri = json_str(json_get(b, "uri"), NULL);
        i64 bl = json_int(json_get(b, "byteLength"), -1);
        if (!uri) {
            if (i == 0 && bin.data) g.bufs[i] = bin;
        } else if (!load_uri(&g, uri, &g.bufs[i])) {
            g_err(model, "buffer uri cozulemedi");
            return FALSE;
        }
        if (g.bufs[i].data && bl >= 0 && (u64)bl <= g.bufs[i].len) g.bufs[i].len = (u32)bl;
        else if (g.bufs[i].data && bl >= 0) { g_err(model, "buffer byteLength veriden buyuk"); return FALSE; }
    }

    /* Materyaller ve dokular */
    const JsonValue* texs = json_get(&root, "textures");
    model->texture_count = json_len(texs);
    model->textures = ARENA_PUSH_ARRAY(out, Texture, model->texture_count + 1u);
    t0 = plat_time_ms();
    for (u32 i = 0; i < model->texture_count; i++) load_texture(&g, i, &model->textures[i]);
    model->t_tex_ms = plat_time_ms() - t0;
    t0 = plat_time_ms();

    const JsonValue* mats = json_get(&root, "materials");
    model->material_count = json_len(mats);
    model->materials = ARENA_PUSH_ARRAY(out, GltfMaterial, model->material_count + 1u);
    for (u32 i = 0; i < model->material_count; i++) {
        const JsonValue* mt = json_at(mats, i);
        GltfMaterial* gm = &model->materials[i];
        const JsonValue* pbr = json_get(mt, "pbrMetallicRoughness");
        const JsonValue* bc = json_get(pbr, "baseColorFactor");
        gm->base_color = vec4(1, 1, 1, 1);
        if (json_len(bc) == 4) {
            gm->base_color = vec4((f32)json_num(json_at(bc, 0), 1), (f32)json_num(json_at(bc, 1), 1),
                                  (f32)json_num(json_at(bc, 2), 1), (f32)json_num(json_at(bc, 3), 1));
        }
        i64 ti = json_int(json_get(json_get(pbr, "baseColorTexture"), "index"), -1);
        gm->base_texture = (ti >= 0 && (u64)ti < model->texture_count && model->textures[ti].px) ? (i32)ti : -1;
        gm->double_sided = json_bool(json_get(mt, "doubleSided"), FALSE);
        const char* am = json_str(json_get(mt, "alphaMode"), "OPAQUE");
        gm->alpha_mode = str_eq(am, "MASK") ? GLTF_ALPHA_MASK : str_eq(am, "BLEND") ? GLTF_ALPHA_BLEND : GLTF_ALPHA_OPAQUE;
        gm->alpha_cutoff = (f32)json_num(json_get(mt, "alphaCutoff"), 0.5);
    }

    /* Sahne */
    const JsonValue* scenes = json_get(&root, "scenes");
    i64 si = json_int(json_get(&root, "scene"), 0);
    const JsonValue* scene = json_at(scenes, (u32)si);
    if (scene) {
        const JsonValue* roots = json_get(scene, "nodes");
        for (u32 i = 0; i < json_len(roots) && !model->err[0]; i++) {
            visit_node(&g, json_int(json_at(roots, i), -1), mat4_identity(), 0);
        }
    } else {
        /* sahnesiz dosya: ebeveyni olmayan tüm node'lar kök */
        const JsonValue* nodes = json_get(&root, "nodes");
        u32 nc = json_len(nodes);
        u8* is_child = ARENA_PUSH_ARRAY(scratch, u8, nc + 1u);
        for (u32 i = 0; i < nc; i++) {
            const JsonValue* ch = json_get(json_at(nodes, i), "children");
            for (u32 k = 0; k < json_len(ch); k++) {
                i64 c = json_int(json_at(ch, k), -1);
                if (c >= 0 && (u64)c < nc) is_child[c] = 1;
            }
        }
        for (u32 i = 0; i < nc && !model->err[0]; i++) if (!is_child[i]) visit_node(&g, i, mat4_identity(), 0);
    }
    model->t_mesh_ms = plat_time_ms() - t0;
    if (model->err[0]) return FALSE;
    if (g.draws == 0) { g_err(model, "cizilecek ucgen yok"); return FALSE; }

    model->draw_count = g.draws;
    model->draws = ARENA_PUSH_ARRAY(out, GltfDraw, g.draws);
    u32 k = 0;
    Vec3 lo = vec3(1e30f, 1e30f, 1e30f), hi = vec3(-1e30f, -1e30f, -1e30f);
    for (struct DrawNode* d = g.head; d; d = d->next) {
        model->draws[k++] = d->d;
        const Mesh* m = d->d.mesh;
        for (u32 c = 0; c < 8; c++) {
            Vec4 p = vec4((c & 1) ? m->aabb_max.x : m->aabb_min.x, (c & 2) ? m->aabb_max.y : m->aabb_min.y,
                          (c & 4) ? m->aabb_max.z : m->aabb_min.z, 1.0f);
            Vec4 w = mat4_mul_vec4(d->d.world, p);
            lo = vec3(m_min(lo.x, w.x), m_min(lo.y, w.y), m_min(lo.z, w.z));
            hi = vec3(m_max(hi.x, w.x), m_max(hi.y, w.y), m_max(hi.z, w.z));
        }
    }
    model->bounds_min = lo;
    model->bounds_max = hi;
    return TRUE;
}
