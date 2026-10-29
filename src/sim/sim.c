/* sim/sim.c — Deterministik simülasyon döngüsü, durum anlık görüntüsü (snapshot), rollback ve replay.
 * Durum anlık görüntüleri tek bir Arena bölgesinin düz memcpy'si olarak halka tamponda saklanır.
 * Gecikmeli gerçek girdi tahminden farklı çıktığında en eski kirli kareye geri sarılıp yeniden simüle edilir.
 */
#include "sim/sim.h"
#include "core/log.h"
#include "core/math.h"
#include "core/rng.h"
#include "gfx3d/mesh.h"
#include "phys/shapes.h"
#include "phys/collide.h"
#include "phys/bvh.h"
#include "scene/transform.h"
#include "thread/thread.h"

#define HASH_PRIME1 0x9E3779B185EBCA87ULL
#define HASH_PRIME2 0xC2B2AE3D27D4EB4FULL
#define HASH_PRIME3 0x165667B19E3779F9ULL
#define HASH_PRIME4 0x85EBCA77C2B2AE63ULL
#define HASH_PRIME5 0x27D4EB2F165667C5ULL

static inline u64 rotl64(u64 x, u32 r) {
    return (x << r) | (x >> (64u - r));
}

u64 sim_hash_init(void) {
    return SIM_HASH_SEED;
}

u64 sim_hash_update(u64 h, const void* data, usize len) {
    if (!data || len == 0u) return h;
    const u8* p = (const u8*)data;
    const u8* end = p + len;

    while (p + 8 <= end) {
        u64 k = 0ULL;
        __builtin_memcpy(&k, p, sizeof(u64));
        k *= HASH_PRIME2;
        k = rotl64(k, 31u);
        k *= HASH_PRIME1;
        h ^= k;
        h = rotl64(h, 27u) * HASH_PRIME1 + HASH_PRIME4;
        p += 8;
    }

    if (p + 4 <= end) {
        u32 k32 = 0u;
        __builtin_memcpy(&k32, p, sizeof(u32));
        h ^= (u64)k32 * HASH_PRIME1;
        h = rotl64(h, 23u) * HASH_PRIME2 + HASH_PRIME3;
        p += 4;
    }

    while (p < end) {
        h ^= (u64)(*p) * HASH_PRIME5;
        h = rotl64(h, 11u) * HASH_PRIME1;
        p++;
    }

    return h;
}

u64 sim_hash_final(u64 h) {
    h ^= h >> 33u;
    h *= HASH_PRIME2;
    h ^= h >> 29u;
    h *= HASH_PRIME3;
    h ^= h >> 32u;
    return h;
}

u64 sim_hash(const void* data, usize len) {
    return sim_hash_final(sim_hash_update(sim_hash_init(), data, len));
}

u64 sim_hash_u32(u64 h, u32 v) {
    return sim_hash_update(h, &v, sizeof(u32));
}

u64 sim_hash_f32(u64 h, f32 v) {
    u32 bits = 0u;
    __builtin_memcpy(&bits, &v, sizeof(u32));
    /* -0.0f ve +0.0f aynı değere kanonikleştirilir; tüm NaN bit kalıpları tek kanonik qNaN'a indirgenir */
    if ((bits & 0x7FFFFFFFu) == 0u) {
        bits = 0u;
    } else if ((bits & 0x7F800000u) == 0x7F800000u && (bits & 0x007FFFFFu) != 0u) {
        bits = 0x7FC00000u;
    }
    return sim_hash_u32(h, bits);
}

u64 sim_hash_vec3(u64 h, Vec3 v) {
    h = sim_hash_f32(h, v.x);
    h = sim_hash_f32(h, v.y);
    h = sim_hash_f32(h, v.z);
    return h;
}

u64 sim_hash_phys(u64 h, const PhysWorld* w) {
    if (!w) return h;
    h = sim_hash_vec3(h, w->gravity);
    h = sim_hash_f32(h, w->fixed_dt);
    h = sim_hash_f32(h, w->accum);
    h = sim_hash_u32(h, w->body_count);
    h = sim_hash_u32(h, w->static_count);
    h = sim_hash_u32(h, w->mesh_count);

    for (u32 i = 0u; i < w->body_count; i++) {
        const PhysBody* b = &w->bodies[i];
        h = sim_hash_u32(h, (u32)b->kind);
        h = sim_hash_vec3(h, b->pos);
        h = sim_hash_vec3(h, b->vel);
        h = sim_hash_vec3(h, b->seg_offset);
        h = sim_hash_f32(h, b->radius);
        h = sim_hash_f32(h, b->inv_mass);
        h = sim_hash_f32(h, b->restitution);
        h = sim_hash_f32(h, b->friction);
        h = sim_hash_f32(h, b->max_slope_cos);
        h = sim_hash_u32(h, b->on_ground);
        h = sim_hash_vec3(h, b->ground_normal);
    }

    for (u32 i = 0u; i < w->static_count; i++) {
        const PhysStaticShape* st = &w->statics[i];
        h = sim_hash_update(h, &st->shape, sizeof(PhysShape));
        h = sim_hash_f32(h, st->restitution);
        h = sim_hash_f32(h, st->friction);
    }

    return h;
}

