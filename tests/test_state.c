/* tests/test_state.c — Ofset tabanlı durum bölgesi (S1) birim testleri ve 16 MB ölçümü.
 * RegionArena, StateRef/StateSlice tipli ofsetler, sürüm/nesil kontrollü tutamaçlar,
 * state_*.h işaretçi yasağı betik denetimi, farklı taban adreslere taşınan SoA ECS +
 * fizik dünyasının bit-bit aynı özeti (hash) ürettiği kanıtı ve 16 MB kopya+hash ölçümü.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "test.h"
#include "core/types.h"
#include "core/math.h"
#include "plat/platform.h"
#include "sim/sim.h"
#include "state/state_region.h"
#include "state/state_world.h"

/* Farklı taban adreslerde test için 16-bayt hizalı statik tamponlar */
static _Alignas(64) u8 g_reg_buf_a[KB(256)];
static _Alignas(64) u8 g_reg_buf_b[KB(256)];
static _Alignas(64) u8 g_reg_buf_c[KB(256)];
static _Alignas(64) u8 g_snap_buf[KB(256)];

/* 16 MB benchmark için 64-bayt hizalı iki ayrı 16 MB bölge */
static _Alignas(64) u8 g_bench_16m_src[MB(16)];
static _Alignas(64) u8 g_bench_16m_dst[MB(16)];

/* ============================================================================
 * 1. RegionArena ve StateRef<T> / StateSlice<T> Birim Testleri
 * ============================================================================ */

TEST(state_region_arena_and_typed_refs) {
    /* Farklı taban adreslerde başlatılan iki bölgede tüm ofsetler birebir aynı olmalı */
    u8* base_a = g_reg_buf_a;
    u8* base_b = g_reg_buf_b + 32; /* kasıtlı farklı hizalama/taban adresi (32-bayt kayık) */
    const u32 CAP = KB(16);

    state_region_init(base_a, CAP, 0x12345678u);
    state_region_init(base_b, CAP, 0x12345678u);
    CHECK((uptr)base_a != (uptr)base_b);

    RegionArena* ra_a = state_region_arena(base_a);
    RegionArena* ra_b = state_region_arena(base_b);

    StateRef(u32)  r_u32_a = STATE_ALLOC(ra_a, base_a, u32);
    StateRef(u32)  r_u32_b = STATE_ALLOC(ra_b, base_b, u32);
    CHECK(STATE_REF_EQ(r_u32_a, r_u32_b));

    /* Tek bayt ayırıp ardından 16-bayt hizalı Vec4 dilimi ayır: aradaki dolgu sıfırlanmalı */
    StateRef(u8)      r_u8_a  = STATE_ALLOC(ra_a, base_a, u8);
    StateRef(u8)      r_u8_b  = STATE_ALLOC(ra_b, base_b, u8);
    StateSlice(Vec4)  s_v4_a  = STATE_ALLOC_SLICE(ra_a, base_a, Vec4, 8u);
    StateSlice(Vec4)  s_v4_b  = STATE_ALLOC_SLICE(ra_b, base_b, Vec4, 8u);

    CHECK(STATE_REF_EQ(r_u8_a, r_u8_b));
    CHECK(s_v4_a.off == s_v4_b.off && s_v4_a.count == 8u);
    CHECK((s_v4_a.off & ((u32)_Alignof(Vec4) - 1u)) == 0u);

    /* Açık 16-bayt hizalama isteği her iki taban adreste de 16'nın katı ofset dönmeli */
    StateOffset simd_off_a = region_alloc(ra_a, base_a, 32u, 16u);
    StateOffset simd_off_b = region_alloc(ra_b, base_b, 32u, 16u);
    CHECK(simd_off_a == simd_off_b);
    CHECK((simd_off_a & 15u) == 0u);

    *STATE_DEREF_CHECKED(ra_a, base_a, u32, r_u32_a) = 0xCAFEBABEu;
    *STATE_DEREF_CHECKED(ra_b, base_b, u32, r_u32_b) = 0xCAFEBABEu;
    *STATE_DEREF_CHECKED(ra_a, base_a, u8,  r_u8_a)  = 42u;
    *STATE_DEREF_CHECKED(ra_b, base_b, u8,  r_u8_b)  = 42u;

    for (u32 i = 0u; i < s_v4_a.count; i++) {
        Vec4 v = vec4((f32)i, (f32)(i * 2u), -0.0f, 1.0f);
        v.z = state_canon_f32(v.z);
        *STATE_AT(base_a, Vec4, s_v4_a, i) = v;
        *STATE_AT(base_b, Vec4, s_v4_b, i) = v;
    }

    /* Mark / pop_to geri alma sonrasında serbest kalan bölge sıfırlanmalı ve özet korunmalı */
    u64 hash_before_temp = state_region_hash_active(base_a);
    u32 mark = region_arena_mark(ra_a);
    StateSlice(u64) temp_slice = STATE_ALLOC_SLICE(ra_a, base_a, u64, 32u);
    for (u32 i = 0u; i < 32u; i++) {
        *STATE_AT(base_a, u64, temp_slice, i) = 0xFFFFFFFFFFFFFFFFULL;
    }
    region_arena_pop_to(ra_a, base_a, mark);
    CHECK(ra_a->used == mark);
    CHECK(state_region_hash_active(base_a) == hash_before_temp);
    CHECK(state_region_hash_active(base_a) == state_region_hash_active(base_b));
    CHECK(state_region_hash_full(base_a, CAP) == state_region_hash_full(base_b, CAP));
}

