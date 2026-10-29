/* state/state_world.c — İşaretçisiz SoA ECS deposu ve fizik gövde dilimi gerçeklemesi.
 * Tüm işlemler yalnızca bölge taban adresi (void* base) ve StateRef/StateSlice ofsetleriyle
 * yürütülür; silinen yoğun dizi elemanları sıfırlanır ve kayan nokta değerleri (-0.0f -> +0.0f)
 * kanonikleştirilir, böylece farklı taban adreslerde ham bellek özeti bit-bit aynı kalır.
 */
#include "state/state_world.h"
#include "sim/sim.h"

static u32 state_world_rng_u32(StateWorld* w) {
    u64 old = w->rng_state;
    w->rng_state = old * 6364136223846793005ULL + (w->rng_inc | 1ULL);
    u32 xorshifted = (u32)(((old >> 18u) ^ old) >> 27u);
    u32 rot = (u32)(old >> 59u);
    return (xorshifted >> rot) | (xorshifted << ((0u - rot) & 31u));
}

static f32 state_world_rng_f32(StateWorld* w, f32 lo, f32 hi) {
    u32 r = state_world_rng_u32(w) >> 8u;
    f32 t = (f32)r * (1.0f / 16777216.0f);
    return state_canon_f32(lo + (hi - lo) * t);
}

StateRef(StateWorld) state_world_init(void* base, u32 cap, u32 max_entities,
                                      u32 max_bodies, u32 max_contacts, u32 seed) {
    ASSERT(base != NULL);
    ASSERT(max_entities > 0u);
    ASSERT(max_bodies > 0u);
    if (max_contacts == 0u) max_contacts = max_bodies * 2u;

    StateRegionHeader* hdr = state_region_init(base, cap, STATE_WORLD_ABI_HASH);
    RegionArena* ra = &hdr->arena;

    StateRef(StateWorld) world_ref = STATE_ALLOC(ra, base, StateWorld);
    state_region_set_root(base, world_ref.off);

    StateWorld* w = STATE_DEREF(base, StateWorld, world_ref);
    w->abi_version     = STATE_REGION_VERSION;
    w->step_count      = 0u;
    w->rng_state       = 0u;
    w->rng_inc         = ((u64)seed << 1u) | 1ULL;
    (void)state_world_rng_u32(w);
    w->rng_state      += (u64)seed * 0x9E3779B97F4A7C15ULL;
    (void)state_world_rng_u32(w);
    w->spawned_total   = 0u;
    w->destroyed_total = 0u;
    w->checksum_acc    = SIM_HASH_SEED;

    /* 1) ECS SoA deposu kurulumu */
    StateEcsSoA* ecs = &w->ecs;
    state_slots_init(&ecs->slots, ra, base, max_entities);
    ecs->dense_count   = 0u;
    ecs->max_entities  = max_entities;
    ecs->sparse        = STATE_ALLOC_SLICE(ra, base, u32,         max_entities);
    ecs->dense_entity  = STATE_ALLOC_SLICE(ra, base, StateHandle, max_entities);
    ecs->pos_x         = STATE_ALLOC_SLICE(ra, base, f32,         max_entities);
    ecs->pos_y         = STATE_ALLOC_SLICE(ra, base, f32,         max_entities);
    ecs->pos_z         = STATE_ALLOC_SLICE(ra, base, f32,         max_entities);
    ecs->vel_x         = STATE_ALLOC_SLICE(ra, base, f32,         max_entities);
    ecs->vel_y         = STATE_ALLOC_SLICE(ra, base, f32,         max_entities);
    ecs->vel_z         = STATE_ALLOC_SLICE(ra, base, f32,         max_entities);
    ecs->hp            = STATE_ALLOC_SLICE(ra, base, i32,         max_entities);
    ecs->comp_mask     = STATE_ALLOC_SLICE(ra, base, u32,         max_entities);
    ecs->phys_body     = STATE_ALLOC_SLICE(ra, base, StateHandle, max_entities);
    ecs->target_entity = STATE_ALLOC_SLICE(ra, base, StateHandle, max_entities);

    u32* ecs_sparse = STATE_SLICE_PTR(base, u32, ecs->sparse);
    for (u32 i = 0u; i < max_entities; i++) {
        ecs_sparse[i] = STATE_SPARSE_NIL;
    }

    /* 2) Fizik gövde dilimi kurulumu */
    StatePhysSlice* phys = &w->phys;
    state_slots_init(&phys->slots, ra, base, max_bodies);
    phys->bodies          = STATE_ALLOC_SLICE(ra, base, StatePhysBody, max_bodies);
    phys->slot_to_dense   = STATE_ALLOC_SLICE(ra, base, u32,           max_bodies);
    phys->dense_to_handle = STATE_ALLOC_SLICE(ra, base, StateHandle,   max_bodies);
    phys->contacts        = STATE_ALLOC_SLICE(ra, base, StateContact,  max_contacts);
    phys->body_count      = 0u;
    phys->contact_count   = 0u;
    phys->gravity         = vec3(0.0f, -9.81f, 0.0f);
    phys->ground_y        = 0.0f;
    phys->bounds_half     = vec3(12.0f, 16.0f, 12.0f);
    phys->bounce_count    = 0u;

    u32* phys_sparse = STATE_SLICE_PTR(base, u32, phys->slot_to_dense);
    for (u32 i = 0u; i < max_bodies; i++) {
        phys_sparse[i] = STATE_SPARSE_NIL;
    }

    return world_ref;
}