u64 sim_hash_ecs(u64 h, const EcsWorld* w) {
    if (!w) return h;
    h = sim_hash_u32(h, w->ent_cap);
    h = sim_hash_u32(h, w->ent_high_water);
    h = sim_hash_u32(h, w->alive_count);
    h = sim_hash_u32(h, w->free_count);
    h = sim_hash_u32(h, w->comp_count);

    if (w->ent_high_water > 0u) {
        h = sim_hash_update(h, w->ent_handle, sizeof(EcsEntity) * (usize)w->ent_high_water);
        h = sim_hash_update(h, w->ent_gen,    sizeof(u16)       * (usize)w->ent_high_water);
        h = sim_hash_update(h, w->ent_mask,   sizeof(u32)       * (usize)w->ent_high_water);
        h = sim_hash_update(h, w->ent_flags,  sizeof(u8)        * (usize)w->ent_high_water);
    }
    if (w->free_count > 0u) {
        h = sim_hash_update(h, w->free_list, sizeof(u32) * (usize)w->free_count);
    }

    for (u32 c = 0u; c < w->comp_count; c++) {
        const EcsStore* s = &w->stores[c];
        h = sim_hash_u32(h, s->size);
        h = sim_hash_u32(h, s->stride);
        h = sim_hash_u32(h, s->count);
        if (s->count > 0u) {
            h = sim_hash_update(h, s->dense_ent, sizeof(EcsEntity) * (usize)s->count);
            if (s->stride > 0u) {
                h = sim_hash_update(h, s->data, (usize)s->count * (usize)s->stride);
            }
        }
    }

    return h;
}

static void sim_clear_rings(Sim* sim) {
    for (u32 p = 0u; p < SIM_MAX_PLAYERS; p++) {
        for (u32 i = 0u; i < SIM_INPUT_RING; i++) {
            sim->input_ring[p][i].frame     = U32_MAX;
            sim->input_ring[p][i].confirmed = FALSE;
            sim->input_ring[p][i].input     = sim_input_zero();
        }
    }
    for (u32 i = 0u; i < SIM_INPUT_RING; i++) {
        sim->frame_hashes[i]    = 0ULL;
        sim->frame_hash_nums[i] = U32_MAX;
    }
    for (u32 i = 0u; i < sim->snapshot_cap; i++) {
        sim->snap_ring[i].frame      = U32_MAX;
        sim->snap_ring[i].arena_used = 0u;
    }
}

void sim_init(Sim* sim, Arena* a, const SimConfig* cfg) {
    ASSERT(sim != NULL);
    ASSERT(a != NULL);
    ASSERT(cfg != NULL);
    ASSERT(cfg->state_size > 0u);

    __builtin_memset(sim, 0, sizeof(*sim));

    u32 player_count = (cfg->player_count == 0u) ? 2u : cfg->player_count;
    ASSERT(player_count <= SIM_MAX_PLAYERS);

    u32 snap_cap = (cfg->snapshot_cap == 0u) ? SIM_DEFAULT_SNAPSHOT_RING : cfg->snapshot_cap;
    ASSERT(snap_cap >= 2u && snap_cap <= SIM_MAX_SNAPSHOT_RING && IS_POW2(snap_cap));

    u32 aligned_state = ALIGN_UP(cfg->state_size, 16u);

    sim->state          = (u8*)arena_push(a, aligned_state, 16u);
    sim->state_size     = aligned_state;
    arena_init(&sim->state_arena, sim->state, aligned_state);

    sim->player_count   = player_count;
    sim->fixed_dt       = (cfg->fixed_dt > 0.0f) ? cfg->fixed_dt : SIM_DEFAULT_DT;
    sim->accum          = 0.0f;
    sim->current_frame  = 0u;
    sim->rollback_frame = U32_MAX;

    sim->step_fn        = cfg->step_fn;
    sim->hash_fn        = cfg->hash_fn;
    sim->user           = cfg->user;

    sim->snapshot_cap   = snap_cap;
    sim->snapshot_mask  = snap_cap - 1u;
    sim->snap_ring      = ARENA_PUSH_ARRAY(a, SimSnapshotSlot, snap_cap);
    sim->snap_pool      = (u8*)arena_push(a, (usize)snap_cap * (usize)aligned_state, 16u);

    for (u32 i = 0u; i < snap_cap; i++) {
        sim->snap_ring[i].frame      = U32_MAX;
        sim->snap_ring[i].arena_used = 0u;
        sim->snap_ring[i].data       = sim->snap_pool + (usize)i * (usize)aligned_state;
    }

    sim_clear_rings(sim);
}

void sim_reset(Sim* sim) {
    if (!sim) return;
    __builtin_memset(sim->state, 0, sim->state_size);
    arena_reset(&sim->state_arena);
    sim->accum          = 0.0f;
    sim->current_frame  = 0u;
    sim->rollback_frame = U32_MAX;
    __builtin_memset(&sim->stats, 0, sizeof(sim->stats));
    sim_clear_rings(sim);
}

void* sim_state(Sim* sim) {
    return sim ? sim->state : NULL;
}

const void* sim_state_const(const Sim* sim) {
    return sim ? sim->state : NULL;
}

Arena* sim_state_arena(Sim* sim) {
    return sim ? &sim->state_arena : NULL;
}

SimOffset sim_state_alloc(Sim* sim, u32 size, u32 align) {
    if (!sim || size == 0u) return SIM_NULL_OFFSET;
    if (align == 0u) align = 4u;
    void* ptr = arena_push(&sim->state_arena, size, align);
    return sim_ptr_offset(sim->state, ptr);
}

void sim_save_snapshot(Sim* sim, u32 frame) {
    if (!sim) return;
    u32 idx = frame & sim->snapshot_mask;
    SimSnapshotSlot* slot = &sim->snap_ring[idx];
    slot->frame      = frame;
    slot->arena_used = sim->state_arena.used;
    __builtin_memcpy(slot->data, sim->state, sim->state_size);
}

