/* tests/test_det.c — Determinizm (belirlenimcilik) kanıtı testleri (F0_G, D-112).
 * 3D fizik + ECS + simülasyon sahnesi üzerinde 10.000 tick çalıştırma,
 * ardışık çalıştırmalarda bit-bit tam eşleşme, NaN olmama garantisi
 * ve host ortamındaki durum özetlerinin doğrulanması.
 */
#include <stdio.h>
#include <string.h>
#include "test.h"
#include "core/types.h"
#include "core/arena.h"
#include "sim/sim.h"

static _Alignas(16) u64 g_host_det_hashes_1[10000];
static _Alignas(16) u64 g_host_det_hashes_2[10000];

TEST(det_10000_ticks_reproducible) {
    SimDetResult r1 = sim_det_run(10000u, SIM_DET_DEFAULT_SEED, g_host_det_hashes_1, 10000u);
    CHECK(r1.total_ticks == 10000u);
    CHECK(!r1.has_nan);
    CHECK(r1.final_hash != 0ULL);
    CHECK(r1.combined_hash != 0ULL);

    SimDetResult r2 = sim_det_run(10000u, SIM_DET_DEFAULT_SEED, g_host_det_hashes_2, 10000u);
    CHECK(r2.total_ticks == 10000u);
    CHECK(!r2.has_nan);
    CHECK(r1.initial_hash  == r2.initial_hash);
    CHECK(r1.final_hash    == r2.final_hash);
    CHECK(r1.combined_hash == r2.combined_hash);

    /* 10.000 tick'in her birinde özetler bit-bit aynı olmalı */
    u32 mismatches = 0u;
    for (u32 i = 0u; i < 10000u; i++) {
        if (g_host_det_hashes_1[i] != g_host_det_hashes_2[i]) {
            mismatches++;
        }
    }
    CHECK(mismatches == 0u);

    for (u32 c = 0u; c < 10u; c++) {
        CHECK(r1.checkpoint_hashes[c] == r2.checkpoint_hashes[c]);
        CHECK(r1.checkpoint_hashes[c] != 0ULL);
    }

    /* Host sonuçlarını build/det_host.json ve build/det_host_hashes.bin olarak kaydet */
    FILE* fbin = fopen("build/det_host_hashes.bin", "wb");
    if (fbin) {
        fwrite(g_host_det_hashes_1, sizeof(u64), 10000u, fbin);
        fclose(fbin);
    }

    FILE* fjson = fopen("build/det_host.json", "w");
    if (fjson) {
        fprintf(fjson, "{\n");
        fprintf(fjson, "  \"environment\": \"host\",\n");
        fprintf(fjson, "  \"threads\": 1,\n");
        fprintf(fjson, "  \"total_ticks\": 10000,\n");
        fprintf(fjson, "  \"initial_hash\": \"0x%016llX\",\n", (unsigned long long)r1.initial_hash);
        fprintf(fjson, "  \"final_hash\": \"0x%016llX\",\n", (unsigned long long)r1.final_hash);
        fprintf(fjson, "  \"combined_hash\": \"0x%016llX\",\n", (unsigned long long)r1.combined_hash);
        fprintf(fjson, "  \"has_nan\": %s,\n", r1.has_nan ? "true" : "false");
        fprintf(fjson, "  \"checkpoints\": [\n");
        for (u32 c = 0u; c < 10u; c++) {
            fprintf(fjson, "    \"0x%016llX\"%s\n", (unsigned long long)r1.checkpoint_hashes[c], (c == 9u) ? "" : ",");
        }
        fprintf(fjson, "  ]\n");
        fprintf(fjson, "}\n");
        fclose(fjson);
    }

    printf("  [det host 10k] initial=0x%016llX final=0x%016llX combined=0x%016llX (nan=%d)\n",
           (unsigned long long)r1.initial_hash,
           (unsigned long long)r1.final_hash,
           (unsigned long long)r1.combined_hash,
           r1.has_nan);
}
