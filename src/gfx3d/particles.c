/* gfx3d/particles.c — Sabit kapasiteli deterministik parçacık havuzu, yayıcılar ve güncelleme döngüsü.
 * Ölen parçacıklar O(1) swap-remove ile dizinin sonundaki aktif elemanla yer değiştirilerek silinir.
 * Renk eğrisi SWAR (2 kanal paralel sabit nokta) ile, boyut eğrisi doğrusal interpolasyonla hesaplanır.
 */
#include "gfx3d/particles.h"

static inline b32 f32_is_finite(f32 x) {
    u32 u;
    __builtin_memcpy(&u, &x, sizeof(u));
    return (u & 0x7F800000u) != 0x7F800000u;
}

#define PARTICLE_MAX_COORD 1.0e12f
#define PARTICLE_MAX_SPEED 1.0e6f
#define PARTICLE_MAX_DT    60.0f

static inline b32 f32_valid_vec3(Vec3 v, f32 limit) {
    return f32_is_finite(v.x) && f32_is_finite(v.y) && f32_is_finite(v.z) &&
           m_abs(v.x) <= limit && m_abs(v.y) <= limit && m_abs(v.z) <= limit;
}

/* İki RGBA8 rengi t in [0, 1] parametresiyle SWAR kullanarak doğrusal karıştırır */
static inline u32 lerp_rgba8(u32 c0, u32 c1, f32 t) {
    if (c0 == c1) return c0;
    f32 tc = m_clamp(t, 0.0f, 1.0f);
    u32 it = (u32)(tc * 256.0f + 0.5f);
    if (it > 256u) it = 256u;
    u32 inv = 256u - it;
    u32 rb = (((c0 & 0x00FF00FFu) * inv + (c1 & 0x00FF00FFu) * it) >> 8) & 0x00FF00FFu;
    u32 ga = (((c0 >> 8) & 0x00FF00FFu) * inv + ((c1 >> 8) & 0x00FF00FFu) * it) & 0xFF00FF00u;
    return rb | ga;
}

u32 particles_pack_color(Vec4 c) {
    f32 r = f32_is_finite(c.x) ? m_clamp(c.x, 0.0f, 1.0f) : 0.0f;
    f32 g = f32_is_finite(c.y) ? m_clamp(c.y, 0.0f, 1.0f) : 0.0f;
    f32 b = f32_is_finite(c.z) ? m_clamp(c.z, 0.0f, 1.0f) : 0.0f;
    f32 a = f32_is_finite(c.w) ? m_clamp(c.w, 0.0f, 1.0f) : 0.0f;
    u32 ru = (u32)(r * 255.0f + 0.5f);
    u32 gu = (u32)(g * 255.0f + 0.5f);
    u32 bu = (u32)(b * 255.0f + 0.5f);
    u32 au = (u32)(a * 255.0f + 0.5f);
    return FB_RGBA(ru, gu, bu, au);
}

ParticleEmitter particles_default_emitter(void) {
    ParticleEmitter em;
    __builtin_memset(&em, 0, sizeof(em));
    em.shape       = EMIT_SHAPE_POINT;
    em.pos         = vec3(0.0f, 0.0f, 0.0f);
    em.radius      = 0.0f;
    em.vel_min     = vec3(-1.0f, 1.0f, -1.0f);
    em.vel_max     = vec3(1.0f, 3.0f, 1.0f);
    em.speed_min   = 0.0f;
    em.speed_max   = 0.0f;
    em.life_min    = 0.5f;
    em.life_max    = 1.5f;
    em.gravity     = vec3(0.0f, -9.8f, 0.0f);
    em.drag        = 0.5f;
    em.size_start  = 0.25f;
    em.size_end    = 0.05f;
    em.color_start = vec4(1.0f, 0.9f, 0.4f, 1.0f);
    em.color_end   = vec4(1.0f, 0.2f, 0.0f, 0.0f);
    return em;
}

