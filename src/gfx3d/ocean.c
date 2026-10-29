/* gfx3d/ocean.c — Tessendorf 3 kademeli FFT okyanus simülasyonu, JONSWAP spektrumu ve ızgara ağı.
 * Her kademede 8 gerçek alan (h, Dx, Dz, Sx, Sz, Dxx, Dzz, Dxz) Hermitsel simetriyle 4 karmaşık 2B IFFT'de
 * paketlenir; satır/sütun geçişleri job_parallel_for ile paralel koşturulur.
 */
#include "gfx3d/ocean.h"
#include "core/rng.h"
#include "core/log.h"
#include "raster/framebuffer.h"
#include "thread/job.h"

static inline f32 ocean_exp(f32 x) {
    return m_exp2(x * 1.44269504f);
}

static inline f32 ocean_ln(f32 x) {
    return m_log2(x) * 0.69314718f;
}

static inline f32 smooth_falloff(f32 d, f32 r0, f32 r1) {
    f32 t = m_clamp((d - r0) / (r1 - r0), 0.0f, 1.0f);
    return 1.0f - t * t * (3.0f - 2.0f * t);
}

/* Box–Muller: iki bağımsız standart normal çekiliş */
static void gauss2(Rng* r, f32* a, f32* b) {
    f32 u1  = m_max(rng_f32(r), 1e-7f);
    f32 u2  = rng_f32(r);
    f32 rad = m_sqrt(-2.0f * ocean_ln(u1));
    *a = rad * m_cos(M_TAU * u2);
    *b = rad * m_sin(M_TAU * u2);
}

static u32 neg_k_index(u32 n, u32 log2n, u32 idx) {
    u32 m = n - 1u;
    u32 i = idx & m;
    u32 j = idx >> log2n;
    return ((n - j) & m) * n + ((n - i) & m);
}

OceanDesc ocean_default_desc(void) {
    OceanDesc d;
    __builtin_memset(&d, 0, sizeof(d));
    d.cascades[0].n    = 128u;
    d.cascades[0].size = 500.0f;
    d.cascades[1].n    = 128u;
    d.cascades[1].size = 60.0f;
    d.cascades[2].n    = 64u;
    d.cascades[2].size = 8.0f;
    d.wind_speed       = 10.0f;
    d.wind_x           = 0.85f;
    d.wind_z           = 0.52f;
    d.fetch            = 100000.0f;
    d.gamma            = 3.3f;
    d.reverse          = 0.05f;
    d.choppy           = 1.25f;
    d.foam_threshold   = 0.78f;
    d.foam_decay       = 1.40f;
    d.foam_scale       = 2.20f;
    d.seed             = 0x4F4345414E323032ULL;
    return d;
}

f32 ocean_jonswap_omega(f32 omega, f32 wind_speed, f32 fetch, f32 gamma) {
    if (omega <= 1e-5f || wind_speed <= 0.1f) return 0.0f;
    f32 u10     = m_max(wind_speed, 0.5f);
    f32 f       = m_max(fetch, 100.0f);
    f32 x_tilde = m_max(OCEAN_G * f / (u10 * u10), 1.0f);
    f32 alpha   = 0.076f * m_pow(x_tilde, -0.22f);
    f32 omega_p = 22.0f * (OCEAN_G / u10) * m_pow(x_tilde, -0.33333334f);
    f32 sigma   = (omega <= omega_p) ? 0.07f : 0.09f;
    f32 d_om    = (omega - omega_p) / (sigma * omega_p);
    f32 r       = ocean_exp(-0.5f * d_om * d_om);
    f32 peak    = (gamma > 1.0f) ? m_pow(gamma, r) : 1.0f;
    f32 om2     = omega * omega;
    f32 om5     = om2 * om2 * omega;
    f32 wp_o    = omega_p / omega;
    f32 wp_o2   = wp_o * wp_o;
    f32 pm      = (alpha * OCEAN_G * OCEAN_G / om5) * ocean_exp(-1.25f * wp_o2 * wp_o2);
    return pm * peak;
}