/* ============================================================================
 * 2. Nesil / Sürüm Kontrollü Tutamaçlar (StateHandle / StateSlotTable) Testi
 * ============================================================================ */

TEST(state_handles_and_slot_table_generations) {
    u8* base = g_reg_buf_a;
    const u32 CAP = KB(16);
    state_region_init(base, CAP, 0x11112222u);
    RegionArena* ra = state_region_arena(base);

    StateRef(StateSlotTable) tbl_ref = STATE_ALLOC(ra, base, StateSlotTable);
    StateSlotTable* tbl = STATE_DEREF(base, StateSlotTable, tbl_ref);
    state_slots_init(tbl, ra, base, 8u);

    StateHandle h0 = state_slots_alloc(tbl, base);
    StateHandle h1 = state_slots_alloc(tbl, base);
    CHECK(state_slots_valid(tbl, base, h0));
    CHECK(state_slots_valid(tbl, base, h1));
    CHECK(state_handle_index(h0) == 0u && state_handle_gen(h0) == 1u);
    CHECK(state_handle_index(h1) == 1u && state_handle_gen(h1) == 1u);
    CHECK(tbl->count == 2u);

    /* h0'ı sil: eski tutamaç (gen=1) geçersiz olmalı, çift silme FALSE dönmeli */
    CHECK(state_slots_free(tbl, base, h0));
    CHECK(!state_slots_valid(tbl, base, h0));
    CHECK(!state_slots_free(tbl, base, h0));
    CHECK(tbl->count == 1u);

    /* Yeni ayırma LIFO serbest listeden 0. yuvayı nesil=2 ile geri vermeli */
    StateHandle h0_v2 = state_slots_alloc(tbl, base);
    CHECK(state_handle_index(h0_v2) == 0u);
    CHECK(state_handle_gen(h0_v2) == 2u);
    CHECK(!state_slots_valid(tbl, base, h0));   /* eski nesil hala geçersiz */
    CHECK(state_slots_valid(tbl, base, h0_v2)); /* yeni nesil geçerli */

    /* Nesil sayacı 12-bit sınırında (4095 -> 1) 0'ı atlayarak sarmalı (wrap-around) */
    StateHandle cur = h0_v2;
    for (u32 step = 0u; step < 4096u; step++) {
        CHECK(state_slots_free(tbl, base, cur));
        cur = state_slots_alloc(tbl, base);
        CHECK(state_handle_non_null(cur));
        CHECK(state_handle_gen(cur) != 0u);
        CHECK(state_slots_valid(tbl, base, cur));
    }

    /* Bölgeyi farklı taban adrese taşı: tutamaç doğrulaması yeni adreste de çalışmalı */
    CHECK(state_region_relocate(g_reg_buf_b, base, CAP));
    const StateSlotTable* tbl_b = STATE_DEREF_CONST(g_reg_buf_b, StateSlotTable, tbl_ref);
    CHECK(state_slots_valid(tbl_b, g_reg_buf_b, cur));
    CHECK(state_slots_valid(tbl_b, g_reg_buf_b, h1));
    CHECK(!state_slots_valid(tbl_b, g_reg_buf_b, h0));
}

