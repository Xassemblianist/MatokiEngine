/* gfx3d/clip.c — Outcode sınıflandırma, guard band türetimi ve Sutherland–Hodgman kırpıcı.
 * Düzlem mesafeleri d >= 0 "içeride" demektir. Kesişim noktası içerideki köşeden
 * p = in + t * (out - in), t = d_in / (d_in - d_out) ile hesaplanır (kenar yönünden bağımsız).
 */
#include "gfx3d/clip.h"

/* Raster i32 kenar fonksiyonu: e = a*px + b*py + c; a, b 24.8 cinsinden kenar farkları (<= aralık*256),
 * px, py ekran içinde [0,W)x[0,H), köşeler guard band içinde [-G, W+G] x [-G, H+G].
 *   |c|           <= 256 * S,  S = (H+2G)(W+G) + (W+2G)(H+G)
 *   |a*px + b*py| <= 256 * ((H+2G)W + (W+2G)H)
 * Ara toplamlar dahil 2.0e9 (< 2^31) altında kalan en büyük G (64'ün katı). Hiçbiri sığmazsa 0
 * (o zaman x/y için viewport'un kendisine kırpılır). Bkz. DECISIONS.md D-081. */
u32 clip_guard_margin_px(u32 vp_w, u32 vp_h) {
    u64 w = vp_w, h = vp_h;
    u32 best = 0;
    for (u32 g = 0; g <= 8192; g += 64) {
        u64 s = (h + 2u * g) * (w + g) + (w + 2u * g) * (h + g);
        u64 p = 256u * ((h + 2u * g) * w + (w + 2u * g) * h) + 256u * s;
        if (p >= 2000000000ull) break;
        best = g;
    }
    return best;
}

ClipGuard clip_guard_for_viewport(u32 vp_w, u32 vp_h) {
    u32 g = clip_guard_margin_px(vp_w, vp_h);
    ClipGuard cg;
    cg.gx = 1.0f + 2.0f * (f32)g / (f32)(vp_w ? vp_w : 1u);
    cg.gy = 1.0f + 2.0f * (f32)g / (f32)(vp_h ? vp_h : 1u);
    return cg;
}

u32 clip_outcode(const ClipVert* v, ClipGuard g) {
    u32 c = 0;
    f32 w = v->w;
    if (v->z > w)        c |= CLIP_NEAR;
    if (v->x < -w)       c |= CLIP_LEFT;
    if (v->x >  w)       c |= CLIP_RIGHT;
    if (v->y < -w)       c |= CLIP_BOTTOM;
    if (v->y >  w)       c |= CLIP_TOP;
    if (v->x < -g.gx * w) c |= CLIP_G_LEFT;
    if (v->x >  g.gx * w) c |= CLIP_G_RIGHT;
    if (v->y < -g.gy * w) c |= CLIP_G_BOTTOM;
    if (v->y >  g.gy * w) c |= CLIP_G_TOP;
    return c;
}

static f32 plane_dist(const ClipVert* v, u32 plane, ClipGuard g) {
    switch (plane) {
    case CLIP_NEAR:     return v->w - v->z;
    case CLIP_G_LEFT:   return g.gx * v->w + v->x;
    case CLIP_G_RIGHT:  return g.gx * v->w - v->x;
    case CLIP_G_BOTTOM: return g.gy * v->w + v->y;
    case CLIP_G_TOP:    return g.gy * v->w - v->y;
    default:            return 0.0f;
    }
}

static void lerp_vert(ClipVert* o, const ClipVert* in, const ClipVert* out, f32 t, u32 nvar) {
    o->x = in->x + t * (out->x - in->x);
    o->y = in->y + t * (out->y - in->y);
    o->z = in->z + t * (out->z - in->z);
    o->w = in->w + t * (out->w - in->w);
    for (u32 k = 0; k < nvar; k++) {
        o->var[k] = in->var[k] + t * (out->var[k] - in->var[k]);
    }
}

static void copy_vert(ClipVert* o, const ClipVert* s, u32 nvar) {
    o->x = s->x; o->y = s->y; o->z = s->z; o->w = s->w;
    for (u32 k = 0; k < nvar; k++) o->var[k] = s->var[k];
}

static u32 clip_against(const ClipVert* src, u32 n, u32 plane, ClipGuard g, u32 nvar, ClipVert* dst) {
    u32 m = 0;
    for (u32 i = 0; i < n; i++) {
        const ClipVert* a = &src[i];
        const ClipVert* b = &src[(i + 1u) % n];
        f32 da = plane_dist(a, plane, g);
        f32 db = plane_dist(b, plane, g);
        b32 ina = (da >= 0.0f), inb = (db >= 0.0f);
        if (ina) copy_vert(&dst[m++], a, nvar);
        if (ina != inb) {
            if (ina) lerp_vert(&dst[m++], a, b, da / (da - db), nvar);
            else     lerp_vert(&dst[m++], b, a, db / (db - da), nvar);
        }
    }
    return m;
}

u32 clip_triangle(const ClipVert in[3], u32 planes, ClipGuard g, u32 nvar,
                  ClipVert out[CLIP_MAX_VERTS]) {
    ClipVert tmp[CLIP_MAX_VERTS];
    ClipVert* src = out;
    ClipVert* dst = tmp;
    for (u32 i = 0; i < 3; i++) copy_vert(&src[i], &in[i], nvar);
    u32 n = 3;
    static const u32 order[5] = { CLIP_NEAR, CLIP_G_LEFT, CLIP_G_RIGHT, CLIP_G_BOTTOM, CLIP_G_TOP };
    for (u32 p = 0; p < 5 && n >= 3; p++) {
        if (!(planes & order[p])) continue;
        n = clip_against(src, n, order[p], g, nvar, dst);
        ClipVert* t = src; src = dst; dst = t;
    }
    if (n < 3) return 0;
    if (src != out) {
        for (u32 i = 0; i < n; i++) copy_vert(&out[i], &src[i], nvar);
    }
    return n;
}