b32 particles_init(ParticleSystem* ps, Arena* a, u32 capacity, u64 seed) {
    if (!ps) return FALSE;
    __builtin_memset(ps, 0, sizeof(*ps));
    if (!a || !a->base || a->used > a->cap ||
        capacity == 0u || capacity > PARTICLES_MAX_CAPACITY) {
        return FALSE;
    }

    usize req_size  = sizeof(Particle) * (usize)capacity;
    usize req_align = _Alignof(Particle);
    uptr  cur_addr  = (uptr)a->base + (uptr)a->used;
    uptr  aligned   = ALIGN_UP(cur_addr, (uptr)req_align);
    usize pad       = (usize)(aligned - cur_addr);
    if (pad > a->cap - a->used || req_size > a->cap - a->used - pad) {
        return FALSE;
    }

    ps->items       = ARENA_PUSH_ARRAY(a, Particle, capacity);
    ps->capacity    = capacity;
    ps->count       = 0u;
    ps->initialized = TRUE;
    rng_seed(&ps->rng, seed);
    return TRUE;
}

void particles_reset(ParticleSystem* ps, u64 seed) {
    if (!ps || !ps->initialized) return;
    ps->count          = 0u;
    ps->overflow_count = 0u;
    ps->skipped_count  = 0u;
    ps->expired_last   = 0u;
    ps->expired_total  = 0u;
    ps->emitted_total  = 0u;
    rng_seed(&ps->rng, seed);
}

static inline f32 rng_lerp(Rng* rng, f32 a, f32 b) {
    f32 lo = m_min(a, b);
    f32 hi = m_max(a, b);
    return lo + (hi - lo) * rng_f32(rng);
}

/* Birim küre yüzeyinde düzgün dağılımlı rastgele yön vektörü (Archimedes silindir teoremi) */
static inline Vec3 rng_unit_sphere(Rng* rng) {
    f32 z   = rng_f32(rng) * 2.0f - 1.0f;
    f32 phi = rng_f32(rng) * M_TAU;
    f32 rxy = m_sqrt(m_max(0.0f, 1.0f - z * z));
    return vec3(rxy * m_cos(phi), z, rxy * m_sin(phi));
}

static inline u32 add_sat_u32(u32 a, u32 b) {
    u64 s = (u64)a + (u64)b;
    return (s > (u64)U32_MAX) ? U32_MAX : (u32)s;
}

static b32 emitter_is_valid(const ParticleEmitter* em) {
    if (!em) return FALSE;
    if (!f32_valid_vec3(em->pos, PARTICLE_MAX_COORD)) return FALSE;
    if (!f32_valid_vec3(em->vel_min, PARTICLE_MAX_SPEED)) return FALSE;
    if (!f32_valid_vec3(em->vel_max, PARTICLE_MAX_SPEED)) return FALSE;
    if (!f32_valid_vec3(em->gravity, PARTICLE_MAX_SPEED)) return FALSE;
    if (!f32_is_finite(em->radius) || em->radius < 0.0f || em->radius > PARTICLE_MAX_COORD) return FALSE;
    if (!f32_is_finite(em->speed_min) || !f32_is_finite(em->speed_max)) return FALSE;
    if (m_abs(em->speed_min) > PARTICLE_MAX_SPEED || m_abs(em->speed_max) > PARTICLE_MAX_SPEED) return FALSE;
    if (!f32_is_finite(em->life_min) || !f32_is_finite(em->life_max)) return FALSE;
    if (em->life_min <= 0.0f || em->life_max <= 0.0f) return FALSE;
    if (em->life_min > PARTICLE_MAX_SPEED || em->life_max > PARTICLE_MAX_SPEED) return FALSE;
    if (!f32_is_finite(em->drag) || em->drag < 0.0f || em->drag > 1.0e6f) return FALSE;
    if (!f32_is_finite(em->size_start) || !f32_is_finite(em->size_end)) return FALSE;
    if (em->size_start < 0.0f || em->size_end < 0.0f) return FALSE;
    if (em->size_start <= 0.0f && em->size_end <= 0.0f) return FALSE;
    if (em->size_start > PARTICLE_MAX_COORD || em->size_end > PARTICLE_MAX_COORD) return FALSE;
    if (!f32_is_finite(em->rot_min) || !f32_is_finite(em->rot_max)) return FALSE;
    if (m_abs(em->rot_min) > PARTICLE_MAX_SPEED || m_abs(em->rot_max) > PARTICLE_MAX_SPEED) return FALSE;
    if (!f32_is_finite(em->rot_vel_min) || !f32_is_finite(em->rot_vel_max)) return FALSE;
    if (m_abs(em->rot_vel_min) > PARTICLE_MAX_SPEED || m_abs(em->rot_vel_max) > PARTICLE_MAX_SPEED) return FALSE;
    return TRUE;
}