b32 sim_restore_snapshot(Sim* sim, u32 frame) {
    if (!sim) return FALSE;
    u32 idx = frame & sim->snapshot_mask;
    const SimSnapshotSlot* slot = &sim->snap_ring[idx];
    if (slot->frame != frame) return FALSE;

    __builtin_memcpy(sim->state, slot->data, sim->state_size);
    sim->state_arena.used = slot->arena_used;
    sim->current_frame    = frame;
    return TRUE;
}

b32 sim_submit_input(Sim* sim, u32 player, u32 frame, SimInput input) {
    if (!sim || player >= sim->player_count) return FALSE;

    if (frame < sim->current_frame) {
        u32 age = sim->current_frame - frame;
        if (age > sim->snapshot_cap) {
            return FALSE;
        }
        SimInputSlot* slot = &sim->input_ring[player][frame & (SIM_INPUT_RING - 1u)];
        b32 changed = (slot->frame != frame) || !sim_input_eq(slot->input, input);
        slot->frame     = frame;
        slot->confirmed = TRUE;
        slot->input     = input;

        if (changed) {
            sim->stats.mispredicted_inputs++;
            if (frame < sim->rollback_frame) {
                sim->rollback_frame = frame;
            }
        }
        return TRUE;
    }

    SimInputSlot* slot = &sim->input_ring[player][frame & (SIM_INPUT_RING - 1u)];
    slot->frame     = frame;
    slot->confirmed = TRUE;
    slot->input     = input;
    return TRUE;
}

SimInput sim_get_input(const Sim* sim, u32 player, u32 frame, b32* out_confirmed) {
    if (!sim || player >= sim->player_count) {
        if (out_confirmed) *out_confirmed = FALSE;
        return sim_input_zero();
    }

    const SimInputSlot* slot = &sim->input_ring[player][frame & (SIM_INPUT_RING - 1u)];
    if (slot->frame == frame && slot->confirmed) {
        if (out_confirmed) *out_confirmed = TRUE;
        return slot->input;
    }

    if (out_confirmed) *out_confirmed = FALSE;

    /* Geriye doğru bilinen en son girdiyi (tahmin veya gerçek) bul ve tekrarla */
    for (u32 back = 1u; back < SIM_INPUT_RING && back <= frame; back++) {
        u32 pf = frame - back;
        const SimInputSlot* prev = &sim->input_ring[player][pf & (SIM_INPUT_RING - 1u)];
        if (prev->frame == pf) {
            return prev->input;
        }
    }
    return sim_input_zero();
}

static void sim_step_single(Sim* sim, b32 is_resim) {
    u32 f = sim->current_frame;

    /* Kare başındaki durumu anlık görüntü halkasına kopyala */
    sim_save_snapshot(sim, f);

    SimInput frame_inputs[SIM_MAX_PLAYERS];
    for (u32 p = 0u; p < sim->player_count; p++) {
        SimInputSlot* slot = &sim->input_ring[p][f & (SIM_INPUT_RING - 1u)];
        if (slot->frame == f && slot->confirmed) {
            frame_inputs[p] = slot->input;
        } else {
            /* Tahmini girdi: bir önceki karenin girdisini tekrarla (f == 0 ise sıfır girdi) */
            SimInput pred = sim_input_zero();
            if (f > 0u) {
                const SimInputSlot* prev = &sim->input_ring[p][(f - 1u) & (SIM_INPUT_RING - 1u)];
                if (prev->frame == f - 1u) {
                    pred = prev->input;
                }
            }
            slot->frame     = f;
            slot->confirmed = FALSE;
            slot->input     = pred;
            frame_inputs[p] = pred;
            if (!is_resim) {
                sim->stats.predicted_inputs++;
            }
        }
    }

    if (sim->replay_rec) {
        sim_replay_record(sim->replay_rec, f, frame_inputs, sim->player_count);
    }

    if (sim->step_fn) {
        sim->step_fn(sim->state, frame_inputs, sim->player_count, sim->fixed_dt, sim->user);
    }

    sim->current_frame = f + 1u;

    u64 h = sim_state_hash(sim);
    u32 h_idx = f & (SIM_INPUT_RING - 1u);
    sim->frame_hashes[h_idx]    = h;
    sim->frame_hash_nums[h_idx] = f;
}

u32 sim_resolve_rollback(Sim* sim) {
    if (!sim || sim->rollback_frame == U32_MAX) return 0u;

    u32 target    = sim->rollback_frame;
    u32 end_frame = sim->current_frame;
    sim->rollback_frame = U32_MAX;

    if (target >= end_frame) return 0u;
    if (!sim_restore_snapshot(sim, target)) return 0u;

    u32 depth = end_frame - target;
    for (u32 f = target; f < end_frame; f++) {
        sim_step_single(sim, TRUE);
    }

    sim->stats.rollback_count++;
    sim->stats.resim_frames += depth;
    if (depth > sim->stats.max_rollback_depth) {
        sim->stats.max_rollback_depth = depth;
    }
    return depth;
}

b32 sim_rollback_to(Sim* sim, u32 target_frame) {
    if (!sim || target_frame >= sim->current_frame) return FALSE;
    if (sim->rollback_frame == U32_MAX || target_frame < sim->rollback_frame) {
        sim->rollback_frame = target_frame;
    }
    return sim_resolve_rollback(sim) > 0u;
}

