/* gfx3d/billboard.c — Kameraya bakan dörtgen (billboard) toplayıcı, radix sıralama ve Mesh üretimi.
 * Kapasite aşımı ve geçersiz (sıfır/negatif/NaN/Inf) girdiler sayaçla güvenli şekilde atlanır.
 * İndeks tamponu init sırasında bir kez doldurulur; her karede yalnız aktif köşeler güncellenir.
 */
#include "gfx3d/billboard.h"

/* IEEE-754 sonlu (NaN veya ±Inf olmayan) f32 denetimi */
static inline b32 f32_is_finite(f32 x) {
    u32 u;
    __builtin_memcpy(&u, &x, sizeof(u));
    return (u & 0x7F800000u) != 0x7F800000u;
}

/* Koordinat/boyut çarpımlarında f32 taşmasını önleyen güvenli büyüklük sınırı */
#define BILLBOARD_MAX_COORD 1.0e15f

static inline b32 f32_valid_coord(f32 x) {
    return f32_is_finite(x) && m_abs(x) <= BILLBOARD_MAX_COORD;
}

static inline b32 f32_valid_size(f32 s) {
    return f32_is_finite(s) && s > 0.0f && s <= BILLBOARD_MAX_COORD;
}

/* m_sin/m_cos içindeki reduce_pio2'nin büyük açılarda i32 taşmasını önleyen açı indirgeme */
static inline f32 sanitize_angle(f32 rad) {
    if (!f32_is_finite(rad)) return 0.0f;
    if (m_abs(rad) > 1000.0f) {
        if (m_abs(rad) < 8388608.0f) {
            rad -= m_trunc(rad * (1.0f / M_TAU)) * M_TAU;
        } else {
            rad = 0.0f;
        }
    }
    return rad;
}

/* Arena içinde verilen boyut ve hizalamanın PANIC olmadan ayrılıp ayrılamayacağını sınar */
static b32 arena_can_push_seq(const Arena* a, const usize* sizes, const usize* aligns, u32 n) {
    if (!a || !a->base || a->used > a->cap) return FALSE;
    uptr base_addr = (uptr)a->base;
    usize sim_used = a->used;
    for (u32 i = 0; i < n; i++) {
        usize align = aligns[i];
        usize size  = sizes[i];
        uptr cur_addr = base_addr + (uptr)sim_used;
        uptr aligned  = ALIGN_UP(cur_addr, (uptr)align);
        usize pad     = (usize)(aligned - cur_addr);
        if (pad > a->cap - sim_used || size > a->cap - sim_used - pad) {
            return FALSE;
        }
        sim_used += pad + size;
    }
    return TRUE;
}

BillboardUv billboard_uv_atlas(u32 cols, u32 rows, u32 frame) {
    if (cols == 0u || rows == 0u) {
        return billboard_uv_full();
    }
    u64 total = (u64)cols * (u64)rows;
    u32 idx   = (total > 0u) ? (u32)((u64)frame % total) : 0u;
    u32 col   = idx % cols;
    u32 row   = idx / cols;
    f32 inv_c = 1.0f / (f32)cols;
    f32 inv_r = 1.0f / (f32)rows;
    f32 u0    = (f32)col * inv_c;
    f32 v0    = (f32)row * inv_r;
    return billboard_uv_rect(u0, v0, u0 + inv_c, v0 + inv_r);
}