/* ============================================================================
 * 3. state_*.h İşaretçi Yasağı Denetimi Testi (Kasıtlı Hatalı Örneklerde Başarısız Olur)
 * ============================================================================ */

static int run_header_check(const char* header_path) {
    char cmd[512];
    snprintf(cmd, sizeof(cmd), "./tools/host/check_state_headers.sh %s >/dev/null 2>&1", header_path);
    return system(cmd);
}

static void write_text_file(const char* path, const char* content) {
    FILE* f = fopen(path, "w");
    CHECK(f != NULL);
    if (f) {
        fputs(content, f);
        fclose(f);
    }
}

TEST(state_header_pointer_check_rejects_bad_header) {
    /* 1) Gerçek state_*.h başlıklarımız denetimden hatasız (0) geçmeli */
    CHECK(run_header_check("src/state/state_region.h") == 0);
    CHECK(run_header_check("src/state/state_world.h") == 0);

    /* 2) Kasıtlı hatalı örnek A: struct içinde ham işaretçi (u32* bad_ptr) */
    const char* bad_ptr_path = "build/test_state_bad_ptr.h";
    write_text_file(bad_ptr_path,
        "#ifndef TEST_BAD_PTR_H\n"
        "#define TEST_BAD_PTR_H\n"
        "#include \"core/types.h\"\n"
        "typedef struct {\n"
        "    u32  id;\n"
        "    u32* bad_ptr;\n"
        "} BadStateWithPtr;\n"
        "_Static_assert(sizeof(BadStateWithPtr) > 0, \"ok\");\n"
        "#endif\n");
    CHECK(run_header_check(bad_ptr_path) != 0);
    remove(bad_ptr_path);

    /* 3) Kasıtlı hatalı örnek B: struct içinde fonksiyon işaretçisi */
    const char* bad_fn_path = "build/test_state_bad_fnptr.h";
    write_text_file(bad_fn_path,
        "#ifndef TEST_BAD_FNPTR_H\n"
        "#define TEST_BAD_FNPTR_H\n"
        "#include \"core/types.h\"\n"
        "typedef struct {\n"
        "    u32 tick;\n"
        "    void (*callback)(void);\n"
        "} BadStateWithFnPtr;\n"
        "_Static_assert(sizeof(BadStateWithFnPtr) > 0, \"ok\");\n"
        "#endif\n");
    CHECK(run_header_check(bad_fn_path) != 0);
    remove(bad_fn_path);

    /* 4) Kasıtlı hatalı örnek C: platforma bağlı boyutlu tip (usize) */
    const char* bad_usize_path = "build/test_state_bad_usize.h";
    write_text_file(bad_usize_path,
        "#ifndef TEST_BAD_USIZE_H\n"
        "#define TEST_BAD_USIZE_H\n"
        "#include \"core/types.h\"\n"
        "typedef struct {\n"
        "    usize host_dependent_count;\n"
        "} BadStateWithUsize;\n"
        "_Static_assert(sizeof(BadStateWithUsize) > 0, \"ok\");\n"
        "#endif\n");
    CHECK(run_header_check(bad_usize_path) != 0);
    remove(bad_usize_path);

    /* 5) Kasıtlı hatalı örnek D: _Static_assert doğrulaması olmayan başlık */
    const char* bad_noassert_path = "build/test_state_bad_noassert.h";
    write_text_file(bad_noassert_path,
        "#ifndef TEST_BAD_NOASSERT_H\n"
        "#define TEST_BAD_NOASSERT_H\n"
        "#include \"core/types.h\"\n"
        "typedef struct { u32 x; } StateWithoutStaticAssert;\n"
        "#endif\n");
    CHECK(run_header_check(bad_noassert_path) != 0);
    remove(bad_noassert_path);
}

/* ============================================================================
 * 4. Asıl Kanıt: Farklı Taban Adrese Kopyalama, Orada Çalıştırma ve Aynı Hash
 * ============================================================================ */