void sim_step(Sim* sim) {
    if (!sim) return;
    if (sim->rollback_frame != U32_MAX) {
        sim_resolve_rollback(sim);
    }
    sim_step_single(sim, FALSE);
    sim->stats.total_frames++;
}

u32 sim_update(Sim* sim, f32 dt) {
    if (!sim || dt <= 0.0f) return 0u;
    sim->accum += dt;
    u32 steps = 0u;
    while (sim->accum >= sim->fixed_dt && steps < SIM_MAX_FRAME_STEPS) {
        sim_step(sim);
        sim->accum -= sim->fixed_dt;
        steps++;
    }
    if (sim->accum >= sim->fixed_dt) {
        sim->accum = 0.0f;
    }
    return steps;
}

u64 sim_state_hash(const Sim* sim) {
    if (!sim) return 0ULL;
    if (sim->hash_fn) {
        return sim->hash_fn(sim->state, sim->state_size, sim->user);
    }
    return sim_hash(sim->state, sim->state_size);
}

u64 sim_frame_hash(const Sim* sim, u32 frame) {
    if (!sim) return 0ULL;
    u32 idx = frame & (SIM_INPUT_RING - 1u);
    if (sim->frame_hash_nums[idx] != frame) return 0ULL;
    return sim->frame_hashes[idx];
}

b32 sim_check_desync(const Sim* sim, u32 frame, u64 remote_hash) {
    if (!sim) return FALSE;
    u32 idx = frame & (SIM_INPUT_RING - 1u);
    if (sim->frame_hash_nums[idx] != frame) return FALSE;
    return sim->frame_hashes[idx] != remote_hash;
}

/* ----------------------------------------------------------------------------
 * Replay (Girdi Kaydı ve Oynatma)
 * ---------------------------------------------------------------------------- */

void sim_replay_init(SimReplay* rep, Arena* a, u32 player_count,
                     u32 max_frames, u32 seed, f32 fixed_dt) {
    ASSERT(rep != NULL);
    ASSERT(a != NULL);
    if (player_count == 0u) player_count = 2u;
    ASSERT(player_count <= SIM_MAX_PLAYERS);

    __builtin_memset(rep, 0, sizeof(*rep));
    rep->seed         = seed;
    rep->player_count = player_count;
    rep->frame_count  = 0u;
    rep->max_frames   = max_frames;
    rep->fixed_dt     = (fixed_dt > 0.0f) ? fixed_dt : SIM_DEFAULT_DT;
    if (max_frames > 0u) {
        rep->inputs = ARENA_PUSH_ARRAY(a, SimInput, (usize)max_frames * (usize)player_count);
    }
}

void sim_replay_clear(SimReplay* rep) {
    if (!rep) return;
    if (rep->inputs && rep->frame_count > 0u) {
        __builtin_memset(rep->inputs, 0,
                         sizeof(SimInput) * (usize)rep->frame_count * (usize)rep->player_count);
    }
    rep->frame_count = 0u;
}

void sim_attach_replay(Sim* sim, SimReplay* rep) {
    if (!sim) return;
    sim->replay_rec = rep;
}

b32 sim_replay_record(SimReplay* rep, u32 frame, const SimInput* inputs, u32 player_count) {
    if (!rep || !inputs || frame >= rep->max_frames) return FALSE;
    u32 count = MIN(player_count, rep->player_count);
    SimInput* dst = rep->inputs + (usize)frame * (usize)rep->player_count;
    for (u32 p = 0u; p < count; p++) {
        dst[p] = inputs[p];
    }
    for (u32 p = count; p < rep->player_count; p++) {
        dst[p] = sim_input_zero();
    }
    if (frame + 1u > rep->frame_count) {
        rep->frame_count = frame + 1u;
    }
    return TRUE;
}

const SimInput* sim_replay_get(const SimReplay* rep, u32 frame) {
    if (!rep || !rep->inputs || frame >= rep->frame_count) return NULL;
    return rep->inputs + (usize)frame * (usize)rep->player_count;
}

typedef struct {
    u32 magic;
    u32 version;
    u32 seed;
    u32 player_count;
    u32 frame_count;
    u32 fixed_dt_bits;
} SimReplayHeader;

_Static_assert(sizeof(SimReplayHeader) == 24, "SimReplayHeader 24 byte olmali");

u32 sim_replay_save(const SimReplay* rep, u8* buf, u32 cap) {
    if (!rep) return 0u;
    usize payload_bytes = sizeof(SimInput) * (usize)rep->frame_count * (usize)rep->player_count;
    usize total_bytes   = sizeof(SimReplayHeader) + payload_bytes;
    if (!buf || (usize)cap < total_bytes) return 0u;

    SimReplayHeader hdr;
    hdr.magic        = SIM_REPLAY_MAGIC;
    hdr.version      = SIM_REPLAY_VERSION;
    hdr.seed         = rep->seed;
    hdr.player_count = rep->player_count;
    hdr.frame_count  = rep->frame_count;
    __builtin_memcpy(&hdr.fixed_dt_bits, &rep->fixed_dt, sizeof(u32));

    __builtin_memcpy(buf, &hdr, sizeof(SimReplayHeader));
    if (payload_bytes > 0u) {
        __builtin_memcpy(buf + sizeof(SimReplayHeader), rep->inputs, payload_bytes);
    }
    return (u32)total_bytes;
}

