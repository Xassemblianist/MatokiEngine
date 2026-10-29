/* phys2/collide.c — dar faz: küre/kutu/düzlem çiftleri ve kutu-kutu SAT.
 * Her fonksiyon normali A'dan B'ye üretir; phys2_collide şekil sırasını (küre < kutu < düzlem)
 * sağlamak için gerekirse argümanları değiştirip normali çevirir. Kutu-kutu: 15 eksen;
 * yüz ekseni → referans yüze karşı olay yüzünü kırpma (Sutherland–Hodgman), kenar ekseni →
 * iki kenarın en yakın noktaları. Yüz eksenleri hafifçe tercih edilir (titreşimi azaltır).
 * SamurayBalgami/Matoki'den uyarlandı (izinle).
 */
#include "phys2/phys.h"
#include "phys2/gjk.h"

typedef struct RawPoint {
    Vec3 pos;
    f32  depth;
} RawPoint;

static void set_normal(Phys2Manifold* m, Vec3 n) {
    m->normal = n;
    phys2_ortho_basis(n, &m->t1, &m->t2);
}

static u32 emit_points(Phys2Manifold* m, const RawPoint* pts, u32 n) {
    m->count = 0;
    for (u32 i = 0; i < n && i < PHYS2_MAX_POINTS; i++) {
        Phys2ContactPoint* c = &m->p[m->count++];
        c->pos = pts[i].pos;
        c->depth = pts[i].depth;
        c->jn = c->jt1 = c->jt2 = 0.0f;
    }
    return m->count;
}

/* >4 noktayı 4'e indir: en derin, ondan en uzak, bu ikisiyle en büyük üçgen, karşı tarafta en büyük üçgen */
static u32 reduce4(RawPoint* p, u32 n, Vec3 normal) {
    if (n <= 4) return n;
    u32 i0 = 0;
    for (u32 i = 1; i < n; i++) if (p[i].depth > p[i0].depth) i0 = i;
    u32 i1 = i0 == 0 ? 1 : 0;
    f32 best = -1.0f;
    for (u32 i = 0; i < n; i++) {
        Vec3 d = vec3_sub(p[i].pos, p[i0].pos);
        f32 l = vec3_dot(d, d);
        if (i != i0 && l > best) { best = l; i1 = i; }
    }
    Vec3 e = vec3_sub(p[i1].pos, p[i0].pos);
    u32 i2 = i0, i3 = i0;
    f32 bmax = 0.0f, bmin = 0.0f;
    for (u32 i = 0; i < n; i++) {
        if (i == i0 || i == i1) continue;
        f32 area = vec3_dot(vec3_cross(e, vec3_sub(p[i].pos, p[i0].pos)), normal);
        if (i2 == i0 || area > bmax) { bmax = area; i2 = i; }
        if (i3 == i0 || area < bmin) { bmin = area; i3 = i; }
    }
    RawPoint out[4];
    u32 k = 0;
    out[k++] = p[i0];
    out[k++] = p[i1];
    if (i2 != i0) out[k++] = p[i2];
    if (i3 != i0 && i3 != i2) out[k++] = p[i3];
    for (u32 i = 0; i < k; i++) p[i] = out[i];
    return k;
}

/* ---- küre ---- */

static u32 sphere_sphere(const Phys2Body* A, const Phys2Body* B, Phys2Manifold* m, f32 margin) {
    Vec3 d = vec3_sub(B->pos, A->pos);
    f32 dist2 = vec3_dot(d, d);
    f32 rs = A->radius + B->radius;
    if (dist2 > (rs + margin) * (rs + margin)) return 0;
    f32 dist = m_sqrt(dist2);
    Vec3 n = dist > 1e-6f ? vec3_scale(d, 1.0f / dist) : vec3(0, 1, 0);
    set_normal(m, n);
    RawPoint p;
    p.depth = rs - dist;
    p.pos = vec3_add(A->pos, vec3_scale(n, A->radius - p.depth * 0.5f));
    return emit_points(m, &p, 1);
}

/* A küre, B düzlem */
static u32 sphere_plane(const Phys2Body* A, const Phys2Body* B, Phys2Manifold* m, f32 margin) {
    f32 dist = vec3_dot(B->plane_n, A->pos) - B->plane_d;
    if (dist > A->radius + margin) return 0;
    set_normal(m, vec3_scale(B->plane_n, -1.0f));
    RawPoint p;
    p.depth = A->radius - dist;
    p.pos = vec3_sub(A->pos, vec3_scale(B->plane_n, (A->radius + dist) * 0.5f));
    return emit_points(m, &p, 1);
}

