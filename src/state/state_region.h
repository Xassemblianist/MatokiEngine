/* state/state_region.h — Taban adres saklamayan ofset tabanlı durum bölgesi (D-112, ARCH_V2 §2/§6).
 * Durum bölgesi içinde ham C işaretçisi (T*) veya platforma bağlı boyutlu tip (usize/uptr) yasaktır.
 * Tüm başvurular bölge tabanından 32-bit bayt ofseti (StateRef/StateSlice) veya nesil sayaçlı
 * tutamaç (StateHandle) ile yapılır; böylece bölge farklı taban adrese memcpy ile taşındığında
 * düzeltme (fixup) gerekmeden çalışır ve ham bayt özeti (hash) adres-bağımsız kalır.
 */
#ifndef STATE_STATE_REGION_H
#define STATE_STATE_REGION_H

#include "core/types.h"
#include "core/math.h"
#include "core/log.h"

#define STATE_REGION_MAGIC     0x4154534Du /* "MSTA" (little-endian) */
#define STATE_REGION_VERSION   1u
#define STATE_NULL_OFFSET      0xFFFFFFFFu
#define STATE_MAX_ALIGN        64u

/* ----------------------------------------------------------------------------
 * İşaretçi Yasağı ve Yerleşim Doğrulama Makroları (_Static_assert)
 *
 * Clang __builtin_classify_type(expr) == 5 ise ifade işaretçi (pointer) tipindedir.
 * Durum başlıklarındaki (state_*.h) her yapı alanı bu makro ile derleme zamanında
 * doğrulanır; ayrıca tools/host/check_state_headers.sh betiği kaynak kodunu denetler.
 * ---------------------------------------------------------------------------- */

#define STATE_CLASSIFY_POINTER 5