StateWorld* state_world_get(void* base) {
    StateOffset root = state_region_root(base);
    ASSERT(root != STATE_NULL_OFFSET);
    return STATE_PTR(base, StateWorld, root);
}

const StateWorld* state_world_get_const(const void* base) {
    StateOffset root = state_region_root(base);
    ASSERT(root != STATE_NULL_OFFSET);
    return STATE_PTR_CONST(base, StateWorld, root);
}

/* ----------------------------------------------------------------------------
 * Fizik Gövde Dilimi İşlemleri
 * ---------------------------------------------------------------------------- */

StateHandleOf(PhysBody) state_phys_add_body(void* base, Vec3 pos, Vec3 vel, f32 radius,
                                            f32 mass, f32 restitution,
                                            StateHandleOf(Entity) owner) {
    StateWorld* w = state_world_get(base);
    StatePhysSlice* phys = &w->phys;

    StateHandle raw = state_slots_alloc(&phys->slots, base);
    if (raw == STATE_HANDLE_NULL) {
        return STATE_HANDLE_TYPED_NULL(PhysBody);
    }

    u32 slot_idx  = state_handle_index(raw);
    u32 dense_idx = phys->body_count++;

    u32*           s2d    = STATE_SLICE_PTR(base, u32,           phys->slot_to_dense);
    StateHandle*   d2h    = STATE_SLICE_PTR(base, StateHandle,   phys->dense_to_handle);
    StatePhysBody* bodies = STATE_SLICE_PTR(base, StatePhysBody, phys->bodies);

    s2d[slot_idx]  = dense_idx;
    d2h[dense_idx] = raw;

    StatePhysBody* b = &bodies[dense_idx];
    __builtin_memset(b, 0, sizeof(*b));
    b->pos          = state_canon_vec3(pos);
    b->radius       = state_canon_f32(radius > 0.05f ? radius : 0.05f);
    b->vel          = state_canon_vec3(vel);
    b->inv_mass     = state_canon_f32(mass > 0.0f ? (1.0f / mass) : 0.0f);
    b->restitution  = state_canon_f32(m_clamp(restitution, 0.0f, 1.0f));
    b->damping      = state_canon_f32(0.995f);
    b->owner_entity = owner;
    b->flags        = STATE_BODY_ACTIVE;

    return STATE_HANDLE_TYPED_MAKE(PhysBody, raw);
}

b32 state_phys_body_valid(const void* base, StateHandleOf(PhysBody) h) {
    const StateWorld* w = state_world_get_const(base);
    return state_slots_valid(&w->phys.slots, base, h.id);
}

StatePhysBody* state_phys_get_body(void* base, StateHandleOf(PhysBody) h) {
    if (!state_phys_body_valid(base, h)) return NULL;
    StateWorld* w = state_world_get(base);
    u32 slot_idx = state_handle_index(h.id);
    const u32* s2d = STATE_SLICE_PTR_CONST(base, u32, w->phys.slot_to_dense);
    u32 dense = s2d[slot_idx];
    if (dense >= w->phys.body_count) return NULL;
    return STATE_AT(base, StatePhysBody, w->phys.bodies, dense);
}