void billboard_extract_camera(Mat4 view, Vec3* out_right, Vec3* out_up,
                              Vec3* out_forward, Vec3* out_eye) {
    Vec3 r = vec3(view.m[0], view.m[4], view.m[8]);
    Vec3 u = vec3(view.m[1], view.m[5], view.m[9]);

    b32 ok_r = f32_valid_coord(r.x) && f32_valid_coord(r.y) && f32_valid_coord(r.z);
    b32 ok_u = f32_valid_coord(u.x) && f32_valid_coord(u.y) && f32_valid_coord(u.z);

    if (ok_r && ok_u) {
        f32 lr = vec3_len(r);
        if (lr > 1e-6f && f32_is_finite(lr)) {
            r = vec3_scale(r, 1.0f / lr);
            /* Gram-Schmidt ile u'yu r'ye tam dik yap */
            u = vec3_sub(u, vec3_scale(r, vec3_dot(u, r)));
            f32 lu = vec3_len(u);
            if (lu > 1e-6f && f32_is_finite(lu)) {
                u = vec3_scale(u, 1.0f / lu);
                Vec3 n = vec3_cross(r, u);
                Vec3 f = vec3_scale(n, -1.0f);
                if (out_right)   *out_right   = r;
                if (out_up)      *out_up      = u;
                if (out_forward) *out_forward = f;
                if (out_eye) {
                    f32 tx = f32_valid_coord(view.m[12]) ? view.m[12] : 0.0f;
                    f32 ty = f32_valid_coord(view.m[13]) ? view.m[13] : 0.0f;
                    f32 tz = f32_valid_coord(view.m[14]) ? view.m[14] : 0.0f;
                    /* V = [R | t], t = -R * eye => eye = -R^T * t */
                    *out_eye = vec3(
                        -(r.x * tx + u.x * ty + n.x * tz),
                        -(r.y * tx + u.y * ty + n.y * tz),
                        -(r.z * tx + u.z * ty + n.z * tz)
                    );
                }
                return;
            }
        }
    }

    /* Bozuk/tekil matris için birim kamera yedek değerleri */
    if (out_right)   *out_right   = vec3(1.0f, 0.0f, 0.0f);
    if (out_up)      *out_up      = vec3(0.0f, 1.0f, 0.0f);
    if (out_forward) *out_forward = vec3(0.0f, 0.0f, -1.0f);
    if (out_eye)     *out_eye     = vec3(0.0f, 0.0f, 0.0f);
}

b32 billboard_init(BillboardBatch* b, Arena* a, u32 capacity) {
    if (!b) return FALSE;
    __builtin_memset(b, 0, sizeof(*b));
    if (!a || capacity == 0u || capacity > BILLBOARD_MAX_CAPACITY) {
        return FALSE;
    }

    u32 v_cap = capacity * 4u;
    u32 i_cap = capacity * 6u;

    /* mesh_alloc + billboard dizileri için toplam arena sığasını önceden doğrula */
    usize sizes[16] = {
        sizeof(Mesh),
        sizeof(f32) * (usize)v_cap, /* px */
        sizeof(f32) * (usize)v_cap, /* py */
        sizeof(f32) * (usize)v_cap, /* pz */
        sizeof(f32) * (usize)v_cap, /* nx */
        sizeof(f32) * (usize)v_cap, /* ny */
        sizeof(f32) * (usize)v_cap, /* nz */
        sizeof(f32) * (usize)v_cap, /* u  */
        sizeof(f32) * (usize)v_cap, /* v  */
        sizeof(u32) * (usize)v_cap, /* color */
        sizeof(u32) * (usize)i_cap, /* indices */
        sizeof(Billboard) * (usize)capacity,
        sizeof(u32) * (usize)capacity, /* order */
        sizeof(u32) * (usize)capacity, /* sort_idx_tmp */
        sizeof(u32) * (usize)capacity, /* sort_keys */
        sizeof(u32) * (usize)capacity  /* sort_keys_tmp */
    };
    usize aligns[16] = {
        _Alignof(Mesh),
        _Alignof(f32), _Alignof(f32), _Alignof(f32),
        _Alignof(f32), _Alignof(f32), _Alignof(f32),
        _Alignof(f32), _Alignof(f32),
        _Alignof(u32), _Alignof(u32),
        _Alignof(Billboard),
        _Alignof(u32), _Alignof(u32), _Alignof(u32), _Alignof(u32)
    };
    if (!arena_can_push_seq(a, sizes, aligns, 16u)) {
        return FALSE;
    }

    b->mesh          = mesh_alloc(a, v_cap, i_cap, TRUE);
    b->items         = ARENA_PUSH_ARRAY(a, Billboard, capacity);
    b->order         = ARENA_PUSH_ARRAY(a, u32, capacity);
    b->sort_idx_tmp  = ARENA_PUSH_ARRAY(a, u32, capacity);
    b->sort_keys     = ARENA_PUSH_ARRAY(a, u32, capacity);
    b->sort_keys_tmp = ARENA_PUSH_ARRAY(a, u32, capacity);
    b->capacity      = capacity;
    b->cam_right     = vec3(1.0f, 0.0f, 0.0f);
    b->cam_up        = vec3(0.0f, 1.0f, 0.0f);
    b->cam_forward   = vec3(0.0f, 0.0f, -1.0f);
    b->normal        = vec3(0.0f, 0.0f, 1.0f);
    b->eye           = vec3(0.0f, 0.0f, 0.0f);
    b->initialized   = TRUE;

    /* Her dörtgenin 6 üçgen indeksi sabittir; init sırasında bir kez doldurulur */
    u32* ix = b->mesh->indices;
    for (u32 q = 0; q < capacity; q++) {
        u32 v = q * 4u;
        ix[0] = v + 0u; ix[1] = v + 1u; ix[2] = v + 2u;
        ix[3] = v + 0u; ix[4] = v + 2u; ix[5] = v + 3u;
        ix += 6;
    }
    b->mesh->vertex_count = 0u;
    b->mesh->index_count  = 0u;
    return TRUE;
}

