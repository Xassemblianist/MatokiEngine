/* core/profiler.h — hafif iç içe zamanlayıcı ve frame bazlı raporlama.
 * En fazla 64 bölgeyi (PROF_MAX_ZONES) ve iç içe çağrıları takip eder.
 * Zaman kaynağı plat_time_ms() olup prof_report() LOG'a hizalı tablo basar.
 */
#ifndef CORE_PROFILER_H
#define CORE_PROFILER_H

#include "core/types.h"

#define PROF_MAX_ZONES 64
#define PROF_MAX_STACK 32

typedef struct {
    const char* name;
    f64         cur_ms;       /* aktif frame içi birikmiş süre */
    u32         cur_calls;    /* aktif frame içi çağrı sayısı */
    f64         last_ms;      /* tamamlanan son frame süresi */
    u32         last_calls;   /* tamamlanan son frame çağrı sayısı */
    f64         total_ms;     /* toplam birikmiş süre (tüm frame'ler) */
    f64         max_ms;       /* tek frame'de görülen en yüksek süre */
    u64         total_calls;  /* toplam çağrı sayısı */
} ProfZone;

void prof_reset(void);
void prof_begin(const char* name);
void prof_end(void);
void prof_frame_end(void);
void prof_report(void);

u32             prof_zone_count(void);
const ProfZone* prof_zone_get(u32 idx);
u32             prof_frame_count(void);

#define PROF_BEGIN(name) prof_begin(name)
#define PROF_END()       prof_end()

#endif
