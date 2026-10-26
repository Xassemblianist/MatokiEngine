/* gfx3d/pipeline.c — Vertex dönüşümü, nesne culling, outcode ile hızlı yol / kırpma yolu ayrımı,
 * ekran izdüşümü ve arka yüz ayıklama. Köşe başına sonuçlar frame arena'da SoA'sız ClipVert
 * dizisinde tutulur; kırpma gerektirmeyen üçgenler köşe başına önceden izdüşürülmüş RasterVert'i kullanır.
 */
#include "gfx3d/pipeline.h"
#include "core/log.h"
#include "core/profiler.h"
#include "gfx3d/vertex.h"

u32 g3d_attr_components(G3dAttr a) {
    switch (a) {
    case G3D_ATTR_UV:         return 2;
    case G3D_ATTR_NORMAL:     return 3;
    case G3D_ATTR_VIEW_DEPTH: return 1;
    case G3D_ATTR_COLOR:      return 4;
    case G3D_ATTR_WORLD_POS:  return 3;
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
    __builtin_memset(&c->stats, 0, sizeof(c->stats));

    /* Gribb–Hartmann: klip düzlemleri view_proj satırlarından (dünya uzayı) */
    Vec4 r0 = mat_row(&c->view_proj, 0), r1 = mat_row(&c->view_proj, 1);
    Vec4 r2 = mat_row(&c->view_proj, 2), r3 = mat_row(&c->view_proj, 3);
    c->planes[0] = plane_norm(vec4_add(r3, r0));   /* x >= -w */
    c->planes[1] = plane_norm(vec4_sub(r3, r0));   /* x <=  w */
    c->planes[2] = plane_norm(vec4_add(r3, r1));   /* y >= -w */
    c->planes[3] = plane_norm(vec4_sub(r3, r1));   /* y <=  w */
    c->planes[4] = plane_norm(vec4_sub(r3, r2));   /* z <=  w (near) */
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
        }
    }
}

/* Kırpma yolu (nadir): klip köşesi talep üzerine kurulur */
static void make_clipvert(const VertexCtx* v, u32 i, ClipVert* o) {
    o->x = v->qx[i]; o->y = v->qy[i]; o->z = v->qz[i]; o->w = v->qw[i];
    vertex_attrs(v, i, o->var);
}