b32 sim_replay_load(SimReplay* rep, Arena* a, const u8* buf, u32 len) {
    if (!rep || !a || !buf || len < sizeof(SimReplayHeader)) return FALSE;

    SimReplayHeader hdr;
    __builtin_memcpy(&hdr, buf, sizeof(SimReplayHeader));
    if (hdr.magic != SIM_REPLAY_MAGIC || hdr.version != SIM_REPLAY_VERSION) return FALSE;
    if (hdr.player_count == 0u || hdr.player_count > SIM_MAX_PLAYERS) return FALSE;

    usize payload_bytes = sizeof(SimInput) * (usize)hdr.frame_count * (usize)hdr.player_count;
    if ((usize)len < sizeof(SimReplayHeader) + payload_bytes) return FALSE;

    f32 fixed_dt = SIM_DEFAULT_DT;
    __builtin_memcpy(&fixed_dt, &hdr.fixed_dt_bits, sizeof(f32));

    sim_replay_init(rep, a, hdr.player_count, hdr.frame_count, hdr.seed, fixed_dt);
    rep->frame_count = hdr.frame_count;
    if (payload_bytes > 0u) {
        __builtin_memcpy(rep->inputs, buf + sizeof(SimReplayHeader), payload_bytes);
    }
    return TRUE;
}

b32 sim_replay_run(Sim* sim, const SimReplay* rep, u64* out_hashes, u32 max_hashes) {
    if (!sim || !rep || rep->player_count != sim->player_count) return FALSE;

    SimReplay* prev_rec = sim->replay_rec;
    sim->replay_rec = NULL;

    for (u32 f = 0u; f < rep->frame_count; f++) {
        const SimInput* frame_in = sim_replay_get(rep, f);
        if (!frame_in) {
            sim->replay_rec = prev_rec;
            return FALSE;
        }
        for (u32 p = 0u; p < rep->player_count; p++) {
            sim_submit_input(sim, p, sim->current_frame, frame_in[p]);
        }
        sim_step(sim);
        if (out_hashes && f < max_hashes) {
            out_hashes[f] = sim_state_hash(sim);
        }
    }

    sim->replay_rec = prev_rec;
    return TRUE;
}

/* ============================================================================
 * Belirlenimcilik Doğrulama Sahnesi (F0_G, D-112)
 * 10.000 tick 3D fizik + ECS simülasyon sahnesi
 * ============================================================================ */

#define DET_SCENE_MAX_PLAYERS  2u
#define DET_SCENE_CRATE_COUNT  6u
#define DET_SCENE_MAX_PROJ     8u
#define DET_SCENE_MAX_ENTITIES 64u
#define DET_SCENE_MAX_BODIES   32u
#define DET_SCENE_MAX_STATICS  16u

typedef struct {
    u32 body_idx;
    u32 player_id;
    i32 hp;
    u32 cooldown;
    u32 score;
} DetCompFighter;

typedef struct {
    EcsEntity owner;
    u32       slot;
    u32       ttl;
} DetCompRocket;

typedef struct {
    f32 angle;
    f32 speed;
} DetCompOrbiter;

typedef struct {
    const Mesh* arena_mesh;
    const Bvh*  arena_bvh;
} DetSceneAssets;

typedef struct {
    u32       tick;
    Rng       rng;
    PhysWorld phys;
    EcsWorld  ecs;

    EcsCompId cid_fighter;
    EcsCompId cid_rocket;
    EcsCompId cid_orbiter;

    EcsEntity player_ent[DET_SCENE_MAX_PLAYERS];
    EcsEntity orbiter_ent[DET_SCENE_MAX_PLAYERS];
    EcsEntity crate_ent[DET_SCENE_CRATE_COUNT];
    u32       crate_body[DET_SCENE_CRATE_COUNT];

    u32       proj_body[DET_SCENE_MAX_PROJ];
    EcsEntity proj_ent[DET_SCENE_MAX_PROJ];
} DetSceneState;

static u64 det_scene_hash(const void* state, u32 state_size, void* user) {
    UNUSED(state_size);
    UNUSED(user);
    const DetSceneState* s = (const DetSceneState*)state;
    u64 h = sim_hash_init();
    h = sim_hash_u32(h, s->tick);
    h = sim_hash_update(h, &s->rng, sizeof(Rng));
    h = sim_hash_phys(h, &s->phys);
    h = sim_hash_ecs(h, &s->ecs);
    h = sim_hash_update(h, s->player_ent,  sizeof(s->player_ent));
    h = sim_hash_update(h, s->orbiter_ent, sizeof(s->orbiter_ent));
    h = sim_hash_update(h, s->crate_ent,   sizeof(s->crate_ent));
    h = sim_hash_update(h, s->crate_body,  sizeof(s->crate_body));
    h = sim_hash_update(h, s->proj_body,   sizeof(s->proj_body));
    h = sim_hash_update(h, s->proj_ent,    sizeof(s->proj_ent));
    return sim_hash_final(h);
}

