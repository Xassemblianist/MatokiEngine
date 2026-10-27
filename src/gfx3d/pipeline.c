/* gfx3d/pipeline.c — Vertex dönüşümü, nesne culling, outcode ile hızlı yol / kırpma yolu ayrımı,
 * ekran izdüşümü ve arka yüz ayıklama. Köşe başına sonuçlar frame arena'da SoA'sız ClipVert
 * dizisinde tutulur; kırpma gerektirmeyen üçgenler köşe başına önceden izdüşürülmüş RasterVert'i kullanır.
 */
#include "gfx3d/pipeline.h"
#include "core/log.h"
#include "core/profiler.h"
#include "gfx3d/vertex.h"
#include "gfx3d/retro.h"
#include "thread/job.h"
#include "thread/thread.h"

u32 g3d_attr_components(G3dAttr a) {
    switch (a) {
    case G3D_ATTR_UV:         return 2;
    case G3D_ATTR_NORMAL:     return 3;
    case G3D_ATTR_VIEW_DEPTH: return 1;
    case G3D_ATTR_COLOR:      return 4;
    case G3D_ATTR_WORLD_POS:  return 3;
    case G3D_ATTR_SHADOW_POS: return 3;
    }
    return 0;
}

static Vec4 mat_row(const Mat4* m, u32 i) {
    return vec4(m->m[0 * 4 + i], m->m[1 * 4 + i], m->m[2 * 4 + i], m->m[3 * 4 + i]);
}

static Vec4 plane_norm(Vec4 p) {
    f32 l = m_sqrt(p.x * p.x + p.y * p.y + p.z * p.z);
    return (l > 0.0f) ? vec4_scale(p, 1.0f / l) : p;
}

void g3d_begin(G3dContext* c, Rasterizer* r, Arena* frame, Mat4 view, Mat4 proj, Vec3 eye) {
    c->r         = r;
    c->frame     = frame;
    c->view      = view;
    c->proj      = proj;
    c->view_proj = mat4_mul(proj, view);
    c->eye       = eye;
    c->vp_w      = r->fb->w;
    c->vp_h      = r->fb->h;
    c->guard     = clip_guard_for_viewport(c->vp_w, c->vp_h);
    c->shadow_mat = mat4_identity();
    c->snap      = 0;
    __builtin_memset(&c->stats, 0, sizeof(c->stats));
    c->cmds      = NULL;
    c->cmd_count = 0;
    c->cmd_cap   = 0;

    /* Gribb–Hartmann: klip düzlemleri view_proj satırlarından (dünya uzayı) */
    Vec4 r0 = mat_row(&c->view_proj, 0), r1 = mat_row(&c->view_proj, 1);
    Vec4 r2 = mat_row(&c->view_proj, 2), r3 = mat_row(&c->view_proj, 3);
    c->planes[0] = plane_norm(vec4_add(r3, r0));   /* x >= -w */
    c->planes[1] = plane_norm(vec4_sub(r3, r0));   /* x <=  w */
    c->planes[2] = plane_norm(vec4_add(r3, r1));   /* y >= -w */
    c->planes[3] = plane_norm(vec4_sub(r3, r1));   /* y <=  w */
    c->planes[4] = plane_norm(vec4_sub(r3, r2));   /* z <=  w (near) */
}

void g3d_set_shadow(G3dContext* c, Mat4 world_to_shadow) {
    c->shadow_mat = world_to_shadow;
}

void g3d_set_snap(G3dContext* c, u32 subpixel) {
    c->snap = subpixel;
}

b32 g3d_sphere_visible(const G3dContext* c, Vec3 p, f32 radius) {
    for (u32 i = 0; i < 5; i++) {
        Vec4 pl = c->planes[i];
        if (pl.x * p.x + pl.y * p.y + pl.z * p.z + pl.w < -radius) return FALSE;
    }
    return TRUE;
}

static inline RasterVert project(const G3dContext* c, const ClipVert* v, u32 nvar) {
    RasterVert o;
    f32 iw = 1.0f / v->w;
    o.x = (v->x * iw * 0.5f + 0.5f) * (f32)c->vp_w;
    o.y = (0.5f - v->y * iw * 0.5f) * (f32)c->vp_h;
    if (c->snap) retro_snap_xy(&o.x, &o.y, c->snap);
    o.z = v->z * iw;
    o.inv_w = iw;
    for (u32 k = 0; k < nvar; k++) o.var[k] = v->var[k];
    return o;
}