/* A küre, B kutu */
static u32 sphere_box(const Phys2Body* A, const Phys2Body* B, Phys2Manifold* m) {
    Vec3 local = phys2_mat3_tmul_v3(&B->rot_m, vec3_sub(A->pos, B->pos));
    const f32 h[3] = { B->half.x, B->half.y, B->half.z };
    f32 l[3] = { local.x, local.y, local.z };
    f32 q[3];
    b32 inside = TRUE;
    for (u32 k = 0; k < 3; k++) {
        q[k] = m_clamp(l[k], -h[k], h[k]);
        if (q[k] != l[k]) inside = FALSE;
    }
    RawPoint p;
    if (inside) {
        u32 axis = 0;
        f32 best = h[0] - m_abs(l[0]);
        for (u32 k = 1; k < 3; k++) {
            f32 pen = h[k] - m_abs(l[k]);
            if (pen < best) { best = pen; axis = k; }
        }
        Vec3 face_n = vec3_scale(phys2_mat3_col(&B->rot_m, axis), l[axis] >= 0.0f ? 1.0f : -1.0f);
        set_normal(m, vec3_scale(face_n, -1.0f));
        p.depth = A->radius + best;
        p.pos = A->pos;
        return emit_points(m, &p, 1);
    }
    Vec3 closest = vec3_add(B->pos, phys2_mat3_mul_v3(&B->rot_m, vec3(q[0], q[1], q[2])));
    Vec3 diff = vec3_sub(A->pos, closest);
    f32 dist2 = vec3_dot(diff, diff);
    if (dist2 > (A->radius + PHYS2_MARGIN) * (A->radius + PHYS2_MARGIN)) return 0;
    f32 dist = m_sqrt(dist2);
    Vec3 n_out = dist > 1e-6f ? vec3_scale(diff, 1.0f / dist) : vec3(0, 1, 0);   /* kutudan küreye */
    set_normal(m, vec3_scale(n_out, -1.0f));
    p.depth = A->radius - dist;
    Vec3 sphere_surface = vec3_sub(A->pos, vec3_scale(n_out, A->radius));
    p.pos = vec3_scale(vec3_add(closest, sphere_surface), 0.5f);
    return emit_points(m, &p, 1);
}

/* ---- kutu ---- */

static void box_corners(const Phys2Body* b, Vec3 out[8]) {
    Vec3 ax = vec3_scale(phys2_mat3_col(&b->rot_m, 0), b->half.x);
    Vec3 ay = vec3_scale(phys2_mat3_col(&b->rot_m, 1), b->half.y);
    Vec3 az = vec3_scale(phys2_mat3_col(&b->rot_m, 2), b->half.z);
    for (u32 i = 0; i < 8; i++) {
        Vec3 p = b->pos;
        p = (i & 1) ? vec3_add(p, ax) : vec3_sub(p, ax);
        p = (i & 2) ? vec3_add(p, ay) : vec3_sub(p, ay);
        p = (i & 4) ? vec3_add(p, az) : vec3_sub(p, az);
        out[i] = p;
    }
}

/* A kutu, B düzlem */
static u32 box_plane(const Phys2Body* A, const Phys2Body* B, Phys2Manifold* m) {
    Vec3 c[8];
    box_corners(A, c);
    RawPoint pts[8];
    u32 n = 0;
    for (u32 i = 0; i < 8; i++) {
        f32 dist = vec3_dot(B->plane_n, c[i]) - B->plane_d;
        if (dist <= PHYS2_MARGIN) {
            pts[n].depth = -dist;
            pts[n].pos = vec3_sub(c[i], vec3_scale(B->plane_n, dist * 0.5f));
            n++;
        }
    }
    if (!n) return 0;
    Vec3 normal = vec3_scale(B->plane_n, -1.0f);
    set_normal(m, normal);
    n = reduce4(pts, n, normal);
    return emit_points(m, pts, n);
}

/* Çokgeni (in, n) düzleme göre kırp: dot(p, pn) <= pd tarafı kalır */
static u32 clip_poly(const Vec3* in, u32 n, Vec3* out, Vec3 pn, f32 pd) {
    u32 k = 0;
    for (u32 i = 0; i < n; i++) {
        Vec3 a = in[i], b = in[(i + 1) % n];
        f32 da = vec3_dot(a, pn) - pd, db = vec3_dot(b, pn) - pd;
        if (da <= 0.0f) out[k++] = a;
        if ((da <= 0.0f) != (db <= 0.0f)) {
            f32 t = da / (da - db);
            out[k++] = vec3_add(a, vec3_scale(vec3_sub(b, a), t));
        }
    }
    return k;
}

/* ref kutusunun 'axis' yüzü (dış normal n_ref, olay kutusuna bakar) referans; inc kutusunun yüzü kırpılır.
 * Dönen noktalar dünya uzayında; derinlik referans yüz düzlemine göre. */
static u32 face_contact(const Phys2Body* ref, u32 axis, Vec3 n_ref, const Phys2Body* inc, RawPoint* out) {
    const f32 rh[3] = { ref->half.x, ref->half.y, ref->half.z };
    const f32 ih[3] = { inc->half.x, inc->half.y, inc->half.z };
    Vec3 face_c = vec3_add(ref->pos, vec3_scale(n_ref, rh[axis]));
    u32 ua = (axis + 1) % 3, va = (axis + 2) % 3;
    Vec3 u = phys2_mat3_col(&ref->rot_m, ua), v = phys2_mat3_col(&ref->rot_m, va);

    /* olay yüzü: normali n_ref'e en ters olan */
    u32 m = 0;
    f32 best = 0.0f;
    for (u32 k = 0; k < 3; k++) {
        f32 d = vec3_dot(phys2_mat3_col(&inc->rot_m, k), n_ref);
        if (k == 0 || m_abs(d) > m_abs(best)) { best = d; m = k; }
    }
    f32 s = best > 0.0f ? -1.0f : 1.0f;
    Vec3 im = phys2_mat3_col(&inc->rot_m, m);
    Vec3 ic = vec3_add(inc->pos, vec3_scale(im, s * ih[m]));
    u32 pa = (m + 1) % 3, qa = (m + 2) % 3;
    Vec3 ip = vec3_scale(phys2_mat3_col(&inc->rot_m, pa), ih[pa]);
    Vec3 iq = vec3_scale(phys2_mat3_col(&inc->rot_m, qa), ih[qa]);
    Vec3 poly[16], tmp[16];
    poly[0] = vec3_add(vec3_add(ic, ip), iq);
    poly[1] = vec3_add(vec3_sub(ic, ip), iq);
    poly[2] = vec3_sub(vec3_sub(ic, ip), iq);
    poly[3] = vec3_sub(vec3_add(ic, ip), iq);
    u32 n = 4;
    f32 cu = vec3_dot(face_c, u), cv = vec3_dot(face_c, v);
    n = clip_poly(poly, n, tmp, u, cu + rh[ua]);
    n = clip_poly(tmp, n, poly, vec3_scale(u, -1.0f), -(cu - rh[ua]));
    n = clip_poly(poly, n, tmp, v, cv + rh[va]);
    n = clip_poly(tmp, n, poly, vec3_scale(v, -1.0f), -(cv - rh[va]));
    u32 k = 0;
    for (u32 i = 0; i < n; i++) {
        f32 sep = vec3_dot(vec3_sub(poly[i], face_c), n_ref);
        if (sep <= PHYS2_MARGIN) {
            out[k].depth = -sep;
            out[k].pos = vec3_sub(poly[i], vec3_scale(n_ref, sep * 0.5f));
            k++;
        }
    }
    return k;
}