const StatePhysBody* state_phys_get_body_const(const void* base, StateHandleOf(PhysBody) h) {
    if (!state_phys_body_valid(base, h)) return NULL;
    const StateWorld* w = state_world_get_const(base);
    u32 slot_idx = state_handle_index(h.id);
    const u32* s2d = STATE_SLICE_PTR_CONST(base, u32, w->phys.slot_to_dense);
    u32 dense = s2d[slot_idx];
    if (dense >= w->phys.body_count) return NULL;
    return STATE_AT_CONST(base, StatePhysBody, w->phys.bodies, dense);
}

b32 state_phys_remove_body(void* base, StateHandleOf(PhysBody) h) {
    if (!state_phys_body_valid(base, h)) return FALSE;
    StateWorld* w = state_world_get(base);
    StatePhysSlice* phys = &w->phys;

    u32*           s2d    = STATE_SLICE_PTR(base, u32,           phys->slot_to_dense);
    StateHandle*   d2h    = STATE_SLICE_PTR(base, StateHandle,   phys->dense_to_handle);
    StatePhysBody* bodies = STATE_SLICE_PTR(base, StatePhysBody, phys->bodies);

    u32 slot_idx  = state_handle_index(h.id);
    u32 dense_idx = s2d[slot_idx];
    u32 last_idx  = phys->body_count - 1u;

    if (dense_idx != last_idx) {
        StateHandle moved_h = d2h[last_idx];
        bodies[dense_idx] = bodies[last_idx];
        d2h[dense_idx]    = moved_h;
        s2d[state_handle_index(moved_h)] = dense_idx;
    }

    /* Silinen kuyruk elemanını sıfırla (deterministik ham bayt özeti için) */
    __builtin_memset(&bodies[last_idx], 0, sizeof(StatePhysBody));
    d2h[last_idx] = STATE_HANDLE_NULL;
    s2d[slot_idx] = STATE_SPARSE_NIL;
    phys->body_count--;

    return state_slots_free(&phys->slots, base, h.id);
}