void billboard_begin(BillboardBatch* b, Mat4 view) {
    if (!b || !b->initialized) return;
    Vec3 eye;
    billboard_extract_camera(view, &b->cam_right, &b->cam_up, &b->cam_forward, &eye);
    b->normal         = vec3_scale(b->cam_forward, -1.0f);
    b->eye            = eye;
    b->count          = 0u;
    b->overflow_count = 0u;
    b->skipped_count  = 0u;
    b->in_begin       = TRUE;
    b->mesh_dirty     = TRUE;
    b->mesh->vertex_count = 0u;
    b->mesh->index_count  = 0u;
}

void billboard_begin_ex(BillboardBatch* b, Mat4 view, Vec3 eye) {
    if (!b || !b->initialized) return;
    billboard_begin(b, view);
    if (f32_valid_coord(eye.x) && f32_valid_coord(eye.y) && f32_valid_coord(eye.z)) {
        b->eye = eye;
    }
}

b32 billboard_push_item(BillboardBatch* b, const Billboard* item) {
    if (!b || !b->initialized || !item) return FALSE;

    if (!f32_valid_coord(item->pos.x) ||
        !f32_valid_coord(item->pos.y) ||
        !f32_valid_coord(item->pos.z) ||
        !f32_valid_size(item->w) ||
        !f32_valid_size(item->h) ||
        !f32_is_finite(item->rot) ||
        !f32_is_finite(item->uv.u0) || !f32_is_finite(item->uv.v0) ||
        !f32_is_finite(item->uv.u1) || !f32_is_finite(item->uv.v1)) {
        b->skipped_count++;
        return FALSE;
    }

    if (b->count >= b->capacity) {
        b->overflow_count++;
        return FALSE;
    }

    u32 idx = b->count++;
    b->items[idx] = *item;
    b->items[idx].rot = sanitize_angle(item->rot);
    b->order[idx] = idx;
    b->mesh_dirty = TRUE;
    return TRUE;
}

b32 billboard_push_ex(BillboardBatch* b, Vec3 pos, Vec2 size, f32 rot, u32 color, BillboardUv uv) {
    Billboard item;
    item.pos   = pos;
    item.w     = size.x;
    item.h     = size.y;
    item.rot   = rot;
    item.color = color;
    item.uv    = uv;
    return billboard_push_item(b, &item);
}

b32 billboard_push(BillboardBatch* b, Vec3 pos, f32 size, f32 rot, u32 color) {
    return billboard_push_ex(b, pos, vec2(size, size), rot, color, billboard_uv_full());
}

b32 billboard_push_atlas(BillboardBatch* b, Vec3 pos, f32 size, f32 rot, u32 color,
                         u32 cols, u32 rows, u32 frame) {
    return billboard_push_ex(b, pos, vec2(size, size), rot, color,
                             billboard_uv_atlas(cols, rows, frame));
}

/* IEEE-754 f32 -> monoton artan u32 dönüşümü; bit terslemesiyle (~) büyükten küçüğe sıralanır */
static inline u32 f32_to_desc_key(f32 d2) {
    if (!(d2 >= 0.0f) || !f32_is_finite(d2)) d2 = 0.0f;
    u32 bits;
    __builtin_memcpy(&bits, &d2, sizeof(bits));
    return ~bits;
}

