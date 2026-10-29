/* phys2/gjk.c — GJK (en yakın nokta, Johnson yerine bölge testleri: Ericson RTCD 5.1) ve EPA.
 * GJK simplex'i her turda orijine en yakın alt simplex'e indirger; çekirdekler üzerindeki en
 * yakın noktalar barisentrik ağırlıklarla geri kurulur. EPA: GJK tetrahedronundan başlayan,
 * ufuk kenarlarıyla genişleyen politop (≤ 64 köşe, ≤ 128 yüz).
 * SamurayBalgami/Matoki'den uyarlandı (izinle).
 */
#include "phys2/gjk.h"
#include "phys2/pmath.h"

void phys2_core_triangle(Phys2Core* c, Vec3 a, Vec3 b, Vec3 cc) {
    c->kind = PHYS2_CORE_POLY;
    c->radius = 0.0f;
    c->tri_v[0] = a;
    c->tri_v[1] = b;
    c->tri_v[2] = cc;
    Vec3 n = phys2_v3_norm(vec3_cross(vec3_sub(b, a), vec3_sub(cc, a)));
    c->tri_f[0].n = n;
    c->tri_f[0].d = vec3_dot(n, a);
    c->tri_f[0].first = 0;
    c->tri_f[0].count = 3;
    c->tri_f[1].n = vec3_scale(n, -1.0f);
    c->tri_f[1].d = -c->tri_f[0].d;
    c->tri_f[1].first = 3;
    c->tri_f[1].count = 3;
    c->tri_i[0] = 0; c->tri_i[1] = 1; c->tri_i[2] = 2;
    c->tri_i[3] = 2; c->tri_i[4] = 1; c->tri_i[5] = 0;
    c->tri_h.verts = c->tri_v;
    c->tri_h.vcount = 3;
    c->tri_h.faces = c->tri_f;
    c->tri_h.fcount = 2;
    c->tri_h.idx = c->tri_i;
    c->tri_h.icount = 6;
    c->hull = &c->tri_h;
    c->scale = vec3(1, 1, 1);
    c->pos = vec3(0, 0, 0);
    c->rot.m[0] = c->rot.m[4] = c->rot.m[8] = 1.0f;
    c->rot.m[1] = c->rot.m[2] = c->rot.m[3] = c->rot.m[5] = c->rot.m[6] = c->rot.m[7] = 0.0f;
}

Vec3 phys2_core_support(const Phys2Core* c, Vec3 d) {
    switch (c->kind) {
    case PHYS2_CORE_POINT:
        return c->p0;
    case PHYS2_CORE_SEGMENT:
        return vec3_dot(d, vec3_sub(c->p1, c->p0)) > 0.0f ? c->p1 : c->p0;
    default: {
        /* yerel yön (R^T d), ölçekli köşede en büyük izdüşüm */
        const f32* m = c->rot.m;
        Vec3 dl = vec3(m[0] * d.x + m[1] * d.y + m[2] * d.z, m[3] * d.x + m[4] * d.y + m[5] * d.z, m[6] * d.x + m[7] * d.y + m[8] * d.z);
        dl = vec3(dl.x * c->scale.x, dl.y * c->scale.y, dl.z * c->scale.z);
        const Phys2ConvexHull* h = c->hull;
        u32 best = 0;
        f32 bd = vec3_dot(h->verts[0], dl);
        for (u32 i = 1; i < h->vcount; i++) {
            f32 s = vec3_dot(h->verts[i], dl);
            if (s > bd) { bd = s; best = i; }
        }
        return phys2_core_vertex(c, best);
    }
    }
}

/* ---- GJK ---- */

typedef struct Simplex {
    u32  n;
    Vec3 w[4], a[4], b[4];
    f32  l[4];
} Simplex;

static void keep(Simplex* s, const u32* ids, const f32* lam, u32 k) {
    Vec3 w[4], a[4], b[4];
    for (u32 i = 0; i < k; i++) { w[i] = s->w[ids[i]]; a[i] = s->a[ids[i]]; b[i] = s->b[ids[i]]; }
    for (u32 i = 0; i < k; i++) { s->w[i] = w[i]; s->a[i] = a[i]; s->b[i] = b[i]; s->l[i] = lam[i]; }
    s->n = k;
}