/* Ekran uzayında (y aşağı) işaretli alan*2. NDC'de CCW ön yüz → ekranda negatif. */
static inline f32 screen_area2(const RasterVert* a, const RasterVert* b, const RasterVert* d) {
    return (b->x - a->x) * (d->y - a->y) - (b->y - a->y) * (d->x - a->x);
}

static inline b32 keep_face(G3dCull cull, f32 area2) {
    if (!(area2 == area2) || area2 == 0.0f) return FALSE;
    if (cull == G3D_CULL_BACK)  return area2 < 0.0f;
    if (cull == G3D_CULL_FRONT) return area2 > 0.0f;
    return TRUE;
}

u32 g_g3d_opt = G3D_OPT_ALL;

void g3d_set_opt(u32 mask) {
    g_g3d_opt = mask;
}

/* Küre 5 düzlemin hepsinin içinde (yarıçap kadar payla) mı */
static b32 sphere_fully_inside(const G3dContext* c, Vec3 p, f32 radius) {
    for (u32 i = 0; i < 5; i++) {
        Vec4 pl = c->planes[i];
        if (pl.x * p.x + pl.y * p.y + pl.z * p.z + pl.w < radius) return FALSE;
    }
    return TRUE;
}

static inline u32 outcode4(f32 x, f32 y, f32 z, f32 w, ClipGuard g) {
    u32 code = 0;
    if (z > w)        code |= CLIP_NEAR;
    if (x < -w)       code |= CLIP_LEFT;
    if (x >  w)       code |= CLIP_RIGHT;
    if (y < -w)       code |= CLIP_BOTTOM;
    if (y >  w)       code |= CLIP_TOP;
    if (x < -g.gx * w) code |= CLIP_G_LEFT;
    if (x >  g.gx * w) code |= CLIP_G_RIGHT;
    if (y < -g.gy * w) code |= CLIP_G_BOTTOM;
    if (y >  g.gy * w) code |= CLIP_G_TOP;
    return code;
}

typedef struct {
    const Mesh*        m;
    const G3dMaterial* mat;
    const Mat4*        model;
    const f32*         iv;      /* model^-1 (normal matrisi = iv^T) */
    const f32*         sm;      /* shadow_mat * model (G3D_ATTR_SHADOW_POS) */
    const f32 *qx, *qy, *qz, *qw;
} VertexCtx;

/* Materyalin istediği varying'leri köşe i için out'a yazar (klip w'si qw'dan) */
static inline void vertex_attrs(const VertexCtx* v, u32 i, f32* out) {
    const Mesh* m = v->m;
    const f32* iv = v->iv;
    u32 k = 0;
    for (u32 a = 0; a < v->mat->attr_count; a++) {
        switch ((G3dAttr)v->mat->attrs[a]) {
        case G3D_ATTR_UV:
            out[k++] = m->u[i];
            out[k++] = m->v[i];
            break;
        case G3D_ATTR_NORMAL: {
            f32 nx = m->nx[i], ny = m->ny[i], nz = m->nz[i];
            out[k++] = iv[0] * nx + iv[1] * ny + iv[2]  * nz;
            out[k++] = iv[4] * nx + iv[5] * ny + iv[6]  * nz;
            out[k++] = iv[8] * nx + iv[9] * ny + iv[10] * nz;
        } break;
        case G3D_ATTR_VIEW_DEPTH:
            out[k++] = v->qw[i];
            break;
        case G3D_ATTR_COLOR: {
            u32 col = m->color ? m->color[i] : 0xFFFFFFFFu;
            out[k++] = (f32)(col & 0xFFu) * (1.0f / 255.0f);
            out[k++] = (f32)((col >> 8) & 0xFFu) * (1.0f / 255.0f);
            out[k++] = (f32)((col >> 16) & 0xFFu) * (1.0f / 255.0f);
            out[k++] = (f32)(col >> 24) * (1.0f / 255.0f);
        } break;
        case G3D_ATTR_WORLD_POS: {
            Vec4 w = mat4_mul_vec4(*v->model, vec4(m->px[i], m->py[i], m->pz[i], 1.0f));
            out[k++] = w.x; out[k++] = w.y; out[k++] = w.z;
        } break;
        case G3D_ATTR_SHADOW_POS: {
            const f32* sm = v->sm;
            f32 x = m->px[i], y = m->py[i], z = m->pz[i];
            out[k++] = sm[0] * x + sm[4] * y + sm[8]  * z + sm[12];
            out[k++] = sm[1] * x + sm[5] * y + sm[9]  * z + sm[13];
            out[k++] = sm[2] * x + sm[6] * y + sm[10] * z + sm[14];
        } break;
        }
    }
}