static u32 box_box(const Phys2Body* A, const Phys2Body* B, Phys2Manifold* m) {
    Vec3 a[3], b[3];
    for (u32 k = 0; k < 3; k++) {
        a[k] = phys2_mat3_col(&A->rot_m, k);
        b[k] = phys2_mat3_col(&B->rot_m, k);
    }
    const f32 ha[3] = { A->half.x, A->half.y, A->half.z };
    const f32 hb[3] = { B->half.x, B->half.y, B->half.z };
    Vec3 d = vec3_sub(B->pos, A->pos);
    f32 C[3][3], AC[3][3];
    for (u32 i = 0; i < 3; i++) {
        for (u32 j = 0; j < 3; j++) {
            C[i][j] = vec3_dot(a[i], b[j]);
            AC[i][j] = m_abs(C[i][j]) + 1e-6f;
        }
    }
    f32 da[3], db[3];
    for (u32 k = 0; k < 3; k++) {
        da[k] = vec3_dot(d, a[k]);
        db[k] = vec3_dot(d, b[k]);
    }

    f32 sep_a = -3.4e38f, sep_b = -3.4e38f;
    u32 axis_a = 0, axis_b = 0;
    for (u32 i = 0; i < 3; i++) {
        f32 sep = m_abs(da[i]) - (ha[i] + hb[0] * AC[i][0] + hb[1] * AC[i][1] + hb[2] * AC[i][2]);
        if (sep > PHYS2_MARGIN) return 0;
        if (sep > sep_a) { sep_a = sep; axis_a = i; }
    }
    for (u32 j = 0; j < 3; j++) {
        f32 sep = m_abs(db[j]) - (hb[j] + ha[0] * AC[0][j] + ha[1] * AC[1][j] + ha[2] * AC[2][j]);
        if (sep > PHYS2_MARGIN) return 0;
        if (sep > sep_b) { sep_b = sep; axis_b = j; }
    }
    /* referans yüz histerezisi: B ancak belirgin biçimde daha iyiyse (yoksa neredeyse eşit
     * değerlerde referans karelere göre A/B arasında gidip gelir, temas noktaları sıçrar) */
    f32 face_sep = sep_a;
    u32 face_axis = axis_a;   /* 0..2 = A, 3..5 = B */
    if (sep_b > 0.95f * sep_a + 0.005f) {
        face_sep = sep_b;
        face_axis = 3 + axis_b;
    }

    f32 edge_sep = -3.4e38f;
    u32 edge_i = 0, edge_j = 0;
    Vec3 edge_n = vec3(0, 1, 0);
    for (u32 i = 0; i < 3; i++) {
        for (u32 j = 0; j < 3; j++) {
            Vec3 L = vec3_cross(a[i], b[j]);
            f32 len = vec3_len(L);
            if (len < 1e-5f) continue;
            L = vec3_scale(L, 1.0f / len);
            f32 pa = ha[0] * m_abs(vec3_dot(a[0], L)) + ha[1] * m_abs(vec3_dot(a[1], L)) + ha[2] * m_abs(vec3_dot(a[2], L));
            f32 pb = hb[0] * m_abs(vec3_dot(b[0], L)) + hb[1] * m_abs(vec3_dot(b[1], L)) + hb[2] * m_abs(vec3_dot(b[2], L));
            f32 dl = vec3_dot(d, L);
            f32 sep = m_abs(dl) - (pa + pb);
            if (sep > PHYS2_MARGIN) return 0;
            if (sep > edge_sep) {
                edge_sep = sep;
                edge_i = i;
                edge_j = j;
                edge_n = dl < 0.0f ? vec3_scale(L, -1.0f) : L;
            }
        }
    }

    RawPoint pts[16];
    u32 n;
    if (edge_sep > 0.95f * face_sep + 0.01f) {
        /* kenar-kenar: A'nın B'ye bakan kenarı ile B'nin A'ya bakan kenarı */
        Vec3 nn = edge_n;
        Vec3 pA = A->pos, pB = B->pos;
        for (u32 k = 0; k < 3; k++) {
            if (k != edge_i) pA = vec3_add(pA, vec3_scale(a[k], vec3_dot(a[k], nn) > 0.0f ? ha[k] : -ha[k]));
            if (k != edge_j) pB = vec3_add(pB, vec3_scale(b[k], vec3_dot(b[k], nn) > 0.0f ? -hb[k] : hb[k]));
        }
        Vec3 u = a[edge_i], v = b[edge_j];
        Vec3 r = vec3_sub(pA, pB);
        f32 bb = vec3_dot(u, v), c = vec3_dot(u, r), f = vec3_dot(v, r);
        f32 den = 1.0f - bb * bb;
        f32 s = 0.0f, t = 0.0f;
        if (den > 1e-6f) {
            s = (bb * f - c) / den;
            t = (f - bb * c) / den;
        }
        s = m_clamp(s, -ha[edge_i], ha[edge_i]);
        t = m_clamp(t, -hb[edge_j], hb[edge_j]);
        Vec3 qa = vec3_add(pA, vec3_scale(u, s));
        Vec3 qb = vec3_add(pB, vec3_scale(v, t));
        set_normal(m, nn);
        pts[0].pos = vec3_scale(vec3_add(qa, qb), 0.5f);
        pts[0].depth = -edge_sep;
        return emit_points(m, pts, 1);
    }

    if (face_axis < 3) {
        u32 i = face_axis;
        Vec3 n_ref = vec3_scale(a[i], da[i] >= 0.0f ? 1.0f : -1.0f);   /* A'dan B'ye */
        n = face_contact(A, i, n_ref, B, pts);
        if (!n) return 0;
        set_normal(m, n_ref);
        n = reduce4(pts, n, n_ref);
    } else {
        u32 j = face_axis - 3;
        Vec3 n_ref = vec3_scale(b[j], db[j] >= 0.0f ? -1.0f : 1.0f);   /* B'den A'ya */
        n = face_contact(B, j, n_ref, A, pts);
        if (!n) return 0;
        Vec3 normal = vec3_scale(n_ref, -1.0f);
        set_normal(m, normal);
        n = reduce4(pts, n, normal);
    }
    return emit_points(m, pts, n);
}