static void run_scripted_mutations(void* base, u32 step) {
    StateWorld* w = state_world_get(base);
    StateEcsSoA* ecs = &w->ecs;

    /* Belirli adımlarda varlık sil ve yeni varlık+fizik gövdesi ekle (nesil/serbest liste testi) */
    if (step % 15u == 5u && ecs->dense_count > 8u) {
        const StateHandle* d_ent = STATE_SLICE_PTR_CONST(base, StateHandle, ecs->dense_entity);
        u32 victim_dense = step % ecs->dense_count;
        StateHandleOf(Entity) victim = STATE_HANDLE_TYPED_MAKE(Entity, d_ent[victim_dense]);
        CHECK(state_ecs_destroy(base, victim));
        CHECK(!state_ecs_alive(base, victim));
    }

    if (step % 15u == 10u && ecs->dense_count + 1u < ecs->max_entities) {
        f32 s = (f32)(step % 23u) * 0.25f - 2.5f;
        Vec3 pos = vec3(s, 3.0f + (f32)(step % 5u) * 0.5f, -s * 0.7f);
        Vec3 vel = vec3(-s * 0.8f, 1.5f, s * 0.6f);
        u32 mask = STATE_COMP_TRANSFORM | STATE_COMP_VELOCITY | STATE_COMP_HEALTH;
        StateHandleOf(Entity) e = state_ecs_spawn(base, pos, vel, 120, mask);
        CHECK(state_ecs_alive(base, e));

        StateHandleOf(PhysBody) b = state_phys_add_body(base, pos, vel, 0.4f, 1.5f, 0.7f, e);
        CHECK(state_phys_body_valid(base, b));
        state_ecs_bind_body(base, e, b);

        const StateHandle* d_ent = STATE_SLICE_PTR_CONST(base, StateHandle, ecs->dense_entity);
        StateHandleOf(Entity) tgt = STATE_HANDLE_TYPED_MAKE(Entity, d_ent[0]);
        state_ecs_set_target(base, e, tgt);
    }
}