/* Kırpma yolu (nadir): klip köşesi talep üzerine kurulur */
static void make_clipvert(const VertexCtx* v, u32 i, ClipVert* o) {
    o->x = v->qx[i]; o->y = v->qy[i]; o->z = v->qz[i]; o->w = v->qw[i];
    vertex_attrs(v, i, o->var);
}

/* Üçgen çıkışı: anlık yolda raster_triangle, ertelenmiş yolda doğrudan raster_setup_tri ile
 * iş parçacığının dilimine kurulum (sıra korunur, sonuç bit düzeyinde aynı). */
typedef struct {
    Rasterizer*              r;          /* != NULL: anlık yol */
    const RasterShaderState* state;
    BinnedTri*               tris;
    f32*                     vc;
    u32                      vstride;    /* üçgen başına varying katsayısı (nvar * 3) */
    u32                      count, cap, submitted;
    i32                      fb_w, fb_h;
    b32                      overflow;
} TriSink;

static inline void sink_emit(TriSink* s, const RasterVert* a, const RasterVert* b, const RasterVert* d) {
    if (s->r) {
        raster_triangle(s->r, *a, *b, *d);
        return;
    }
    if (s->count >= s->cap) {
        s->overflow = TRUE;
        return;
    }
    s->submitted++;
    f32* vc = s->vstride ? s->vc + s->count * s->vstride : NULL;
    if (raster_setup_tri(a, b, d, s->state, FALSE, s->fb_w, s->fb_h, &s->tris[s->count], vc)) s->count++;
}

/* Nesne culling: yerel sınır küresi → dünya (en büyük eksen ölçeğiyle). Görünmezse FALSE. */
static b32 draw_bounds(const G3dContext* c, const Mesh* m, const Mat4* model, Vec3* wc, f32* rad) {
    const f32* mm = model->m;
    Vec4 w = mat4_mul_vec4(*model, vec4(m->center.x, m->center.y, m->center.z, 1.0f));
    f32 sx2 = mm[0] * mm[0] + mm[1] * mm[1] + mm[2] * mm[2];
    f32 sy2 = mm[4] * mm[4] + mm[5] * mm[5] + mm[6] * mm[6];
    f32 sz2 = mm[8] * mm[8] + mm[9] * mm[9] + mm[10] * mm[10];
    *wc  = vec3(w.x, w.y, w.z);
    *rad = m->radius * m_sqrt(m_max(sx2, m_max(sy2, sz2)));
    return g3d_sphere_visible(c, *wc, *rad);
}

static u32 material_nvar(const G3dMaterial* mat) {
    u32 nvar = 0;
    for (u32 i = 0; i < mat->attr_count; i++) nvar += g3d_attr_components((G3dAttr)mat->attrs[i]);
    ASSERT(nvar <= SHADE_MAX_VARYINGS);
    return nvar;
}

static void apply_raster_state(Rasterizer* r, const G3dMaterial* mat, u32 nvar) {
    raster_set_cull(r, FALSE);
    raster_set_depth_ex(r, mat->depth_test, mat->depth_write, DEPTH_GREATER, mat->alpha_test);
    raster_set_shader_ex(r, mat->shade_fn, mat->uniforms, mat->textures, nvar, mat->blend, mat->fused);
}