/* ---- genel çekirdek çarpışması (GJK/EPA + manifold zenginleştirme) ---- */

void phys2_body_core(const Phys2Body* b, Phys2Core* c) {
    c->radius = 0.0f;
    c->scale = vec3(1, 1, 1);
    c->pos = b->pos;
    c->rot = b->rot_m;
    c->hull = NULL;
    switch (b->shape) {
    case PHYS2_SHAPE_SPHERE:
        c->kind = PHYS2_CORE_POINT;
        c->p0 = c->p1 = b->pos;
        c->radius = b->radius;
        break;
    case PHYS2_SHAPE_CAPSULE: {
        Vec3 ax = vec3_scale(phys2_mat3_col(&b->rot_m, 1), b->half_height);
        c->kind = PHYS2_CORE_SEGMENT;
        c->p0 = vec3_sub(b->pos, ax);
        c->p1 = vec3_add(b->pos, ax);
        c->radius = b->radius;
        break;
    }
    case PHYS2_SHAPE_BOX:
        c->kind = PHYS2_CORE_POLY;
        c->hull = phys2_hull_unit_cube();
        c->scale = b->half;
        break;
    default:
        c->kind = PHYS2_CORE_POLY;
        c->hull = b->hull;
        break;
    }
}

#define MAX_POLY 64

static u32 core_face_poly(const Phys2Core* c, u32 f, Vec3* out) {
    const Phys2HullFace* hf = &c->hull->faces[f];
    u32 n = hf->count < MAX_POLY ? hf->count : MAX_POLY;
    for (u32 i = 0; i < n; i++) out[i] = phys2_core_vertex(c, c->hull->idx[hf->first + i]);
    return n;
}

static u32 best_face(const Phys2Core* c, Vec3 dir, f32* dot_out) {
    u32 best = 0;
    f32 bd = -3.4e38f;
    for (u32 f = 0; f < c->hull->fcount; f++) {
        f32 d = vec3_dot(phys2_core_face_normal(c, f), dir);
        if (d > bd) { bd = d; best = f; }
    }
    if (dot_out) *dot_out = bd;
    return best;
}

/* n yönündeki çekirdek örtüşmesi (A → B; > 0 girişim) */
static f32 overlap_along(const Phys2Core* A, const Phys2Core* B, Vec3 n) {
    return vec3_dot(vec3_sub(phys2_core_support(A, n), phys2_core_support(B, vec3_scale(n, -1.0f))), n);
}

/* çokyüzlü-çokyüzlü: referans yüze karşı olay yüzünü kırp (n: A → B, depth: GJK/EPA derinliği).
 * Yüz normali ancak o eksendeki örtüşme gerçek en küçük derinlik kadar iyiyse kullanılır; değilse
 * (kenar-kenar ya da eğik temas) 0 döner → çağıran tek nokta + MTD normali kullanır. */