/* Üçgen (i0, i1, i2) üzerinde orijine en yakın nokta; simplex'i bölgeye indirger */
static Vec3 closest_tri(Simplex* s, u32 i0, u32 i1, u32 i2) {
    Vec3 A = s->w[i0], B = s->w[i1], C = s->w[i2];
    Vec3 ab = vec3_sub(B, A), ac = vec3_sub(C, A), ap = vec3_scale(A, -1.0f);
    f32 d1 = vec3_dot(ab, ap), d2 = vec3_dot(ac, ap);
    if (d1 <= 0.0f && d2 <= 0.0f) { u32 id[1] = { i0 }; f32 l[1] = { 1 }; keep(s, id, l, 1); return A; }
    Vec3 bp = vec3_scale(B, -1.0f);
    f32 d3 = vec3_dot(ab, bp), d4 = vec3_dot(ac, bp);
    if (d3 >= 0.0f && d4 <= d3) { u32 id[1] = { i1 }; f32 l[1] = { 1 }; keep(s, id, l, 1); return B; }
    f32 vc = d1 * d4 - d3 * d2;
    if (vc <= 0.0f && d1 >= 0.0f && d3 <= 0.0f) {
        f32 v = d1 / (d1 - d3);
        u32 id[2] = { i0, i1 }; f32 l[2] = { 1.0f - v, v };
        keep(s, id, l, 2);
        return vec3_add(A, vec3_scale(ab, v));
    }
    Vec3 cp = vec3_scale(C, -1.0f);
    f32 d5 = vec3_dot(ab, cp), d6 = vec3_dot(ac, cp);
    if (d6 >= 0.0f && d5 <= d6) { u32 id[1] = { i2 }; f32 l[1] = { 1 }; keep(s, id, l, 1); return C; }
    f32 vb = d5 * d2 - d1 * d6;
    if (vb <= 0.0f && d2 >= 0.0f && d6 <= 0.0f) {
        f32 w = d2 / (d2 - d6);
        u32 id[2] = { i0, i2 }; f32 l[2] = { 1.0f - w, w };
        keep(s, id, l, 2);
        return vec3_add(A, vec3_scale(ac, w));
    }
    f32 va = d3 * d6 - d5 * d4;
    if (va <= 0.0f && (d4 - d3) >= 0.0f && (d5 - d6) >= 0.0f) {
        f32 w = (d4 - d3) / ((d4 - d3) + (d5 - d6));
        u32 id[2] = { i1, i2 }; f32 l[2] = { 1.0f - w, w };
        keep(s, id, l, 2);
        return vec3_add(B, vec3_scale(vec3_sub(C, B), w));
    }
    f32 den = 1.0f / (va + vb + vc);
    f32 v = vb * den, w = vc * den;
    u32 id[3] = { i0, i1, i2 }; f32 l[3] = { 1.0f - v - w, v, w };
    keep(s, id, l, 3);
    return vec3_add(A, vec3_add(vec3_scale(ab, v), vec3_scale(ac, w)));
}

/* orijin ile p, (a,b,c) düzleminin zıt taraflarında mı */
static b32 outside_plane(Vec3 a, Vec3 b, Vec3 c, Vec3 p) {
    Vec3 n = vec3_cross(vec3_sub(b, a), vec3_sub(c, a));
    f32 so = vec3_dot(vec3_scale(a, -1.0f), n);
    f32 sp = vec3_dot(vec3_sub(p, a), n);
    return so * sp < 0.0f;
}