static void det_scene_setup(Sim* sim, const DetSceneAssets* assets, u32 seed) {
    sim_reset(sim);
    Arena* sa = sim_state_arena(sim);

    DetSceneState* s = ARENA_PUSH_STRUCT(sa, DetSceneState);
    s->tick = 0u;
    rng_seed(&s->rng, seed);

    phys_world_init(&s->phys, sa, DET_SCENE_MAX_BODIES, DET_SCENE_MAX_STATICS, 2u);
    s->phys.fixed_dt = sim->fixed_dt;

    phys_world_add_static(&s->phys, shape_from_plane(plane_make(vec3(0.0f, 1.0f, 0.0f), 0.0f)), 0.2f, 0.7f);
    phys_world_add_static(&s->phys, shape_from_aabb(aabb_make(vec3(-12.0f, 0.0f, -12.0f), vec3(12.0f, 4.0f, -11.0f))), 0.3f, 0.5f);
    phys_world_add_static(&s->phys, shape_from_aabb(aabb_make(vec3(-12.0f, 0.0f,  11.0f), vec3(12.0f, 4.0f,  12.0f))), 0.3f, 0.5f);
    phys_world_add_static(&s->phys, shape_from_aabb(aabb_make(vec3(-12.0f, 0.0f, -12.0f), vec3(-11.0f, 4.0f, 12.0f))), 0.3f, 0.5f);
    phys_world_add_static(&s->phys, shape_from_aabb(aabb_make(vec3( 11.0f, 0.0f, -12.0f), vec3( 12.0f, 4.0f, 12.0f))), 0.3f, 0.5f);
    phys_world_add_static(&s->phys, shape_from_sphere(sphere_make(vec3(0.0f, 0.5f, 0.0f), 1.2f)), 0.4f, 0.6f);

    if (assets && assets->arena_mesh && assets->arena_bvh) {
        phys_world_add_mesh(&s->phys, assets->arena_mesh, assets->arena_bvh, 0.2f, 0.6f);
    }

    ecs_world_init(&s->ecs, sa, DET_SCENE_MAX_ENTITIES);
    transform_register(&s->ecs);
    s->cid_fighter = ecs_register(&s->ecs, "Fighter", sizeof(DetCompFighter), _Alignof(DetCompFighter), NULL);
    s->cid_rocket  = ecs_register(&s->ecs, "Rocket",  sizeof(DetCompRocket),  _Alignof(DetCompRocket),  NULL);
    s->cid_orbiter = ecs_register(&s->ecs, "Orbiter", sizeof(DetCompOrbiter), _Alignof(DetCompOrbiter), NULL);

    EcsCompId cid_tr = transform_comp_id(&s->ecs);

    for (u32 p = 0u; p < DET_SCENE_MAX_PLAYERS; p++) {
        f32 px = (p == 0u) ? -4.0f : 4.0f;
        f32 pz = (p == 0u) ? -2.0f : 2.0f;
        u32 bidx = phys_world_add_capsule(&s->phys,
                                          vec3(px, 0.5f, pz),
                                          vec3(px, 1.3f, pz),
                                          0.4f, 1.0f, 0.1f, 0.8f);

        EcsEntity pe = ecs_spawn(&s->ecs);
        s->player_ent[p] = pe;
        Transform tr = transform_trs(vec3(px, 0.9f, pz), quat_identity(), vec3(1.0f, 1.0f, 1.0f));
        ecs_set(&s->ecs, pe, cid_tr, &tr);

        DetCompFighter cf = { bidx, p, 100, 0u, 0u };
        ecs_set(&s->ecs, pe, s->cid_fighter, &cf);

        EcsEntity oe = ecs_spawn(&s->ecs);
        s->orbiter_ent[p] = oe;
        Transform otr = transform_trs(vec3(0.8f, 0.5f, 0.0f), quat_identity(), vec3(0.3f, 0.3f, 0.3f));
        ecs_set(&s->ecs, oe, cid_tr, &otr);
        DetCompOrbiter co = { (f32)p * M_PI, 2.5f + (f32)p * 0.5f };
        ecs_set(&s->ecs, oe, s->cid_orbiter, &co);
        transform_set_parent(&s->ecs, oe, pe);
    }

    for (u32 i = 0u; i < DET_SCENE_CRATE_COUNT; i++) {
        f32 angle = (f32)i * (M_TAU / (f32)DET_SCENE_CRATE_COUNT);
        Vec3 pos = vec3(m_cos(angle) * 3.0f, 1.5f + (f32)i * 0.25f, m_sin(angle) * 3.0f);
        u32 bidx = phys_world_add_sphere(&s->phys, pos, 0.45f, 0.8f, 0.45f, 0.5f);
        s->crate_body[i] = bidx;

        EcsEntity ce = ecs_spawn(&s->ecs);
        s->crate_ent[i] = ce;
        Transform ctr = transform_trs(pos, quat_identity(), vec3(0.9f, 0.9f, 0.9f));
        ecs_set(&s->ecs, ce, cid_tr, &ctr);
    }

    for (u32 i = 0u; i < DET_SCENE_MAX_PROJ; i++) {
        Vec3 park = vec3((f32)i * 2.0f - 8.0f, -50.0f, 0.0f);
        u32 bidx = phys_world_add_sphere(&s->phys, park, 0.25f, 0.0f, 0.6f, 0.2f);
        s->proj_body[i] = bidx;
        s->proj_ent[i]  = ECS_NULL;
    }

    transform_update(&s->ecs);
}

