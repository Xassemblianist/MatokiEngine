/* state/state_region.c — Taban adres saklamayan RegionArena, StateRegionHeader ve StateSlotTable
 * gerçeklemesi. Tüm hizalamalar bölge başından bayt ofsetine (used) göre hesaplanır; ayırma ve
 * geri alma (pop) sırasında hizalama dolguları sıfırlanarak ham bellek özeti belirlenimci tutulur.
 */
#include "state/state_region.h"
#include "sim/sim.h"

void region_arena_init(RegionArena* ra, u32 cap, u32 initial_used) {
    ASSERT(ra != NULL);
    ASSERT(initial_used <= cap);
    ra->cap   = cap;
    ra->used  = initial_used;
    ra->_pad0 = 0u;
    ra->_pad1 = 0u;
}

StateOffset region_alloc_raw(RegionArena* ra, u32 size, u32 align) {
    ASSERT(ra != NULL);
    if (size == 0u) return STATE_NULL_OFFSET;
    if (align == 0u) align = 4u;
    ASSERT(IS_POW2(align));
    ASSERT(align <= STATE_MAX_ALIGN);

    u32 aligned = ALIGN_UP(ra->used, align);
    u32 pad     = aligned - ra->used;

    if (pad > ra->cap - ra->used || size > ra->cap - ra->used - pad) {
        PANIC("region_arena overflow: used=%u, pad=%u, req=%u, cap=%u",
              ra->used, pad, size, ra->cap);
    }

    ra->used = aligned + size;
    return aligned;
}

StateOffset region_alloc(RegionArena* ra, void* base, u32 size, u32 align) {
    ASSERT(ra != NULL);
    ASSERT(base != NULL);
    u32 prev_used = ra->used;
    StateOffset off = region_alloc_raw(ra, size, align);
    if (off == STATE_NULL_OFFSET) return STATE_NULL_OFFSET;

    /* Hizalama dolgusu (pad) dahil [prev_used .. off + size) aralığını sıfırla */
    u32 total_zero = ra->used - prev_used;
    __builtin_memset((u8*)base + (usize)prev_used, 0, (usize)total_zero);
    return off;
}

void region_arena_reset(RegionArena* ra, void* base, u32 initial_used) {
    ASSERT(ra != NULL);
    ASSERT(initial_used <= ra->cap);
    if (base && ra->used > initial_used) {
        __builtin_memset((u8*)base + (usize)initial_used, 0,
                         (usize)(ra->used - initial_used));
    }
    ra->used  = initial_used;
    ra->_pad0 = 0u;
    ra->_pad1 = 0u;
}

u32 region_arena_mark(const RegionArena* ra) {
    ASSERT(ra != NULL);
    return ra->used;
}

void region_arena_pop_to(RegionArena* ra, void* base, u32 mark) {
    ASSERT(ra != NULL);
    ASSERT(mark <= ra->used);
    if (base && ra->used > mark) {
        __builtin_memset((u8*)base + (usize)mark, 0, (usize)(ra->used - mark));
    }
    ra->used = mark;
}

RegionArena region_arena_sub(RegionArena* parent, void* base, u32 sub_cap, StateOffset* out_base_off) {
    ASSERT(parent != NULL);
    ASSERT(sub_cap > 0u);
    StateOffset off = base ? region_alloc(parent, base, sub_cap, 16u)
                           : region_alloc_raw(parent, sub_cap, 16u);
    if (out_base_off) {
        *out_base_off = off;
    }
    RegionArena sub;
    region_arena_init(&sub, sub_cap, 0u);
    return sub;
}

/* ----------------------------------------------------------------------------
 * StateRegionHeader
 * ---------------------------------------------------------------------------- */