void state_phys_step(void* base, f32 dt) {
    StateWorld* w = state_world_get(base);
    StatePhysSlice* phys = &w->phys;
    u32 count = phys->body_count;

    StatePhysBody* bodies   = STATE_SLICE_PTR(base, StatePhysBody, phys->bodies);
    StateHandle*   d2h      = STATE_SLICE_PTR(base, StateHandle,   phys->dense_to_handle);
    StateContact*  contacts = STATE_SLICE_PTR(base, StateContact,  phys->contacts);

    /* Önceki adımdan kalan temas kayıtlarını sıfırla */
    if (phys->contact_count > 0u) {
        __builtin_memset(contacts, 0, sizeof(StateContact) * (usize)phys->contact_count);
        phys->contact_count = 0u;
    }

    /* 1) Hız ve konum entegrasyonu + kapalı kutu / zemin sınır çarpışmaları */
    Vec3 g    = phys->gravity;
    f32  gy   = phys->ground_y;
    Vec3 half = phys->bounds_half;

    for (u32 i = 0u; i < count; i++) {
        StatePhysBody* b = &bodies[i];
        if (b->inv_mass <= 0.0f) continue;

        Vec3 v = vec3_add(b->vel, vec3_scale(g, dt));
        v = vec3_scale(v, b->damping);
        Vec3 p = vec3_add(b->pos, vec3_scale(v, dt));
        f32  r = b->radius;

        b->flags &= ~STATE_BODY_ON_GROUND;

        /* Alt zemin düzlemi */
        if (p.y - r < gy) {
            p.y = gy + r;
            if (v.y < 0.0f) {
                v.y = -v.y * b->restitution;
                if (v.y < 0.25f) v.y = 0.0f;
                phys->bounce_count++;
            }
            b->flags |= STATE_BODY_ON_GROUND;
        }
        /* Üst tavan */
        if (p.y + r > half.y) {
            p.y = half.y - r;
            if (v.y > 0.0f) {
                v.y = -v.y * b->restitution;
                phys->bounce_count++;
            }
        }
        /* X duvarları */
        if (p.x - r < -half.x) {
            p.x = -half.x + r;
            if (v.x < 0.0f) { v.x = -v.x * b->restitution; phys->bounce_count++; }
        } else if (p.x + r > half.x) {
            p.x = half.x - r;
            if (v.x > 0.0f) { v.x = -v.x * b->restitution; phys->bounce_count++; }
        }
        /* Z duvarları */
        if (p.z - r < -half.z) {
            p.z = -half.z + r;
            if (v.z < 0.0f) { v.z = -v.z * b->restitution; phys->bounce_count++; }
        } else if (p.z + r > half.z) {
            p.z = half.z - r;
            if (v.z > 0.0f) { v.z = -v.z * b->restitution; phys->bounce_count++; }
        }

        b->pos = state_canon_vec3(p);
        b->vel = state_canon_vec3(v);
    }

    /* 2) Kanonik (i < j) sırada küre-küre çarpışma tespiti ve impuls çözümü */
    u32 max_contacts = phys->contacts.count;
    for (u32 i = 0u; i < count; i++) {
        for (u32 j = i + 1u; j < count; j++) {
            StatePhysBody* a = &bodies[i];
            StatePhysBody* b = &bodies[j];
            f32 inv_m_sum = a->inv_mass + b->inv_mass;
            if (inv_m_sum <= 0.0f) continue;

            Vec3 delta = vec3_sub(b->pos, a->pos);
            f32 dist2  = vec3_dot(delta, delta);
            f32 r_sum  = a->radius + b->radius;
            if (dist2 >= r_sum * r_sum) continue;

            f32 dist = m_sqrt(dist2);
            Vec3 n = (dist > 1e-5f) ? vec3_scale(delta, 1.0f / dist) : vec3(1.0f, 0.0f, 0.0f);
            f32 pen = r_sum - dist;

            /* Konumsal ayrıştırma (Baumgarte / projeksiyon) */
            f32 corr_mag = (pen / inv_m_sum) * 0.8f;
            Vec3 corr = vec3_scale(n, corr_mag);
            a->pos = state_canon_vec3(vec3_sub(a->pos, vec3_scale(corr, a->inv_mass)));
            b->pos = state_canon_vec3(vec3_add(b->pos, vec3_scale(corr, b->inv_mass)));

            /* Bağıl hız ve normal impuls */
            Vec3 rv = vec3_sub(b->vel, a->vel);
            f32 vel_along_n = vec3_dot(rv, n);
            f32 j_imp = 0.0f;
            if (vel_along_n < 0.0f) {
                f32 e = m_min(a->restitution, b->restitution);
                j_imp = -(1.0f + e) * vel_along_n / inv_m_sum;
                Vec3 imp = vec3_scale(n, j_imp);
                a->vel = state_canon_vec3(vec3_sub(a->vel, vec3_scale(imp, a->inv_mass)));
                b->vel = state_canon_vec3(vec3_add(b->vel, vec3_scale(imp, b->inv_mass)));
                phys->bounce_count++;
            }

            if (phys->contact_count < max_contacts) {
                StateContact* c = &contacts[phys->contact_count++];
                c->body_a      = STATE_HANDLE_TYPED_MAKE(PhysBody, d2h[i]);
                c->body_b      = STATE_HANDLE_TYPED_MAKE(PhysBody, d2h[j]);
                c->normal      = state_canon_vec3(n);
                c->penetration = state_canon_f32(pen);
                c->impulse     = state_canon_f32(j_imp);
                c->tick        = w->step_count;
            }
        }
    }
}

/* ----------------------------------------------------------------------------
 * ECS SoA Deposu İşlemleri
 * ---------------------------------------------------------------------------- */