TEST(state_world_relocate_different_base_same_hash) {
    const u32 CAP = KB(128);
    const f32 DT  = 1.0f / 60.0f;

    /* Üç farklı taban adresi: g_reg_buf_a, g_reg_buf_b (+64 kayık), g_reg_buf_c (+128 kayık) */
    void* base_a = g_reg_buf_a;
    void* base_b = g_reg_buf_b + 64;
    void* base_c = g_reg_buf_c + 128;
    const u32 SUB_CAP = CAP - 128u;

    CHECK((uptr)base_a != (uptr)base_b);
    CHECK((uptr)base_b != (uptr)base_c);
    CHECK((uptr)base_a != (uptr)base_c);

    /* A ve C bölgelerini kendi farklı taban adreslerinde aynı tohumla başlat */
    state_world_init(base_a, SUB_CAP, 96u, 96u, 192u, 0x20261029u);
    state_world_populate_demo(base_a, 48u, 40u);

    state_world_init(base_c, SUB_CAP, 96u, 96u, 192u, 0x20261029u);
    state_world_populate_demo(base_c, 48u, 40u);

    /* Farklı taban adreslerde başlangıç özetleri bit-bit aynı olmalı */
    CHECK(state_region_hash_active(base_a) == state_region_hash_active(base_c));
    CHECK(state_region_hash_full(base_a, SUB_CAP) == state_region_hash_full(base_c, SUB_CAP));

    /* Aşamalı kanıt 1: A bölgesinde 80 adım çalıştır */
    for (u32 step = 0u; step < 80u; step++) {
        run_scripted_mutations(base_a, step);
        state_world_step(base_a, DT);
    }
    u64 hash_a_80_active = state_region_hash_active(base_a);
    u64 hash_a_80_full   = state_region_hash_full(base_a, SUB_CAP);

    /* A bölgesini B taban adresine kopyala (relocate) ve A bölgesini 0xDD ile zehirle!
     * Eğer B içinde A'ya işaret eden tek bir ham işaretçi bile olsaydı bozulurdu. */
    CHECK(state_region_relocate(base_b, base_a, SUB_CAP));
    memset(base_a, 0xDD, SUB_CAP);

    /* Taşıma sonrası B adresindeki özet, A'nın taşınmadan önceki özetiyle bit-bit aynı olmalı */
    CHECK(state_region_hash_active(base_b) == hash_a_80_active);
    CHECK(state_region_hash_full(base_b, SUB_CAP) == hash_a_80_full);

    /* Adım 80'de B'den anlık görüntü (snapshot) al */
    state_region_snapshot_save(base_b, g_snap_buf, SUB_CAP);

    /* Aşamalı kanıt 2: B bölgesini (yeni taban adresinde!) 80..160 arası 80 adım daha ilerlet */
    for (u32 step = 80u; step < 160u; step++) {
        run_scripted_mutations(base_b, step);
        state_world_step(base_b, DT);
    }

    /* C bölgesini kendi taban adresinde kesintisiz 0..160 arası ilerlet */
    for (u32 step = 0u; step < 160u; step++) {
        run_scripted_mutations(base_c, step);
        state_world_step(base_c, DT);
    }

    /* B (yarı yolda taban adresi değişen) ve C (başka taban adreste koşan) bit-bit aynı olmalı! */
    u64 hash_b_160_active = state_region_hash_active(base_b);
    u64 hash_c_160_active = state_region_hash_active(base_c);
    u64 hash_b_160_full   = state_region_hash_full(base_b, SUB_CAP);
    u64 hash_c_160_full   = state_region_hash_full(base_c, SUB_CAP);

    CHECK(hash_b_160_active == hash_c_160_active);
    CHECK(hash_b_160_full   == hash_c_160_full);
    CHECK(memcmp(base_b, base_c, SUB_CAP) == 0);

    const StateWorld* wb = state_world_get_const(base_b);
    const StateWorld* wc = state_world_get_const(base_c);
    CHECK(wb->step_count == 160u && wc->step_count == 160u);
    CHECK(wb->phys.bounce_count > 0u);
    CHECK(wb->phys.bounce_count == wc->phys.bounce_count);
    CHECK(wb->spawned_total == wc->spawned_total);
    CHECK(wb->destroyed_total == wc->destroyed_total);
    CHECK(wb->checksum_acc == wc->checksum_acc);

    /* Aşamalı kanıt 3: Adım 80 anlık görüntüsünü zehirlenmiş A adresine geri yükle,
     * 80..160 arasını A adresinde yeniden simüle et -> B ve C ile bit-bit aynı olmalı */
    state_region_snapshot_restore(base_a, g_snap_buf, SUB_CAP);
    CHECK(state_region_validate(base_a, SUB_CAP, STATE_WORLD_ABI_HASH));
    CHECK(state_region_hash_active(base_a) == hash_a_80_active);

    for (u32 step = 80u; step < 160u; step++) {
        run_scripted_mutations(base_a, step);
        state_world_step(base_a, DT);
    }
    CHECK(state_region_hash_active(base_a) == hash_b_160_active);
    CHECK(state_region_hash_full(base_a, SUB_CAP) == hash_b_160_full);
}

/* ============================================================================
 * 5. Ölçüm: 16 MB Durum Bölgesi Kopya + Hash Süresi (F0_D / H4 Karşılaştırması)
 * ============================================================================ */

static f64 sort_median_f64(f64* v, u32 n) {
    for (u32 i = 1u; i < n; i++) {
        f64 key = v[i];
        i32 j = (i32)i - 1;
        while (j >= 0 && v[j] > key) {
            v[j + 1] = v[j];
            j--;
        }
        v[j + 1] = key;
    }
    return v[n / 2u];
}

