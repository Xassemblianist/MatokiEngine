/* phys/bvh.c — Statik üçgen BVH kurulumu (8-bin SAH + ortanca yedek bölmesi) ve AABB sorgusu.
 * Geçici kurulum dizileri arena_mark/arena_pop_to ile kurulum sonunda geri bırakılır.
 * Kök düğüm her zaman nodes[0]'dır; çocuk çiftleri bitişik (left_first, left_first + 1) tutulur.
 */
#include "phys/bvh.h"

#define SAH_BINS 8u
#define BVH_MAX_DEPTH 48u

typedef struct {
    Aabb bounds;
    u32  count;
} SahBin;

static void swap_tri_data(BvhTri* tris, Aabb* bounds, Vec3* centroids, u32 i, u32 j) {
    if (i == j) return;
    BvhTri tt = tris[i]; tris[i] = tris[j]; tris[j] = tt;
    Aabb   tb = bounds[i]; bounds[i] = bounds[j]; bounds[j] = tb;
    Vec3   tc = centroids[i]; centroids[i] = centroids[j]; centroids[j] = tc;
}

static f32 vec3_axis(Vec3 v, u32 axis) {
    return (axis == 0u) ? v.x : ((axis == 1u) ? v.y : v.z);
}

static void bvh_build_node(Bvh* bvh, u32 node_idx, u32 first, u32 count,
                           Aabb* tri_bounds, Vec3* centroids, u32 depth) {
    BvhNode* node = &bvh->nodes[node_idx];
    Aabb b = aabb_empty();
    Aabb cb = aabb_empty();
    for (u32 i = 0; i < count; i++) {
        u32 idx = first + i;
        b  = aabb_merge(b, tri_bounds[idx]);
        cb = aabb_expand_point(cb, centroids[idx]);
    }
    /* Sınırda teğet ışınların float yuvarlama hatasıyla kaçmaması için küçük pay */
    node->bounds = aabb_expand_margin(b, 1e-5f);

    if (count <= BVH_LEAF_MAX || depth >= BVH_MAX_DEPTH || bvh->node_count + 2u > bvh->node_cap) {
        node->left_first = first;
        node->count = count;
        return;
    }

    Vec3 ext = vec3_sub(cb.max, cb.min);
    u32 axis = 0u;
    if (ext.y > ext.x && ext.y >= ext.z) axis = 1u;
    else if (ext.z > ext.x && ext.z > ext.y) axis = 2u;

    f32 axis_len = vec3_axis(ext, axis);
    f32 axis_min = vec3_axis(cb.min, axis);

    u32 mid = first + count / 2u;
    if (axis_len > 1e-7f) {
        SahBin bins[SAH_BINS];
        for (u32 k = 0; k < SAH_BINS; k++) {
            bins[k].bounds = aabb_empty();
            bins[k].count = 0u;
        }
        f32 scale = (f32)SAH_BINS / axis_len;
        for (u32 i = 0; i < count; i++) {
            u32 idx = first + i;
            f32 c = vec3_axis(centroids[idx], axis);
            i32 bi = (i32)((c - axis_min) * scale);
            if (bi < 0) bi = 0;
            if (bi >= (i32)SAH_BINS) bi = (i32)SAH_BINS - 1;
            bins[bi].count++;
            bins[bi].bounds = aabb_merge(bins[bi].bounds, tri_bounds[idx]);
        }

        f32 left_area[SAH_BINS - 1];
        u32 left_cnt[SAH_BINS - 1];
        f32 right_area[SAH_BINS - 1];
        u32 right_cnt[SAH_BINS - 1];

        Aabb cur_b = aabb_empty();
        u32  cur_c = 0u;
        for (u32 k = 0; k < SAH_BINS - 1; k++) {
            cur_c += bins[k].count;
            cur_b = aabb_merge(cur_b, bins[k].bounds);
            left_cnt[k]  = cur_c;
            left_area[k] = aabb_surface_area(cur_b);
        }

        cur_b = aabb_empty();
        cur_c = 0u;
        for (u32 k = SAH_BINS - 1; k >= 1; k--) {
            cur_c += bins[k].count;
            cur_b = aabb_merge(cur_b, bins[k].bounds);
            right_cnt[k - 1]  = cur_c;
            right_area[k - 1] = aabb_surface_area(cur_b);
        }

        f32 best_cost = 1e30f;
        i32 best_split = -1;
        for (u32 k = 0; k < SAH_BINS - 1; k++) {
            if (left_cnt[k] == 0u || right_cnt[k] == 0u) continue;
            f32 cost = left_area[k] * (f32)left_cnt[k] + right_area[k] * (f32)right_cnt[k];
            if (cost < best_cost) {
                best_cost = cost;
                best_split = (i32)k;
            }
        }

        if (best_split >= 0) {
            u32 i = first;
            u32 j = first + count - 1u;
            while (i <= j) {
                f32 c = vec3_axis(centroids[i], axis);
                i32 bi = (i32)((c - axis_min) * scale);
                if (bi < 0) bi = 0;
                if (bi >= (i32)SAH_BINS) bi = (i32)SAH_BINS - 1;
                if (bi <= best_split) {
                    i++;
                } else {
                    swap_tri_data(bvh->tris, tri_bounds, centroids, i, j);
                    if (j == 0u) break;
                    j--;
                }
            }
            if (i > first && i < first + count) {
                mid = i;
            }
        }
    }

    u32 left_idx = bvh->node_count;
    bvh->node_count += 2u;
    node->left_first = left_idx;
    node->count = 0u;

    u32 left_count = mid - first;
    u32 right_count = count - left_count;
    bvh_build_node(bvh, left_idx,      first, left_count,  tri_bounds, centroids, depth + 1u);
    bvh_build_node(bvh, left_idx + 1u, mid,   right_count, tri_bounds, centroids, depth + 1u);
}