void ocean_rebuild_spectrum(Ocean* o) {
    ASSERT(o != NULL);
    f32 wl = m_sqrt(o->d.wind_x * o->d.wind_x + o->d.wind_z * o->d.wind_z);
    f32 wx = (wl > 1e-6f) ? (o->d.wind_x / wl) : 1.0f;
    f32 wz = (wl > 1e-6f) ? (o->d.wind_z / wl) : 0.0f;
    f32 rev = m_clamp(o->d.reverse, 0.0f, 1.0f);
    /* ∫_{-π}^{π} D(θ) dθ = 1 olacak şekilde cos² yönlü yayılım normu */
    f32 dir_norm = 2.0f / (M_PI * (1.0f + rev));

    f64 sample_var = 0.0;
    f64 theory_var = 0.0;

    for (u32 ci = 0; ci < OCEAN_CASCADES; ci++) {
        OceanCascade* c = &o->cascades[ci];
        u32 n  = c->n;
        u32 nn = n * n;
        f32 dk = M_TAU / c->size;

        for (u32 j = 0; j < n; j++) {
            for (u32 i = 0; i < n; i++) {
                u32 idx = j * n + i;
                b32 nyq = (i == (n >> 1)) || (j == (n >> 1));
                f32 kx  = c->kx[idx];
                f32 kz  = c->kz[idx];
                f32 k   = m_sqrt(kx * kx + kz * kz);

                if (nyq || k <= c->k_min || k > c->k_max) {
                    c->h0r[idx] = 0.0f;
                    c->h0i[idx] = 0.0f;
                    continue;
                }

                f32 om     = c->omega[idx];
                f32 s_om   = ocean_jonswap_omega(om, o->d.wind_speed, o->d.fetch, o->d.gamma);
                f32 dw_dk  = 0.5f * OCEAN_G / om;
                f32 cos_th = (kx * wx + kz * wz) / k;
                f32 dir    = cos_th * cos_th * ((cos_th < 0.0f) ? rev : 1.0f) * dir_norm;
                f32 psi_k  = s_om * (dw_dk / k) * dir;
                f32 var_k  = psi_k * dk * dk;
                f32 amp    = m_sqrt(var_k * 0.25f);

                c->h0r[idx] = c->xi_r[idx] * amp;
                c->h0i[idx] = c->xi_i[idx] * amp;

                theory_var += (f64)var_k;
                sample_var += 2.0 * ((f64)c->h0r[idx] * (f64)c->h0r[idx] +
                                     (f64)c->h0i[idx] * (f64)c->h0i[idx]);
            }
        }

        for (u32 idx = 0; idx < nn; idx++) {
            u32 neg = neg_k_index(n, c->log2n, idx);
            c->h0mr[idx] =  c->h0r[neg];
            c->h0mi[idx] = -c->h0i[neg];
        }
    }

    o->theory_hs = 4.0f * m_sqrt((f32)theory_var);
    o->hs        = 4.0f * m_sqrt((f32)sample_var);
}

b32 ocean_set_wind(Ocean* o, f32 wind_speed, f32 wind_x, f32 wind_z) {
    ASSERT(o != NULL);
    f32 u10 = m_clamp(wind_speed, 1.0f, 28.0f);
    f32 wl  = m_sqrt(wind_x * wind_x + wind_z * wind_z);
    f32 wx  = (wl > 1e-6f) ? (wind_x / wl) : 1.0f;
    f32 wz  = (wl > 1e-6f) ? (wind_z / wl) : 0.0f;

    /* Deterministik kuantizasyon: geri sarma (rollback) ve replay sıfırlamalarında
     * spektrumun geçmiş durumdan bağımsız olarak birebir aynı kurulmasını sağlar. */
    f32 q_u10 = m_floor(u10 * 10.0f + 0.5f) * 0.1f;
    f32 q_wx  = m_floor(wx * 100.0f + 0.5f) * 0.01f;
    f32 q_wz  = m_floor(wz * 100.0f + 0.5f) * 0.01f;

    o->d.choppy         = m_clamp(0.95f + 0.030f * q_u10, 0.85f, 1.65f);
    o->d.foam_threshold = m_clamp(0.68f + 0.010f * q_u10, 0.70f, 0.92f);
    o->d.foam_scale     = m_clamp(1.80f + 0.040f * q_u10, 0.80f, 2.80f);

    if (o->d.wind_speed != q_u10 || o->d.wind_x != q_wx || o->d.wind_z != q_wz) {
        o->d.wind_speed = q_u10;
        o->d.wind_x     = q_wx;
        o->d.wind_z     = q_wz;
        o->d.fetch      = 100000.0f + m_max(q_u10 - 10.0f, 0.0f) * 9000.0f;
        ocean_rebuild_spectrum(o);
        return TRUE;
    }
    return FALSE;
}