static u32 poly_clip(const Phys2Core* A, const Phys2Core* B, Vec3 n, f32 depth, Phys2Manifold* m) {
    f32 da, db;
    u32 fa = best_face(A, n, &da), fb = best_face(B, vec3_scale(n, -1.0f), &db);
    if (m_max(da, db) < 0.95f) return 0;
    b32 ref_is_a = !(db > da + 0.001f);
    const Phys2Core* R = ref_is_a ? A : B;
    const Phys2Core* I = ref_is_a ? B : A;
    u32 rf = ref_is_a ? fa : fb;
    Vec3 nr = phys2_core_face_normal(R, rf);
    Vec3 mn = ref_is_a ? nr : vec3_scale(nr, -1.0f);
    if (overlap_along(A, B, mn) > depth + 0.005f + 0.02f * m_abs(depth)) return 0;
    u32 inc = best_face(I, vec3_scale(nr, -1.0f), NULL);
    Vec3 ref_poly[MAX_POLY], buf_a[MAX_POLY * 2 + 8], buf_b[MAX_POLY * 2 + 8];
    u32 rn = core_face_poly(R, rf, ref_poly);
    u32 cnt = core_face_poly(I, inc, buf_a);
    Vec3* src = buf_a;
    Vec3* dst = buf_b;
    for (u32 i = 0; i < rn && cnt; i++) {
        Vec3 v0 = ref_poly[i], v1 = ref_poly[(i + 1) % rn];
        Vec3 side = vec3_cross(vec3_sub(v1, v0), nr);
        f32 len = vec3_len(side);
        if (len < 1e-9f) continue;
        side = vec3_scale(side, 1.0f / len);
        if (cnt + 1 > MAX_POLY * 2 + 8) break;
        cnt = clip_poly(src, cnt, dst, side, vec3_dot(v0, side));
        Vec3* t = src; src = dst; dst = t;
    }
    RawPoint pts[MAX_POLY * 2 + 8];
    u32 k = 0;
    for (u32 i = 0; i < cnt; i++) {
        f32 sep = vec3_dot(vec3_sub(src[i], ref_poly[0]), nr);
        if (sep <= PHYS2_MARGIN) {
            pts[k].depth = -sep;
            pts[k].pos = vec3_sub(src[i], vec3_scale(nr, sep * 0.5f));
            k++;
        }
    }
    if (!k) return 0;
    Vec3 normal = ref_is_a ? nr : vec3_scale(nr, -1.0f);
    set_normal(m, normal);
    k = reduce4(pts, k, normal);
    return emit_points(m, pts, k);
}

/* doğru parçasını düzleme kırp: dot(p, pn) <= pd tarafı */
static b32 clip_segment(Vec3* p0, Vec3* p1, Vec3 pn, f32 pd) {
    f32 d0 = vec3_dot(*p0, pn) - pd, d1 = vec3_dot(*p1, pn) - pd;
    if (d0 > 0.0f && d1 > 0.0f) return FALSE;
    if (d0 > 0.0f) *p0 = vec3_add(*p0, vec3_scale(vec3_sub(*p1, *p0), d0 / (d0 - d1)));
    else if (d1 > 0.0f) *p1 = vec3_add(*p1, vec3_scale(vec3_sub(*p0, *p1), d1 / (d1 - d0)));
    return TRUE;
}

/* kapsül (S) yüze paralelken 2 nokta (n: A → B) */
static u32 segment_poly(const Phys2Core* S, const Phys2Core* P, Vec3 n, b32 poly_is_a, Phys2Manifold* m) {
    Vec3 u = vec3_sub(S->p1, S->p0);
    f32 ul = vec3_len(u);
    if (ul < 1e-6f) return 0;
    u = vec3_scale(u, 1.0f / ul);
    u32 f = best_face(P, poly_is_a ? n : vec3_scale(n, -1.0f), NULL);
    Vec3 nr = phys2_core_face_normal(P, f);
    if (m_abs(vec3_dot(u, nr)) > 0.3f) return 0;
    Vec3 poly[MAX_POLY];
    u32 pn = core_face_poly(P, f, poly);
    Vec3 q0 = S->p0, q1 = S->p1;
    for (u32 i = 0; i < pn; i++) {
        Vec3 v0 = poly[i], v1 = poly[(i + 1) % pn];
        Vec3 side = vec3_cross(vec3_sub(v1, v0), nr);
        f32 len = vec3_len(side);
        if (len < 1e-9f) continue;
        side = vec3_scale(side, 1.0f / len);
        if (!clip_segment(&q0, &q1, side, vec3_dot(v0, side))) return 0;
    }
    RawPoint pts[2];
    u32 k = 0;
    const Vec3 qs[2] = { q0, q1 };
    for (u32 i = 0; i < 2; i++) {
        f32 dist = vec3_dot(vec3_sub(qs[i], poly[0]), nr);
        if (dist - S->radius <= PHYS2_MARGIN) {
            pts[k].depth = S->radius - dist;
            pts[k].pos = vec3_sub(qs[i], vec3_scale(nr, (S->radius + dist) * 0.5f));
            k++;
        }
    }
    if (!k) return 0;
    set_normal(m, poly_is_a ? nr : vec3_scale(nr, -1.0f));
    return emit_points(m, pts, k);
}