/* dönüş: TRUE = orijin tetrahedronun içinde */
static b32 closest_simplex(Simplex* s, Vec3* v) {
    if (s->n == 1) {
        s->l[0] = 1.0f;
        *v = s->w[0];
        return FALSE;
    }
    if (s->n == 2) {
        Vec3 A = s->w[0], ab = vec3_sub(s->w[1], A);
        f32 den = vec3_dot(ab, ab);
        f32 t = den > 0.0f ? -vec3_dot(A, ab) / den : 0.0f;
        if (t <= 0.0f) { u32 id[1] = { 0 }; f32 l[1] = { 1 }; keep(s, id, l, 1); *v = A; return FALSE; }
        if (t >= 1.0f) { u32 id[1] = { 1 }; f32 l[1] = { 1 }; keep(s, id, l, 1); *v = s->w[0]; return FALSE; }
        s->l[0] = 1.0f - t;
        s->l[1] = t;
        *v = vec3_add(A, vec3_scale(ab, t));
        return FALSE;
    }
    if (s->n == 3) {
        *v = closest_tri(s, 0, 1, 2);
        return FALSE;
    }
    /* tetrahedron. Yassıysa (hacim ≈ 0) işaret testleri güvenilmez: içeride kararı verme, dört
     * yüzün en yakınını al (gerçekten değiyorsa mesafe ≈ 0 çıkar ve çağıran kesişim sayar). */
    static const u32 FACES[4][4] = { { 0, 1, 2, 3 }, { 0, 2, 3, 1 }, { 0, 3, 1, 2 }, { 1, 3, 2, 0 } };
    Vec3 e1 = vec3_sub(s->w[1], s->w[0]), e2 = vec3_sub(s->w[2], s->w[0]), e3 = vec3_sub(s->w[3], s->w[0]);
    f32 vol = m_abs(vec3_dot(e1, vec3_cross(e2, e3)));
    f32 sc = m_max(vec3_dot(e1, e1), m_max(vec3_dot(e2, e2), vec3_dot(e3, e3)));
    b32 flat = vol * vol <= 1e-10f * sc * sc * sc;
    b32 any_out = FALSE;
    f32 best = 3.4e38f;
    Simplex best_s = *s;
    Vec3 best_v = vec3(0, 0, 0);
    for (u32 f = 0; f < 4; f++) {
        const u32* q = FACES[f];
        if (!flat && !outside_plane(s->w[q[0]], s->w[q[1]], s->w[q[2]], s->w[q[3]])) continue;
        any_out = TRUE;
        Simplex t = *s;
        Vec3 c = closest_tri(&t, q[0], q[1], q[2]);
        f32 d = vec3_dot(c, c);
        if (d < best) { best = d; best_s = t; best_v = c; }
    }
    if (!any_out) return TRUE;
    *s = best_s;
    *v = best_v;
    return FALSE;
}

void phys2_gjk_distance(const Phys2Core* A, const Phys2Core* B, Phys2GjkResult* r) {
    Simplex s;
    Vec3 d = vec3(1, 0, 0);
    s.a[0] = phys2_core_support(A, d);
    s.b[0] = phys2_core_support(B, vec3_scale(d, -1.0f));
    s.w[0] = vec3_sub(s.a[0], s.b[0]);
    s.l[0] = 1.0f;
    s.n = 1;
    Vec3 v = s.w[0];
    r->overlap = FALSE;
    r->exit = 2;
    /* en iyi (en küçük |v|) durum: f32'de simplex küçültme salınabilir; mesafe azalmıyorsa dur */
    Simplex best_s = s;
    Vec3 best_v = v;
    f32 best_vv = 3.4e38f;
    u32 it = 0;
    for (; it < 48; it++) {
        f32 vv = vec3_dot(v, v);
        if (vv < 1e-12f) { r->overlap = TRUE; r->exit = 3; break; }
        if (vv < best_vv * (1.0f - 1e-6f)) {
            best_vv = vv;
            best_s = s;
            best_v = v;
        } else if (it > 0) {
            r->exit = 4;   /* ilerleme durdu (döngü) */
            break;
        }
        Vec3 dir = vec3_scale(v, -1.0f);
        Vec3 a = phys2_core_support(A, dir), b = phys2_core_support(B, v);
        Vec3 w = vec3_sub(a, b);
        if (vv - vec3_dot(v, w) <= 1e-6f * vv + 1e-10f) { r->exit = 0; break; }   /* ilerleme yok */
        b32 dup = FALSE;
        for (u32 i = 0; i < s.n; i++) {
            Vec3 e = vec3_sub(s.w[i], w);
            if (vec3_dot(e, e) < 1e-14f) dup = TRUE;
        }
        if (dup) { r->exit = 1; break; }
        s.w[s.n] = w;
        s.a[s.n] = a;
        s.b[s.n] = b;
        s.n++;
        if (closest_simplex(&s, &v)) { r->overlap = TRUE; r->exit = 3; break; }
    }
    r->iters = it;
    if (!r->overlap && vec3_dot(v, v) > best_vv) {
        s = best_s;
        v = best_v;
    }
    Vec3 pa = vec3(0, 0, 0), pb = vec3(0, 0, 0);
    for (u32 i = 0; i < s.n; i++) {
        pa = vec3_add(pa, vec3_scale(s.a[i], s.l[i]));
        pb = vec3_add(pb, vec3_scale(s.b[i], s.l[i]));
    }
    r->pa = pa;
    r->pb = pb;
    r->dist = r->overlap ? 0.0f : vec3_len(v);
    r->n = s.n;
    for (u32 i = 0; i < s.n; i++) { r->w[i] = s.w[i]; r->a[i] = s.a[i]; r->b[i] = s.b[i]; }
}

/* ---- EPA ---- */

#define EPA_MAX_V 64
#define EPA_MAX_F 128