void ocean_init(Ocean* o, Arena* a, const OceanDesc* d) {
    ASSERT(o != NULL && a != NULL && d != NULL);
    __builtin_memset(o, 0, sizeof(*o));
    o->d = *d;

    Rng rng;
    rng_seed(&rng, d->seed ? d->seed : 0x4F4345414E323032ULL);

    f32 prev_k_max = 0.0f;
    for (u32 ci = 0; ci < OCEAN_CASCADES; ci++) {
        OceanCascade* c = &o->cascades[ci];
        u32 n = d->cascades[ci].n;
        ASSERT(n >= 4u && IS_POW2(n));
        u32 log2n = 0;
        while ((1u << log2n) < n) log2n++;

        u32 nn   = n * n;
        c->n     = n;
        c->log2n = log2n;
        c->size  = d->cascades[ci].size;
        c->k_min = prev_k_max;
        c->k_max = (M_PI * (f32)n) / c->size;
        prev_k_max = c->k_max;

        c->xi_r    = ARENA_PUSH_ARRAY(a, f32, nn);
        c->xi_i    = ARENA_PUSH_ARRAY(a, f32, nn);
        c->h0r     = ARENA_PUSH_ARRAY(a, f32, nn);
        c->h0i     = ARENA_PUSH_ARRAY(a, f32, nn);
        c->h0mr    = ARENA_PUSH_ARRAY(a, f32, nn);
        c->h0mi    = ARENA_PUSH_ARRAY(a, f32, nn);
        c->omega   = ARENA_PUSH_ARRAY(a, f32, nn);
        c->kx      = ARENA_PUSH_ARRAY(a, f32, nn);
        c->kz      = ARENA_PUSH_ARRAY(a, f32, nn);
        c->bitrev  = ARENA_PUSH_ARRAY(a, u32, n);
        c->tw_c    = ARENA_PUSH_ARRAY(a, f32, n >> 1);
        c->tw_s    = ARENA_PUSH_ARRAY(a, f32, n >> 1);
        for (u32 f = 0; f < 4u; f++) {
            c->wr[f] = ARENA_PUSH_ARRAY(a, f32, nn);
            c->wi[f] = ARENA_PUSH_ARRAY(a, f32, nn);
        }
        c->height  = ARENA_PUSH_ARRAY(a, f32, nn);
        c->disp_x  = ARENA_PUSH_ARRAY(a, f32, nn);
        c->disp_z  = ARENA_PUSH_ARRAY(a, f32, nn);
        c->slope_x = ARENA_PUSH_ARRAY(a, f32, nn);
        c->slope_z = ARENA_PUSH_ARRAY(a, f32, nn);
        c->dxx     = ARENA_PUSH_ARRAY(a, f32, nn);
        c->dzz     = ARENA_PUSH_ARRAY(a, f32, nn);
        c->dxz     = ARENA_PUSH_ARRAY(a, f32, nn);
        c->foam    = ARENA_PUSH_ARRAY(a, f32, nn);

        tex_init_mipmapped(&c->tex, a, n, n, TEX_FLAG_BILINEAR);

        for (u32 i = 0; i < n; i++) {
            u32 r = 0;
            for (u32 b = 0; b < log2n; b++) {
                r |= ((i >> b) & 1u) << (log2n - 1u - b);
            }
            c->bitrev[i] = r;
        }
        for (u32 j = 0; j < (n >> 1); j++) {
            f32 ang = M_TAU * (f32)j / (f32)n;
            c->tw_c[j] = m_cos(ang);
            c->tw_s[j] = m_sin(ang);
        }

        for (u32 j = 0; j < n; j++) {
            for (u32 i = 0; i < n; i++) {
                u32 idx = j * n + i;
                i32 si  = (i < (n >> 1)) ? (i32)i : (i32)i - (i32)n;
                i32 sj  = (j < (n >> 1)) ? (i32)j : (i32)j - (i32)n;
                f32 kx  = M_TAU * (f32)si / c->size;
                f32 kz  = M_TAU * (f32)sj / c->size;
                f32 k   = m_sqrt(kx * kx + kz * kz);
                c->kx[idx]    = kx;
                c->kz[idx]    = kz;
                c->omega[idx] = m_sqrt(OCEAN_G * k);
                gauss2(&rng, &c->xi_r[idx], &c->xi_i[idx]);
            }
        }
    }

    ocean_rebuild_spectrum(o);
}