/* Köşeler [v0, v1): klip konumu, outcode, kırpılmayanlar için izdüşürülmüş RasterVert */
static void vertex_range(const G3dContext* c, const VertexCtx* vx, const Mat4* mvp, b32 all_inside,
                         RasterVert* sv, u32* oc, u32 v0, u32 v1) {
    const Mesh* m = vx->m;
    f32 *qx = (f32*)vx->qx, *qy = (f32*)vx->qy, *qz = (f32*)vx->qz, *qw = (f32*)vx->qw;
    if (g_g3d_opt & G3D_OPT_SIMD_XFORM) {
        g3d_transform_positions(mvp, m->px + v0, m->py + v0, m->pz + v0, v1 - v0,
                                qx + v0, qy + v0, qz + v0, qw + v0);
    } else {
        for (u32 i = v0; i < v1; i++) {
            Vec4 q = mat4_mul_vec4(*mvp, vec4(m->px[i], m->py[i], m->pz[i], 1.0f));
            qx[i] = q.x; qy[i] = q.y; qz[i] = q.z; qw[i] = q.w;
        }
    }
    f32 vpw = (f32)c->vp_w, vph = (f32)c->vp_h;
    for (u32 i = v0; i < v1; i++) {
        f32 x = qx[i], y = qy[i], z = qz[i], w = qw[i];
        u32 code = all_inside ? 0u : outcode4(x, y, z, w, c->guard);
        oc[i] = code;
        if (code & CLIP_NEAR) continue;
        RasterVert* o = &sv[i];
        f32 iw = 1.0f / w;
        o->x = (x * iw * 0.5f + 0.5f) * vpw;
        o->y = (0.5f - y * iw * 0.5f) * vph;
        if (c->snap) retro_snap_xy(&o->x, &o->y, c->snap);
        o->z = z * iw;
        o->inv_w = iw;
        vertex_attrs(vx, i, o->var);
    }
}

/* Üçgenler [t0, t1): outcode reddi, hızlı yol / kırpma yolu, arka yüz; çıkış sink'e */
static void tri_range(const G3dContext* c, const VertexCtx* vx, const RasterVert* sv, const u32* oc,
                      u32 nvar, u32 t0, u32 t1, TriSink* sink, G3dStats* st) {
    G3dCull cull = vx->mat->cull;
    const u32* ix = vx->m->indices;
    for (u32 t = t0; t < t1; t++) {
        u32 i0 = ix[t * 3], i1 = ix[t * 3 + 1], i2 = ix[t * 3 + 2];
        u32 o0 = oc[i0], o1 = oc[i1], o2 = oc[i2];
        if (o0 & o1 & o2 & CLIP_VIEW_MASK) {
            st->tris_rejected++;
            continue;
        }
        u32 must = (o0 | o1 | o2) & CLIP_MUST_MASK;
        if (must == 0) {
            if (!keep_face(cull, screen_area2(&sv[i0], &sv[i1], &sv[i2]))) {
                st->tris_backface++;
                continue;
            }
            sink_emit(sink, &sv[i0], &sv[i1], &sv[i2]);
            st->tris_out++;
            continue;
        }

        st->tris_clipped++;
        ClipVert in[3], poly[CLIP_MAX_VERTS];
        make_clipvert(vx, i0, &in[0]);
        make_clipvert(vx, i1, &in[1]);
        make_clipvert(vx, i2, &in[2]);
        u32 n = clip_triangle(in, must, c->guard, nvar, poly);
        if (n < 3) continue;
        RasterVert pv[CLIP_MAX_VERTS];
        for (u32 k = 0; k < n; k++) pv[k] = project(c, &poly[k], nvar);
        for (u32 k = 1; k + 1 < n; k++) {
            if (!keep_face(cull, screen_area2(&pv[0], &pv[k], &pv[k + 1]))) {
                st->tris_backface++;
                continue;
            }
            sink_emit(sink, &pv[0], &pv[k], &pv[k + 1]);
            st->tris_out++;
        }
    }
}

static void stats_add(G3dStats* d, const G3dStats* s) {
    d->tris_rejected += s->tris_rejected;
    d->tris_clipped  += s->tris_clipped;
    d->tris_backface += s->tris_backface;
    d->tris_out      += s->tris_out;
}

/* Ertelenmiş çizim komutu: matrisler ve materyal kopyalanır, raster durumu kayıtta sabitlenir */
struct G3dCmd {
    const Mesh*              m;
    G3dMaterial              mat;
    Mat4                     model, mvp, inv, smodel;
    const RasterShaderState* state;
    u32                      nvar;
    b32                      all_inside;
    f32 *qx, *qy, *qz, *qw;
    RasterVert*              sv;
    u32*                     oc;
};

static VertexCtx cmd_vertex_ctx(const G3dCmd* d) {
    VertexCtx vx = { d->m, &d->mat, &d->model, d->inv.m, d->smodel.m, d->qx, d->qy, d->qz, d->qw };
    return vx;
}