/* paralel kapsüller: B'nin parçasını A'nın uçlarına kırp → 2 nokta */
static u32 segment_segment(const Phys2Core* A, const Phys2Core* B, Vec3 n, Phys2Manifold* m) {
    Vec3 ua = vec3_sub(A->p1, A->p0), ub = vec3_sub(B->p1, B->p0);
    f32 la = vec3_len(ua), lb = vec3_len(ub);
    if (la < 1e-6f || lb < 1e-6f) return 0;
    ua = vec3_scale(ua, 1.0f / la);
    if (m_abs(vec3_dot(ua, vec3_scale(ub, 1.0f / lb))) < 0.98f) return 0;
    Vec3 q0 = B->p0, q1 = B->p1;
    if (!clip_segment(&q0, &q1, vec3_scale(ua, -1.0f), -vec3_dot(A->p0, ua))) return 0;
    if (!clip_segment(&q0, &q1, ua, vec3_dot(A->p1, ua))) return 0;
    RawPoint pts[2];
    u32 k = 0;
    const Vec3 qs[2] = { q0, q1 };
    f32 r = A->radius + B->radius;
    for (u32 i = 0; i < 2; i++) {
        Vec3 c = vec3_add(A->p0, vec3_scale(ua, m_clamp(vec3_dot(vec3_sub(qs[i], A->p0), ua), 0.0f, la)));
        f32 s = vec3_dot(vec3_sub(qs[i], c), n);
        if (r - s >= -PHYS2_MARGIN) {
            pts[k].depth = r - s;
            Vec3 sa = vec3_add(c, vec3_scale(n, A->radius)), sb = vec3_sub(qs[i], vec3_scale(n, B->radius));
            pts[k].pos = vec3_scale(vec3_add(sa, sb), 0.5f);
            k++;
        }
    }
    if (!k) return 0;
    set_normal(m, n);
    return emit_points(m, pts, k);
}

u32 phys2_collide_cores(const Phys2Core* A, const Phys2Core* B, Phys2Manifold* m) {
    m->count = 0;
    Phys2GjkResult g;
    phys2_gjk_distance(A, B, &g);
    f32 r = A->radius + B->radius;
    Vec3 n, pa, pb;
    f32 depth;
    if (!g.overlap && g.dist > 1e-6f) {
        if (g.dist > r + PHYS2_MARGIN) return 0;
        n = vec3_scale(vec3_sub(g.pb, g.pa), 1.0f / g.dist);
        depth = r - g.dist;
        pa = g.pa;
        pb = g.pb;
    } else if (A->kind == PHYS2_CORE_POLY || B->kind == PHYS2_CORE_POLY) {
        Phys2EpaResult e;
        phys2_epa_penetration(A, B, &g, &e);
        if (!e.ok) return 0;
        n = e.normal;
        depth = e.depth + r;
        pa = e.pa;
        pb = e.pb;
    } else {
        /* nokta/parça çekirdekleri kesişiyor (ör. kapsül eksenleri çapraz): çapraz çarpım normali */
        Vec3 da = vec3_sub(A->p1, A->p0), db = vec3_sub(B->p1, B->p0);
        n = vec3_cross(da, db);
        if (vec3_dot(n, n) < 1e-12f) n = vec3_sub(vec3_scale(vec3_add(B->p0, B->p1), 0.5f), vec3_scale(vec3_add(A->p0, A->p1), 0.5f));
        if (vec3_dot(n, n) < 1e-12f) n = vec3(0, 1, 0);
        n = phys2_v3_norm(n);
        Vec3 cd = vec3_sub(vec3_add(B->p0, B->p1), vec3_add(A->p0, A->p1));
        if (vec3_dot(cd, n) < 0.0f) n = vec3_scale(n, -1.0f);
        depth = r;
        pa = g.pa;
        pb = g.pb;
    }
    u32 k = 0;
    if (A->kind == PHYS2_CORE_POLY && B->kind == PHYS2_CORE_POLY) k = poly_clip(A, B, n, depth, m);
    else if (A->kind == PHYS2_CORE_SEGMENT && B->kind == PHYS2_CORE_POLY) k = segment_poly(A, B, n, FALSE, m);
    else if (A->kind == PHYS2_CORE_POLY && B->kind == PHYS2_CORE_SEGMENT) k = segment_poly(B, A, n, TRUE, m);
    else if (A->kind == PHYS2_CORE_SEGMENT && B->kind == PHYS2_CORE_SEGMENT) k = segment_segment(A, B, n, m);
    if (k) return k;
    set_normal(m, n);
    RawPoint p;
    p.depth = depth;
    p.pos = vec3_scale(vec3_add(vec3_add(pa, vec3_scale(n, A->radius)), vec3_sub(pb, vec3_scale(n, B->radius))), 0.5f);
    return emit_points(m, &p, 1);
}

/* A kapsül, B düzlem: iki uç küresi */
static u32 capsule_plane(const Phys2Body* A, const Phys2Body* B, Phys2Manifold* m) {
    Vec3 ax = vec3_scale(phys2_mat3_col(&A->rot_m, 1), A->half_height);
    const Vec3 ends[2] = { vec3_sub(A->pos, ax), vec3_add(A->pos, ax) };
    RawPoint pts[2];
    u32 k = 0;
    for (u32 i = 0; i < 2; i++) {
        f32 dist = vec3_dot(B->plane_n, ends[i]) - B->plane_d;
        if (dist <= A->radius + PHYS2_MARGIN) {
            pts[k].depth = A->radius - dist;
            pts[k].pos = vec3_sub(ends[i], vec3_scale(B->plane_n, (A->radius + dist) * 0.5f));
            k++;
        }
    }
    if (!k) return 0;
    set_normal(m, vec3_scale(B->plane_n, -1.0f));
    return emit_points(m, pts, k);
}