StateRegionHeader* state_region_init(void* base, u32 cap, u32 abi_hash) {
    ASSERT(base != NULL);
    ASSERT(cap >= (u32)sizeof(StateRegionHeader));
    ASSERT(((uptr)base & 15u) == 0u); /* bölge tabanı en az 16-bayt hizalı olmalı */

    /* Başlığı ve bölgeyi deterministik başlangıç için sıfırla */
    __builtin_memset(base, 0, (usize)cap);

    StateRegionHeader* hdr = (StateRegionHeader*)base;
    hdr->magic    = STATE_REGION_MAGIC;
    hdr->version  = STATE_REGION_VERSION;
    hdr->abi_hash = abi_hash;
    hdr->root_off = STATE_NULL_OFFSET;
    hdr->tick     = 0ULL;
    hdr->flags    = 0ULL;
    region_arena_init(&hdr->arena, cap, (u32)sizeof(StateRegionHeader));
    return hdr;
}

StateRegionHeader* state_region_header(void* base) {
    ASSERT(base != NULL);
    return (StateRegionHeader*)base;
}

const StateRegionHeader* state_region_header_const(const void* base) {
    ASSERT(base != NULL);
    return (const StateRegionHeader*)base;
}

RegionArena* state_region_arena(void* base) {
    ASSERT(base != NULL);
    return &((StateRegionHeader*)base)->arena;
}

b32 state_region_validate(const void* base, u32 cap, u32 expected_abi_hash) {
    if (!base || cap < (u32)sizeof(StateRegionHeader)) return FALSE;
    const StateRegionHeader* hdr = (const StateRegionHeader*)base;
    if (hdr->magic != STATE_REGION_MAGIC) return FALSE;
    if (hdr->version != STATE_REGION_VERSION) return FALSE;
    if (expected_abi_hash != 0u && hdr->abi_hash != expected_abi_hash) return FALSE;
    if (hdr->arena.cap != cap) return FALSE;
    if (hdr->arena.used < (u32)sizeof(StateRegionHeader) || hdr->arena.used > cap) return FALSE;
    if (hdr->root_off != STATE_NULL_OFFSET && hdr->root_off >= hdr->arena.used) return FALSE;
    return TRUE;
}

StateOffset state_region_alloc(void* base, u32 size, u32 align) {
    StateRegionHeader* hdr = state_region_header(base);
    return region_alloc(&hdr->arena, base, size, align);
}

void state_region_set_root(void* base, StateOffset root_off) {
    StateRegionHeader* hdr = state_region_header(base);
    ASSERT(root_off == STATE_NULL_OFFSET ||
           (root_off >= (u32)sizeof(StateRegionHeader) && root_off < hdr->arena.used));
    hdr->root_off = root_off;
}

StateOffset state_region_root(const void* base) {
    const StateRegionHeader* hdr = state_region_header_const(base);
    return hdr->root_off;
}

void state_region_snapshot_save(const void* src_base, void* dst_snap, u32 bytes) {
    ASSERT(src_base != NULL);
    ASSERT(dst_snap != NULL);
    ASSERT(bytes > 0u);
    __builtin_memcpy(dst_snap, src_base, (usize)bytes);
}

void state_region_snapshot_restore(void* dst_base, const void* src_snap, u32 bytes) {
    ASSERT(dst_base != NULL);
    ASSERT(src_snap != NULL);
    ASSERT(bytes > 0u);
    __builtin_memcpy(dst_base, src_snap, (usize)bytes);
}

b32 state_region_relocate(void* new_base, const void* old_base, u32 cap) {
    if (!new_base || !old_base || cap < (u32)sizeof(StateRegionHeader)) return FALSE;
    if (!state_region_validate(old_base, cap, 0u)) return FALSE;
    /* İşaretçisiz durum bölgesi: tek blok memcpy ile farklı taban adrese taşınır */
    __builtin_memcpy(new_base, old_base, (usize)cap);
    return state_region_validate(new_base, cap, 0u);
}

