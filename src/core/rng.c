/* core/rng.c — PCG32 (pcg_onseq_64_xsh_rr_32) implementasyonu.
 * Standart PCG32 çarpanı ve iki adımlı tohumlama protokolü kullanılır.
 * rng_seed(r, seed) varsayılan akış olarak seq = 54u kullanır.
 */
#include "core/rng.h"

#define PCG32_MULT      6364136223846793005ULL
#define PCG32_DEFAULT_SEQ 54ULL

u32 rng_u32(Rng* r) {
    u64 oldstate = r->state;
    r->state = oldstate * PCG32_MULT + r->inc;
    u32 xorshifted = (u32)(((oldstate >> 18u) ^ oldstate) >> 27u);
    u32 rot = (u32)(oldstate >> 59u);
    return (xorshifted >> rot) | (xorshifted << ((-rot) & 31u));
}

void rng_seed_seq(Rng* r, u64 seed, u64 seq) {
    r->state = 0ULL;
    r->inc = (seq << 1u) | 1ULL;
    rng_u32(r);
    r->state += seed;
    rng_u32(r);
}

void rng_seed(Rng* r, u64 seed) {
    rng_seed_seq(r, seed, PCG32_DEFAULT_SEQ);
}

f32 rng_f32(Rng* r) {
    return (f32)(rng_u32(r) >> 8) * (1.0f / 16777216.0f);
}

i32 rng_range(Rng* r, i32 lo, i32 hi) {
    if (hi <= lo) return lo;
    u32 bound = (u32)((i64)hi - (i64)lo);
    u32 threshold = (0u - bound) % bound;
    for (;;) {
        u32 v = rng_u32(r);
        if (v >= threshold) {
            return lo + (i32)(v % bound);
        }
    }
}
