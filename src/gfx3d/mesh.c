/* gfx3d/mesh.c — Mesh ayırma, prosedürel üreteçler, normal ve sınır hesabı.
 * Tüm bellek verilen Arena'dan gelir; üreteçler deterministiktir.
 */
#include "gfx3d/mesh.h"

Mesh* mesh_alloc(Arena* a, u32 vertex_count, u32 index_count, b32 with_color) {
    Mesh* m = ARENA_PUSH_STRUCT(a, Mesh);
    m->vertex_count = vertex_count;
    m->index_count  = index_count;
    m->px = ARENA_PUSH_ARRAY(a, f32, vertex_count);
    m->py = ARENA_PUSH_ARRAY(a, f32, vertex_count);
    m->pz = ARENA_PUSH_ARRAY(a, f32, vertex_count);
    m->nx = ARENA_PUSH_ARRAY(a, f32, vertex_count);
    m->ny = ARENA_PUSH_ARRAY(a, f32, vertex_count);
    m->nz = ARENA_PUSH_ARRAY(a, f32, vertex_count);
    m->u  = ARENA_PUSH_ARRAY(a, f32, vertex_count);
    m->v  = ARENA_PUSH_ARRAY(a, f32, vertex_count);
    m->color   = with_color ? ARENA_PUSH_ARRAY(a, u32, vertex_count) : NULL;
    m->indices = ARENA_PUSH_ARRAY(a, u32, index_count);
    return m;
}

void mesh_compute_bounds(Mesh* m) {
    if (m->vertex_count == 0) {
        m->aabb_min = m->aabb_max = m->center = vec3(0.0f, 0.0f, 0.0f);
        m->radius = 0.0f;
        return;
    }
    Vec3 lo = vec3(m->px[0], m->py[0], m->pz[0]);
    Vec3 hi = lo;
    for (u32 i = 1; i < m->vertex_count; i++) {
        lo.x = m_min(lo.x, m->px[i]); hi.x = m_max(hi.x, m->px[i]);
        lo.y = m_min(lo.y, m->py[i]); hi.y = m_max(hi.y, m->py[i]);
        lo.z = m_min(lo.z, m->pz[i]); hi.z = m_max(hi.z, m->pz[i]);
    }
    m->aabb_min = lo;
    m->aabb_max = hi;
    m->center   = vec3_scale(vec3_add(lo, hi), 0.5f);
    f32 r2 = 0.0f;
    for (u32 i = 0; i < m->vertex_count; i++) {
        Vec3 d = vec3_sub(vec3(m->px[i], m->py[i], m->pz[i]), m->center);
        r2 = m_max(r2, vec3_dot(d, d));
    }
    m->radius = m_sqrt(r2);
}

void mesh_compute_normals(Mesh* m) {
    for (u32 i = 0; i < m->vertex_count; i++) {
        m->nx[i] = m->ny[i] = m->nz[i] = 0.0f;
    }
    for (u32 t = 0; t + 2 < m->index_count; t += 3) {
        u32 i0 = m->indices[t], i1 = m->indices[t + 1], i2 = m->indices[t + 2];
        Vec3 p0 = vec3(m->px[i0], m->py[i0], m->pz[i0]);
        Vec3 e1 = vec3_sub(vec3(m->px[i1], m->py[i1], m->pz[i1]), p0);
        Vec3 e2 = vec3_sub(vec3(m->px[i2], m->py[i2], m->pz[i2]), p0);
        Vec3 n  = vec3_cross(e1, e2);   /* uzunluk = 2 * alan → alan ağırlıklı */
        u32 ids[3] = { i0, i1, i2 };
        for (u32 k = 0; k < 3; k++) {
            m->nx[ids[k]] += n.x;
            m->ny[ids[k]] += n.y;
            m->nz[ids[k]] += n.z;
        }
    }
    for (u32 i = 0; i < m->vertex_count; i++) {
        Vec3 n = vec3_norm(vec3(m->nx[i], m->ny[i], m->nz[i]));
        m->nx[i] = n.x; m->ny[i] = n.y; m->nz[i] = n.z;
    }
}

static void set_vertex(Mesh* m, u32 i, Vec3 p, Vec3 n, f32 u, f32 v) {
    m->px[i] = p.x; m->py[i] = p.y; m->pz[i] = p.z;
    m->nx[i] = n.x; m->ny[i] = n.y; m->nz[i] = n.z;
    m->u[i]  = u;   m->v[i]  = v;
}

