/* raster/raster.c — Karo tabanlı yazılım rasterizer çekirdeği implementasyonu.
 * 24.8 sabit nokta kenar fonksiyonları ve top-left doldurma kuralı ile boşluksuz/örtüşmesiz
 * üçgen kurulumu ve karo binlemesi yapar; wasm'da raster_simd_wasm.c çekirdeklerini,
 * host'ta ise bit-bit eşdeğer skaler yedek yolu çalıştırır.
 */
#include "raster/raster.h"
#include "raster/ref_shaders.h"
#include "core/fixed.h"
#include "core/math.h"
#include "core/log.h"
#include "core/profiler.h"
#include "thread/thread.h"
#include "thread/job.h"

#define TILE_CHUNK_CAP 64

static b32                   g_raster_dynamic_sched = TRUE;
static RasterWatEdgeBlockFn  g_wat_edge_fn          = NULL;
static RasterWatBlendAlphaFn g_wat_blend_fn         = NULL;

struct TileBinChunk {
    u32           count;
    TileBinChunk* next;
    BinnedTri*    tris[TILE_CHUNK_CAP];
};

static RasterShaderState* ensure_pinned_state(Rasterizer* r) {
    if (r->pinned_state == NULL) {
        r->pinned_state = ARENA_PUSH_STRUCT(r->frame, RasterShaderState);
        *r->pinned_state = r->cur_state;
    }
    return r->pinned_state;
}

static inline void tile_bin_push(Arena* frame, TileBin* bin, BinnedTri* tri) {
    TileBinChunk* tail = bin->tail;
    if (tail == NULL || tail->count >= TILE_CHUNK_CAP) {
        TileBinChunk* chunk = ARENA_PUSH_STRUCT(frame, TileBinChunk);
        chunk->count = 0;
        chunk->next  = NULL;
        if (tail != NULL) {
            tail->next = chunk;
        } else {
            bin->head = chunk;
        }
        bin->tail = chunk;
        tail = chunk;
    }
    tail->tris[tail->count++] = tri;
}

static inline void setup_edge(Fx8 xa, Fx8 ya, Fx8 xb, Fx8 yb,
                              i32* out_a, i32* out_b, i32* out_c) {
    i32 a = ya - yb;
    i32 b = xb - xa;
    b32 top_left = (a > 0) || (a == 0 && b > 0);
    i64 bias = top_left ? 0 : -1;
    i64 k = (i64)a * (i64)(FX8_HALF - xa) + (i64)b * (i64)(FX8_HALF - ya) + bias;
    *out_a = a;
    *out_b = b;
    *out_c = (i32)(k >> FX8_SHIFT);
}