void ocean_cascade_amplitude(const OceanCascade* c, u32 idx, f64 t, f32* out_re, f32* out_im) {
    f64 ph = (f64)c->omega[idx] * t;
    f64 k  = (f64)(i64)(ph * (1.0 / 6.28318530717958647692));
    ph -= k * 6.28318530717958647692;
    f32 cs = m_cos((f32)ph);
    f32 sn = m_sin((f32)ph);
    /* h̃(k,t) = h̃0(k)·e^{-iωt} + conj(h̃0(-k))·e^{+iωt} */
    f32 h0r  = c->h0r[idx],  h0i  = c->h0i[idx];
    f32 h0mr = c->h0mr[idx], h0mi = c->h0mi[idx];
    *out_re = (h0r + h0mr) * cs + (h0i - h0mi) * sn;
    *out_im = (h0i + h0mi) * cs + (h0mr - h0r) * sn;
}

void ocean_fft_1d(const OceanCascade* c, f32* re, f32* im, u32 stride) {
    u32 n = c->n;
    const u32* bitrev = c->bitrev;
    const f32* tw_c   = c->tw_c;
    const f32* tw_s   = c->tw_s;

    for (u32 i = 0; i < n; i++) {
        u32 r = bitrev[i];
        if (r > i) {
            u32 pi = i * stride, pr = r * stride;
            f32 tr = re[pi]; re[pi] = re[pr]; re[pr] = tr;
            f32 ti = im[pi]; im[pi] = im[pr]; im[pr] = ti;
        }
    }

    for (u32 len = 2u; len <= n; len <<= 1u) {
        u32 half = len >> 1u;
        u32 step = n / len;
        for (u32 b = 0; b < n; b += len) {
            for (u32 k = 0; k < half; k++) {
                u32 tw = k * step;
                f32 wc = tw_c[tw], ws = tw_s[tw];
                u32 p  = (b + k) * stride;
                u32 q  = (b + k + half) * stride;
                f32 rq = re[q], iq = im[q];
                f32 xr = rq * wc - iq * ws;
                f32 xi = rq * ws + iq * wc;
                re[q] = re[p] - xr;
                im[q] = im[p] - xi;
                re[p] += xr;
                im[p] += xi;
            }
        }
    }
}

void ocean_fft_2d(const OceanCascade* c, f32* re, f32* im) {
    u32 n = c->n;
    for (u32 j = 0; j < n; j++) {
        ocean_fft_1d(c, re + j * n, im + j * n, 1u);
    }
    for (u32 i = 0; i < n; i++) {
        ocean_fft_1d(c, re + i, im + i, n);
    }
}

/* ---------- Paralel kademe güncelleme işleri ---------- */

typedef struct {
    Ocean*        ocean;
    OceanCascade* cas;
    f64           t;
    f32           dt;
    b32           first_update;
} CascadeJobCtx;

static void job_pack_rows(u32 start, u32 end, void* user) {
    CascadeJobCtx* jc = (CascadeJobCtx*)user;
    OceanCascade* c   = jc->cas;
    u32 n  = c->n;
    f64 t  = jc->t;

    for (u32 j = start; j < end; j++) {
        u32 row = j * n;
        for (u32 i = 0; i < n; i++) {
            u32 idx = row + i;
            f32 hr, hi;
            ocean_cascade_amplitude(c, idx, t, &hr, &hi);

            f32 kx = c->kx[idx], kz = c->kz[idx];
            f32 k2 = kx * kx + kz * kz;
            f32 kl = (k2 > 1e-12f) ? m_sqrt(k2) : 0.0f;
            f32 ik = (kl > 1e-12f) ? (1.0f / kl) : 0.0f;
            f32 ux = kx * ik, uz = kz * ik;

            /* 8 gerçek alanın Fourier katsayıları:
             *  h    = hr + i·hi
             *  Dxz  = -(kx·kz/k)·h̃
             *  Dx   = i·ux·h̃ = -ux·hi + i·ux·hr
             *  Dz   = i·uz·h̃ = -uz·hi + i·uz·hr
             *  Sx   = i·kx·h̃ = -kx·hi + i·kx·hr
             *  Sz   = i·kz·h̃ = -kz·hi + i·kz·hr
             *  Dxx  = -(kx²/k)·h̃
             *  Dzz  = -(kz²/k)·h̃
             * Hermitsel iki alan Z̃ = Ã + i·B̃ = (Ar - Bi) + i·(Ai + Br) olarak paketlenir. */
            f32 kxz = -ux * kz;
            f32 kxx = -ux * kx;
            f32 kzz = -uz * kz;

            f32 dxz_r = kxz * hr, dxz_i = kxz * hi;
            f32 dx_r  = -ux * hi, dx_i  =  ux * hr;
            f32 dz_r  = -uz * hi, dz_i  =  uz * hr;
            f32 sx_r  = -kx * hi, sx_i  =  kx * hr;
            f32 sz_r  = -kz * hi, sz_i  =  kz * hr;
            f32 dxx_r = kxx * hr, dxx_i = kxx * hi;
            f32 dzz_r = kzz * hr, dzz_i = kzz * hi;

            c->wr[0][idx] = hr    - dxz_i; c->wi[0][idx] = hi    + dxz_r;
            c->wr[1][idx] = dx_r  - dz_i;  c->wi[1][idx] = dx_i  + dz_r;
            c->wr[2][idx] = sx_r  - sz_i;  c->wi[2][idx] = sx_i  + sz_r;
            c->wr[3][idx] = dxx_r - dzz_i; c->wi[3][idx] = dxx_i + dzz_r;
        }
    }
}

