/* core/rng.h — PCG32 sözde-rastgele sayı üreteci (XSH-RR).
 * Küçük durumlu (16 byte), deterministik ve hızlı RNG.
 * Referans: O'Neill, "PCG: A Family of Simple Fast Space-Efficient Statistically Good Algorithms".
 */
#ifndef CORE_RNG_H
#define CORE_RNG_H

#include "core/types.h"

typedef struct {
    u64 state;
    u64 inc;  /* her zaman tek sayı (inc | 1) */
} Rng;

void rng_seed_seq(Rng* r, u64 seed, u64 seq);
void rng_seed(Rng* r, u64 seed);
u32  rng_u32(Rng* r);
f32  rng_f32(Rng* r);                    /* [0, 1) aralığında */
i32  rng_range(Rng* r, i32 lo, i32 hi);  /* [lo, hi) aralığında */

#endif