StateHandleOf(Entity) state_ecs_spawn(void* base, Vec3 pos, Vec3 vel, i32 hp, u32 mask) {
    StateWorld* w = state_world_get(base);
    StateEcsSoA* ecs = &w->ecs;

    StateHandle raw = state_slots_alloc(&ecs->slots, base);
    if (raw == STATE_HANDLE_NULL) {
        return STATE_HANDLE_TYPED_NULL(Entity);
    }

    u32 slot_idx  = state_handle_index(raw);
    u32 dense_idx = ecs->dense_count++;

    u32*         sparse = STATE_SLICE_PTR(base, u32,         ecs->sparse);
    StateHandle* d_ent  = STATE_SLICE_PTR(base, StateHandle, ecs->dense_entity);
    f32*         px     = STATE_SLICE_PTR(base, f32,         ecs->pos_x);
    f32*         py     = STATE_SLICE_PTR(base, f32,         ecs->pos_y);
    f32*         pz     = STATE_SLICE_PTR(base, f32,         ecs->pos_z);
    f32*         vx     = STATE_SLICE_PTR(base, f32,         ecs->vel_x);
    f32*         vy     = STATE_SLICE_PTR(base, f32,         ecs->vel_y);
    f32*         vz     = STATE_SLICE_PTR(base, f32,         ecs->vel_z);
    i32*         hps    = STATE_SLICE_PTR(base, i32,         ecs->hp);
    u32*         masks  = STATE_SLICE_PTR(base, u32,         ecs->comp_mask);
    StateHandle* bodies = STATE_SLICE_PTR(base, StateHandle, ecs->phys_body);
    StateHandle* tgts   = STATE_SLICE_PTR(base, StateHandle, ecs->target_entity);

    sparse[slot_idx]  = dense_idx;
    d_ent[dense_idx]  = raw;
    px[dense_idx]     = state_canon_f32(pos.x);
    py[dense_idx]     = state_canon_f32(pos.y);
    pz[dense_idx]     = state_canon_f32(pos.z);
    vx[dense_idx]     = state_canon_f32(vel.x);
    vy[dense_idx]     = state_canon_f32(vel.y);
    vz[dense_idx]     = state_canon_f32(vel.z);
    hps[dense_idx]    = hp;
    masks[dense_idx]  = mask;
    bodies[dense_idx] = STATE_HANDLE_NULL;
    tgts[dense_idx]   = STATE_HANDLE_NULL;

    w->spawned_total++;
    return STATE_HANDLE_TYPED_MAKE(Entity, raw);
}

b32 state_ecs_alive(const void* base, StateHandleOf(Entity) e) {
    const StateWorld* w = state_world_get_const(base);
    return state_slots_valid(&w->ecs.slots, base, e.id);
}

u32 state_ecs_dense_index(const void* base, StateHandleOf(Entity) e) {
    if (!state_ecs_alive(base, e)) return STATE_SPARSE_NIL;
    const StateWorld* w = state_world_get_const(base);
    const u32* sparse = STATE_SLICE_PTR_CONST(base, u32, w->ecs.sparse);
    return sparse[state_handle_index(e.id)];
}

void state_ecs_bind_body(void* base, StateHandleOf(Entity) e, StateHandleOf(PhysBody) body) {
    u32 d = state_ecs_dense_index(base, e);
    if (d == STATE_SPARSE_NIL) return;
    StateWorld* w = state_world_get(base);
    *STATE_AT(base, StateHandle, w->ecs.phys_body, d) = body.id;
    *STATE_AT(base, u32, w->ecs.comp_mask, d) |= STATE_COMP_PHYS_LINK;
}

void state_ecs_set_target(void* base, StateHandleOf(Entity) e, StateHandleOf(Entity) target) {
    u32 d = state_ecs_dense_index(base, e);
    if (d == STATE_SPARSE_NIL) return;
    StateWorld* w = state_world_get(base);
    *STATE_AT(base, StateHandle, w->ecs.target_entity, d) = target.id;
    *STATE_AT(base, u32, w->ecs.comp_mask, d) |= STATE_COMP_SEEKER;
}