/* A hull, B düzlem: düzlemin altındaki köşeler */
static u32 hull_plane(const Phys2Body* A, const Phys2Body* B, Phys2Manifold* m) {
    Phys2Core c;
    phys2_body_core(A, &c);
    RawPoint pts[PHYS2_HULL_MAX_VERTS];
    u32 k = 0;
    for (u32 i = 0; i < c.hull->vcount; i++) {
        Vec3 v = phys2_core_vertex(&c, i);
        f32 dist = vec3_dot(B->plane_n, v) - B->plane_d;
        if (dist <= PHYS2_MARGIN) {
            pts[k].depth = -dist;
            pts[k].pos = vec3_sub(v, vec3_scale(B->plane_n, dist * 0.5f));
            k++;
        }
    }
    if (!k) return 0;
    Vec3 normal = vec3_scale(B->plane_n, -1.0f);
    set_normal(m, normal);
    k = reduce4(pts, k, normal);
    return emit_points(m, pts, k);
}

/* İç kenar düzeltmesi (Jolt "active edges" fikri): üçgenden gövdeye normal yüz normalinden sapıyorsa
 * ve temas noktalarının hiçbiri aktif bir kenarda / köşede değilse, normal yüz normaline çevrilir ve
 * derinlikler o normale göre yeniden hesaplanır. Böylece düz ya da içbükey birleşimlerde kenar
 * normalleri gövdeyi yana / yukarı itmez (kayarken sekme). n_t: üçgenden gövdeye. */
static void mesh_fix_edges(Phys2Manifold* m, b32 mesh_is_a, const Phys2Core* oc, Vec3 p0, Vec3 p1, Vec3 p2, u8 flags) {
    Vec3 nt = mesh_is_a ? m->normal : vec3_scale(m->normal, -1.0f);
    Vec3 nf = phys2_v3_norm(vec3_cross(vec3_sub(p1, p0), vec3_sub(p2, p0)));
    f32 cd = vec3_dot(nt, nf);
    if (cd > 0.9998f) return;
    if (cd < 0.0f) nf = vec3_scale(nf, -1.0f);   /* arka yüzden temas: yüzü gövdeye çevir */
    Vec3 e0 = vec3_sub(p1, p0), e1 = vec3_sub(p2, p0);
    f32 d00 = vec3_dot(e0, e0), d01 = vec3_dot(e0, e1), d11 = vec3_dot(e1, e1);
    f32 den = d00 * d11 - d01 * d01;
    if (!(den > 0.0f)) return;
    f32 tol = 1e-3f;
    b32 on_edge = FALSE;
    for (u32 i = 0; i < m->count; i++) {
        const Phys2ContactPoint* c = &m->p[i];
        Vec3 pt = vec3_add(c->pos, vec3_scale(nt, c->depth * 0.5f));   /* üçgen yüzeyindeki nokta */
        Vec3 r = vec3_sub(pt, p0);
        f32 d20 = vec3_dot(r, e0), d21 = vec3_dot(r, e1);
        f32 v = (d11 * d20 - d01 * d21) / den, w = (d00 * d21 - d01 * d20) / den, u = 1.0f - v - w;
        /* kenar k: v_k → v_k+1; karşı köşenin barisentriği ≈ 0 */
        b32 e_01 = w < tol, e_12 = u < tol, e_20 = v < tol;
        u32 n_on = (u32)e_01 + (u32)e_12 + (u32)e_20;
        if (!n_on) continue;
        on_edge = TRUE;
        b32 active = (e_01 && (flags & 1u)) || (e_12 && (flags & 2u)) || (e_20 && (flags & 4u));
        if (active) return;
    }
    if (!on_edge) return;
    u32 k = 0;
    if (oc->kind != PHYS2_CORE_POLY) {
        /* küre / kapsül: temas, çekirdek uçlarının yüz normali boyunca en alt noktası. (Eğik normal
         * boyunca kalan yüzey noktası yuvarlanırken aşağı döner → çözücü bunu girişim sanıp zıplatır.) */
        Vec3 ends[2] = { oc->p0, oc->p1 };
        u32 ne = oc->kind == PHYS2_CORE_SEGMENT ? 2 : 1;
        Phys2ContactPoint tmpl = m->p[0];
        for (u32 i = 0; i < ne; i++) {
            Vec3 po = vec3_sub(ends[i], vec3_scale(nf, oc->radius));
            f32 s = vec3_dot(vec3_sub(po, p0), nf);
            if (s > PHYS2_MARGIN) continue;
            Phys2ContactPoint c = tmpl;
            c.depth = -s;
            c.pos = vec3_sub(po, vec3_scale(nf, s * 0.5f));
            m->p[k++] = c;
        }
        m->count = k;
        set_normal(m, mesh_is_a ? nf : vec3_scale(nf, -1.0f));
        return;
    }
    /* çokyüzlü: noktalar gerçek yüzey noktaları; gövde noktası = pt + nt·s (s = −depth) → düzleme uzaklık */
    for (u32 i = 0; i < m->count; i++) {
        Phys2ContactPoint c = m->p[i];
        Vec3 po = vec3_sub(vec3_add(c.pos, vec3_scale(nt, c.depth * 0.5f)), vec3_scale(nt, c.depth));
        f32 s = vec3_dot(vec3_sub(po, p0), nf);
        if (s > PHYS2_MARGIN) continue;
        c.depth = -s;
        c.pos = vec3_sub(po, vec3_scale(nf, s * 0.5f));
        m->p[k++] = c;
    }
    m->count = k;
    set_normal(m, mesh_is_a ? nf : vec3_scale(nf, -1.0f));
}