static void job_fft_rows(u32 start, u32 end, void* user) {
    CascadeJobCtx* jc = (CascadeJobCtx*)user;
    OceanCascade* c   = jc->cas;
    u32 n = c->n;
    for (u32 m = start; m < end; m++) {
        u32 f   = m >> c->log2n;
        u32 row = m & (n - 1u);
        ocean_fft_1d(c, c->wr[f] + row * n, c->wi[f] + row * n, 1u);
    }
}

static void job_fft_cols(u32 start, u32 end, void* user) {
    CascadeJobCtx* jc = (CascadeJobCtx*)user;
    OceanCascade* c   = jc->cas;
    u32 n = c->n;
    for (u32 m = start; m < end; m++) {
        u32 f   = m >> c->log2n;
        u32 col = m & (n - 1u);
        ocean_fft_1d(c, c->wr[f] + col, c->wi[f] + col, n);
    }
}

static void job_unpack_rows(u32 start, u32 end, void* user) {
    CascadeJobCtx* jc = (CascadeJobCtx*)user;
    Ocean* o          = jc->ocean;
    OceanCascade* c   = jc->cas;
    u32 n      = c->n;
    f32 lam    = o->d.choppy;
    f32 j_thr  = o->d.foam_threshold;
    f32 f_gain = o->d.foam_scale;
    f32 dt     = jc->dt;
    f32 decay  = (dt > 0.0f) ? ocean_exp(-o->d.foam_decay * dt) : 1.0f;
    f32 inject = jc->first_update ? 1.0f : (dt > 0.0f ? m_min(dt * 3.8f, 1.0f) : 0.0f);
    f32 s_enc  = 127.0f / OCEAN_TEX_SLOPE_MAX;
    f32 h_norm = 1.0f / m_max(o->hs, 0.5f);

    for (u32 j = start; j < end; j++) {
        u32 row = j * n;
        u32* tex_row = c->tex.px + row;
        for (u32 i = 0; i < n; i++) {
            u32 idx = row + i;
            f32 h   = c->wr[0][idx];
            f32 dxz = c->wi[0][idx];
            f32 dx  = c->wr[1][idx];
            f32 dz  = c->wi[1][idx];
            f32 sx  = c->wr[2][idx];
            f32 sz  = c->wi[2][idx];
            f32 dxx = c->wr[3][idx];
            f32 dzz = c->wi[3][idx];

            c->height[idx]  = h;
            c->dxz[idx]     = dxz;
            c->disp_x[idx]  = dx;
            c->disp_z[idx]  = dz;
            c->slope_x[idx] = sx;
            c->slope_z[idx] = sz;
            c->dxx[idx]     = dxx;
            c->dzz[idx]     = dzz;

            /* Jacobian J = (1 + λ·Dxx)(1 + λ·Dzz) - λ²·Dxz² */
            f32 jac = (1.0f + lam * dxx) * (1.0f + lam * dzz) - (lam * dxz) * (lam * dxz);
            f32 gen = (jac < j_thr) ? m_clamp((j_thr - jac) * f_gain, 0.0f, 1.0f) : 0.0f;
            f32 fm  = jc->first_update ? gen : m_clamp(c->foam[idx] * decay + gen * inject, 0.0f, 1.0f);
            c->foam[idx] = fm;

            /* RGBA8 paketleme: R=slope_x, G=slope_z (0 eğim = 128), B=normalize yükseklik, A=köpük */
            i32 r_b = (i32)m_floor(m_clamp(128.0f + sx * s_enc + 0.5f, 1.0f, 255.0f));
            i32 g_b = (i32)m_floor(m_clamp(128.0f + sz * s_enc + 0.5f, 1.0f, 255.0f));
            i32 b_b = (i32)m_floor(m_clamp((h * h_norm * 0.5f + 0.5f) * 255.0f + 0.5f, 0.0f, 255.0f));
            i32 a_b = (i32)m_floor(m_clamp(fm * 255.0f + 0.5f, 0.0f, 255.0f));
            tex_row[i] = FB_RGBA((u32)r_b, (u32)g_b, (u32)b_b, (u32)a_b);
        }
    }
}