static void raster_process_tri_buf(Rasterizer* r) {
    u32 count = r->tri_buf_count;
    if (count == 0) return;
    r->tri_buf_count = 0;

    RasterShaderState* state = ensure_pinned_state(r);
    u32 vcount = state->varying_count;
    i32 fb_w = (i32)r->fb->w;
    i32 fb_h = (i32)r->fb->h;

    PROF_BEGIN("setup");
    /* Üçgen başına ayrı arena_push/memset çağrısını önlemek için blok halinde ayır */
    BinnedTri* tri_pool = ARENA_PUSH_ARRAY(r->frame, BinnedTri, count);
    f32* vc_pool = (vcount > 0) ? ARENA_PUSH_ARRAY(r->frame, f32, count * vcount * 3u) : NULL;
    u32 setup_count = 0;

    for (u32 i = 0; i < count; i++) {
        RasterVert v0 = r->tri_buf[i].v0;
        RasterVert v1 = r->tri_buf[i].v1;
        RasterVert v2 = r->tri_buf[i].v2;

        Fx8 x0 = fx8_from_f32(v0.x);
        Fx8 y0 = fx8_from_f32(v0.y);
        Fx8 x1 = fx8_from_f32(v1.x);
        Fx8 y1 = fx8_from_f32(v1.y);
        Fx8 x2 = fx8_from_f32(v2.x);
        Fx8 y2 = fx8_from_f32(v2.y);

        i64 area2 = (i64)(x1 - x0) * (i64)(y2 - y0) - (i64)(y1 - y0) * (i64)(x2 - x0);
        if (area2 == 0) continue;

        if (area2 < 0) {
            if (r->cull_back) continue;
            RasterVert tv = v1; v1 = v2; v2 = tv;
            Fx8 tx = x1; x1 = x2; x2 = tx;
            Fx8 ty = y1; y1 = y2; y2 = ty;
            area2 = -area2;
        }

        Fx8 min_fx = MIN(x0, MIN(x1, x2));
        Fx8 max_fx = MAX(x0, MAX(x1, x2));
        Fx8 min_fy = MIN(y0, MIN(y1, y2));
        Fx8 max_fy = MAX(y0, MAX(y1, y2));

        i32 min_px = (min_fx + (FX8_HALF - 1)) >> FX8_SHIFT;
        i32 max_px = (max_fx - FX8_HALF) >> FX8_SHIFT;
        i32 min_py = (min_fy + (FX8_HALF - 1)) >> FX8_SHIFT;
        i32 max_py = (max_fy - FX8_HALF) >> FX8_SHIFT;

        min_px = MAX(min_px, 0);
        min_py = MAX(min_py, 0);
        max_px = MIN(max_px, fb_w - 1);
        max_py = MIN(max_py, fb_h - 1);

        if (min_px > max_px || min_py > max_py) continue;

        f32 fx0 = v0.x, fy0 = v0.y;
        f32 fx1 = v1.x, fy1 = v1.y;
        f32 fx2 = v2.x, fy2 = v2.y;
        f32 f_area2 = (fx1 - fx0) * (fy2 - fy0) - (fy1 - fy0) * (fx2 - fx0);
        if (m_abs(f_area2) < 1e-12f) {
            fx0 = fx8_to_f32(x0); fy0 = fx8_to_f32(y0);
            fx1 = fx8_to_f32(x1); fy1 = fx8_to_f32(y1);
            fx2 = fx8_to_f32(x2); fy2 = fx8_to_f32(y2);
            f_area2 = (fx1 - fx0) * (fy2 - fy0) - (fy1 - fy0) * (fx2 - fx0);
        }
        f32 inv_area = 1.0f / f_area2;

        BinnedTri* tri = &tri_pool[setup_count];
        tri->state = state;
        tri->min_x = (i16)min_px;
        tri->min_y = (i16)min_py;
        tri->max_x = (i16)max_px;
        tri->max_y = (i16)max_py;

        setup_edge(x0, y0, x1, y1, &tri->a0, &tri->b0, &tri->c0);
        setup_edge(x1, y1, x2, y2, &tri->a1, &tri->b1, &tri->c1);
        setup_edge(x2, y2, x0, y0, &tri->a2, &tri->b2, &tri->c2);

        f32 dl1_dx = (fy2 - fy0) * inv_area;
        f32 dl1_dy = (fx0 - fx2) * inv_area;
        f32 dl2_dx = (fy0 - fy1) * inv_area;
        f32 dl2_dy = (fx1 - fx0) * inv_area;

        tri->x0     = fx0;
        tri->y0     = fy0;
        tri->dl1_dx = dl1_dx;
        tri->dl1_dy = dl1_dy;
        tri->dl2_dx = dl2_dx;
        tri->dl2_dy = dl2_dy;

        /* Derinlik düzlemi: z(x,y) = z0 + (x + 0.5 - x0)*dz_dx + (y + 0.5 - y0)*dz_dy */
        f32 dz1 = v1.z - v0.z;
        f32 dz2 = v2.z - v0.z;
        tri->z0    = v0.z;
        tri->dz_dx = dz1 * dl1_dx + dz2 * dl2_dx;
        tri->dz_dy = dz1 * dl1_dy + dz2 * dl2_dy;

        f32 iw0 = (v0.inv_w == 0.0f) ? 1.0f : v0.inv_w;
        f32 iw1 = (v1.inv_w == 0.0f) ? 1.0f : v1.inv_w;
        f32 iw2 = (v2.inv_w == 0.0f) ? 1.0f : v2.inv_w;
        b32 is_affine = (iw0 == 1.0f && iw1 == 1.0f && iw2 == 1.0f);
        tri->inv_w0    = iw0;
        tri->inv_w1    = iw1;
        tri->inv_w2    = iw2;
        tri->is_affine = is_affine;

        if (vcount > 0) {
            f32* vc = vc_pool + setup_count * vcount * 3u;
            tri->var_coeffs = vc;
            if (is_affine) {
                for (u32 k = 0; k < vcount; k++) {
                    f32 f0 = v0.var[k];
                    f32 d1 = v1.var[k] - f0;
                    f32 d2 = v2.var[k] - f0;
                    vc[k * 3u + 0u] = f0;
                    vc[k * 3u + 1u] = d1 * dl1_dx + d2 * dl2_dx;
                    vc[k * 3u + 2u] = d1 * dl1_dy + d2 * dl2_dy;
                }
            } else {
                for (u32 k = 0; k < vcount; k++) {
                    f32 u0 = v0.var[k] * iw0;
                    f32 u1 = v1.var[k] * iw1;
                    f32 u2 = v2.var[k] * iw2;
                    vc[k * 3u + 0u] = u0;
                    vc[k * 3u + 1u] = u1 - u0;
                    vc[k * 3u + 2u] = u2 - u0;
                }
            }
        }

        setup_count++;
    }
    PROF_END();

    PROF_BEGIN("bin");
    u32 tiles_x = r->tiles_x;
    for (u32 i = 0; i < setup_count; i++) {
        BinnedTri* tri = &tri_pool[i];
        u32 tx0 = (u32)tri->min_x >> 6;
        u32 tx1 = (u32)tri->max_x >> 6;
        u32 ty0 = (u32)tri->min_y >> 6;
        u32 ty1 = (u32)tri->max_y >> 6;

        /* Hızlı yol: tek 64x64 karo içine düşen küçük üçgenler */
        if (((tx0 ^ tx1) | (ty0 ^ ty1)) == 0u) {
            tile_bin_push(r->frame, &r->tiles[ty0 * tiles_x + tx0], tri);
            r->stat_tris_binned++;
        } else {
            const i32 ts = RASTER_TILE_SIZE - 1;
            i32 max_t0 = (tri->a0 > 0 ? ts * tri->a0 : 0) + (tri->b0 > 0 ? ts * tri->b0 : 0);
            i32 max_t1 = (tri->a1 > 0 ? ts * tri->a1 : 0) + (tri->b1 > 0 ? ts * tri->b1 : 0);
            i32 max_t2 = (tri->a2 > 0 ? ts * tri->a2 : 0) + (tri->b2 > 0 ? ts * tri->b2 : 0);

            for (u32 ty = ty0; ty <= ty1; ty++) {
                i32 py = (i32)(ty << 6);
                for (u32 tx = tx0; tx <= tx1; tx++) {
                    i32 px = (i32)(tx << 6);
                    i32 e0 = tri->a0 * px + tri->b0 * py + tri->c0 + max_t0;
                    i32 e1 = tri->a1 * px + tri->b1 * py + tri->c1 + max_t1;
                    i32 e2 = tri->a2 * px + tri->b2 * py + tri->c2 + max_t2;
                    if ((e0 | e1 | e2) < 0) continue;

                    tile_bin_push(r->frame, &r->tiles[ty * tiles_x + tx], tri);
                    r->stat_tris_binned++;
                }
            }
        }
    }
    PROF_END();
}