b32 state_ecs_destroy(void* base, StateHandleOf(Entity) e) {
    if (!state_ecs_alive(base, e)) return FALSE;
    StateWorld* w = state_world_get(base);
    StateEcsSoA* ecs = &w->ecs;

    u32*         sparse = STATE_SLICE_PTR(base, u32,         ecs->sparse);
    StateHandle* d_ent  = STATE_SLICE_PTR(base, StateHandle, ecs->dense_entity);
    f32*         px     = STATE_SLICE_PTR(base, f32,         ecs->pos_x);
    f32*         py     = STATE_SLICE_PTR(base, f32,         ecs->pos_y);
    f32*         pz     = STATE_SLICE_PTR(base, f32,         ecs->pos_z);
    f32*         vx     = STATE_SLICE_PTR(base, f32,         ecs->vel_x);
    f32*         vy     = STATE_SLICE_PTR(base, f32,         ecs->vel_y);
    f32*         vz     = STATE_SLICE_PTR(base, f32,         ecs->vel_z);
    i32*         hps    = STATE_SLICE_PTR(base, i32,         ecs->hp);
    u32*         masks  = STATE_SLICE_PTR(base, u32,         ecs->comp_mask);
    StateHandle* bodies = STATE_SLICE_PTR(base, StateHandle, ecs->phys_body);
    StateHandle* tgts   = STATE_SLICE_PTR(base, StateHandle, ecs->target_entity);

    u32 slot_idx  = state_handle_index(e.id);
    u32 dense_idx = sparse[slot_idx];
    u32 last_idx  = ecs->dense_count - 1u;

    /* Bağlı fizik gövdesi varsa onu da serbest bırak */
    StateHandle linked_body = bodies[dense_idx];
    if (linked_body != STATE_HANDLE_NULL) {
        state_phys_remove_body(base, STATE_HANDLE_TYPED_MAKE(PhysBody, linked_body));
    }

    /* Yoğun SoA dizilerinde swap-and-pop */
    if (dense_idx != last_idx) {
        StateHandle moved_e = d_ent[last_idx];
        d_ent[dense_idx]    = moved_e;
        px[dense_idx]       = px[last_idx];
        py[dense_idx]       = py[last_idx];
        pz[dense_idx]       = pz[last_idx];
        vx[dense_idx]       = vx[last_idx];
        vy[dense_idx]       = vy[last_idx];
        vz[dense_idx]       = vz[last_idx];
        hps[dense_idx]      = hps[last_idx];
        masks[dense_idx]    = masks[last_idx];
        bodies[dense_idx]   = bodies[last_idx];
        tgts[dense_idx]     = tgts[last_idx];
        sparse[state_handle_index(moved_e)] = dense_idx;
    }

    /* Silinen kuyruk elemanını sıfırla (ham bellek özeti geçmişten bağımsız kalsın) */
    d_ent[last_idx]  = STATE_HANDLE_NULL;
    px[last_idx]     = 0.0f;
    py[last_idx]     = 0.0f;
    pz[last_idx]     = 0.0f;
    vx[last_idx]     = 0.0f;
    vy[last_idx]     = 0.0f;
    vz[last_idx]     = 0.0f;
    hps[last_idx]    = 0;
    masks[last_idx]  = 0u;
    bodies[last_idx] = STATE_HANDLE_NULL;
    tgts[last_idx]   = STATE_HANDLE_NULL;

    sparse[slot_idx] = STATE_SPARSE_NIL;
    ecs->dense_count--;
    w->destroyed_total++;

    return state_slots_free(&ecs->slots, base, e.id);
}

/* ----------------------------------------------------------------------------
 * Dünya Simülasyon Adımı (SoA + Fizik + Nesilli Tutamaç Çözümleme)
 * ---------------------------------------------------------------------------- */

void state_world_step(void* base, f32 dt) {
    StateRegionHeader* hdr = state_region_header(base);
    StateWorld* w = state_world_get(base);
    StateEcsSoA* ecs = &w->ecs;

    f32*         px     = STATE_SLICE_PTR(base, f32,         ecs->pos_x);
    f32*         py     = STATE_SLICE_PTR(base, f32,         ecs->pos_y);
    f32*         pz     = STATE_SLICE_PTR(base, f32,         ecs->pos_z);
    f32*         vx     = STATE_SLICE_PTR(base, f32,         ecs->vel_x);
    f32*         vy     = STATE_SLICE_PTR(base, f32,         ecs->vel_y);
    f32*         vz     = STATE_SLICE_PTR(base, f32,         ecs->vel_z);
    u32*         masks  = STATE_SLICE_PTR(base, u32,         ecs->comp_mask);
    StateHandle* bodies = STATE_SLICE_PTR(base, StateHandle, ecs->phys_body);
    StateHandle* tgts   = STATE_SLICE_PTR(base, StateHandle, ecs->target_entity);

    u32 n = ecs->dense_count;

    /* 1) SoA takip (seeker) sistemi: geçerli hedef varlığa doğru ivmelen (ölü hedefi nesil ile tespit et) */
    for (u32 i = 0u; i < n; i++) {
        u32 m = masks[i];
        if ((m & STATE_COMP_SEEKER) == 0u) continue;

        StateHandleOf(Entity) th = STATE_HANDLE_TYPED_MAKE(Entity, tgts[i]);
        u32 td = state_ecs_dense_index(base, th);
        if (td == STATE_SPARSE_NIL) {
            /* Hedef silinmiş (nesil uyuşmadı): referansı temizle */
            tgts[i] = STATE_HANDLE_NULL;
            masks[i] &= ~STATE_COMP_SEEKER;
            continue;
        }

        Vec3 self_p = vec3(px[i], py[i], pz[i]);
        Vec3 tgt_p  = vec3(px[td], py[td], pz[td]);
        Vec3 dir    = vec3_norm(vec3_sub(tgt_p, self_p));
        Vec3 steer  = vec3_scale(dir, 2.5f * dt);

        vx[i] = state_canon_f32(vx[i] + steer.x);
        vy[i] = state_canon_f32(vy[i] + steer.y * 0.25f);
        vz[i] = state_canon_f32(vz[i] + steer.z);

        if (m & STATE_COMP_PHYS_LINK) {
            StatePhysBody* pb = state_phys_get_body(base, STATE_HANDLE_TYPED_MAKE(PhysBody, bodies[i]));
            if (pb) {
                pb->vel = state_canon_vec3(vec3_add(pb->vel, steer));
            }
        }
    }

    /* 2) Fizik gövdelerini ilerlet */
    state_phys_step(base, dt);

    /* 3) SoA konum güncellemesi: fizik bağlı olanlar gövdeden okur, bağımsız olanlar hız ile ilerler */
    for (u32 i = 0u; i < n; i++) {
        u32 m = masks[i];
        if (m & STATE_COMP_PHYS_LINK) {
            const StatePhysBody* pb = state_phys_get_body_const(
                base, STATE_HANDLE_TYPED_MAKE(PhysBody, bodies[i]));
            if (pb) {
                px[i] = pb->pos.x;
                py[i] = pb->pos.y;
                pz[i] = pb->pos.z;
                vx[i] = pb->vel.x;
                vy[i] = pb->vel.y;
                vz[i] = pb->vel.z;
                continue;
            }
        }
        if (m & STATE_COMP_VELOCITY) {
            px[i] = state_canon_f32(px[i] + vx[i] * dt);
            py[i] = state_canon_f32(py[i] + vy[i] * dt);
            pz[i] = state_canon_f32(pz[i] + vz[i] * dt);
        }
    }

    w->step_count++;
    hdr->tick++;

    /* Adım sonu yürüyen doğrulama özeti güncellemesi */
    u64 h = sim_hash_u32(w->checksum_acc, w->step_count);
    h = sim_hash_u32(h, ecs->dense_count);
    h = sim_hash_u32(h, w->phys.body_count);
    h = sim_hash_u32(h, w->phys.bounce_count);
    w->checksum_acc = sim_hash_final(h);
}