void ocean_update(Ocean* o, f64 time_sec, f32 dt) {
    ASSERT(o != NULL);
    b32 first = !o->has_updated;
    for (u32 ci = 0; ci < OCEAN_CASCADES; ci++) {
        OceanCascade* c = &o->cascades[ci];
        CascadeJobCtx jc = { o, c, time_sec, dt, first };
        u32 n = c->n;
        job_parallel_for(n, 8u, job_pack_rows, &jc);
        job_parallel_for(4u * n, 8u, job_fft_rows, &jc);
        job_parallel_for(4u * n, 8u, job_fft_cols, &jc);
        job_parallel_for(n, 8u, job_unpack_rows, &jc);
        tex_build_mips(&c->tex);
    }
    o->has_updated = TRUE;
}

static void job_unpack_sim_rows(u32 start, u32 end, void* user) {
    CascadeJobCtx* jc = (CascadeJobCtx*)user;
    OceanCascade* c   = jc->cas;
    u32 n = c->n;
    for (u32 j = start; j < end; j++) {
        u32 row = j * n;
        for (u32 i = 0; i < n; i++) {
            u32 idx = row + i;
            c->height[idx]  = c->wr[0][idx];
            c->dxz[idx]     = c->wi[0][idx];
            c->disp_x[idx]  = c->wr[1][idx];
            c->disp_z[idx]  = c->wi[1][idx];
            c->slope_x[idx] = c->wr[2][idx];
            c->slope_z[idx] = c->wi[2][idx];
            c->dxx[idx]     = c->wr[3][idx];
            c->dzz[idx]     = c->wi[3][idx];
        }
    }
}

void ocean_update_sim(Ocean* o, f64 time_sec) {
    ASSERT(o != NULL);
    for (u32 ci = 0; ci < OCEAN_CASCADES; ci++) {
        OceanCascade* c = &o->cascades[ci];
        CascadeJobCtx jc = { o, c, time_sec, 0.0f, FALSE };
        u32 n = c->n;
        job_parallel_for(n, 8u, job_pack_rows, &jc);
        job_parallel_for(4u * n, 8u, job_fft_rows, &jc);
        job_parallel_for(4u * n, 8u, job_fft_cols, &jc);
        job_parallel_for(n, 8u, job_unpack_sim_rows, &jc);
    }
}