u32 particles_emit_burst(ParticleSystem* ps, const ParticleEmitter* em, u32 count) {
    if (!ps || !ps->initialized || count == 0u) return 0u;
    if (!emitter_is_valid(em)) {
        ps->skipped_count = add_sat_u32(ps->skipped_count, count);
        return 0u;
    }

    f32 life_lo = m_max(1.0e-5f, m_min(em->life_min, em->life_max));
    f32 life_hi = m_max(life_lo, m_max(em->life_min, em->life_max));
    u32 c0_rgba = particles_pack_color(em->color_start);
    u32 c1_rgba = particles_pack_color(em->color_end);
    u16 a_cols  = (em->atlas_cols > 0u && em->atlas_cols <= 4096u) ? (u16)em->atlas_cols : 1u;
    u16 a_rows  = (em->atlas_rows > 0u && em->atlas_rows <= 4096u) ? (u16)em->atlas_rows : 1u;
    u32 a_total = (u32)a_cols * (u32)a_rows;
    u16 a_frame = (a_total > 0u) ? (u16)(em->atlas_frame % a_total) : 0u;

    b32 use_sphere = (em->shape == EMIT_SHAPE_SPHERE && em->radius > 0.0f);
    b32 use_radial = (em->speed_min != 0.0f || em->speed_max != 0.0f);
    b32 use_rot    = (em->rot_min != 0.0f || em->rot_max != 0.0f);
    b32 use_rotvel = (em->rot_vel_min != 0.0f || em->rot_vel_max != 0.0f);

    u32 spawned = 0u;
    Rng* rng = &ps->rng;

    for (u32 i = 0; i < count; i++) {
        if (ps->count >= ps->capacity) {
            ps->overflow_count = add_sat_u32(ps->overflow_count, count - i);
            break;
        }

        Vec3 dir = vec3(0.0f, 1.0f, 0.0f);
        if (use_sphere || use_radial) {
            dir = rng_unit_sphere(rng);
        }

        Vec3 pos = em->pos;
        if (use_sphere) {
            f32 r = em->radius * rng_f32(rng);
            pos = vec3_add(pos, vec3_scale(dir, r));
        }

        Vec3 vel = vec3(
            rng_lerp(rng, em->vel_min.x, em->vel_max.x),
            rng_lerp(rng, em->vel_min.y, em->vel_max.y),
            rng_lerp(rng, em->vel_min.z, em->vel_max.z)
        );
        if (use_radial) {
            f32 sp = rng_lerp(rng, em->speed_min, em->speed_max);
            vel = vec3_add(vel, vec3_scale(dir, sp));
        }

        f32 life = rng_lerp(rng, life_lo, life_hi);
        f32 rot  = use_rot    ? rng_lerp(rng, em->rot_min, em->rot_max) : 0.0f;
        f32 rvel = use_rotvel ? rng_lerp(rng, em->rot_vel_min, em->rot_vel_max) : 0.0f;

        Particle* p = &ps->items[ps->count++];
        p->pos           = pos;
        p->vel           = vel;
        p->gravity       = em->gravity;
        p->drag          = em->drag;
        p->age           = 0.0f;
        p->life          = life;
        p->inv_life      = 1.0f / life;
        p->size_start    = em->size_start;
        p->size_end      = em->size_end;
        p->size          = (em->size_start > 0.0f) ? em->size_start : em->size_end;
        p->rot           = rot;
        p->rot_vel       = rvel;
        p->color0_rgba   = c0_rgba;
        p->color1_rgba   = c1_rgba;
        p->color         = c0_rgba;
        p->atlas_cols    = a_cols;
        p->atlas_rows    = a_rows;
        p->atlas_frame   = em->animate_atlas ? 0u : a_frame;
        p->animate_atlas = (u16)(em->animate_atlas ? 1u : 0u);
        spawned++;
    }

    ps->emitted_total += spawned;
    return spawned;
}