void billboard_sort_by_eye(BillboardBatch* b, Vec3 eye) {
    if (!b || !b->initialized) return;
    if (!f32_valid_coord(eye.x) || !f32_valid_coord(eye.y) || !f32_valid_coord(eye.z)) {
        eye = b->eye;
    }

    u32 n = b->count;
    if (n <= 1u) {
        if (!b->in_begin && b->mesh_dirty) {
            billboard_build_mesh(b);
        }
        return;
    }

    u32* keys0 = b->sort_keys;
    u32* keys1 = b->sort_keys_tmp;
    u32* idx0  = b->order;
    u32* idx1  = b->sort_idx_tmp;

    /* 4 geçişli (8-bit basamak) LSD radix histogramlarını tek taramada hazırla */
    u32 hist[4][256];
    __builtin_memset(hist, 0, sizeof(hist));

    const Billboard* items = b->items;
    for (u32 i = 0; i < n; i++) {
        u32 id = idx0[i];
        Vec3 d = vec3_sub(items[id].pos, eye);
        f32 d2 = vec3_dot(d, d);
        u32 k  = f32_to_desc_key(d2);
        keys0[i] = k;
        hist[0][k & 0xFFu]++;
        hist[1][(k >> 8) & 0xFFu]++;
        hist[2][(k >> 16) & 0xFFu]++;
        hist[3][(k >> 24) & 0xFFu]++;
    }

    u32* src_k = keys0;
    u32* dst_k = keys1;
    u32* src_i = idx0;
    u32* dst_i = idx1;

    for (u32 pass = 0; pass < 4u; pass++) {
        u32 shift = pass * 8u;
        u32* h = hist[pass];

        /* Tüm elemanlar aynı kovadaysa bu basamak geçişi sıralamayı değiştirmez */
        b32 trivial = FALSE;
        for (u32 bkt = 0; bkt < 256u; bkt++) {
            if (h[bkt] == n) {
                trivial = TRUE;
                break;
            }
        }
        if (trivial) continue;

        u32 sum = 0u;
        for (u32 bkt = 0; bkt < 256u; bkt++) {
            u32 c = h[bkt];
            h[bkt] = sum;
            sum += c;
        }

        for (u32 i = 0; i < n; i++) {
            u32 k = src_k[i];
            u32 pos = h[(k >> shift) & 0xFFu]++;
            dst_k[pos] = k;
            dst_i[pos] = src_i[i];
        }

        u32* tk = src_k; src_k = dst_k; dst_k = tk;
        u32* ti = src_i; src_i = dst_i; dst_i = ti;
    }

    if (src_i != b->order) {
        __builtin_memcpy(b->order, src_i, sizeof(u32) * (usize)n);
    }

    b->mesh_dirty = TRUE;
    if (!b->in_begin) {
        billboard_build_mesh(b);
    }
}

void billboard_sort(BillboardBatch* b) {
    if (!b || !b->initialized) return;
    billboard_sort_by_eye(b, b->eye);
}