b32 g3d_draw(G3dContext* c, const Mesh* m, const Mat4* model, const G3dMaterial* mat) {
    c->stats.draws++;

    /* Nesne culling: yerel sınır küresi → dünya (en büyük eksen ölçeğiyle) */
    const f32* mm = model->m;
    Vec4 wc = mat4_mul_vec4(*model, vec4(m->center.x, m->center.y, m->center.z, 1.0f));
    f32 sx2 = mm[0] * mm[0] + mm[1] * mm[1] + mm[2] * mm[2];
    f32 sy2 = mm[4] * mm[4] + mm[5] * mm[5] + mm[6] * mm[6];
    f32 sz2 = mm[8] * mm[8] + mm[9] * mm[9] + mm[10] * mm[10];
    f32 smax = m_sqrt(m_max(sx2, m_max(sy2, sz2)));
    if (!g3d_sphere_visible(c, vec3(wc.x, wc.y, wc.z), m->radius * smax)) {
        c->stats.draws_culled++;
        return FALSE;
    }

    u32 nvar = 0;
    for (u32 i = 0; i < mat->attr_count; i++) nvar += g3d_attr_components((G3dAttr)mat->attrs[i]);
    ASSERT(nvar <= SHADE_MAX_VARYINGS);

    Rasterizer* r = c->r;
    raster_set_cull(r, FALSE);
    raster_set_depth_ex(r, mat->depth_test, mat->depth_write, DEPTH_GREATER, mat->alpha_test);
    raster_set_shader_ex(r, mat->shade_fn, mat->uniforms, mat->textures, nvar, mat->blend, mat->fused);

    PROF_BEGIN("g3d_vertex");
    /* --- Vertex aşaması --- */
    Mat4 mvp = mat4_mul(c->view_proj, *model);
    Mat4 inv = mat4_inverse(*model);   /* normal matrisi = inv^T */
    VertexCtx vx = { m, mat, model, inv.m, NULL, NULL, NULL, NULL };
    u32 vn = m->vertex_count;
    f32* qx = ARENA_PUSH_ARRAY(c->frame, f32, vn);
    f32* qy = ARENA_PUSH_ARRAY(c->frame, f32, vn);
    f32* qz = ARENA_PUSH_ARRAY(c->frame, f32, vn);
    f32* qw = ARENA_PUSH_ARRAY(c->frame, f32, vn);
    vx.qx = qx; vx.qy = qy; vx.qz = qz; vx.qw = qw;
    RasterVert* sv = ARENA_PUSH_ARRAY(c->frame, RasterVert, vn);
    u32*        oc = ARENA_PUSH_ARRAY(c->frame, u32, vn);

    if (g_g3d_opt & G3D_OPT_SIMD_XFORM) {
        g3d_transform_positions(&mvp, m->px, m->py, m->pz, vn, qx, qy, qz, qw);
    } else {
        for (u32 i = 0; i < vn; i++) {
            Vec4 q = mat4_mul_vec4(mvp, vec4(m->px[i], m->py[i], m->pz[i], 1.0f));
            qx[i] = q.x; qy[i] = q.y; qz[i] = q.z; qw[i] = q.w;
        }
    }

    /* Nesne tamamen görüş hacmi içindeyse köşe başına outcode gereksiz (hepsi 0) */
    b32 all_inside = (g_g3d_opt & G3D_OPT_INSIDE_SKIP) &&
                     sphere_fully_inside(c, vec3(wc.x, wc.y, wc.z), m->radius * smax);
    if (all_inside) c->stats.draws_inside++;

    f32 vpw = (f32)c->vp_w, vph = (f32)c->vp_h;
    for (u32 i = 0; i < vn; i++) {
        f32 x = qx[i], y = qy[i], z = qz[i], w = qw[i];
        u32 code = all_inside ? 0u : outcode4(x, y, z, w, c->guard);
        oc[i] = code;
        if (code & CLIP_NEAR) continue;
        RasterVert* o = &sv[i];
        f32 iw = 1.0f / w;
        o->x = (x * iw * 0.5f + 0.5f) * vpw;
        o->y = (0.5f - y * iw * 0.5f) * vph;
        o->z = z * iw;
        o->inv_w = iw;
        vertex_attrs(&vx, i, o->var);
    }
    PROF_END();

    /* --- Üçgenler --- */
    PROF_BEGIN("g3d_tri");
    u32 tri_count = m->index_count / 3u;
    c->stats.tris_in += tri_count;
    const u32* ix = m->indices;
    for (u32 t = 0; t < tri_count; t++) {
        u32 i0 = ix[t * 3], i1 = ix[t * 3 + 1], i2 = ix[t * 3 + 2];
        u32 o0 = oc[i0], o1 = oc[i1], o2 = oc[i2];
        if (o0 & o1 & o2 & CLIP_VIEW_MASK) {
            c->stats.tris_rejected++;
            continue;
        }
        u32 must = (o0 | o1 | o2) & CLIP_MUST_MASK;
        if (must == 0) {
            if (!keep_face(mat->cull, screen_area2(&sv[i0], &sv[i1], &sv[i2]))) {
                c->stats.tris_backface++;
                continue;
            }
            raster_triangle(r, sv[i0], sv[i1], sv[i2]);
            c->stats.tris_out++;
            continue;
        }

        c->stats.tris_clipped++;
        ClipVert in[3], poly[CLIP_MAX_VERTS];
        make_clipvert(&vx, i0, &in[0]);
        make_clipvert(&vx, i1, &in[1]);
        make_clipvert(&vx, i2, &in[2]);
        u32 n = clip_triangle(in, must, c->guard, nvar, poly);
        if (n < 3) continue;
        RasterVert pv[CLIP_MAX_VERTS];
        for (u32 k = 0; k < n; k++) pv[k] = project(c, &poly[k], nvar);
        for (u32 k = 1; k + 1 < n; k++) {
            if (!keep_face(mat->cull, screen_area2(&pv[0], &pv[k], &pv[k + 1]))) {
                c->stats.tris_backface++;
                continue;
            }
            raster_triangle(r, pv[0], pv[k], pv[k + 1]);
            c->stats.tris_out++;
        }
    }
    PROF_END();
    return TRUE;
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