static void raster_commit_depth(Rasterizer* r, u32 count) {
    f32* depth = r->fb->depth;
    if (depth == NULL) return;
    u32 fb_w = r->fb->w;
    for (u32 q = 0; q < count; q++) {
        u32 mask = r->quads[q].mask;
        if (mask == 0) continue;
        u32 qx = r->quads[q].x;
        u32 qy = r->quads[q].y;
        const f32* zv = r->quad_z + q * 4u;
        f32* d0 = depth + qy * fb_w + qx;
        f32* d1 = d0 + fb_w;
        if (mask == 0xFu) {
            d0[0] = zv[0];
            d0[1] = zv[1];
            d1[0] = zv[2];
            d1[1] = zv[3];
        } else {
            if (mask & 1u) d0[0] = zv[0];
            if (mask & 2u) d0[1] = zv[1];
            if (mask & 4u) d1[0] = zv[2];
            if (mask & 8u) d1[1] = zv[3];
        }
    }
}

static void raster_flush_batch(Rasterizer* r, const RasterShaderState* state) {
    u32 count = r->batch_count;
    if (count == 0) return;
    r->batch_count = 0;

    u32 vcount = state->varying_count;
    if (count < SHADE_MAX_QUADS && vcount > 1) {
        for (u32 v = 1; v < vcount; v++) {
            f32* src = r->varyings + (v * SHADE_MAX_QUADS) * 4u;
            f32* dst = r->varyings + (v * count) * 4u;
            __builtin_memmove(dst, src, (usize)count * 4u * sizeof(f32));
        }
    }

    ShadeBatch batch;
    batch.count         = count;
    batch.varying_count = vcount;
    batch.quads         = r->quads;
    batch.varyings      = r->varyings;
    batch.uniforms      = (const u8*)state->uniforms;
    batch.out_rgba      = r->out_rgba;
    batch.textures      = state->textures;
    batch.fb_px         = r->fb->px;
    batch.fb_stride     = r->fb->w;

    PROF_BEGIN("shade");
#if defined(__wasm32__)
    if (state->shade_fn == REF_SHADE_SPRITE_FN) {
        ref_shade_sprite(&batch);
    } else if (state->shade_fn == REF_SHADE_SOLID_FN) {
        ref_shade_solid(&batch);
    } else if (state->shade_fn == REF_SHADE_VCOLOR_FN) {
        ref_shade_vcolor(&batch);
    } else if (state->shade_fn != NULL) {
        state->shade_fn(&batch);
    }
#else
    if (state->shade_fn != NULL) {
        state->shade_fn(&batch);
    }
#endif
    PROF_END();

    if (state->depth_write && state->depth_late && r->fb->depth != NULL) {
        raster_commit_depth(r, count);
    }

    if (state->fused) {
        u64 pixels_written = 0;
        for (u32 q = 0; q < count; q++) {
            u32 m = r->quads[q].mask & 0xFu;
            pixels_written += (u64)__builtin_popcount(m);
        }
        r->stat_quads  += count;
        r->stat_pixels += pixels_written;
        return;
    }

    PROF_BEGIN("blend");
    u64 pixels_written = raster_kern_blend_batch(r, state, count);
    r->stat_quads  += count;
    r->stat_pixels += pixels_written;
    PROF_END();
}

__attribute__((noinline)) void raster_flush_full_batch(Rasterizer* r,
                                                       const RasterShaderState* state) {
    PROF_END();
    raster_flush_batch(r, state);
    PROF_BEGIN("raster");
}

#if !defined(__wasm_simd128__)

static inline u8 f32_to_u8(f32 v) {
    f32 c = m_clamp(v, 0.0f, 1.0f);
    return (u8)(i32)(c * 255.0f + 0.5f);
}

static inline u32 pack_rgba_f32(f32 r, f32 g, f32 b, f32 a) {
    return FB_RGBA(f32_to_u8(r), f32_to_u8(g), f32_to_u8(b), f32_to_u8(a));
}

static inline b32 depth_cmp_scalar(DepthFunc fn, f32 z, f32 ref) {
    switch (fn) {
        case DEPTH_NEVER:    return FALSE;
        case DEPTH_LESS:     return z < ref;
        case DEPTH_EQUAL:    return z == ref;
        case DEPTH_LEQUAL:   return z <= ref;
        case DEPTH_GREATER:  return z > ref;
        case DEPTH_NOTEQUAL: return z != ref;
        case DEPTH_GEQUAL:   return z >= ref;
        case DEPTH_ALWAYS:   return TRUE;
    }
    return TRUE;
}