u32 particles_update(ParticleSystem* ps, f32 dt) {
    if (!ps || !ps->initialized) return 0u;
    ps->expired_last = 0u;

    if (!f32_is_finite(dt) || dt <= 0.0f) {
        return 0u;
    }
    if (dt > PARTICLE_MAX_DT) {
        dt = PARTICLE_MAX_DT;
    }

    u32 expired = 0u;
    u32 i = 0u;
    Particle* items = ps->items;

    while (i < ps->count) {
        Particle* p = &items[i];
        f32 age = p->age + dt;

        /* Ömrü dolan parçacığı O(1) swap-remove ile sil */
        if (age >= p->life || !(p->life > 0.0f)) {
            u32 last = --ps->count;
            if (i != last) {
                items[i] = items[last];
            }
            expired++;
            continue;
        }

        p->age = age;
        f32 t = age * p->inv_life;
        if (t < 0.0f) t = 0.0f;
        if (t > 1.0f) t = 1.0f;

        /* Hız: yerçekimi + örtük Euler sürtünme sönümü */
        Vec3 v = vec3_add(p->vel, vec3_scale(p->gravity, dt));
        if (p->drag > 0.0f) {
            f32 damp = 1.0f / (1.0f + p->drag * dt);
            v = vec3_scale(v, damp);
        }
        p->vel = v;
        p->pos = vec3_add(p->pos, vec3_scale(v, dt));
        p->rot += p->rot_vel * dt;

        /* Boyut ve renk eğrisi (başlangıç -> bitiş) */
        p->size  = p->size_start + (p->size_end - p->size_start) * t;
        p->color = lerp_rgba8(p->color0_rgba, p->color1_rgba, t);

        if (p->animate_atlas) {
            u32 total = (u32)p->atlas_cols * (u32)p->atlas_rows;
            if (total > 1u) {
                u32 f = (u32)(t * (f32)total);
                if (f >= total) f = total - 1u;
                p->atlas_frame = (u16)f;
            }
        }

        i++;
    }

    ps->expired_last   = expired;
    ps->expired_total += expired;
    return expired;
}

u32 particles_to_billboards(const ParticleSystem* ps, BillboardBatch* b) {
    if (!ps || !ps->initialized || !b || !b->initialized) return 0u;
    u32 n = ps->count;
    u32 pushed = 0u;
    const Particle* items = ps->items;

    for (u32 i = 0; i < n; i++) {
        const Particle* p = &items[i];
        BillboardUv uv;
        if (p->atlas_cols > 1u || p->atlas_rows > 1u) {
            uv = billboard_uv_atlas(p->atlas_cols, p->atlas_rows, p->atlas_frame);
        } else {
            uv = billboard_uv_full();
        }
        if (billboard_push_ex(b, p->pos, vec2(p->size, p->size), p->rot, p->color, uv)) {
            pushed++;
        }
    }
    return pushed;
}

Mesh* particles_build_billboards(const ParticleSystem* ps, BillboardBatch* b,
                                 Mat4 view, b32 sort_back_to_front) {
    if (!ps || !ps->initialized || !b || !b->initialized) return NULL;
    billboard_begin(b, view);
    particles_to_billboards(ps, b);
    if (sort_back_to_front) {
        billboard_sort(b);
    }
    return billboard_end(b);
}