static void det_scene_step(void* state, const SimInput* inputs, u32 player_count, f32 dt, void* user) {
    UNUSED(user);
    DetSceneState* s = (DetSceneState*)state;
    EcsCompId cid_tr = transform_comp_id(&s->ecs);

    /* 1) Oyuncu girdilerini fizik gövdelerine ve silahlara uygula */
    EcsQuery qf = ecs_query(&s->ecs, (1u << s->cid_fighter) | (1u << cid_tr));
    while (ecs_query_next(&qf)) {
        DetCompFighter* cf = (DetCompFighter*)ecs_query_get(&qf, s->cid_fighter);
        const SimInput* in = &inputs[cf->player_id % player_count];
        PhysBody* b = &s->phys.bodies[cf->body_idx];

        f32 mx = (f32)in->move_x * (1.0f / 32767.0f);
        f32 mz = (f32)in->move_y * (1.0f / 32767.0f);
        f32 spd = (in->buttons & SIM_BTN_SPRINT) ? 9.0f : 6.0f;

        b->vel.x = b->vel.x * 0.75f + mx * spd * 0.25f;
        b->vel.z = b->vel.z * 0.75f + mz * spd * 0.25f;

        if ((in->buttons & SIM_BTN_JUMP) && b->on_ground) {
            b->vel.y = 5.5f;
            b->on_ground = FALSE;
        }

        if (cf->cooldown > 0u) cf->cooldown--;
        if ((in->buttons & SIM_BTN_FIRE) && cf->cooldown == 0u) {
            for (u32 slot = 0u; slot < DET_SCENE_MAX_PROJ; slot++) {
                if (s->proj_ent[slot] == ECS_NULL || !ecs_alive(&s->ecs, s->proj_ent[slot])) {
                    EcsEntity re = ecs_spawn(&s->ecs);
                    s->proj_ent[slot] = re;

                    Vec3 dir = vec3(mx, 0.15f, mz);
                    if (vec3_dot(dir, dir) < 1e-4f) {
                        dir = (cf->player_id == 0u) ? vec3(1.0f, 0.1f, 0.0f) : vec3(-1.0f, 0.1f, 0.0f);
                    }
                    dir = vec3_norm(dir);

                    PhysBody* pb = &s->phys.bodies[s->proj_body[slot]];
                    pb->pos      = vec3_add(b->pos, vec3_scale(dir, 0.9f));
                    pb->vel      = vec3_scale(dir, 14.0f);
                    pb->inv_mass = 2.0f;

                    Transform rtr = transform_trs(pb->pos, quat_identity(), vec3(0.25f, 0.25f, 0.25f));
                    ecs_set(&s->ecs, re, cid_tr, &rtr);
                    DetCompRocket cr = { qf.entity, slot, 24u };
                    ecs_set(&s->ecs, re, s->cid_rocket, &cr);

                    cf->cooldown = 8u;
                    cf->score += 1u;
                    break;
                }
            }
        }
    }

    /* 2) 3D fizik adımı */
    phys_world_step(&s->phys);

    /* 3) Fizik -> Transform */
    for (u32 p = 0u; p < DET_SCENE_MAX_PLAYERS; p++) {
        EcsEntity pe = s->player_ent[p];
        DetCompFighter* cf = (DetCompFighter*)ecs_get(&s->ecs, pe, s->cid_fighter);
        Transform* tr   = (Transform*)ecs_get(&s->ecs, pe, cid_tr);
        if (cf && tr) {
            tr->pos = s->phys.bodies[cf->body_idx].pos;
            f32 yaw = (f32)inputs[p % player_count].look_dx * 0.01f;
            tr->rot = quat_from_axis_angle(vec3(0.0f, 1.0f, 0.0f), yaw);
        }
    }

    for (u32 i = 0u; i < DET_SCENE_CRATE_COUNT; i++) {
        Transform* ctr = (Transform*)ecs_get(&s->ecs, s->crate_ent[i], cid_tr);
        if (ctr) {
            ctr->pos = s->phys.bodies[s->crate_body[i]].pos;
        }
    }

    /* 4) Mermiler */
    EcsQuery qr = ecs_query(&s->ecs, (1u << s->cid_rocket) | (1u << cid_tr));
    while (ecs_query_next(&qr)) {
        DetCompRocket* cr = (DetCompRocket*)ecs_query_get(&qr, s->cid_rocket);
        Transform*  tr = (Transform*)ecs_query_get(&qr, cid_tr);
        PhysBody*   pb = &s->phys.bodies[s->proj_body[cr->slot]];
        tr->pos = pb->pos;

        if (cr->ttl <= 1u) {
            pb->inv_mass = 0.0f;
            pb->vel      = vec3(0.0f, 0.0f, 0.0f);
            pb->pos      = vec3((f32)cr->slot * 2.0f - 8.0f, -50.0f, 0.0f);
            s->proj_ent[cr->slot] = ECS_NULL;
            ecs_destroy(&s->ecs, qr.entity);
        } else {
            cr->ttl--;
        }
    }

    /* 5) Uydular */
    EcsQuery qo = ecs_query(&s->ecs, (1u << s->cid_orbiter) | (1u << cid_tr));
    while (ecs_query_next(&qo)) {
        DetCompOrbiter* co = (DetCompOrbiter*)ecs_query_get(&qo, s->cid_orbiter);
        Transform*   tr = (Transform*)ecs_query_get(&qo, cid_tr);
        co->angle += co->speed * dt;
        if (co->angle > M_TAU) co->angle -= M_TAU;
        tr->pos = vec3(m_cos(co->angle) * 0.8f, 0.5f, m_sin(co->angle) * 0.8f);
    }

    transform_update(&s->ecs);
    s->tick++;
}