u64 raster_kern_blend_batch(Rasterizer* r, const RasterShaderState* state, u32 count) {
    u32 total_lanes = count * 4u;
    const f32* out_r = r->out_rgba + 0u * total_lanes;
    const f32* out_g = r->out_rgba + 1u * total_lanes;
    const f32* out_b = r->out_rgba + 2u * total_lanes;
    const f32* out_a = r->out_rgba + 3u * total_lanes;

    u32* fb_px = r->fb->px;
    u32  fb_w  = r->fb->w;
    u64  pixels_written = 0;

    if (state->blend == BLEND_NONE) {
        for (u32 q = 0; q < count; q++) {
            u32 mask = r->quads[q].mask;
            if (mask == 0) continue;
            u32 qx = r->quads[q].x;
            u32 qy = r->quads[q].y;
            u32 base = q * 4u;
            u32* row0 = fb_px + qy * fb_w + qx;
            u32* row1 = row0 + fb_w;

            if (mask == 0xFu) {
                row0[0] = pack_rgba_f32(out_r[base + 0u], out_g[base + 0u], out_b[base + 0u], out_a[base + 0u]);
                row0[1] = pack_rgba_f32(out_r[base + 1u], out_g[base + 1u], out_b[base + 1u], out_a[base + 1u]);
                row1[0] = pack_rgba_f32(out_r[base + 2u], out_g[base + 2u], out_b[base + 2u], out_a[base + 2u]);
                row1[1] = pack_rgba_f32(out_r[base + 3u], out_g[base + 3u], out_b[base + 3u], out_a[base + 3u]);
                pixels_written += 4u;
            } else {
                if (mask & 1u) {
                    row0[0] = pack_rgba_f32(out_r[base + 0u], out_g[base + 0u], out_b[base + 0u], out_a[base + 0u]);
                    pixels_written++;
                }
                if (mask & 2u) {
                    row0[1] = pack_rgba_f32(out_r[base + 1u], out_g[base + 1u], out_b[base + 1u], out_a[base + 1u]);
                    pixels_written++;
                }
                if (mask & 4u) {
                    row1[0] = pack_rgba_f32(out_r[base + 2u], out_g[base + 2u], out_b[base + 2u], out_a[base + 2u]);
                    pixels_written++;
                }
                if (mask & 8u) {
                    row1[1] = pack_rgba_f32(out_r[base + 3u], out_g[base + 3u], out_b[base + 3u], out_a[base + 3u]);
                    pixels_written++;
                }
            }
        }
    } else if (state->blend == BLEND_ALPHA) {
        const f32 inv255 = 1.0f / 255.0f;
        for (u32 q = 0; q < count; q++) {
            u32 mask = r->quads[q].mask;
            if (mask == 0) continue;
            u32 qx = r->quads[q].x;
            u32 qy = r->quads[q].y;
            u32 base = q * 4u;

            for (u32 lane = 0; lane < 4; lane++) {
                if ((mask & (1u << lane)) == 0) continue;
                u32 idx = base + lane;
                u32* dst_ptr = fb_px + (qy + (lane >> 1u)) * fb_w + (qx + (lane & 1u));
                u32 dst = *dst_ptr;

                f32 sr = m_clamp(out_r[idx], 0.0f, 1.0f);
                f32 sg = m_clamp(out_g[idx], 0.0f, 1.0f);
                f32 sb = m_clamp(out_b[idx], 0.0f, 1.0f);
                f32 sa = m_clamp(out_a[idx], 0.0f, 1.0f);
                f32 inv_sa = 1.0f - sa;

                f32 dr = (f32)(dst & 0xFFu) * inv255;
                f32 dg = (f32)((dst >> 8) & 0xFFu) * inv255;
                f32 db = (f32)((dst >> 16) & 0xFFu) * inv255;
                f32 da = (f32)((dst >> 24) & 0xFFu) * inv255;

                u8 r8 = f32_to_u8(sr * sa + dr * inv_sa);
                u8 g8 = f32_to_u8(sg * sa + dg * inv_sa);
                u8 b8 = f32_to_u8(sb * sa + db * inv_sa);
                u8 a8 = f32_to_u8(sa + da * inv_sa);
                *dst_ptr = FB_RGBA(r8, g8, b8, a8);
                pixels_written++;
            }
        }
    } else { /* BLEND_ADD */
        const f32 inv255 = 1.0f / 255.0f;
        for (u32 q = 0; q < count; q++) {
            u32 mask = r->quads[q].mask;
            if (mask == 0) continue;
            u32 qx = r->quads[q].x;
            u32 qy = r->quads[q].y;
            u32 base = q * 4u;

            for (u32 lane = 0; lane < 4; lane++) {
                if ((mask & (1u << lane)) == 0) continue;
                u32 idx = base + lane;
                u32* dst_ptr = fb_px + (qy + (lane >> 1u)) * fb_w + (qx + (lane & 1u));
                u32 dst = *dst_ptr;

                f32 sr = m_clamp(out_r[idx], 0.0f, 1.0f);
                f32 sg = m_clamp(out_g[idx], 0.0f, 1.0f);
                f32 sb = m_clamp(out_b[idx], 0.0f, 1.0f);
                f32 sa = m_clamp(out_a[idx], 0.0f, 1.0f);

                f32 dr = (f32)(dst & 0xFFu) * inv255;
                f32 dg = (f32)((dst >> 8) & 0xFFu) * inv255;
                f32 db = (f32)((dst >> 16) & 0xFFu) * inv255;
                f32 da = (f32)((dst >> 24) & 0xFFu) * inv255;

                u8 r8 = f32_to_u8(sr + dr);
                u8 g8 = f32_to_u8(sg + dg);
                u8 b8 = f32_to_u8(sb + db);
                u8 a8 = f32_to_u8(sa + da);
                *dst_ptr = FB_RGBA(r8, g8, b8, a8);
                pixels_written++;
            }
        }
    }

    return pixels_written;
}