Mesh* billboard_build_mesh(BillboardBatch* b) {
    if (!b || !b->initialized || !b->mesh) return NULL;
    Mesh* m = b->mesh;
    u32 n = b->count;

    if (n == 0u) {
        m->vertex_count = 0u;
        m->index_count  = 0u;
        m->aabb_min = m->aabb_max = m->center = vec3(0.0f, 0.0f, 0.0f);
        m->radius = 0.0f;
        b->mesh_dirty = FALSE;
        return m;
    }

    if (!b->mesh_dirty) {
        return m;
    }

    Vec3 right = b->cam_right;
    Vec3 up    = b->cam_up;
    Vec3 norm  = b->normal;

    f32* restrict px  = m->px;
    f32* restrict py  = m->py;
    f32* restrict pz  = m->pz;
    f32* restrict nx  = m->nx;
    f32* restrict ny  = m->ny;
    f32* restrict nz  = m->nz;
    f32* restrict u   = m->u;
    f32* restrict v   = m->v;
    u32* restrict col = m->color;

    const Billboard* items = b->items;
    const u32* order = b->order;

    Vec3 lo = vec3(1.0e30f, 1.0e30f, 1.0e30f);
    Vec3 hi = vec3(-1.0e30f, -1.0e30f, -1.0e30f);

    for (u32 q = 0; q < n; q++) {
        const Billboard* it = &items[order[q]];
        f32 hw = it->w * 0.5f;
        f32 hh = it->h * 0.5f;
        Vec3 rx, ry;
        if (it->rot == 0.0f) {
            rx = vec3_scale(right, hw);
            ry = vec3_scale(up, hh);
        } else {
            f32 c = m_cos(it->rot);
            f32 s = m_sin(it->rot);
            rx = vec3_add(vec3_scale(right, c * hw), vec3_scale(up, s * hw));
            ry = vec3_add(vec3_scale(up, c * hh), vec3_scale(right, -s * hh));
        }

        /* d0 = rx - ry, d1 = rx + ry
         * Köşeler (CCW ön yüz, s × t = normal):
         * 0 (sol-alt):  pos - rx - ry = pos - d1, UV (u0, v1)
         * 1 (sağ-alt):  pos + rx - ry = pos + d0, UV (u1, v1)
         * 2 (sağ-üst):  pos + rx + ry = pos + d1, UV (u1, v0)
         * 3 (sol-üst):  pos - rx + ry = pos - d0, UV (u0, v0) */
        Vec3 d0 = vec3_sub(rx, ry);
        Vec3 d1 = vec3_add(rx, ry);
        Vec3 p  = it->pos;

        u32 vi = q * 4u;
        px[vi + 0u] = p.x - d1.x; py[vi + 0u] = p.y - d1.y; pz[vi + 0u] = p.z - d1.z;
        px[vi + 1u] = p.x + d0.x; py[vi + 1u] = p.y + d0.y; pz[vi + 1u] = p.z + d0.z;
        px[vi + 2u] = p.x + d1.x; py[vi + 2u] = p.y + d1.y; pz[vi + 2u] = p.z + d1.z;
        px[vi + 3u] = p.x - d0.x; py[vi + 3u] = p.y - d0.y; pz[vi + 3u] = p.z - d0.z;

        nx[vi + 0u] = norm.x; ny[vi + 0u] = norm.y; nz[vi + 0u] = norm.z;
        nx[vi + 1u] = norm.x; ny[vi + 1u] = norm.y; nz[vi + 1u] = norm.z;
        nx[vi + 2u] = norm.x; ny[vi + 2u] = norm.y; nz[vi + 2u] = norm.z;
        nx[vi + 3u] = norm.x; ny[vi + 3u] = norm.y; nz[vi + 3u] = norm.z;

        f32 u0 = it->uv.u0, v0 = it->uv.v0, u1 = it->uv.u1, v1 = it->uv.v1;
        u[vi + 0u] = u0; v[vi + 0u] = v1;
        u[vi + 1u] = u1; v[vi + 1u] = v1;
        u[vi + 2u] = u1; v[vi + 2u] = v0;
        u[vi + 3u] = u0; v[vi + 3u] = v0;

        u32 c = it->color;
        col[vi + 0u] = c;
        col[vi + 1u] = c;
        col[vi + 2u] = c;
        col[vi + 3u] = c;

        /* 4 köşenin tam AABB sınırı: p ± (|rx| + |ry|) */
        f32 ex = m_abs(rx.x) + m_abs(ry.x);
        f32 ey = m_abs(rx.y) + m_abs(ry.y);
        f32 ez = m_abs(rx.z) + m_abs(ry.z);
        lo.x = m_min(lo.x, p.x - ex); hi.x = m_max(hi.x, p.x + ex);
        lo.y = m_min(lo.y, p.y - ey); hi.y = m_max(hi.y, p.y + ey);
        lo.z = m_min(lo.z, p.z - ez); hi.z = m_max(hi.z, p.z + ez);
    }

    m->vertex_count = n * 4u;
    m->index_count  = n * 6u;
    m->aabb_min     = lo;
    m->aabb_max     = hi;
    m->center       = vec3_scale(vec3_add(lo, hi), 0.5f);
    m->radius       = vec3_len(vec3_sub(hi, m->center));
    b->mesh_dirty   = FALSE;
    return m;
}

Mesh* billboard_end(BillboardBatch* b) {
    if (!b || !b->initialized) return NULL;
    b->in_begin = FALSE;
    return billboard_build_mesh(b);
}

b32 billboard_draw(BillboardBatch* b, G3dContext* ctx, const G3dMaterial* mat) {
    if (!b || !b->initialized || !ctx || !mat) return FALSE;
    Mesh* m = billboard_end(b);
    if (!m || m->vertex_count == 0u || m->index_count == 0u) return FALSE;
    Mat4 model = mat4_identity();
    return g3d_draw(ctx, m, &model, mat);
}