void g3d_set_deferred(G3dContext* c, u32 max_draws) {
    c->cmd_count = 0;
    c->cmd_cap   = max_draws;
    c->cmds      = max_draws ? ARENA_PUSH_ARRAY(c->frame, G3dCmd, max_draws) : NULL;
}

b32 g3d_draw(G3dContext* c, const Mesh* m, const Mat4* model, const G3dMaterial* mat) {
    c->stats.draws++;

    Vec3 wc;
    f32 rad;
    if (!draw_bounds(c, m, model, &wc, &rad)) {
        c->stats.draws_culled++;
        return FALSE;
    }
    /* Nesne tamamen görüş hacmi içindeyse köşe başına outcode gereksiz (hepsi 0) */
    b32 all_inside = (g_g3d_opt & G3D_OPT_INSIDE_SKIP) && sphere_fully_inside(c, wc, rad);
    if (all_inside) c->stats.draws_inside++;
    c->stats.tris_in += m->index_count / 3u;

    u32 nvar = material_nvar(mat);
    Rasterizer* r = c->r;
    apply_raster_state(r, mat, nvar);

    if (c->cmds) {
        if (c->cmd_count == c->cmd_cap) g3d_flush(c);
        G3dCmd* d = &c->cmds[c->cmd_count++];
        d->m          = m;
        d->mat        = *mat;
        d->model      = *model;
        d->mvp        = mat4_mul(c->view_proj, *model);
        d->inv        = mat4_inverse(*model);
        d->smodel     = mat4_mul(c->shadow_mat, *model);
        d->state      = raster_pin_state(r);
        d->nvar       = nvar;
        d->all_inside = all_inside;
        return TRUE;
    }

    PROF_BEGIN("g3d_vertex");
    Mat4 mvp = mat4_mul(c->view_proj, *model);
    Mat4 inv = mat4_inverse(*model);   /* normal matrisi = inv^T */
    Mat4 smodel = mat4_mul(c->shadow_mat, *model);
    u32 vn = m->vertex_count;
    VertexCtx vx = { m, mat, model, inv.m, smodel.m,
                     ARENA_PUSH_ARRAY_RAW(c->frame, f32, vn), ARENA_PUSH_ARRAY_RAW(c->frame, f32, vn),
                     ARENA_PUSH_ARRAY_RAW(c->frame, f32, vn), ARENA_PUSH_ARRAY_RAW(c->frame, f32, vn) };
    RasterVert* sv = ARENA_PUSH_ARRAY_RAW(c->frame, RasterVert, vn);
    u32*        oc = ARENA_PUSH_ARRAY_RAW(c->frame, u32, vn);
    vertex_range(c, &vx, &mvp, all_inside, sv, oc, 0, vn);
    PROF_END();

    PROF_BEGIN("g3d_tri");
    TriSink sink;
    __builtin_memset(&sink, 0, sizeof(sink));
    sink.r = r;
    tri_range(c, &vx, sv, oc, nvar, 0, m->index_count / 3u, &sink, &c->stats);
    PROF_END();
    return TRUE;
}

/* ---------- Paralel boşaltma ---------- */

static usize g_g3d_slice_limit = 0;

void g3d_set_slice_limit(usize bytes) {
    g_g3d_slice_limit = bytes;
}

#define G3D_VERT_CHUNK 4096u   /* köşe işi başına köşe */
#define G3D_TRI_CHUNK  1024u   /* üçgen işi başına üçgen */

typedef struct { u32 cmd, v0, v1; } VertItem;

typedef struct {
    u32        cmd, t0, t1;
    BinnedTri* tris;
    u32        count, submitted;
    b32        failed;     /* dilim yetmedi → ana iş parçacığında yeniden işlenir */
    G3dStats   st;
} TriItem;

typedef struct {
    const G3dContext* c;
    G3dCmd*           cmds;
    VertItem*         vitems;
    TriItem*          titems;
    Arena*            tarena;  /* [narena] iş parçacığı başına dilim */
    u32               narena;
    i32               fb_w, fb_h;
} FlushCtx;