static inline void emit_quad_scalar(Rasterizer* r, const BinnedTri* tri,
                                    const RasterShaderState* state, u16 qx, u16 qy, u32 mask) {
    f32 dx = ((f32)qx + 0.5f) - tri->x0;
    f32 dy = ((f32)qy + 0.5f) - tri->y0;

    /* Early-Z: shade öncesinde 4-lane z interpolasyonu ve derinlik testi */
    f32 z_lanes[4];
    b32 has_depth = (r->fb->depth != NULL) && (state->depth_test || state->depth_write);
    if (has_depth) {
        f32 zb = tri->z0 + dx * tri->dz_dx + dy * tri->dz_dy;
        f32 gx = tri->dz_dx;
        f32 gy = tri->dz_dy;
        z_lanes[0] = zb;
        z_lanes[1] = zb + gx;
        z_lanes[2] = zb + gy;
        z_lanes[3] = zb + gx + gy;

        if (state->depth_test) {
            const f32* d0 = r->fb->depth + (u32)qy * r->fb->w + (u32)qx;
            const f32* d1 = d0 + r->fb->w;
            u32 zmask = 0;
            if ((mask & 1u) && depth_cmp_scalar(state->depth_func, z_lanes[0], d0[0])) zmask |= 1u;
            if ((mask & 2u) && depth_cmp_scalar(state->depth_func, z_lanes[1], d0[1])) zmask |= 2u;
            if ((mask & 4u) && depth_cmp_scalar(state->depth_func, z_lanes[2], d1[0])) zmask |= 4u;
            if ((mask & 8u) && depth_cmp_scalar(state->depth_func, z_lanes[3], d1[1])) zmask |= 8u;
            mask &= zmask;
            if (mask == 0) return;
        }
    }

    if (state->shade_fn == NULL) {
        /* Yalnız derinlik (gölge haritası): shade/blend yok, quad kuyruğa girmez */
        if (has_depth && state->depth_write) {
            f32* d0 = r->fb->depth + (u32)qy * r->fb->w + (u32)qx;
            f32* d1 = d0 + r->fb->w;
            if (mask & 1u) d0[0] = z_lanes[0];
            if (mask & 2u) d0[1] = z_lanes[1];
            if (mask & 4u) d1[0] = z_lanes[2];
            if (mask & 8u) d1[1] = z_lanes[3];
        }
        return;
    }

    u32 q = r->batch_count++;
    r->quads[q].mask = mask;
    r->quads[q].x    = qx;
    r->quads[q].y    = qy;

    if (has_depth && state->depth_write) {
        if (state->depth_late) {
            f32* qz = r->quad_z + q * 4u;
            qz[0] = z_lanes[0];
            qz[1] = z_lanes[1];
            qz[2] = z_lanes[2];
            qz[3] = z_lanes[3];
        } else {
            /* Early-Z yazımı: sonraki üçgenler aynı batch içinde güncel derinliği görür */
            f32* d0 = r->fb->depth + (u32)qy * r->fb->w + (u32)qx;
            f32* d1 = d0 + r->fb->w;
            if (mask & 1u) d0[0] = z_lanes[0];
            if (mask & 2u) d0[1] = z_lanes[1];
            if (mask & 4u) d1[0] = z_lanes[2];
            if (mask & 8u) d1[1] = z_lanes[3];
        }
    }

    u32 vcount = state->varying_count;
    if (vcount > 0) {
        const f32* vc = tri->var_coeffs;
        if (tri->is_affine) {
            for (u32 v = 0; v < vcount; v++) {
                f32 v0 = vc[v * 3u + 0u];
                f32 gx = vc[v * 3u + 1u];
                f32 gy = vc[v * 3u + 2u];
                f32 b0 = v0 + dx * gx + dy * gy;
                f32* dst = r->varyings + (v * SHADE_MAX_QUADS + q) * 4u;
                dst[0] = b0;
                dst[1] = b0 + gx;
                dst[2] = b0 + gy;
                dst[3] = b0 + gx + gy;
            }
        } else {
            f32 l1_0 = dx * tri->dl1_dx + dy * tri->dl1_dy;
            f32 l2_0 = dx * tri->dl2_dx + dy * tri->dl2_dy;
            for (u32 lane = 0; lane < 4; lane++) {
                f32 ox = (f32)(lane & 1u);
                f32 oy = (f32)(lane >> 1u);
                f32 l1 = l1_0 + ox * tri->dl1_dx + oy * tri->dl1_dy;
                f32 l2 = l2_0 + ox * tri->dl2_dx + oy * tri->dl2_dy;
                f32 l0 = 1.0f - l1 - l2;
                f32 iw = l0 * tri->inv_w0 + l1 * tri->inv_w1 + l2 * tri->inv_w2;
                f32 w  = (m_abs(iw) > 1e-20f) ? (1.0f / iw) : 1.0f;
                for (u32 v = 0; v < vcount; v++) {
                    f32 u0  = vc[v * 3u + 0u];
                    f32 du1 = vc[v * 3u + 1u];
                    f32 du2 = vc[v * 3u + 2u];
                    r->varyings[(v * SHADE_MAX_QUADS + q) * 4u + lane] =
                        (u0 + l1 * du1 + l2 * du2) * w;
                }
            }
        }
    }

    if (r->batch_count == SHADE_MAX_QUADS) {
        raster_flush_full_batch(r, state);
    }
}