typedef struct EpaFace {
    u32  i[3];
    Vec3 n;
    f32  d;
    b32  alive;
} EpaFace;

typedef struct Epa {
    Vec3    w[EPA_MAX_V], a[EPA_MAX_V], b[EPA_MAX_V];
    u32     nv;
    EpaFace f[EPA_MAX_F];
    u32     nf;
} Epa;

static void epa_support(const Phys2Core* A, const Phys2Core* B, Vec3 d, Vec3* w, Vec3* a, Vec3* b) {
    *a = phys2_core_support(A, d);
    *b = phys2_core_support(B, vec3_scale(d, -1.0f));
    *w = vec3_sub(*a, *b);
}

static b32 epa_add_face(Epa* e, u32 i0, u32 i1, u32 i2) {
    if (e->nf >= EPA_MAX_F) return FALSE;
    EpaFace* f = &e->f[e->nf++];
    Vec3 n = vec3_cross(vec3_sub(e->w[i1], e->w[i0]), vec3_sub(e->w[i2], e->w[i0]));
    f32 len = vec3_len(n);
    f->i[0] = i0; f->i[1] = i1; f->i[2] = i2;
    f->n = len > 1e-12f ? vec3_scale(n, 1.0f / len) : vec3(0, 1, 0);
    f->d = vec3_dot(f->n, e->w[i0]);
    f->alive = len > 1e-12f;
    return TRUE;
}