#define STATE_HASH_P1 0x9E3779B185EBCA87ULL
#define STATE_HASH_P2 0xC2B2AE3D27D4EB4FULL
#define STATE_HASH_P3 0x165667B19E3779F9ULL
#define STATE_HASH_P4 0x85EBCA77C2B2AE63ULL
#define STATE_HASH_P5 0x27D4EB2F165667C5ULL

static inline u64 state_rotl64(u64 x, u32 r) {
    return (x << r) | (x >> (64u - r));
}

static inline u64 state_round64(u64 acc, u64 input) {
    acc += input * STATE_HASH_P2;
    acc  = state_rotl64(acc, 31u);
    acc *= STATE_HASH_P1;
    return acc;
}

static inline u64 state_merge_round64(u64 acc, u64 val) {
    val  = state_round64(0ULL, val);
    acc ^= val;
    acc  = acc * STATE_HASH_P1 + STATE_HASH_P4;
    return acc;
}

u64 state_region_hash_bytes(const void* data, usize len) {
    if (!data || len == 0u) return 0ULL;
    const u8* p   = (const u8*)data;
    const u8* end = p + len;
    u64 h = 0ULL;

    if (len >= 32u) {
        const u8* limit = end - 32u;
        u64 v1 = SIM_HASH_SEED + STATE_HASH_P1 + STATE_HASH_P2;
        u64 v2 = SIM_HASH_SEED + STATE_HASH_P2;
        u64 v3 = SIM_HASH_SEED;
        u64 v4 = SIM_HASH_SEED - STATE_HASH_P1;

        do {
            u64 k1, k2, k3, k4;
            __builtin_memcpy(&k1, p,       sizeof(u64));
            __builtin_memcpy(&k2, p + 8u,  sizeof(u64));
            __builtin_memcpy(&k3, p + 16u, sizeof(u64));
            __builtin_memcpy(&k4, p + 24u, sizeof(u64));
            v1 = state_round64(v1, k1);
            v2 = state_round64(v2, k2);
            v3 = state_round64(v3, k3);
            v4 = state_round64(v4, k4);
            p += 32u;
        } while (p <= limit);

        h = state_rotl64(v1, 1u)  + state_rotl64(v2, 7u) +
            state_rotl64(v3, 12u) + state_rotl64(v4, 18u);
        h = state_merge_round64(h, v1);
        h = state_merge_round64(h, v2);
        h = state_merge_round64(h, v3);
        h = state_merge_round64(h, v4);
    } else {
        h = SIM_HASH_SEED + STATE_HASH_P5;
    }

    h += (u64)len;

    while (p + 8u <= end) {
        u64 k = 0ULL;
        __builtin_memcpy(&k, p, sizeof(u64));
        k *= STATE_HASH_P2;
        k  = state_rotl64(k, 31u);
        k *= STATE_HASH_P1;
        h ^= k;
        h  = state_rotl64(h, 27u) * STATE_HASH_P1 + STATE_HASH_P4;
        p += 8u;
    }

    if (p + 4u <= end) {
        u32 k32 = 0u;
        __builtin_memcpy(&k32, p, sizeof(u32));
        h ^= (u64)k32 * STATE_HASH_P1;
        h  = state_rotl64(h, 23u) * STATE_HASH_P2 + STATE_HASH_P3;
        p += 4u;
    }

    while (p < end) {
        h ^= (u64)(*p) * STATE_HASH_P5;
        h  = state_rotl64(h, 11u) * STATE_HASH_P1;
        p++;
    }

    h ^= h >> 33u;
    h *= STATE_HASH_P2;
    h ^= h >> 29u;
    h *= STATE_HASH_P3;
    h ^= h >> 32u;
    return h;
}

u64 state_region_hash_active(const void* base) {
    if (!base) return 0ULL;
    const StateRegionHeader* hdr = state_region_header_const(base);
    return state_region_hash_bytes(base, (usize)hdr->arena.used);
}

u64 state_region_hash_full(const void* base, u32 cap) {
    if (!base || cap == 0u) return 0ULL;
    return state_region_hash_bytes(base, (usize)cap);
}