void raster_kern_tri_scan(Rasterizer* r, const BinnedTri* tri,
                          const RasterShaderState* state, i32 tile_x0, i32 tile_y0,
                          i32 tile_x1, i32 tile_y1) {
    i32 min_x = MAX((i32)tri->min_x, tile_x0);
    i32 max_x = MIN((i32)tri->max_x, tile_x1);
    i32 min_y = MAX((i32)tri->min_y, tile_y0);
    i32 max_y = MIN((i32)tri->max_y, tile_y1);
    if (min_x > max_x || min_y > max_y) return;

    i32 a0 = tri->a0, b0 = tri->b0, c0 = tri->c0;
    i32 a1 = tri->a1, b1 = tri->b1, c1 = tri->c1;
    i32 a2 = tri->a2, b2 = tri->b2, c2 = tri->c2;

    i32 qx_min = min_x & ~1;
    i32 qy_min = min_y & ~1;

    if (max_x - qx_min < 16 && max_y - qy_min < 16) {
        i32 qrow_e0 = a0 * qx_min + b0 * qy_min + c0;
        i32 qrow_e1 = a1 * qx_min + b1 * qy_min + c1;
        i32 qrow_e2 = a2 * qx_min + b2 * qy_min + c2;

        for (i32 qy = qy_min; qy <= max_y; qy += 2) {
            i32 qe0 = qrow_e0;
            i32 qe1 = qrow_e1;
            i32 qe2 = qrow_e2;

            for (i32 qx = qx_min; qx <= max_x; qx += 2) {
                u32 m0 = (u32)((qe0 | qe1 | qe2) >= 0);
                u32 m1 = (u32)(((qe0 + a0) | (qe1 + a1) | (qe2 + a2)) >= 0);
                u32 m2 = (u32)(((qe0 + b0) | (qe1 + b1) | (qe2 + b2)) >= 0);
                u32 m3 = (u32)(((qe0 + a0 + b0) | (qe1 + a1 + b1) | (qe2 + a2 + b2)) >= 0);
                u32 mask = m0 | (m1 << 1) | (m2 << 2) | (m3 << 3);

                if (qx + 1 > tile_x1) mask &= ~0xAu;
                if (qy + 1 > tile_y1) mask &= ~0xCu;

                if (mask != 0) {
                    emit_quad_scalar(r, tri, state, (u16)qx, (u16)qy, mask);
                }

                qe0 += 2 * a0;
                qe1 += 2 * a1;
                qe2 += 2 * a2;
            }
            qrow_e0 += 2 * b0;
            qrow_e1 += 2 * b1;
            qrow_e2 += 2 * b2;
        }
    } else {
        i32 max_off0 = (a0 > 0 ? 7 * a0 : 0) + (b0 > 0 ? 7 * b0 : 0);
        i32 max_off1 = (a1 > 0 ? 7 * a1 : 0) + (b1 > 0 ? 7 * b1 : 0);
        i32 max_off2 = (a2 > 0 ? 7 * a2 : 0) + (b2 > 0 ? 7 * b2 : 0);
        i32 min_off0 = (a0 < 0 ? 7 * a0 : 0) + (b0 < 0 ? 7 * b0 : 0);
        i32 min_off1 = (a1 < 0 ? 7 * a1 : 0) + (b1 < 0 ? 7 * b1 : 0);
        i32 min_off2 = (a2 < 0 ? 7 * a2 : 0) + (b2 < 0 ? 7 * b2 : 0);

        i32 bx0 = min_x & ~7;
        i32 by0 = min_y & ~7;

        i32 row_e0 = a0 * bx0 + b0 * by0 + c0;
        i32 row_e1 = a1 * bx0 + b1 * by0 + c1;
        i32 row_e2 = a2 * bx0 + b2 * by0 + c2;

        for (i32 by = by0; by <= max_y; by += 8) {
            i32 blk_e0 = row_e0;
            i32 blk_e1 = row_e1;
            i32 blk_e2 = row_e2;

            for (i32 bx = bx0; bx <= max_x; bx += 8) {
                if (((blk_e0 + max_off0) | (blk_e1 + max_off1) | (blk_e2 + max_off2)) >= 0) {
                    if (bx + 7 <= tile_x1 && by + 7 <= tile_y1 &&
                        ((blk_e0 + min_off0) | (blk_e1 + min_off1) | (blk_e2 + min_off2)) >= 0) {
                        for (i32 qy = by; qy < by + 8; qy += 2) {
                            for (i32 qx = bx; qx < bx + 8; qx += 2) {
                                emit_quad_scalar(r, tri, state, (u16)qx, (u16)qy, 0xFu);
                            }
                        }
                    } else {
                        i32 qx0 = MAX(bx, qx_min);
                        i32 qy0 = MAX(by, qy_min);
                        i32 qx1 = MIN(bx + 6, max_x);
                        i32 qy1 = MIN(by + 6, max_y);

                        i32 qrow_e0 = blk_e0 + a0 * (qx0 - bx) + b0 * (qy0 - by);
                        i32 qrow_e1 = blk_e1 + a1 * (qx0 - bx) + b1 * (qy0 - by);
                        i32 qrow_e2 = blk_e2 + a2 * (qx0 - bx) + b2 * (qy0 - by);

                        for (i32 qy = qy0; qy <= qy1; qy += 2) {
                            i32 qe0 = qrow_e0;
                            i32 qe1 = qrow_e1;
                            i32 qe2 = qrow_e2;

                            for (i32 qx = qx0; qx <= qx1; qx += 2) {
                                u32 m0 = (u32)((qe0 | qe1 | qe2) >= 0);
                                u32 m1 = (u32)(((qe0 + a0) | (qe1 + a1) | (qe2 + a2)) >= 0);
                                u32 m2 = (u32)(((qe0 + b0) | (qe1 + b1) | (qe2 + b2)) >= 0);
                                u32 m3 = (u32)(((qe0 + a0 + b0) | (qe1 + a1 + b1) | (qe2 + a2 + b2)) >= 0);
                                u32 mask = m0 | (m1 << 1) | (m2 << 2) | (m3 << 3);

                                if (qx + 1 > tile_x1) mask &= ~0xAu;
                                if (qy + 1 > tile_y1) mask &= ~0xCu;

                                if (mask != 0) {
                                    emit_quad_scalar(r, tri, state, (u16)qx, (u16)qy, mask);
                                }

                                qe0 += 2 * a0;
                                qe1 += 2 * a1;
                                qe2 += 2 * a2;
                            }
                            qrow_e0 += 2 * b0;
                            qrow_e1 += 2 * b1;
                            qrow_e2 += 2 * b2;
                        }
                    }
                }
                blk_e0 += 8 * a0;
                blk_e1 += 8 * a1;
                blk_e2 += 8 * a2;
            }
            row_e0 += 8 * b0;
            row_e1 += 8 * b1;
            row_e2 += 8 * b2;
        }
    }
}

#endif /* !defined(__wasm_simd128__) */