void state_world_populate_demo(void* base, u32 entity_count, u32 body_count) {
    StateWorld* w = state_world_get(base);
    ASSERT(entity_count <= w->ecs.max_entities);
    ASSERT(body_count <= w->phys.bodies.count);

    for (u32 i = 0u; i < entity_count; i++) {
        f32 angle = (f32)i * (M_TAU / (f32)(entity_count > 0u ? entity_count : 1u));
        f32 rad   = 2.0f + (f32)(i % 5u) * 1.1f;
        Vec3 pos  = vec3(m_cos(angle) * rad,
                         1.0f + (f32)(i % 7u) * 0.65f,
                         m_sin(angle) * rad);
        Vec3 vel  = vec3(state_world_rng_f32(w, -3.5f, 3.5f),
                         state_world_rng_f32(w, -1.0f, 4.0f),
                         state_world_rng_f32(w, -3.5f, 3.5f));
        u32 mask  = STATE_COMP_TRANSFORM | STATE_COMP_VELOCITY | STATE_COMP_HEALTH;

        StateHandleOf(Entity) e = state_ecs_spawn(base, pos, vel, 100 + (i32)(i % 50u), mask);
        ASSERT(state_handle_non_null(e.id));

        if (i < body_count) {
            f32 br = 0.35f + (f32)(i % 3u) * 0.1f;
            StateHandleOf(PhysBody) b = state_phys_add_body(base, pos, vel, br,
                                                            1.0f + (f32)(i % 4u) * 0.5f,
                                                            0.65f, e);
            ASSERT(state_handle_non_null(b.id));
            state_ecs_bind_body(base, e, b);
        }
    }

    /* Varlıkları dairesel hedef zincirine bağla */
    StateEcsSoA* ecs = &w->ecs;
    const StateHandle* d_ent = STATE_SLICE_PTR_CONST(base, StateHandle, ecs->dense_entity);
    for (u32 i = 0u; i < ecs->dense_count; i++) {
        StateHandleOf(Entity) cur = STATE_HANDLE_TYPED_MAKE(Entity, d_ent[i]);
        StateHandleOf(Entity) nxt = STATE_HANDLE_TYPED_MAKE(Entity, d_ent[(i + 1u) % ecs->dense_count]);
        state_ecs_set_target(base, cur, nxt);
    }
}