void phys2_epa_penetration(const Phys2Core* A, const Phys2Core* B, const Phys2GjkResult* g, Phys2EpaResult* r) {
    Epa e;
    e.nv = g->n;
    e.nf = 0;
    for (u32 i = 0; i < g->n; i++) { e.w[i] = g->w[i]; e.a[i] = g->a[i]; e.b[i] = g->b[i]; }
    r->ok = FALSE;

    /* simplex'i tetrahedrona tamamla */
    static const f32 DIRS[6][3] = { { 1, 0, 0 }, { -1, 0, 0 }, { 0, 1, 0 }, { 0, -1, 0 }, { 0, 0, 1 }, { 0, 0, -1 } };
    if (e.nv == 1) {
        for (u32 k = 0; k < 6 && e.nv == 1; k++) {
            Vec3 w, a, b;
            epa_support(A, B, vec3(DIRS[k][0], DIRS[k][1], DIRS[k][2]), &w, &a, &b);
            Vec3 dd = vec3_sub(w, e.w[0]);
            if (vec3_dot(dd, dd) > 1e-10f) { e.w[1] = w; e.a[1] = a; e.b[1] = b; e.nv = 2; }
        }
    }
    if (e.nv == 2) {
        Vec3 d = vec3_sub(e.w[1], e.w[0]);
        Vec3 t1, t2;
        phys2_ortho_basis(phys2_v3_norm(d), &t1, &t2);
        Vec3 cand[4] = { t1, vec3_scale(t1, -1.0f), t2, vec3_scale(t2, -1.0f) };
        for (u32 k = 0; k < 4 && e.nv == 2; k++) {
            Vec3 w, a, b;
            epa_support(A, B, cand[k], &w, &a, &b);
            Vec3 c = vec3_cross(vec3_sub(w, e.w[0]), d);
            if (vec3_dot(c, c) > 1e-12f) { e.w[2] = w; e.a[2] = a; e.b[2] = b; e.nv = 3; }
        }
    }
    if (e.nv == 3) {
        Vec3 n = vec3_cross(vec3_sub(e.w[1], e.w[0]), vec3_sub(e.w[2], e.w[0]));
        for (u32 k = 0; k < 2 && e.nv == 3; k++) {
            Vec3 w, a, b;
            epa_support(A, B, k ? vec3_scale(n, -1.0f) : n, &w, &a, &b);
            if (m_abs(vec3_dot(vec3_sub(w, e.w[0]), n)) > 1e-10f) { e.w[3] = w; e.a[3] = a; e.b[3] = b; e.nv = 4; }
        }
    }
    if (e.nv < 4) {
        /* dejenere (yüzeyler tam temasta): sıfır derinlik */
        r->ok = TRUE;
        r->depth = 0.0f;
        r->pa = g->pa;
        r->pb = g->pb;
        Vec3 c = vec3_sub(phys2_core_support(B, vec3(0, 1, 0)), phys2_core_support(A, vec3(0, -1, 0)));
        r->normal = vec3_dot(c, c) > 1e-12f ? phys2_v3_norm(c) : vec3(0, 1, 0);
        return;
    }
    /* tetrahedronu dışa dönük yüzlerle kur */
    Vec3 cen = vec3_scale(vec3_add(vec3_add(e.w[0], e.w[1]), vec3_add(e.w[2], e.w[3])), 0.25f);
    static const u32 T[4][3] = { { 0, 1, 2 }, { 0, 3, 1 }, { 0, 2, 3 }, { 1, 3, 2 } };
    for (u32 k = 0; k < 4; k++) {
        u32 i0 = T[k][0], i1 = T[k][1], i2 = T[k][2];
        Vec3 n = vec3_cross(vec3_sub(e.w[i1], e.w[i0]), vec3_sub(e.w[i2], e.w[i0]));
        if (vec3_dot(n, vec3_sub(cen, e.w[i0])) > 0.0f) { u32 t = i1; i1 = i2; i2 = t; }
        epa_add_face(&e, i0, i1, i2);
    }

    EpaFace* best = NULL;
    for (u32 it = 0; it < 64; it++) {
        best = NULL;
        for (u32 k = 0; k < e.nf; k++) {
            if (e.f[k].alive && (!best || e.f[k].d < best->d)) best = &e.f[k];
        }
        if (!best) return;
        Vec3 w, a, b;
        epa_support(A, B, best->n, &w, &a, &b);
        f32 dist = vec3_dot(w, best->n);
        if (dist - best->d < 1e-4f || e.nv >= EPA_MAX_V) break;
        u32 vi = e.nv++;
        e.w[vi] = w; e.a[vi] = a; e.b[vi] = b;
        /* görünür yüzleri sil, ufuk kenarlarını topla */
        u32 edges[EPA_MAX_F * 3][2];
        u32 ne = 0;
        for (u32 k = 0; k < e.nf; k++) {
            EpaFace* f = &e.f[k];
            if (!f->alive || vec3_dot(f->n, vec3_sub(w, e.w[f->i[0]])) <= 1e-7f) continue;
            f->alive = FALSE;
            for (u32 j = 0; j < 3; j++) {
                u32 ea = f->i[j], eb = f->i[(j + 1) % 3];
                b32 found = FALSE;
                for (u32 q = 0; q < ne; q++) {
                    if (edges[q][0] == eb && edges[q][1] == ea) {
                        edges[q][0] = edges[ne - 1][0];
                        edges[q][1] = edges[ne - 1][1];
                        ne--;
                        found = TRUE;
                        break;
                    }
                }
                if (!found && ne < ARRAY_LEN(edges)) {
                    edges[ne][0] = ea;
                    edges[ne][1] = eb;
                    ne++;
                }
            }
        }
        /* ölü yüzleri sıkıştır */
        u32 nf = 0;
        for (u32 k = 0; k < e.nf; k++) if (e.f[k].alive) e.f[nf++] = e.f[k];
        e.nf = nf;
        b32 full = FALSE;
        for (u32 q = 0; q < ne && !full; q++) full = !epa_add_face(&e, edges[q][0], edges[q][1], vi);
        if (full) break;
    }
    if (!best) return;
    /* orijinin en yakın yüze izdüşümü → barisentrik → çekirdek noktaları */
    Vec3 p = vec3_scale(best->n, best->d);
    Vec3 A0 = e.w[best->i[0]], B0 = e.w[best->i[1]], C0 = e.w[best->i[2]];
    Vec3 v0 = vec3_sub(B0, A0), v1 = vec3_sub(C0, A0), v2 = vec3_sub(p, A0);
    f32 d00 = vec3_dot(v0, v0), d01 = vec3_dot(v0, v1), d11 = vec3_dot(v1, v1);
    f32 d20 = vec3_dot(v2, v0), d21 = vec3_dot(v2, v1);
    f32 den = d00 * d11 - d01 * d01;
    f32 lv = 0.0f, lw = 0.0f;
    if (m_abs(den) > 1e-14f) {
        lv = (d11 * d20 - d01 * d21) / den;
        lw = (d00 * d21 - d01 * d20) / den;
    }
    f32 lu = 1.0f - lv - lw;
    r->pa = vec3_add(vec3_add(vec3_scale(e.a[best->i[0]], lu), vec3_scale(e.a[best->i[1]], lv)), vec3_scale(e.a[best->i[2]], lw));
    r->pb = vec3_add(vec3_add(vec3_scale(e.b[best->i[0]], lu), vec3_scale(e.b[best->i[1]], lv)), vec3_scale(e.b[best->i[2]], lw));
    r->normal = best->n;
    r->depth = best->d;
    r->ok = TRUE;
}