void raster_begin(Rasterizer* r, Framebuffer* fb, Arena* frame) {
    ASSERT(r != NULL && fb != NULL && frame != NULL);
    r->fb               = fb;
    r->frame            = frame;
    r->cull_back        = FALSE;
    r->pinned_state     = NULL;
    r->tri_buf_count    = 0;
    r->batch_count      = 0;
    r->stat_tris_in     = 0;
    r->stat_tris_binned = 0;
    r->stat_quads       = 0;
    r->stat_pixels      = 0;

    r->cur_state.shade_fn      = NULL;
    r->cur_state.uniforms      = NULL;
    r->cur_state.textures      = NULL;
    r->cur_state.varying_count = 0;
    r->cur_state.blend         = BLEND_NONE;
    r->cur_state.fused         = FALSE;
    r->cur_state.depth_test    = FALSE;
    r->cur_state.depth_write   = FALSE;
    r->cur_state.depth_func    = DEPTH_LESS;
    r->cur_state.depth_late    = FALSE;

    r->tiles_x = (fb->w + RASTER_TILE_SIZE - 1u) / RASTER_TILE_SIZE;
    r->tiles_y = (fb->h + RASTER_TILE_SIZE - 1u) / RASTER_TILE_SIZE;
    r->tiles   = ARENA_PUSH_ARRAY(frame, TileBin, r->tiles_x * r->tiles_y);
    r->tri_buf = ARENA_PUSH_ARRAY(frame, RawTri, RASTER_TRI_BATCH);
}

void raster_set_cull(Rasterizer* r, b32 cull_back) {
    if (r->cull_back != cull_back) {
        raster_process_tri_buf(r);
        r->cull_back = cull_back;
    }
}

void raster_set_depth_ex(Rasterizer* r, b32 test, b32 write, DepthFunc func, b32 late_write) {
    if (r->cur_state.depth_test != test ||
        r->cur_state.depth_write != write ||
        r->cur_state.depth_func != func ||
        r->cur_state.depth_late != late_write) {
        raster_process_tri_buf(r);
        r->cur_state.depth_test  = test;
        r->cur_state.depth_write = write;
        r->cur_state.depth_func  = func;
        r->cur_state.depth_late  = late_write;
        r->pinned_state          = NULL;
    }
}

void raster_set_depth(Rasterizer* r, b32 test, b32 write, DepthFunc func) {
    raster_set_depth_ex(r, test, write, func, FALSE);
}

void raster_set_shader_ex(Rasterizer* r, ShadeFn shade_fn, const void* uniforms,
                          const Texture* textures, u32 varying_count, BlendMode blend, b32 fused) {
    ASSERT(varying_count <= SHADE_MAX_VARYINGS);
    raster_process_tri_buf(r);
    r->cur_state.shade_fn      = shade_fn;
    r->cur_state.uniforms      = uniforms;
    r->cur_state.textures      = textures;
    r->cur_state.varying_count = varying_count;
    r->cur_state.blend         = blend;
    r->cur_state.fused         = fused;
    r->pinned_state            = NULL;
}

void raster_set_shader(Rasterizer* r, ShadeFn shade_fn, const void* uniforms,
                       const Texture* textures, u32 varying_count, BlendMode blend) {
    raster_set_shader_ex(r, shade_fn, uniforms, textures, varying_count, blend, FALSE);
}

void raster_triangle(Rasterizer* r, RasterVert v0, RasterVert v1, RasterVert v2) {
    r->stat_tris_in++;
    r->tri_buf[r->tri_buf_count].v0 = v0;
    r->tri_buf[r->tri_buf_count].v1 = v1;
    r->tri_buf[r->tri_buf_count].v2 = v2;
    r->tri_buf_count++;
    if (r->tri_buf_count == RASTER_TRI_BATCH) {
        raster_process_tri_buf(r);
    }
}

void raster_set_parallel_dynamic(b32 dynamic_sched) {
    g_raster_dynamic_sched = dynamic_sched ? TRUE : FALSE;
}

b32 raster_get_parallel_dynamic(void) {
    return g_raster_dynamic_sched;
}

void raster_set_wat_kernels(RasterWatEdgeBlockFn edge_fn, RasterWatBlendAlphaFn blend_fn) {
    g_wat_edge_fn  = edge_fn;
    g_wat_blend_fn = blend_fn;
}

RasterWatEdgeBlockFn raster_get_wat_edge_fn(void) {
    return g_wat_edge_fn;
}

RasterWatBlendAlphaFn raster_get_wat_blend_fn(void) {
    return g_wat_blend_fn;
}

static void raster_process_single_tile(Rasterizer* tr, TileBin* bin,
                                       i32 tile_x0, i32 tile_y0, i32 tile_x1, i32 tile_y1) {
    const RasterShaderState* active_state = NULL;

    for (TileBinChunk* chunk = bin->head; chunk != NULL; chunk = chunk->next) {
        u32 c_count = chunk->count;
        for (u32 i = 0; i < c_count; i++) {
            const BinnedTri* tri = chunk->tris[i];
            if (active_state != tri->state) {
                if (tr->batch_count > 0 && active_state != NULL) {
                    raster_flush_full_batch(tr, active_state);
                }
                active_state = tri->state;
            }

            raster_kern_tri_scan(tr, tri, active_state,
                                 tile_x0, tile_y0, tile_x1, tile_y1);
            /* Geç derinlik yazımı (discard'lı shader): sonraki üçgen güncel derinliği görsün */
            if (active_state->depth_late && active_state->depth_write && tr->batch_count > 0) {
                raster_flush_full_batch(tr, active_state);
            }
        }
    }

    if (tr->batch_count > 0 && active_state != NULL) {
        raster_flush_full_batch(tr, active_state);
    }

    bin->head = NULL;
    bin->tail = NULL;
}

typedef struct {
    Rasterizer* r;
    Rasterizer* thread_r;
    const u32*  active_tiles;
    u32         tiles_x;
    i32         fb_w;
    i32         fb_h;
} RasterParallelCtx;