Mesh* mesh_cube(Arena* a, f32 half) {
    Mesh* m = mesh_alloc(a, 24, 36, FALSE);
    /* Her yüz: normal n, yüzey eksenleri s (u yönü) ve t (v yönü); s × t = n (CCW dışarıdan) */
    static const f32 faces[6][9] = {
        /*  n            s             t        */
        {  1, 0, 0,   0, 0,-1,   0, 1, 0 },
        { -1, 0, 0,   0, 0, 1,   0, 1, 0 },
        {  0, 1, 0,   1, 0, 0,   0, 0,-1 },
        {  0,-1, 0,   1, 0, 0,   0, 0, 1 },
        {  0, 0, 1,   1, 0, 0,   0, 1, 0 },
        {  0, 0,-1,  -1, 0, 0,   0, 1, 0 },
    };
    static const f32 corners[4][2] = { {-1,-1}, {1,-1}, {1,1}, {-1,1} };
    for (u32 f = 0; f < 6; f++) {
        Vec3 n = vec3(faces[f][0], faces[f][1], faces[f][2]);
        Vec3 s = vec3(faces[f][3], faces[f][4], faces[f][5]);
        Vec3 t = vec3(faces[f][6], faces[f][7], faces[f][8]);
        for (u32 c = 0; c < 4; c++) {
            Vec3 p = vec3_add(n, vec3_add(vec3_scale(s, corners[c][0]), vec3_scale(t, corners[c][1])));
            /* v aşağı doğru artar (doku satır sırası) */
            set_vertex(m, f * 4 + c, vec3_scale(p, half), n,
                       corners[c][0] * 0.5f + 0.5f, 0.5f - corners[c][1] * 0.5f);
        }
        u32 b = f * 4;
        u32* ix = m->indices + f * 6;
        ix[0] = b; ix[1] = b + 1; ix[2] = b + 2;
        ix[3] = b; ix[4] = b + 2; ix[5] = b + 3;
    }
    mesh_compute_bounds(m);
    return m;
}

Mesh* mesh_plane(Arena* a, f32 size, u32 div, f32 uv_scale) {
    if (div == 0) div = 1;
    u32 n = div + 1;
    Mesh* m = mesh_alloc(a, n * n, div * div * 6, FALSE);
    f32 step = size / (f32)div;
    f32 o = -0.5f * size;
    for (u32 j = 0; j < n; j++) {
        for (u32 i = 0; i < n; i++) {
            f32 x = o + (f32)i * step;
            f32 z = o + (f32)j * step;
            set_vertex(m, j * n + i, vec3(x, 0.0f, z), vec3(0.0f, 1.0f, 0.0f),
                       ((f32)i / (f32)div) * uv_scale, ((f32)j / (f32)div) * uv_scale);
        }
    }
    u32* ix = m->indices;
    for (u32 j = 0; j < div; j++) {
        for (u32 i = 0; i < div; i++) {
            u32 a0 = j * n + i, a1 = a0 + 1, b0 = a0 + n, b1 = b0 + 1;
            /* +Y'den bakınca CCW: (x,z) düzleminde z ekseni "aşağı" görünür */
            *ix++ = a0; *ix++ = b0; *ix++ = b1;
            *ix++ = a0; *ix++ = b1; *ix++ = a1;
        }
    }
    mesh_compute_bounds(m);
    return m;
}

Mesh* mesh_sphere(Arena* a, f32 radius, u32 seg, u32 rings) {
    if (seg < 3) seg = 3;
    if (rings < 2) rings = 2;
    u32 cols = seg + 1, rows = rings + 1;
    Mesh* m = mesh_alloc(a, cols * rows, seg * rings * 6, FALSE);
    for (u32 j = 0; j < rows; j++) {
        f32 vt = (f32)j / (f32)rings;
        f32 th = vt * M_PI;                  /* 0 = kuzey kutbu */
        f32 st = m_sin(th), ct = m_cos(th);
        for (u32 i = 0; i < cols; i++) {
            f32 ut = (f32)i / (f32)seg;
            f32 ph = ut * M_TAU;
            Vec3 n = vec3(st * m_cos(ph), ct, -st * m_sin(ph));
            set_vertex(m, j * cols + i, vec3_scale(n, radius), n, ut, vt);
        }
    }
    u32* ix = m->indices;
    for (u32 j = 0; j < rings; j++) {
        for (u32 i = 0; i < seg; i++) {
            u32 a0 = j * cols + i, a1 = a0 + 1, b0 = a0 + cols, b1 = b0 + 1;
            *ix++ = a0; *ix++ = b0; *ix++ = b1;
            *ix++ = a0; *ix++ = b1; *ix++ = a1;
        }
    }
    mesh_compute_bounds(m);
    return m;
}