void ocean_sample_cascade(const OceanCascade* c, f32 choppy, f32 x, f32 z, OceanSample* out) {
    ASSERT(c != NULL && out != NULL);
    u32 n   = c->n;
    u32 m   = n - 1u;
    f32 inv = (f32)n / c->size;
    f32 gx  = x * inv;
    f32 gz  = z * inv;
    f32 fx  = m_floor(gx);
    f32 fz  = m_floor(gz);
    f32 tx  = gx - fx;
    f32 tz  = gz - fz;

    u32 x0 = (u32)(i32)fx & m, z0 = (u32)(i32)fz & m;
    u32 x1 = (x0 + 1u) & m,    z1 = (z0 + 1u) & m;
    u32 i00 = z0 * n + x0, i10 = z0 * n + x1;
    u32 i01 = z1 * n + x0, i11 = z1 * n + x1;

    f32 w00 = (1.0f - tx) * (1.0f - tz);
    f32 w10 = tx * (1.0f - tz);
    f32 w01 = (1.0f - tx) * tz;
    f32 w11 = tx * tz;

#define BL(arr) ((arr)[i00] * w00 + (arr)[i10] * w10 + (arr)[i01] * w01 + (arr)[i11] * w11)
    f32 h   = BL(c->height);
    f32 dx  = BL(c->disp_x);
    f32 dz  = BL(c->disp_z);
    f32 sx  = BL(c->slope_x);
    f32 sz  = BL(c->slope_z);
    f32 dxx = BL(c->dxx);
    f32 dzz = BL(c->dzz);
    f32 dxz = BL(c->dxz);
    f32 fm  = BL(c->foam);
#undef BL

    out->disp     = vec3(choppy * dx, h, choppy * dz);
    out->slope_x  = sx;
    out->slope_z  = sz;
    out->normal   = vec3_norm(vec3(-sx, 1.0f, -sz));
    out->jacobian = (1.0f + choppy * dxx) * (1.0f + choppy * dzz) - (choppy * dxz) * (choppy * dxz);
    out->foam     = fm;
}

void ocean_sample(const Ocean* o, f32 x, f32 z, f32 dist_from_eye, OceanSample* out) {
    ASSERT(o != NULL && out != NULL);
    /* Yüksek frekanslı kademeler uzaklıkla yumuşakça söner */
    f32 w[OCEAN_CASCADES];
    w[0] = smooth_falloff(dist_from_eye, 450.0f, 1750.0f);
    w[1] = smooth_falloff(dist_from_eye,  55.0f,  180.0f);
    w[2] = smooth_falloff(dist_from_eye,  12.0f,   42.0f);

    Vec3 disp   = vec3(0.0f, 0.0f, 0.0f);
    f32  sx     = 0.0f, sz = 0.0f;
    f32  dxx    = 0.0f, dzz = 0.0f, dxz = 0.0f;
    f32  foam   = 0.0f;
    f32  choppy = o->d.choppy;

    for (u32 ci = 0; ci < OCEAN_CASCADES; ci++) {
        f32 wc = w[ci];
        if (wc <= 1e-4f) continue;
        const OceanCascade* c = &o->cascades[ci];
        u32 n   = c->n;
        u32 m   = n - 1u;
        f32 inv = (f32)n / c->size;
        f32 gx  = x * inv;
        f32 gz  = z * inv;
        f32 fx  = m_floor(gx);
        f32 fz  = m_floor(gz);
        f32 tx  = gx - fx;
        f32 tz  = gz - fz;

        u32 x0 = (u32)(i32)fx & m, z0 = (u32)(i32)fz & m;
        u32 x1 = (x0 + 1u) & m,    z1 = (z0 + 1u) & m;
        u32 i00 = z0 * n + x0, i10 = z0 * n + x1;
        u32 i01 = z1 * n + x0, i11 = z1 * n + x1;

        f32 w00 = (1.0f - tx) * (1.0f - tz);
        f32 w10 = tx * (1.0f - tz);
        f32 w01 = (1.0f - tx) * tz;
        f32 w11 = tx * tz;

#define BL(arr) ((arr)[i00] * w00 + (arr)[i10] * w10 + (arr)[i01] * w01 + (arr)[i11] * w11)
        disp.x += wc * choppy * BL(c->disp_x);
        disp.y += wc * BL(c->height);
        disp.z += wc * choppy * BL(c->disp_z);
        sx     += wc * BL(c->slope_x);
        sz     += wc * BL(c->slope_z);
        dxx    += wc * BL(c->dxx);
        dzz    += wc * BL(c->dzz);
        dxz    += wc * BL(c->dxz);
        foam    = m_max(foam, wc * BL(c->foam));
#undef BL
    }

    out->disp     = disp;
    out->slope_x  = sx;
    out->slope_z  = sz;
    out->normal   = vec3_norm(vec3(-sx, 1.0f, -sz));
    out->jacobian = (1.0f + choppy * dxx) * (1.0f + choppy * dzz) - (choppy * dxz) * (choppy * dxz);
    out->foam     = m_clamp(foam, 0.0f, 1.0f);
}

/* ---------- Kamera merkezli geometrik okyanus ızgarası ---------- */