TEST(state_benchmark_16mb_copy_and_hash) {
    const u32 CAP_16M = (u32)MB(16);
    void* src = g_bench_16m_src;
    void* dst = g_bench_16m_dst;

    /* 16 MB bölge içinde gerçek StateWorld + kalan alanı dolduran büyük SoA veri dilimi kur */
    state_world_init(src, CAP_16M, 2048u, 512u, 1024u, 0x51512026u);
    state_world_populate_demo(src, 256u, 128u);
    state_world_step(src, 1.0f / 60.0f);

    RegionArena* ra = state_region_arena(src);
    u32 active_world_bytes = ra->used;

    /* 16 MB'ın tamamını anlamlı (sıfır olmayan) deterministik durum verisiyle doldur */
    u32 rem_bytes = CAP_16M - ra->used;
    u32 rem_u32_count = rem_bytes / (u32)sizeof(u32);
    StateSlice(u32) bulk = STATE_ALLOC_SLICE(ra, src, u32, rem_u32_count);
    u32* bulk_ptr = STATE_SLICE_PTR(src, u32, bulk);
    for (u32 i = 0u; i < rem_u32_count; i++) {
        bulk_ptr[i] = i * 2654435761u ^ 0xA5A55A5Au;
    }
    CHECK(ra->used == CAP_16M);

    /* Farklı taban adrese (dst) taşıyıp 16 MB özetinin birebir aynı olduğunu doğrula */
    CHECK(state_region_relocate(dst, src, CAP_16M));
    u64 h_src = state_region_hash_full(src, CAP_16M);
    u64 h_dst = state_region_hash_full(dst, CAP_16M);
    CHECK(h_src != 0ULL && h_src == h_dst);
    CHECK(sim_hash(src, CAP_16M) == sim_hash(dst, CAP_16M));

    const u32 WARMUP = 10u;
    const u32 ITERS  = 41u;
    f64 copy_16m_ms[41];
    f64 hash_16m_ms[41];
    f64 sim_hash_16m_ms[41];
    f64 both_16m_ms[41];
    f64 active_copy_ms[41];
    f64 active_hash_ms[41];

    u64 acc_guard = 0ULL;
    for (u32 i = 0u; i < WARMUP + ITERS; i++) {
        /* Yalnız aktif dünya dilimi (active_world_bytes) kopya + özet */
        f64 ta0 = plat_time_ms();
        state_region_snapshot_save(src, dst, active_world_bytes);
        f64 ta1 = plat_time_ms();
        u64 ha = state_region_hash_bytes(dst, active_world_bytes);
        f64 ta2 = plat_time_ms();

        /* Tam 16 MB kopya (snapshot) + tam 16 MB 4-hatlı özet + 1-hatlı sim_hash karşılaştırması */
        f64 t0 = plat_time_ms();
        state_region_snapshot_save(src, dst, CAP_16M);
        f64 t1 = plat_time_ms();
        u64 hf = state_region_hash_full(dst, CAP_16M);
        f64 t2 = plat_time_ms();
        u64 hs = sim_hash(dst, CAP_16M);
        f64 t3 = plat_time_ms();

        CHECK(hf == h_src);
        acc_guard ^= ha ^ hf ^ hs;

        if (i >= WARMUP) {
            u32 k = i - WARMUP;
            active_copy_ms[k]   = ta1 - ta0;
            active_hash_ms[k]   = ta2 - ta1;
            copy_16m_ms[k]      = t1 - t0;
            hash_16m_ms[k]      = t2 - t1;
            both_16m_ms[k]      = t2 - t0;
            sim_hash_16m_ms[k]  = t3 - t2;
        }
    }
    CHECK(acc_guard != 1ULL);

    f64 med_copy_16m   = sort_median_f64(copy_16m_ms, ITERS);
    f64 med_hash_16m   = sort_median_f64(hash_16m_ms, ITERS);
    f64 med_shash_16m  = sort_median_f64(sim_hash_16m_ms, ITERS);
    f64 med_both_16m   = sort_median_f64(both_16m_ms, ITERS);
    f64 med_act_copy   = sort_median_f64(active_copy_ms, ITERS);
    f64 med_act_hash   = sort_median_f64(active_hash_ms, ITERS);

    f64 copy_gbs  = (16.0 / 1024.0) / (med_copy_16m  / 1000.0);
    f64 hash_gbs  = (16.0 / 1024.0) / (med_hash_16m  / 1000.0);
    f64 shash_gbs = (16.0 / 1024.0) / (med_shash_16m / 1000.0);

    printf("  [bench S1] 16 MB kopya: %.3f ms (%.1f GB/s) | 16 MB 4-lane hash: %.3f ms (%.1f GB/s) "
           "[1-lane sim_hash: %.3f ms (%.1f GB/s)] | kopya+hash: %.3f ms | "
           "aktif (%u KB) kopya: %.4f ms, hash: %.4f ms\n",
           med_copy_16m, copy_gbs, med_hash_16m, hash_gbs,
           med_shash_16m, shash_gbs, med_both_16m,
           active_world_bytes / 1024u, med_act_copy, med_act_hash);
}