static SimInput det_make_input(u32 player, u32 frame) {
    Rng r;
    u32 epoch = frame / 3u;
    rng_seed_seq(&r, (u64)epoch * 1315423911ULL + (u64)player * 97ULL, player + 1u);

    SimInput in = sim_input_zero();
    u32 bits = rng_u32(&r);
    if ((bits & 3u) == 0u)      in.move_x =  24000;
    else if ((bits & 3u) == 1u) in.move_x = -24000;
    else                        in.move_x = (i16)((i32)(bits & 0xFFu) * 64 - 8192);

    if (((bits >> 2u) & 3u) == 0u)      in.move_y =  24000;
    else if (((bits >> 2u) & 3u) == 1u) in.move_y = -24000;
    else                                in.move_y = (i16)((i32)((bits >> 8u) & 0xFFu) * 64 - 8192);

    in.look_dx = (i16)((i32)((bits >> 16u) & 0x3Fu) - 32);
    in.look_dy = 0;

    if ((frame + player * 5u) % 11u == 0u) in.buttons |= SIM_BTN_JUMP;
    if ((frame + player * 3u) % 7u  == 0u) in.buttons |= SIM_BTN_FIRE;
    if ((bits >> 24u) & 1u)                in.buttons |= SIM_BTN_SPRINT;
    in.aux = epoch;
    return in;
}

static inline u64 det_fnv1a_combine(u64 h, u64 v) {
    for (u32 b = 0u; b < 8u; b++) {
        h ^= (v >> (b * 8u)) & 0xFFu;
        h *= 1099511628211ULL;
    }
    return h;
}

static b32 det_check_no_nan(const DetSceneState* s) {
    for (u32 i = 0u; i < s->phys.body_count; i++) {
        Vec3 p = s->phys.bodies[i].pos;
        Vec3 v = s->phys.bodies[i].vel;
        if (p.x != p.x || p.y != p.y || p.z != p.z) return FALSE;
        if (v.x != v.x || v.y != v.y || v.z != v.z) return FALSE;
    }
    EcsCompId cid_tr = transform_comp_id(&s->ecs);
    const EcsStore* ts = &s->ecs.stores[cid_tr];
    for (u32 i = 0u; i < ts->count; i++) {
        const Transform* tr = (const Transform*)(ts->data + (usize)i * (usize)ts->stride);
        for (u32 m = 0u; m < 16u; m++) {
            if (tr->world.m[m] != tr->world.m[m]) return FALSE;
        }
    }
    return TRUE;
}

static _Alignas(16) u8  g_sim_det_mem[MB(8)];
static _Alignas(16) u64 g_sim_det_hashes[10000];
static SimDetResult     g_sim_det_result;

SimDetResult sim_det_run(u32 total_ticks, u32 seed, u64* out_hashes, u32 max_hashes) {
    SimDetResult res;
    __builtin_memset(&res, 0, sizeof(res));
    res.total_ticks = total_ticks;

    Arena a;
    arena_init(&a, g_sim_det_mem, sizeof(g_sim_det_mem));

    Mesh* ramp = mesh_cube(&a, 1.2f);
    for (u32 i = 0u; i < ramp->vertex_count; i++) {
        ramp->px[i] -= 2.5f;
        ramp->py[i] -= 0.7f;
    }
    mesh_compute_bounds(ramp);
    Bvh* ramp_bvh = bvh_build(&a, ramp);
    DetSceneAssets assets = { ramp, ramp_bvh };

    SimConfig cfg;
    __builtin_memset(&cfg, 0, sizeof(cfg));
    cfg.state_size   = KB(64);
    cfg.player_count = DET_SCENE_MAX_PLAYERS;
    cfg.snapshot_cap = 16u;
    cfg.fixed_dt     = SIM_DEFAULT_DT;
    cfg.step_fn      = det_scene_step;
    cfg.hash_fn      = det_scene_hash;
    cfg.user         = &assets;

    Sim sim;
    sim_init(&sim, &a, &cfg);
    det_scene_setup(&sim, &assets, seed);

    res.initial_hash = sim_state_hash(&sim);
    u64 comb = SIM_HASH_SEED;

    u32 cp_idx = 0u;
    for (u32 f = 0u; f < total_ticks; f++) {
        sim_submit_input(&sim, 0u, f, det_make_input(0u, f));
        sim_submit_input(&sim, 1u, f, det_make_input(1u, f));
        sim_step(&sim);
        u64 h = sim_state_hash(&sim);
        if (out_hashes && f < max_hashes) {
            out_hashes[f] = h;
        }
        comb = det_fnv1a_combine(comb, h);

        if ((f + 1u) % 1000u == 0u && cp_idx < 10u) {
            res.checkpoint_hashes[cp_idx++] = h;
        }
    }

    res.final_hash = sim_state_hash(&sim);
    res.combined_hash = comb;
    const DetSceneState* st = (const DetSceneState*)sim_state_const(&sim);
    res.has_nan = !det_check_no_nan(st);

    return res;
}

EXPORT(engine_sim_det_run)
u32 engine_sim_det_run(u32 total_ticks, u32 seed, u32 num_threads) {
    if (total_ticks == 0u) total_ticks = SIM_DET_DEFAULT_TICKS;
    if (total_ticks > 10000u) total_ticks = 10000u;
    if (seed == 0u) seed = SIM_DET_DEFAULT_SEED;

    if (num_threads > 0u) {
        engine_set_worker_count(num_threads - 1u);
    }

    g_sim_det_result = sim_det_run(total_ticks, seed, g_sim_det_hashes, 10000u);
    return g_sim_det_result.has_nan ? 1u : 0u;
}

EXPORT(engine_sim_det_get_result)
void engine_sim_det_get_result(SimDetResult* out_result) {
    if (out_result) {
        __builtin_memcpy(out_result, &g_sim_det_result, sizeof(SimDetResult));
    }
}

EXPORT(engine_sim_det_get_hashes)
u64* engine_sim_det_get_hashes(void) {
    return g_sim_det_hashes;
}