static void flush_vertex_job(u32 start, u32 end, void* user) {
    FlushCtx* f = (FlushCtx*)user;
    for (u32 k = start; k < end; k++) {
        const VertItem* it = &f->vitems[k];
        G3dCmd* d = &f->cmds[it->cmd];
        VertexCtx vx = cmd_vertex_ctx(d);
        vertex_range(f->c, &vx, &d->mvp, d->all_inside, d->sv, d->oc, it->v0, it->v1);
    }
}

/* Taşmayı panik yerine başarısızlık olarak bildiren dilim ayırması */
static void* try_push(Arena* a, usize size) {
    usize at = ALIGN_UP(a->used, (usize)16);
    if (at > a->cap || size > a->cap - at) return NULL;
    return arena_push_raw(a, size, 16);
}

static void run_tri_item(FlushCtx* f, TriItem* it, Arena* a) {
    const G3dCmd* d = &f->cmds[it->cmd];
    u32 n = it->t1 - it->t0;
    /* Kırpma bir üçgeni en çok CLIP_MAX_VERTS-2 parçaya böler; dilimde iyimser kapasite, ana
     * iş parçacığındaki yedekte kesin üst sınır */
    u32 cap = a ? n + n / 4u + 16u : n * (CLIP_MAX_VERTS - 2u);
    TriSink sink;
    __builtin_memset(&sink, 0, sizeof(sink));
    sink.state   = d->state;
    sink.vstride = d->nvar * 3u;
    sink.cap     = cap;
    sink.fb_w    = f->fb_w;
    sink.fb_h    = f->fb_h;
    if (a) {
        sink.tris = (BinnedTri*)try_push(a, sizeof(BinnedTri) * cap);
        sink.vc   = sink.vstride ? (f32*)try_push(a, sizeof(f32) * sink.vstride * cap) : NULL;
        if (!sink.tris || (sink.vstride && !sink.vc)) {
            it->failed = TRUE;
            return;
        }
    } else {
        Arena* fr = f->c->frame;
        sink.tris = ARENA_PUSH_ARRAY_RAW(fr, BinnedTri, cap);
        sink.vc   = sink.vstride ? ARENA_PUSH_ARRAY_RAW(fr, f32, sink.vstride * cap) : NULL;
    }
    VertexCtx vx = cmd_vertex_ctx(d);
    __builtin_memset(&it->st, 0, sizeof(it->st));
    tri_range(f->c, &vx, d->sv, d->oc, d->nvar, it->t0, it->t1, &sink, &it->st);
    if (sink.overflow) {
        it->failed = TRUE;
        return;
    }
    it->tris      = sink.tris;
    it->count     = sink.count;
    it->submitted = sink.submitted;
    it->failed    = FALSE;
}

static void flush_tri_job(u32 start, u32 end, void* user) {
    FlushCtx* f = (FlushCtx*)user;
    u32 tid = thread_id();
    Arena* a = (tid < f->narena) ? &f->tarena[tid] : NULL;
    for (u32 k = start; k < end; k++) {
        if (a) run_tri_item(f, &f->titems[k], a);
        else   f->titems[k].failed = TRUE;
    }
}