/* ----------------------------------------------------------------------------
 * StateSlotTable (Nesil / Sürüm Kontrollü Tutamaç Tablosu)
 * ---------------------------------------------------------------------------- */

void state_slots_init(StateSlotTable* t, RegionArena* ra, void* base, u32 cap) {
    ASSERT(t != NULL);
    ASSERT(ra != NULL);
    ASSERT(base != NULL);
    ASSERT(cap > 0u && cap <= STATE_HANDLE_MAX_INDEX);

    __builtin_memset(t, 0, sizeof(*t));
    t->cap        = cap;
    t->count      = 0u;
    t->high_water = 0u;
    t->free_count = 0u;
    t->gens       = STATE_ALLOC_ARRAY(ra, base, u16, cap);
    t->alive      = STATE_ALLOC_ARRAY(ra, base, u8,  cap);
    t->free_list  = STATE_ALLOC_ARRAY(ra, base, u32, cap);
    t->_pad       = 0u;

    /* Tüm başlangıç nesillerini 1 yap (0 nesli geçersiz tutamaçlar için ayrılmıştır) */
    u16* gens = STATE_DEREF(base, u16, t->gens);
    for (u32 i = 0u; i < cap; i++) {
        gens[i] = 1u;
    }
}

StateHandle state_slots_alloc(StateSlotTable* t, void* base) {
    ASSERT(t != NULL);
    ASSERT(base != NULL);

    u32 idx = 0u;
    if (t->free_count > 0u) {
        u32* fl = STATE_DEREF(base, u32, t->free_list);
        t->free_count--;
        idx = fl[t->free_count];
        /* Çekilen serbest liste yuvasını sıfırla (ham bellek özeti geçmişten bağımsız kalsın) */
        fl[t->free_count] = 0u;
    } else {
        if (t->high_water >= t->cap) {
            return STATE_HANDLE_NULL;
        }
        idx = t->high_water++;
    }

    u16* gens  = STATE_DEREF(base, u16, t->gens);
    u8*  alive = STATE_DEREF(base, u8,  t->alive);
    if (gens[idx] == 0u) {
        gens[idx] = 1u;
    }
    alive[idx] = 1u;
    t->count++;
    return state_handle_make(idx, (u32)gens[idx]);
}

b32 state_slots_valid(const StateSlotTable* t, const void* base, StateHandle h) {
    if (!t || !base || !state_handle_non_null(h)) return FALSE;
    u32 idx = state_handle_index(h);
    u32 gen = state_handle_gen(h);
    if (idx >= t->high_water) return FALSE;

    const u8*  alive = STATE_DEREF_CONST(base, u8,  t->alive);
    const u16* gens  = STATE_DEREF_CONST(base, u16, t->gens);
    return alive[idx] != 0u && (u32)gens[idx] == gen;
}

b32 state_slots_free(StateSlotTable* t, void* base, StateHandle h) {
    if (!state_slots_valid(t, base, h)) return FALSE;

    u32 idx = state_handle_index(h);
    u16* gens  = STATE_DEREF(base, u16, t->gens);
    u8*  alive = STATE_DEREF(base, u8,  t->alive);
    u32* fl    = STATE_DEREF(base, u32, t->free_list);

    alive[idx] = 0u;
    /* Nesil sayacını 1..4095 aralığında sarmalı artır (0'ı atla) */
    u32 next_gen = ((u32)gens[idx] + 1u) & STATE_HANDLE_GEN_MASK;
    if (next_gen == 0u) next_gen = 1u;
    gens[idx] = (u16)next_gen;

    fl[t->free_count++] = idx;
    t->count--;
    return TRUE;
}

u16 state_slots_gen_at(const StateSlotTable* t, const void* base, u32 index) {
    ASSERT(t != NULL);
    ASSERT(base != NULL);
    ASSERT(index < t->cap);
    const u16* gens = STATE_DEREF_CONST(base, u16, t->gens);
    return gens[index];
}