u32 phys2_collide_mesh(const Phys2Body* a, const Phys2Body* b, Phys2Manifold* out, u32 max, b32 edge_fix) {
    b32 mesh_is_a = a->shape == PHYS2_SHAPE_MESH;
    const Phys2Body* mb = mesh_is_a ? a : b;
    const Phys2Body* ob = mesh_is_a ? b : a;
    if (ob->shape == PHYS2_SHAPE_MESH || ob->shape == PHYS2_SHAPE_PLANE || !max) return 0;
    const Phys2TriMesh* tm = mb->mesh;
    /* diğer gövdenin AABB'sini mesh yerel uzayına taşı (köşeler) */
    Vec3 mn = vec3(3.4e38f, 3.4e38f, 3.4e38f), mx = vec3(-3.4e38f, -3.4e38f, -3.4e38f);
    for (u32 i = 0; i < 8; i++) {
        Vec3 c = vec3((i & 1) ? ob->aabb_max.x : ob->aabb_min.x, (i & 2) ? ob->aabb_max.y : ob->aabb_min.y,
                    (i & 4) ? ob->aabb_max.z : ob->aabb_min.z);
        Vec3 l = phys2_mat3_tmul_v3(&mb->rot_m, vec3_sub(c, mb->pos));
        mn = vec3(m_min(mn.x, l.x), m_min(mn.y, l.y), m_min(mn.z, l.z));
        mx = vec3(m_max(mx.x, l.x), m_max(mx.y, l.y), m_max(mx.z, l.z));
    }
    u32 tris[64];
    u32 nt = phys2_trimesh_query_aabb(tm, mn, mx, tris, 64);
    Phys2Core oc;
    phys2_body_core(ob, &oc);
    u32 written = 0;
    for (u32 i = 0; i < nt && written < max; i++) {
        Vec3 p0, p1, p2;
        phys2_trimesh_tri(tm, tris[i], &p0, &p1, &p2);
        p0 = vec3_add(mb->pos, phys2_mat3_mul_v3(&mb->rot_m, p0));
        p1 = vec3_add(mb->pos, phys2_mat3_mul_v3(&mb->rot_m, p1));
        p2 = vec3_add(mb->pos, phys2_mat3_mul_v3(&mb->rot_m, p2));
        Vec3 cr = vec3_cross(vec3_sub(p1, p0), vec3_sub(p2, p0));
        if (vec3_dot(cr, cr) < 1e-14f) continue;
        Phys2Core tc;
        phys2_core_triangle(&tc, p0, p1, p2);
        Phys2Manifold* m = &out[written];
        u32 k = mesh_is_a ? phys2_collide_cores(&tc, &oc, m) : phys2_collide_cores(&oc, &tc, m);
        if (!k) continue;
        if (edge_fix) {
            mesh_fix_edges(m, mesh_is_a, &oc, p0, p1, p2, tm->edge_flags[tris[i]]);
            if (!m->count) continue;
        }
        m->sub = tris[i] + 1;
        written++;
    }
    return written;
}

u32 phys2_collide(const Phys2Body* a, const Phys2Body* b, Phys2Manifold* m) {
    m->count = 0;
    m->sub = 0;
    if (a->shape > b->shape) {
        u32 n = phys2_collide(b, a, m);
        if (n) set_normal(m, vec3_scale(m->normal, -1.0f));
        return n;
    }
    if (a->shape == PHYS2_SHAPE_MESH || b->shape == PHYS2_SHAPE_MESH) return 0;   /* phys2_collide_mesh */
    if (b->shape == PHYS2_SHAPE_PLANE) {
        switch (a->shape) {
        case PHYS2_SHAPE_SPHERE: return sphere_plane(a, b, m, PHYS2_MARGIN);
        case PHYS2_SHAPE_CAPSULE: return capsule_plane(a, b, m);
        case PHYS2_SHAPE_BOX: return box_plane(a, b, m);
        case PHYS2_SHAPE_HULL: return hull_plane(a, b, m);
        default: return 0;
        }
    }
    if (a->shape == PHYS2_SHAPE_SPHERE && b->shape == PHYS2_SHAPE_SPHERE) return sphere_sphere(a, b, m, PHYS2_MARGIN);
    if (a->shape == PHYS2_SHAPE_SPHERE && b->shape == PHYS2_SHAPE_BOX) return sphere_box(a, b, m);
    if (a->shape == PHYS2_SHAPE_BOX && b->shape == PHYS2_SHAPE_BOX) return box_box(a, b, m);
    Phys2Core ca, cb;
    phys2_body_core(a, &ca);
    phys2_body_core(b, &cb);
    return phys2_collide_cores(&ca, &cb, m);
}

u32 phys2_collide_margin(const Phys2Body* a, const Phys2Body* b, Phys2Manifold* m, f32 margin) {
    if (a->shape > b->shape) {
        u32 n = phys2_collide_margin(b, a, m, margin);
        if (n) set_normal(m, vec3_scale(m->normal, -1.0f));
        return n;
    }
    if (a->shape == PHYS2_SHAPE_SPHERE && (b->shape == PHYS2_SHAPE_SPHERE || b->shape == PHYS2_SHAPE_PLANE)) {
        m->count = 0;
        m->sub = 0;
        return b->shape == PHYS2_SHAPE_PLANE ? sphere_plane(a, b, m, margin) : sphere_sphere(a, b, m, margin);
    }
    return phys2_collide(a, b, m);
}