Bvh* bvh_build(Arena* a, const Mesh* mesh) {
    Bvh* bvh = ARENA_PUSH_STRUCT(a, Bvh);
    bvh->mesh = mesh;
    if (!mesh || mesh->index_count < 3u) {
        return bvh;
    }

    u32 tri_count = mesh->index_count / 3u;
    bvh->tri_count = tri_count;
    bvh->node_cap  = 2u * tri_count;
    bvh->tris      = ARENA_PUSH_ARRAY(a, BvhTri, tri_count);
    bvh->nodes     = ARENA_PUSH_ARRAY(a, BvhNode, bvh->node_cap);

    usize mark = arena_mark(a);
    Aabb* tri_bounds = ARENA_PUSH_ARRAY(a, Aabb, tri_count);
    Vec3* centroids  = ARENA_PUSH_ARRAY(a, Vec3, tri_count);

    for (u32 t = 0; t < tri_count; t++) {
        u32 i0 = mesh->indices[t * 3u + 0u];
        u32 i1 = mesh->indices[t * 3u + 1u];
        u32 i2 = mesh->indices[t * 3u + 2u];
        Vec3 v0 = vec3(mesh->px[i0], mesh->py[i0], mesh->pz[i0]);
        Vec3 v1 = vec3(mesh->px[i1], mesh->py[i1], mesh->pz[i1]);
        Vec3 v2 = vec3(mesh->px[i2], mesh->py[i2], mesh->pz[i2]);
        bvh->tris[t].v0 = v0;
        bvh->tris[t].v1 = v1;
        bvh->tris[t].v2 = v2;
        bvh->tris[t].tri_index = t;
        tri_bounds[t] = aabb_for_triangle(v0, v1, v2);
        centroids[t]  = vec3_scale(vec3_add(v0, vec3_add(v1, v2)), 1.0f / 3.0f);
    }

    bvh->node_count = 1u;
    bvh_build_node(bvh, 0u, 0u, tri_count, tri_bounds, centroids, 0u);

    arena_pop_to(a, mark);
    return bvh;
}

u32 bvh_query_aabb(const Bvh* bvh, Aabb query, u32* out_tri_slots, u32 max_out) {
    if (!bvh || bvh->node_count == 0u || max_out == 0u) return 0u;
    u32 stack[64];
    u32 sp = 0u;
    stack[sp++] = 0u;
    u32 found = 0u;

    while (sp > 0u) {
        u32 idx = stack[--sp];
        const BvhNode* node = &bvh->nodes[idx];
        if (!aabb_overlaps(node->bounds, query)) continue;
        if (node->count > 0u) {
            for (u32 i = 0; i < node->count; i++) {
                if (found < max_out) {
                    out_tri_slots[found++] = node->left_first + i;
                }
            }
        } else if (sp + 2u <= 64u) {
            stack[sp++] = node->left_first + 1u;
            stack[sp++] = node->left_first;
        }
    }
    return found;
}