static void raster_tile_worker_fn(u32 start, u32 end, void* arg) {
    RasterParallelCtx* ctx = (RasterParallelCtx*)arg;
    u32 tid = thread_id();
    if (tid >= THREAD_MAX_COUNT) {
        tid = 0u;
    }
    Rasterizer* tr = &ctx->thread_r[tid];
    u32 tiles_x    = ctx->tiles_x;
    i32 fb_w       = ctx->fb_w;
    i32 fb_h       = ctx->fb_h;

    if (tid != 0u) {
        PROF_BEGIN("raster");
    }

    for (u32 k = start; k < end; k++) {
        u32 tile_idx = ctx->active_tiles[k];
        u32 ty       = tile_idx / tiles_x;
        u32 tx       = tile_idx % tiles_x;
        i32 tile_x0  = (i32)(tx * RASTER_TILE_SIZE);
        i32 tile_y0  = (i32)(ty * RASTER_TILE_SIZE);
        i32 tile_x1  = MIN(tile_x0 + RASTER_TILE_SIZE - 1, fb_w - 1);
        i32 tile_y1  = MIN(tile_y0 + RASTER_TILE_SIZE - 1, fb_h - 1);

        raster_process_single_tile(tr, &ctx->r->tiles[tile_idx],
                                   tile_x0, tile_y0, tile_x1, tile_y1);
    }

    if (tid != 0u) {
        PROF_END();
    }
}

static _Alignas(16) Rasterizer g_thread_rasters[THREAD_MAX_COUNT];

void raster_flush(Rasterizer* r) {
    raster_process_tri_buf(r);

    u32 tiles_x     = r->tiles_x;
    u32 tiles_y     = r->tiles_y;
    u32 total_tiles = tiles_x * tiles_y;
    i32 fb_w        = (i32)r->fb->w;
    i32 fb_h        = (i32)r->fb->h;

    /* Dolu karoları topla */
    u32* active_tiles = ARENA_PUSH_ARRAY(r->frame, u32, total_tiles);
    u32  active_count = 0;
    for (u32 i = 0; i < total_tiles; i++) {
        if (r->tiles[i].head != NULL) {
            active_tiles[active_count++] = i;
        }
    }

    if (active_count == 0u) {
        return;
    }

    u32 nthreads = thread_count();

    PROF_BEGIN("raster");
    if (nthreads <= 1u || active_count <= 1u) {
        for (u32 k = 0; k < active_count; k++) {
            u32 tile_idx = active_tiles[k];
            u32 ty       = tile_idx / tiles_x;
            u32 tx       = tile_idx % tiles_x;
            i32 tile_x0  = (i32)(tx * RASTER_TILE_SIZE);
            i32 tile_y0  = (i32)(ty * RASTER_TILE_SIZE);
            i32 tile_x1  = MIN(tile_x0 + RASTER_TILE_SIZE - 1, fb_w - 1);
            i32 tile_y1  = MIN(tile_y0 + RASTER_TILE_SIZE - 1, fb_h - 1);

            raster_process_single_tile(r, &r->tiles[tile_idx],
                                       tile_x0, tile_y0, tile_x1, tile_y1);
        }
    } else {
        usize mark = arena_mark(r->frame);
        /* Thread başına bağımsız Rasterizer durumu (ShadeBatch, quads, varyings, out_rgba, quad_z)
         * ve frame arena'dan thread başına scratch dilimi */
        Rasterizer* thread_r = g_thread_rasters;
        Arena* thread_arenas = ARENA_PUSH_ARRAY(r->frame, Arena, THREAD_MAX_COUNT);

        usize avail_after = r->frame->cap - r->frame->used;
        usize slice_bytes = (avail_after >= KB(128)) ? KB(4) : 0u;

        for (u32 t = 0; t < THREAD_MAX_COUNT; t++) {
            if (slice_bytes > 0u) {
                thread_arenas[t] = arena_sub(r->frame, slice_bytes);
            } else {
                arena_init(&thread_arenas[t], NULL, 0u);
            }
            thread_r[t].fb               = r->fb;
            thread_r[t].frame            = &thread_arenas[t];
            thread_r[t].cull_back        = r->cull_back;
            thread_r[t].pinned_state     = NULL;
            thread_r[t].tiles_x          = tiles_x;
            thread_r[t].tiles_y          = tiles_y;
            thread_r[t].tiles            = r->tiles;
            thread_r[t].tri_buf_count    = 0;
            thread_r[t].tri_buf          = NULL;
            thread_r[t].batch_count      = 0;
            thread_r[t].stat_tris_in     = 0;
            thread_r[t].stat_tris_binned = 0;
            thread_r[t].stat_quads       = 0;
            thread_r[t].stat_pixels      = 0;
        }

        RasterParallelCtx pctx;
        pctx.r            = r;
        pctx.thread_r     = thread_r;
        pctx.active_tiles = active_tiles;
        pctx.tiles_x      = tiles_x;
        pctx.fb_w         = fb_w;
        pctx.fb_h         = fb_h;

        /* Dinamik yük dengeleme: atomik sayaçla 1'er karo çekme (grain = 1).
         * Statik bölme karşılaştırması için: grain = ceil(active_count / nthreads). */
        u32 grain = g_raster_dynamic_sched
                        ? 1u
                        : ((active_count + nthreads - 1u) / nthreads);
        if (grain == 0u) grain = 1u;

        job_parallel_for(active_count, grain, raster_tile_worker_fn, &pctx);

        for (u32 t = 0; t < THREAD_MAX_COUNT; t++) {
            r->stat_quads  += thread_r[t].stat_quads;
            r->stat_pixels += thread_r[t].stat_pixels;
        }

        arena_pop_to(r->frame, mark);
    }
    PROF_END();
}