Mesh* ocean_mesh_init(Ocean* o, Arena* a, u32 div, f32 snap, f32 max_radius) {
    ASSERT(o != NULL && a != NULL);
    if (div < 8u) div = 8u;
    u32 verts_1d = div + 1u;
    u32 vcount   = verts_1d * verts_1d;
    u32 icount   = div * div * 6u;

    Mesh* m = mesh_alloc(a, vcount, icount, FALSE);
    o->mesh        = m;
    o->grid_div    = div;
    o->grid_snap   = (snap > 0.1f) ? snap : 4.0f;
    o->grid_radius = (max_radius > 50.0f) ? max_radius : 1600.0f;
    o->grid_ofs    = ARENA_PUSH_ARRAY(a, f32, verts_1d);

    /* Merkezde ince (~0.45 m), dışa doğru geometrik (s⁴) büyüyen 1B koordinat tablosu */
    f32 r_fine = 26.0f;
    f32 r_far  = m_max(o->grid_radius - r_fine, 0.0f);
    for (u32 i = 0; i < verts_1d; i++) {
        f32 u   = (2.0f * (f32)i) / (f32)div - 1.0f;
        f32 s   = m_abs(u);
        f32 sgn = (u >= 0.0f) ? 1.0f : -1.0f;
        f32 s2  = s * s;
        o->grid_ofs[i] = sgn * (r_fine * s + r_far * s2 * s2);
    }

    u32* ix = m->indices;
    for (u32 j = 0; j < div; j++) {
        for (u32 i = 0; i < div; i++) {
            u32 a0 = j * verts_1d + i;
            u32 a1 = a0 + 1u;
            u32 b0 = a0 + verts_1d;
            u32 b1 = b0 + 1u;
            /* +Y yukarıdan bakınca CCW */
            *ix++ = a0; *ix++ = b0; *ix++ = b1;
            *ix++ = a0; *ix++ = b1; *ix++ = a1;
        }
    }

    return m;
}

typedef struct {
    const Ocean* o;
    Mesh*        m;
    f32          cx, cz;
    Vec3         eye;
} MeshUpdateCtx;

static void job_mesh_rows(u32 start, u32 end, void* user) {
    MeshUpdateCtx* mc = (MeshUpdateCtx*)user;
    const Ocean* o    = mc->o;
    Mesh* m           = mc->m;
    u32 verts_1d      = o->grid_div + 1u;
    f32 inv_l0        = 1.0f / o->cascades[0].size;

    for (u32 j = start; j < end; j++) {
        f32 wz  = mc->cz + o->grid_ofs[j];
        f32 dz  = wz - mc->eye.z;
        u32 row = j * verts_1d;
        for (u32 i = 0; i < verts_1d; i++) {
            f32 wx   = mc->cx + o->grid_ofs[i];
            f32 dx   = wx - mc->eye.x;
            f32 dist = m_sqrt(dx * dx + dz * dz);

            OceanSample smp;
            ocean_sample(o, wx, wz, dist, &smp);

            u32 vi    = row + i;
            m->px[vi] = wx + smp.disp.x;
            m->py[vi] = smp.disp.y;
            m->pz[vi] = wz + smp.disp.z;
            m->nx[vi] = smp.normal.x;
            m->ny[vi] = smp.normal.y;
            m->nz[vi] = smp.normal.z;
            /* Yer değiştirmemiş Lagrange koordinatı (x/L0, z/L0) → doku dalga tepesine kilitlenir */
            m->u[vi]  = wx * inv_l0;
            m->v[vi]  = wz * inv_l0;
        }
    }
}

void ocean_mesh_update(Ocean* o, Vec3 eye) {
    ASSERT(o != NULL && o->mesh != NULL);
    f32 snap = o->grid_snap;
    f32 cx   = m_floor(eye.x / snap + 0.5f) * snap;
    f32 cz   = m_floor(eye.z / snap + 0.5f) * snap;

    MeshUpdateCtx mc = { o, o->mesh, cx, cz, eye };
    job_parallel_for(o->grid_div + 1u, 4u, job_mesh_rows, &mc);

    f32 r = o->grid_radius + 16.0f;
    f32 h = m_max(o->hs * 2.5f, 10.0f);
    o->mesh->aabb_min = vec3(cx - r, -h, cz - r);
    o->mesh->aabb_max = vec3(cx + r,  h, cz + r);
    o->mesh->center   = vec3(cx, 0.0f, cz);
    o->mesh->radius   = m_sqrt(2.0f * r * r + h * h);
}