#define STATE_ASSERT_NO_PTR_FIELD(T, field) \
    _Static_assert(__builtin_classify_type(((const T*)0)->field) != STATE_CLASSIFY_POINTER, \
                   #T "." #field " alani isaretci (pointer) olamaz")

#define STATE_ASSERT_VALUE_STRUCT(T, expected_bytes) \
    _Static_assert(sizeof(T) == (expected_bytes), \
                   #T " boyutu wasm32 ve 64-bit host'ta sabit olmali"); \
    _Static_assert(_Alignof(T) <= 16u, \
                   #T " hizalamasi 16 bayti asmamali")

/* ----------------------------------------------------------------------------
 * Temel Ofset ve Tipli Başvuru (StateRef<T> / StateSlice<T>) Makroları
 * ---------------------------------------------------------------------------- */

typedef u32 StateOffset;

#define STATE_REF_DEFINE(T) \
    typedef struct { StateOffset off; } StateRef_##T; \
    typedef struct { StateOffset off; u32 count; } StateSlice_##T; \
    STATE_ASSERT_NO_PTR_FIELD(StateRef_##T, off); \
    STATE_ASSERT_VALUE_STRUCT(StateRef_##T, 4u); \
    STATE_ASSERT_NO_PTR_FIELD(StateSlice_##T, off); \
    STATE_ASSERT_NO_PTR_FIELD(StateSlice_##T, count); \
    STATE_ASSERT_VALUE_STRUCT(StateSlice_##T, 8u)

#define StateRef(T)   StateRef_##T
#define StateSlice(T) StateSlice_##T

/* Temel motor tipleri için tipli StateRef / StateSlice tanımları */
STATE_REF_DEFINE(u8);
STATE_REF_DEFINE(u16);
STATE_REF_DEFINE(u32);
STATE_REF_DEFINE(u64);
STATE_REF_DEFINE(i8);
STATE_REF_DEFINE(i16);
STATE_REF_DEFINE(i32);
STATE_REF_DEFINE(i64);
STATE_REF_DEFINE(f32);
STATE_REF_DEFINE(f64);
STATE_REF_DEFINE(Vec2);
STATE_REF_DEFINE(Vec3);
STATE_REF_DEFINE(Vec4);

#define STATE_REF_NULL(T)          ((StateRef(T)){ STATE_NULL_OFFSET })
#define STATE_REF_MAKE(T, offset)  ((StateRef(T)){ (StateOffset)(offset) })
#define STATE_REF_IS_NULL(ref)     ((ref).off == STATE_NULL_OFFSET)
#define STATE_REF_EQ(a, b)         ((a).off == (b).off)

#define STATE_SLICE_EMPTY(T)            ((StateSlice(T)){ STATE_NULL_OFFSET, 0u })
#define STATE_SLICE_MAKE(T, off_, cnt_) ((StateSlice(T)){ (StateOffset)(off_), (u32)(cnt_) })
#define STATE_SLICE_IS_EMPTY(s)         ((s).count == 0u || (s).off == STATE_NULL_OFFSET)

/* ----------------------------------------------------------------------------
 * Sürüm / Nesil Kontrollü Tutamaçlar (StateHandle / StateHandleOf<T>)
 *
 * Bit yerleşimi: [31..20: 12-bit nesil (1..4095)] [19..0: 20-bit yuva indeksi]
 * Nesil 0 hiçbir zaman geçerli bir tutamaçta kullanılmaz; böylece 0 (STATE_HANDLE_NULL)
 * her zaman geçersiz (null) tutamaçtır.
 * ---------------------------------------------------------------------------- */

typedef u32 StateHandle;
STATE_REF_DEFINE(StateHandle);

#define STATE_HANDLE_NULL       0u
#define STATE_HANDLE_INDEX_BITS 20u
#define STATE_HANDLE_GEN_BITS   12u
#define STATE_HANDLE_INDEX_MASK ((1u << STATE_HANDLE_INDEX_BITS) - 1u)
#define STATE_HANDLE_GEN_MASK   ((1u << STATE_HANDLE_GEN_BITS) - 1u)
#define STATE_HANDLE_MAX_INDEX  STATE_HANDLE_INDEX_MASK

#define STATE_HANDLE_DEFINE(Tag) \
    typedef struct { StateHandle id; } StateHandle_##Tag; \
    STATE_ASSERT_NO_PTR_FIELD(StateHandle_##Tag, id); \
    STATE_ASSERT_VALUE_STRUCT(StateHandle_##Tag, 4u); \
    STATE_REF_DEFINE(StateHandle_##Tag)

#define StateHandleOf(Tag)          StateHandle_##Tag
#define STATE_HANDLE_TYPED_NULL(Tag) ((StateHandleOf(Tag)){ STATE_HANDLE_NULL })
#define STATE_HANDLE_TYPED_MAKE(Tag, raw_h) ((StateHandleOf(Tag)){ (raw_h) })
#define STATE_HANDLE_TYPED_EQ(a, b) ((a).id == (b).id)

static inline u32 state_handle_index(StateHandle h) {
    return h & STATE_HANDLE_INDEX_MASK;
}

static inline u32 state_handle_gen(StateHandle h) {
    return (h >> STATE_HANDLE_INDEX_BITS) & STATE_HANDLE_GEN_MASK;
}

static inline StateHandle state_handle_make(u32 index, u32 gen) {
    u32 g = gen & STATE_HANDLE_GEN_MASK;
    if (g == 0u) g = 1u;
    return (g << STATE_HANDLE_INDEX_BITS) | (index & STATE_HANDLE_INDEX_MASK);
}

static inline b32 state_handle_non_null(StateHandle h) {
    return h != STATE_HANDLE_NULL && state_handle_gen(h) != 0u;
}

/* ----------------------------------------------------------------------------
 * RegionArena — Taban Adres Saklamayan Doğrusal Ofset Ayırıcı
 *
 * Hizalama mutlak bellek adresine göre değil, bölge başlangıcından olan ofsete
 * (used) göre yapılır. Böylece bölgenin iç yerleşimi taban adresten tamamen bağımsızdır.
 * ---------------------------------------------------------------------------- */

typedef struct {
    u32 cap;   /* bölge toplam bayt kapasitesi */
    u32 used;  /* ayrılmış (aktif) bayt ofseti */
    u32 _pad0; /* 16-bayt hizalama dolgusu (her zaman 0) */
    u32 _pad1; /* 16-bayt hizalama dolgusu (her zaman 0) */
} RegionArena;

STATE_ASSERT_NO_PTR_FIELD(RegionArena, cap);
STATE_ASSERT_NO_PTR_FIELD(RegionArena, used);
STATE_ASSERT_NO_PTR_FIELD(RegionArena, _pad0);
STATE_ASSERT_NO_PTR_FIELD(RegionArena, _pad1);
STATE_ASSERT_VALUE_STRUCT(RegionArena, 16u);

/* ----------------------------------------------------------------------------
 * StateSlotTable — İşaretçisiz Nesil Kontrollü Yuva Tablosu
 *
 * Dinamik dizilerini RegionArena içinde StateRef ofsetleriyle tutar.
 * Silinen yuvalar LIFO serbest listesine girer ve nesil sayacı (1..4095) artırılır;
 * eski tutamaçla erişim anında tespit edilir (use-after-free koruması).
 * ---------------------------------------------------------------------------- */

typedef struct {
    u32           cap;        /* maksimum yuva kapasitesi */
    u32           count;      /* canlı yuva sayısı */
    u32           high_water; /* şimdiye dek kullanılan en yüksek yuva indeksi + 1 */
    u32           free_count; /* serbest listedeki eleman sayısı */
    StateRef(u16) gens;       /* [cap] yuva başına nesil sayacı */
    StateRef(u8)  alive;      /* [cap] yuva canlılık bayrağı (0 veya 1) */
    StateRef(u32) free_list;  /* [cap] serbest yuva indeks yığını */
    u32           _pad;       /* 32 bayta hizalama dolgusu (0) */
} StateSlotTable;

STATE_ASSERT_NO_PTR_FIELD(StateSlotTable, cap);
STATE_ASSERT_NO_PTR_FIELD(StateSlotTable, count);
STATE_ASSERT_NO_PTR_FIELD(StateSlotTable, high_water);
STATE_ASSERT_NO_PTR_FIELD(StateSlotTable, free_count);
STATE_ASSERT_NO_PTR_FIELD(StateSlotTable, gens);
STATE_ASSERT_NO_PTR_FIELD(StateSlotTable, alive);
STATE_ASSERT_NO_PTR_FIELD(StateSlotTable, free_list);
STATE_ASSERT_NO_PTR_FIELD(StateSlotTable, _pad);
STATE_ASSERT_VALUE_STRUCT(StateSlotTable, 32u);
STATE_REF_DEFINE(RegionArena);
STATE_REF_DEFINE(StateSlotTable);

/* ----------------------------------------------------------------------------
 * StateRegionHeader — Durum Bölgesi Başlığı (Ofset 0, 64 bayt)
 * ---------------------------------------------------------------------------- */

typedef struct {
    u32         magic;       /* STATE_REGION_MAGIC */
    u32         version;     /* STATE_REGION_VERSION */
    u32         abi_hash;    /* şema / derleme ABI özeti */
    StateOffset root_off;    /* kullanıcı kök durum yapısının ofseti */
    u64         tick;        /* simülasyon tick sayacı */
    u64         flags;       /* durum bayrakları */
    RegionArena arena;       /* gömülü ofset ayırıcı (16 bayt) */
    u32         reserved[4]; /* 64 bayt hizalama dolgusu (her zaman 0) */
} StateRegionHeader;

STATE_ASSERT_NO_PTR_FIELD(StateRegionHeader, magic);
STATE_ASSERT_NO_PTR_FIELD(StateRegionHeader, version);
STATE_ASSERT_NO_PTR_FIELD(StateRegionHeader, abi_hash);
STATE_ASSERT_NO_PTR_FIELD(StateRegionHeader, root_off);
STATE_ASSERT_NO_PTR_FIELD(StateRegionHeader, tick);
STATE_ASSERT_NO_PTR_FIELD(StateRegionHeader, flags);
STATE_ASSERT_NO_PTR_FIELD(StateRegionHeader, arena);
STATE_ASSERT_NO_PTR_FIELD(StateRegionHeader, reserved[0]);
STATE_ASSERT_VALUE_STRUCT(StateRegionHeader, 64u);

/* ----------------------------------------------------------------------------
 * Ofset Çözümleme Yardımcıları ve Makroları
 * ---------------------------------------------------------------------------- */

static inline void* state_resolve_ptr(void* base, StateOffset off) {
    ASSERT(base != NULL);
    ASSERT(off != STATE_NULL_OFFSET);
    return (u8*)base + (usize)off;
}

static inline const void* state_resolve_ptr_const(const void* base, StateOffset off) {
    ASSERT(base != NULL);
    ASSERT(off != STATE_NULL_OFFSET);
    return (const u8*)base + (usize)off;
}

static inline void* state_resolve_checked(const RegionArena* ra, void* base,
                                          StateOffset off, u32 size) {
    ASSERT(ra != NULL);
    ASSERT(base != NULL);
    ASSERT(off != STATE_NULL_OFFSET);
    ASSERT(off <= ra->used && size <= ra->used - off);
    return (u8*)base + (usize)off;
}

static inline const void* state_resolve_checked_const(const RegionArena* ra, const void* base,
                                                      StateOffset off, u32 size) {
    ASSERT(ra != NULL);
    ASSERT(base != NULL);
    ASSERT(off != STATE_NULL_OFFSET);
    ASSERT(off <= ra->used && size <= ra->used - off);
    return (const u8*)base + (usize)off;
}

/* Ham ofset çözümleme makroları */
#define STATE_PTR(base, T, off) \
    ((T*)state_resolve_ptr((base), (off)))

#define STATE_PTR_CONST(base, T, off) \
    ((const T*)state_resolve_ptr_const((base), (off)))

/* Tipli StateRef<T> çözümleme makroları */
#define STATE_DEREF(base, T, ref) \
    ((T*)state_resolve_ptr((base), (ref).off))

#define STATE_DEREF_CONST(base, T, ref) \
    ((const T*)state_resolve_ptr_const((base), (ref).off))

#define STATE_DEREF_CHECKED(ra, base, T, ref) \
    ((T*)state_resolve_checked((ra), (base), (ref).off, (u32)sizeof(T)))

#define STATE_DEREF_CHECKED_CONST(ra, base, T, ref) \
    ((const T*)state_resolve_checked_const((ra), (base), (ref).off, (u32)sizeof(T)))

static inline void* state_slice_elem(void* base, StateOffset off, u32 count,
                                     u32 idx, u32 elem_size) {
    ASSERT(base != NULL);
    ASSERT(off != STATE_NULL_OFFSET);
    ASSERT(idx < count);
    return (u8*)base + (usize)off + (usize)idx * (usize)elem_size;
}

static inline const void* state_slice_elem_const(const void* base, StateOffset off, u32 count,
                                                 u32 idx, u32 elem_size) {
    ASSERT(base != NULL);
    ASSERT(off != STATE_NULL_OFFSET);
    ASSERT(idx < count);
    return (const u8*)base + (usize)off + (usize)idx * (usize)elem_size;
}

/* Tipli StateSlice<T> çözümleme ve sınır denetimli indeksleme makroları */
#define STATE_SLICE_PTR(base, T, slice) \
    ((T*)state_resolve_ptr((base), (slice).off))

#define STATE_SLICE_PTR_CONST(base, T, slice) \
    ((const T*)state_resolve_ptr_const((base), (slice).off))

#define STATE_AT(base, T, slice, idx) \
    ((T*)state_slice_elem((base), (slice).off, (slice).count, (u32)(idx), (u32)sizeof(T)))

#define STATE_AT_CONST(base, T, slice, idx) \
    ((const T*)state_slice_elem_const((base), (slice).off, (slice).count, (u32)(idx), (u32)sizeof(T)))

/* Tipli ayırma makroları (StateRef<T> / StateSlice<T> döner) */
#define STATE_ALLOC(ra, base, T) \
    ((StateRef(T)){ region_alloc((ra), (base), (u32)sizeof(T), (u32)_Alignof(T)) })

#define STATE_ALLOC_ARRAY(ra, base, T, n) \
    ((StateRef(T)){ region_alloc((ra), (base), (u32)(sizeof(T) * (usize)(n)), (u32)_Alignof(T)) })

#define STATE_ALLOC_SLICE(ra, base, T, n) \
    ((StateSlice(T)){ \
        region_alloc((ra), (base), (u32)(sizeof(T) * (usize)(n)), (u32)_Alignof(T)), \
        (u32)(n) \
    })

/* ----------------------------------------------------------------------------
 * RegionArena ve StateRegion Fonksiyonları
 * ---------------------------------------------------------------------------- */

void        region_arena_init(RegionArena* ra, u32 cap, u32 initial_used);
StateOffset region_alloc_raw(RegionArena* ra, u32 size, u32 align);
StateOffset region_alloc(RegionArena* ra, void* base, u32 size, u32 align);
void        region_arena_reset(RegionArena* ra, void* base, u32 initial_used);
u32         region_arena_mark(const RegionArena* ra);
void        region_arena_pop_to(RegionArena* ra, void* base, u32 mark);
RegionArena region_arena_sub(RegionArena* parent, void* base, u32 sub_cap, StateOffset* out_base_off);

/* Başlıklı durum bölgesi yaşam döngüsü, anlık görüntü (snapshot) ve taşıma (relocate) */
StateRegionHeader*       state_region_init(void* base, u32 cap, u32 abi_hash);
StateRegionHeader*       state_region_header(void* base);
const StateRegionHeader* state_region_header_const(const void* base);
RegionArena*             state_region_arena(void* base);
b32                      state_region_validate(const void* base, u32 cap, u32 expected_abi_hash);

StateOffset state_region_alloc(void* base, u32 size, u32 align);
void        state_region_set_root(void* base, StateOffset root_off);
StateOffset state_region_root(const void* base);

/* Anlık görüntü (snapshot) = bölgenin düz memcpy kopyası; farklı taban adrese doğrudan kopyalanır */
void state_region_snapshot_save(const void* src_base, void* dst_snap, u32 bytes);
void state_region_snapshot_restore(void* dst_base, const void* src_snap, u32 bytes);
b32  state_region_relocate(void* new_base, const void* old_base, u32 cap);

/* Durum bölgesi özeti (hash): aktif kullanılan bölge [0..used) veya tam bölge [0..bytes) */
u64  state_region_hash_bytes(const void* data, usize len);
u64  state_region_hash_active(const void* base);
u64  state_region_hash_full(const void* base, u32 cap);

/* Kanonik kayan nokta yardımcısı: -0.0f -> +0.0f ve tüm NaN -> tek kanonik qNaN */
static inline f32 state_canon_f32(f32 x) {
    u32 bits = 0u;
    __builtin_memcpy(&bits, &x, sizeof(u32));
    if ((bits & 0x7FFFFFFFu) == 0u) {
        bits = 0u;
    } else if ((bits & 0x7F800000u) == 0x7F800000u && (bits & 0x007FFFFFu) != 0u) {
        bits = 0x7FC00000u;
    }
    f32 out = 0.0f;
    __builtin_memcpy(&out, &bits, sizeof(f32));
    return out;
}

static inline Vec3 state_canon_vec3(Vec3 v) {
    return vec3(state_canon_f32(v.x), state_canon_f32(v.y), state_canon_f32(v.z));
}

/* ----------------------------------------------------------------------------
 * StateSlotTable Fonksiyonları
 * ---------------------------------------------------------------------------- */

void        state_slots_init(StateSlotTable* t, RegionArena* ra, void* base, u32 cap);
StateHandle state_slots_alloc(StateSlotTable* t, void* base);
b32         state_slots_free(StateSlotTable* t, void* base, StateHandle h);
b32         state_slots_valid(const StateSlotTable* t, const void* base, StateHandle h);
u16         state_slots_gen_at(const StateSlotTable* t, const void* base, u32 index);

#endif