void g3d_flush(G3dContext* c) {
    u32 ncmd = c->cmd_count;
    if (ncmd == 0) return;
    c->cmd_count = 0;
    Arena* fr = c->frame;

    /* İş listeleri + köşe dizileri (ana iş parçacığı, sıfırlamasız) */
    u32 nv = 0, nt = 0;
    usize out_bytes = 0;
    for (u32 i = 0; i < ncmd; i++) {
        const Mesh* m = c->cmds[i].m;
        u32 tc = m->index_count / 3u;
        nv += (m->vertex_count + G3D_VERT_CHUNK - 1u) / G3D_VERT_CHUNK;
        nt += (tc + G3D_TRI_CHUNK - 1u) / G3D_TRI_CHUNK;
        out_bytes += (usize)(tc + tc / 4u + 16u) * (sizeof(BinnedTri) + sizeof(f32) * c->cmds[i].nvar * 3u + 32u);
    }
    VertItem* vitems = ARENA_PUSH_ARRAY_RAW(fr, VertItem, nv ? nv : 1u);
    TriItem*  titems = ARENA_PUSH_ARRAY(fr, TriItem, nt ? nt : 1u);
    nv = 0; nt = 0;
    for (u32 i = 0; i < ncmd; i++) {
        G3dCmd* d = &c->cmds[i];
        u32 vn = d->m->vertex_count, tc = d->m->index_count / 3u;
        d->qx = ARENA_PUSH_ARRAY_RAW(fr, f32, vn);
        d->qy = ARENA_PUSH_ARRAY_RAW(fr, f32, vn);
        d->qz = ARENA_PUSH_ARRAY_RAW(fr, f32, vn);
        d->qw = ARENA_PUSH_ARRAY_RAW(fr, f32, vn);
        d->sv = ARENA_PUSH_ARRAY_RAW(fr, RasterVert, vn);
        d->oc = ARENA_PUSH_ARRAY_RAW(fr, u32, vn);
        for (u32 v = 0; v < vn; v += G3D_VERT_CHUNK) {
            vitems[nv].cmd = i; vitems[nv].v0 = v; vitems[nv].v1 = MIN(v + G3D_VERT_CHUNK, vn);
            nv++;
        }
        for (u32 t = 0; t < tc; t += G3D_TRI_CHUNK) {
            titems[nt].cmd = i; titems[nt].t0 = t; titems[nt].t1 = MIN(t + G3D_TRI_CHUNK, tc);
            nt++;
        }
    }

    /* İş parçacığı dilimleri: tahmini çıktının ~2 katı, boş belleğin yarısını geçmeden */
    u32 nth = thread_count();
    if (nth > THREAD_MAX_COUNT) nth = THREAD_MAX_COUNT;
    Arena tarena[THREAD_MAX_COUNT];
    usize free_b = fr->cap - fr->used;
    usize slice = (out_bytes * 2u) / nth + KB(256);
    if (slice * nth > free_b / 2u) slice = (free_b / 2u) / nth;
    if (g_g3d_slice_limit && slice > g_g3d_slice_limit) slice = g_g3d_slice_limit;
    slice &= ~(usize)15;
    for (u32 t = 0; t < nth; t++) tarena[t] = arena_sub(fr, slice);

    FlushCtx f;
    f.c      = c;
    f.cmds   = c->cmds;
    f.vitems = vitems;
    f.titems = titems;
    f.tarena = tarena;
    f.narena = nth;
    f.fb_w   = (i32)c->r->fb->w;
    f.fb_h   = (i32)c->r->fb->h;

    PROF_BEGIN("g3d_vertex");
    job_parallel_for(nv, 1, flush_vertex_job, &f);
    PROF_END();
    PROF_BEGIN("g3d_tri");
    job_parallel_for(nt, 1, flush_tri_job, &f);
    PROF_END();

    /* Gönderim sırasıyla dağıt; dilimi yetmeyen işler burada kesin kapasiteyle yeniden işlenir */
    u32 fallback = 0;
    for (u32 k = 0; k < nt; k++) {
        TriItem* it = &titems[k];
        if (it->failed) {
            run_tri_item(&f, it, NULL);
            fallback++;
        }
        stats_add(&c->stats, &it->st);
        raster_bin_tris(c->r, it->tris, it->count, it->submitted);
    }
    c->stats.items_fallback += fallback;
}

Mat4 mat4_translate(Vec3 t) {
    Mat4 r = mat4_identity();
    r.m[12] = t.x; r.m[13] = t.y; r.m[14] = t.z;
    return r;
}

Mat4 mat4_scale(Vec3 s) {
    Mat4 r = mat4_identity();
    r.m[0] = s.x; r.m[5] = s.y; r.m[10] = s.z;
    return r;
}

Mat4 mat4_rotate_y(f32 rad) {
    Mat4 r = mat4_identity();
    f32 cs = m_cos(rad), sn = m_sin(rad);
    r.m[0] = cs;  r.m[2] = -sn;
    r.m[8] = sn;  r.m[10] = cs;
    return r;
}

Mat4 mat4_rotate_x(f32 rad) {
    Mat4 r = mat4_identity();
    f32 cs = m_cos(rad), sn = m_sin(rad);
    r.m[5] = cs;  r.m[6] = sn;
    r.m[9] = -sn; r.m[10] = cs;
    return r;
}

Mat4 mat4_trs(Vec3 t, f32 rot_y, f32 rot_x, Vec3 s) {
    return mat4_mul(mat4_translate(t), mat4_mul(mat4_rotate_y(rot_y), mat4_mul(mat4_rotate_x(rot_x), mat4_scale(s))));
}
