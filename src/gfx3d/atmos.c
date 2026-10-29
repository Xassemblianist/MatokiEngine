/* gfx3d/atmos.c — Hillaire 2020 fiziksel atmosfer, Beer-Powder zayıflaması ve 3B bulut gürültüsü.
 * Rayleigh + Mie + Ozon optik derinlik integrali, çoklu saçılım geometrik serisi, Sky-View ve
 * Aerial Perspective ışın yürüyüşü ile periyodik 3B Perlin-Worley gürültü hesaplarını gerçekler.
 */
#include "gfx3d/atmos.h"

#define LOG2_E 1.44269504f

static inline f32 atmos_exp(f32 x) {
    return m_exp2(x * LOG2_E);
}

static inline Vec3 vec3_mul(Vec3 a, Vec3 b) {
    return vec3(a.x * b.x, a.y * b.y, a.z * b.z);
}

static inline Vec3 vec3_exp_neg(Vec3 tau) {
    return vec3(atmos_exp(-tau.x), atmos_exp(-tau.y), atmos_exp(-tau.z));
}

static inline f32 smoothstep_f(f32 e0, f32 e1, f32 x) {
    f32 t = m_clamp((x - e0) / (e1 - e0), 0.0f, 1.0f);
    return t * t * (3.0f - 2.0f * t);
}

AtmosParams atmos_default_params(void) {
    AtmosParams p;
    __builtin_memset(&p, 0, sizeof(p));
    p.r_bottom       = 6360.0f;
    p.r_top          = 6460.0f;
    p.rayleigh_scat  = vec3(0.005802f, 0.013558f, 0.033100f);
    p.rayleigh_h     = 8.0f;
    p.mie_scat       = vec3(0.003996f, 0.003996f, 0.003996f);
    p.mie_ext        = vec3(0.004440f, 0.004440f, 0.004440f);
    p.mie_h          = 1.2f;
    p.mie_g          = 0.80f;
    p.ozone_abs      = vec3(0.000650f, 0.001881f, 0.000085f);
    p.ozone_center_h = 25.0f;
    p.ozone_half_w   = 15.0f;
    p.ground_albedo  = vec3(0.04f, 0.07f, 0.12f);

    p.cloud_coverage  = 0.48f;
    p.cloud_bottom_km = 1.50f;
    p.cloud_top_km    = 4.20f;
    p.cloud_wind_x    = 14.0f;
    p.cloud_wind_z    = 6.0f;
    p.day_cycle_speed = 0.0f;

    atmos_set_sun_angles(&p, 0.42f, 2.35f);
    return p;
}

f32 atmos_ray_sphere_intersect(f32 r, f32 mu, f32 r_sphere) {
    f32 disc = r * r * (mu * mu - 1.0f) + r_sphere * r_sphere;
    if (disc < 0.0f) return -1.0f;
    f32 s = m_sqrt(disc);
    f32 d0 = -r * mu - s;
    if (d0 >= 0.0f) return d0;
    f32 d1 = -r * mu + s;
    return d1 >= 0.0f ? d1 : -1.0f;
}

b32 atmos_hits_ground(f32 r, f32 mu, f32 r_bottom) {
    if (mu >= 0.0f) return FALSE;
    f32 disc = r * r * (mu * mu - 1.0f) + r_bottom * r_bottom;
    return disc >= 0.0f;
}

void atmos_medium_sample(const AtmosParams* p, f32 h_km,
                         Vec3* out_scat_r, Vec3* out_scat_m, Vec3* out_ext) {
    f32 h   = m_max(h_km, 0.0f);
    f32 d_r = atmos_exp(-h / p->rayleigh_h);
    f32 d_m = atmos_exp(-h / p->mie_h);
    f32 d_o = m_max(0.0f, 1.0f - m_abs(h - p->ozone_center_h) / p->ozone_half_w);

    Vec3 sr  = vec3_scale(p->rayleigh_scat, d_r);
    Vec3 sm  = vec3_scale(p->mie_scat, d_m);
    Vec3 em  = vec3_scale(p->mie_ext, d_m);
    Vec3 ao  = vec3_scale(p->ozone_abs, d_o);
    Vec3 ext = vec3_add(vec3_add(sr, em), ao);

    if (out_scat_r) *out_scat_r = sr;
    if (out_scat_m) *out_scat_m = sm;
    if (out_ext)    *out_ext    = ext;
}

/* Rayleigh ve Cornette-Shanks Mie faz fonksiyonları */
static f32 phase_rayleigh(f32 cos_theta) {
    return (3.0f / (16.0f * M_PI)) * (1.0f + cos_theta * cos_theta);
}

static f32 phase_mie(f32 cos_theta, f32 g) {
    f32 g2    = g * g;
    f32 num   = 3.0f * (1.0f - g2) * (1.0f + cos_theta * cos_theta);
    f32 base  = 1.0f + g2 - 2.0f * g * cos_theta;
    f32 denom = (8.0f * M_PI) * (2.0f + g2) * base * m_sqrt(m_max(base, 1e-4f));
    return num / denom;
}

Vec3 atmos_eval_transmittance(const AtmosParams* p, f32 h_km, f32 mu) {
    f32 r = m_clamp(p->r_bottom + h_km, p->r_bottom + 0.001f, p->r_top - 0.001f);
    f32 s_max = atmos_ray_sphere_intersect(r, mu, p->r_top);
    if (s_max <= 0.0f) return vec3(1.0f, 1.0f, 1.0f);

    const u32 steps = 16u;
    f32 ds = s_max / (f32)steps;
    Vec3 tau = vec3(0.0f, 0.0f, 0.0f);

    for (u32 i = 0; i < steps; i++) {
        f32 s   = ((f32)i + 0.5f) * ds;
        f32 r_s = m_sqrt(r * r + s * s + 2.0f * r * s * mu);
        f32 h_s = r_s - p->r_bottom;
        Vec3 ext;
        atmos_medium_sample(p, h_s, NULL, NULL, &ext);
        tau = vec3_add(tau, vec3_scale(ext, ds));
    }
    return vec3_exp_neg(tau);
}

/* Hillaire 2020 §4: İkinci mertebe izotropik saçılım ve sonsuz geometrik seri Ψ_ms = L_2nd / (1 - f_ms) */
Vec3 atmos_eval_multiscatter(const AtmosParams* p, f32 h_km, f32 mu_s) {
    f32 r = m_clamp(p->r_bottom + h_km, p->r_bottom + 0.01f, p->r_top - 0.01f);

    /* 8 zenit açısı üzerinden küresel integral (simetrik azimut düzleminde) */
    const u32 dir_count = 8u;
    const u32 ray_steps = 8u;
    const f32 iso_phase = 1.0f / (4.0f * M_PI);

    Vec3 l_2nd = vec3(0.0f, 0.0f, 0.0f);
    Vec3 f_ms  = vec3(0.0f, 0.0f, 0.0f);

    for (u32 d = 0; d < dir_count; d++) {
        f32 mu_v  = -1.0f + 2.0f * (((f32)d + 0.5f) / (f32)dir_count);
        b32 hit_g = atmos_hits_ground(r, mu_v, p->r_bottom);
        f32 s_max = hit_g ? atmos_ray_sphere_intersect(r, mu_v, p->r_bottom)
                          : atmos_ray_sphere_intersect(r, mu_v, p->r_top);
        if (s_max <= 0.0f) continue;

        f32 ds = s_max / (f32)ray_steps;
        Vec3 trans = vec3(1.0f, 1.0f, 1.0f);
        Vec3 dir_l = vec3(0.0f, 0.0f, 0.0f);
        Vec3 dir_f = vec3(0.0f, 0.0f, 0.0f);

        for (u32 i = 0; i < ray_steps; i++) {
            f32 s   = ((f32)i + 0.5f) * ds;
            f32 r_s = m_sqrt(r * r + s * s + 2.0f * r * s * mu_v);
            f32 h_s = m_max(0.0f, r_s - p->r_bottom);

            Vec3 sr, sm, ext;
            atmos_medium_sample(p, h_s, &sr, &sm, &ext);
            Vec3 scat     = vec3_add(sr, sm);
            Vec3 step_t   = vec3_exp_neg(vec3_scale(ext, ds));
            Vec3 sint     = vec3(
                (1.0f - step_t.x) / m_max(ext.x, 1e-6f),
                (1.0f - step_t.y) / m_max(ext.y, 1e-6f),
                (1.0f - step_t.z) / m_max(ext.z, 1e-6f)
            );

            dir_f = vec3_add(dir_f, vec3_mul(trans, vec3_mul(scat, sint)));

            if (!atmos_hits_ground(r_s, mu_s, p->r_bottom)) {
                Vec3 t_sun = atmos_eval_transmittance(p, h_s, mu_s);
                Vec3 s_in  = vec3_scale(vec3_mul(scat, t_sun), iso_phase);
                dir_l = vec3_add(dir_l, vec3_mul(trans, vec3_mul(s_in, sint)));
            }
            trans = vec3_mul(trans, step_t);
        }

        if (hit_g && mu_s > 0.0f) {
            Vec3 t_sun_g = atmos_eval_transmittance(p, 0.0f, mu_s);
            Vec3 g_lum   = vec3_scale(vec3_mul(t_sun_g, p->ground_albedo), (mu_s / M_PI) * iso_phase * 4.0f * M_PI);
            dir_l = vec3_add(dir_l, vec3_mul(trans, g_lum));
        }

        f32 w = 1.0f / (f32)dir_count;
        l_2nd = vec3_add(l_2nd, vec3_scale(dir_l, w));
        f_ms  = vec3_add(f_ms,  vec3_scale(dir_f, w));
    }

    return vec3(
        l_2nd.x / m_max(1.0f - f_ms.x, 0.05f),
        l_2nd.y / m_max(1.0f - f_ms.y, 0.05f),
        l_2nd.z / m_max(1.0f - f_ms.z, 0.05f)
    );
}

Vec3 atmos_eval_sky_ray(const AtmosParams* p, f32 eye_h_km, Vec3 ray_dir, Vec3 sun_dir) {
    ray_dir = vec3_norm(ray_dir);
    sun_dir = vec3_norm(sun_dir);

    f32 r     = m_clamp(p->r_bottom + eye_h_km, p->r_bottom + 0.002f, p->r_top - 0.01f);
    f32 mu_v  = ray_dir.y;
    b32 hit_g = atmos_hits_ground(r, mu_v, p->r_bottom);
    f32 s_max = hit_g ? atmos_ray_sphere_intersect(r, mu_v, p->r_bottom)
                      : atmos_ray_sphere_intersect(r, mu_v, p->r_top);
    if (s_max <= 0.0f) return vec3(0.0f, 0.0f, 0.0f);

    f32 cos_theta = vec3_dot(ray_dir, sun_dir);
    f32 pr        = phase_rayleigh(cos_theta);
    f32 pm        = phase_mie(cos_theta, p->mie_g);

    const u32 steps = 16u;
    f32 ds = s_max / (f32)steps;
    Vec3 trans = vec3(1.0f, 1.0f, 1.0f);
    Vec3 L     = vec3(0.0f, 0.0f, 0.0f);

    for (u32 i = 0; i < steps; i++) {
        f32 s   = ((f32)i + 0.5f) * ds;
        /* Küresel yerel zenit ve güneş açısı */
        Vec3 pos  = vec3(ray_dir.x * s, r + ray_dir.y * s, ray_dir.z * s);
        f32  r_s  = vec3_len(pos);
        f32  h_s  = m_max(0.0f, r_s - p->r_bottom);
        Vec3 up_s = vec3_scale(pos, 1.0f / m_max(r_s, 1.0f));
        f32  mu_s = vec3_dot(up_s, sun_dir);

        Vec3 sr, sm, ext;
        atmos_medium_sample(p, h_s, &sr, &sm, &ext);
        Vec3 step_t = vec3_exp_neg(vec3_scale(ext, ds));
        Vec3 sint   = vec3(
            (1.0f - step_t.x) / m_max(ext.x, 1e-6f),
            (1.0f - step_t.y) / m_max(ext.y, 1e-6f),
            (1.0f - step_t.z) / m_max(ext.z, 1e-6f)
        );

        Vec3 single_s = vec3(0.0f, 0.0f, 0.0f);
        if (!atmos_hits_ground(r_s, mu_s, p->r_bottom)) {
            Vec3 t_sun = atmos_eval_transmittance(p, h_s, mu_s);
            Vec3 phase_scat = vec3_add(vec3_scale(sr, pr), vec3_scale(sm, pm));
            single_s = vec3_mul(phase_scat, t_sun);
        }

        Vec3 psi_ms  = atmos_eval_multiscatter(p, h_s, mu_s);
        Vec3 multi_s = vec3_mul(vec3_add(sr, sm), psi_ms);
        Vec3 s_tot   = vec3_add(single_s, multi_s);

        L     = vec3_add(L, vec3_mul(trans, vec3_mul(s_tot, sint)));
        trans = vec3_mul(trans, step_t);
    }

    return vec3_scale(L, 14.0f);
}

void atmos_eval_aerial(const AtmosParams* p, f32 eye_h_km, Vec3 ray_dir, Vec3 sun_dir,
                       f32 max_dist_km, Vec3* out_inscatter, f32* out_trans) {
    ray_dir = vec3_norm(ray_dir);
    sun_dir = vec3_norm(sun_dir);

    f32 r = m_clamp(p->r_bottom + eye_h_km, p->r_bottom + 0.002f, p->r_top - 0.01f);
    f32 cos_theta = vec3_dot(ray_dir, sun_dir);
    f32 pr        = phase_rayleigh(cos_theta);
    f32 pm        = phase_mie(cos_theta, p->mie_g);

    const u32 steps = 8u;
    f32 ds = m_max(max_dist_km, 0.01f) / (f32)steps;
    Vec3 trans = vec3(1.0f, 1.0f, 1.0f);
    Vec3 L     = vec3(0.0f, 0.0f, 0.0f);

    for (u32 i = 0; i < steps; i++) {
        f32  s    = ((f32)i + 0.5f) * ds;
        Vec3 pos  = vec3(ray_dir.x * s, r + ray_dir.y * s, ray_dir.z * s);
        f32  r_s  = vec3_len(pos);
        f32  h_s  = m_max(0.0f, r_s - p->r_bottom);
        Vec3 up_s = vec3_scale(pos, 1.0f / m_max(r_s, 1.0f));
        f32  mu_s = vec3_dot(up_s, sun_dir);

        Vec3 sr, sm, ext;
        atmos_medium_sample(p, h_s, &sr, &sm, &ext);
        Vec3 step_t = vec3_exp_neg(vec3_scale(ext, ds));
        Vec3 sint   = vec3(
            (1.0f - step_t.x) / m_max(ext.x, 1e-6f),
            (1.0f - step_t.y) / m_max(ext.y, 1e-6f),
            (1.0f - step_t.z) / m_max(ext.z, 1e-6f)
        );

        Vec3 t_sun      = atmos_eval_transmittance(p, h_s, m_max(mu_s, -0.15f));
        Vec3 phase_scat = vec3_add(vec3_scale(sr, pr), vec3_scale(sm, pm));
        Vec3 s_in       = vec3_mul(phase_scat, t_sun);

        L     = vec3_add(L, vec3_mul(trans, vec3_mul(s_in, sint)));
        trans = vec3_mul(trans, step_t);
    }

    if (out_inscatter) *out_inscatter = vec3_scale(L, 14.0f);
    if (out_trans)     *out_trans     = (trans.x + trans.y + trans.z) * (1.0f / 3.0f);
}

static Vec3 tone_map_sky(Vec3 hdr) {
    /* Yumuşak ACES benzeri omuz ton eşlemesi */
    f32 r = hdr.x / (1.0f + hdr.x * 0.65f);
    f32 g = hdr.y / (1.0f + hdr.y * 0.65f);
    f32 b = hdr.z / (1.0f + hdr.z * 0.65f);
    return vec3(m_clamp(r, 0.0f, 1.0f), m_clamp(g, 0.0f, 1.0f), m_clamp(b, 0.0f, 1.0f));
}

void atmos_update_lighting(AtmosParams* p) {
    f32 ce = m_cos(p->sun_elev);
    f32 se = m_sin(p->sun_elev);
    p->sun_dir = vec3_norm(vec3(ce * m_cos(p->sun_azim), se, ce * m_sin(p->sun_azim)));

    /* Ay yönü: güneş battığında gökyüzünde yükselir, azimut olarak hafif kaydırılmıştır */
    f32 moon_elev = m_clamp(-p->sun_elev + 0.15f, 0.22f, 1.15f);
    f32 moon_azim = p->sun_azim + 2.75f;
    f32 mce = m_cos(moon_elev);
    f32 mse = m_sin(moon_elev);
    p->moon_dir = vec3_norm(vec3(mce * m_cos(moon_azim), mse, mce * m_sin(moon_azim)));

    /* Gece yıldız ve ay yoğunluğu (güneş ufkun altına indiğinde devreye girer) */
    p->star_intensity = smoothstep_f(0.02f, -0.22f, p->sun_dir.y);
    p->moon_intensity = smoothstep_f(0.05f, -0.18f, p->sun_dir.y);

    /* Fiziksel atmosferden zenit ve ufuk renklerini örnekle */
    Vec3 view_zen = vec3(0.0f, 0.96f, 0.28f);
    Vec3 view_hor = vec3(p->sun_dir.x, 0.04f, p->sun_dir.z);
    if (vec3_len(view_hor) < 0.01f) view_hor = vec3(0.0f, 0.04f, 1.0f);

    Vec3 hdr_zen = atmos_eval_sky_ray(p, 0.01f, view_zen, p->sun_dir);
    Vec3 hdr_hor = atmos_eval_sky_ray(p, 0.01f, view_hor, p->sun_dir);

    /* Gece fazında ay ışığı saçılım tabanını ekle */
    Vec3 night_zen = vec3(0.020f, 0.036f, 0.082f);
    Vec3 night_hor = vec3(0.045f, 0.072f, 0.135f);

    Vec3 ldr_zen = tone_map_sky(hdr_zen);
    Vec3 ldr_hor = tone_map_sky(hdr_hor);

    p->sky_zenith = vec3(
        m_lerp(ldr_zen.x, night_zen.x, p->moon_intensity),
        m_lerp(ldr_zen.y, night_zen.y, p->moon_intensity),
        m_lerp(ldr_zen.z, night_zen.z, p->moon_intensity)
    );
    p->sky_horizon = vec3(
        m_lerp(ldr_hor.x, night_hor.x, p->moon_intensity),
        m_lerp(ldr_hor.y, night_hor.y, p->moon_intensity),
        m_lerp(ldr_hor.z, night_hor.z, p->moon_intensity)
    );

    /* Doğrudan güneş geçirgenliği + gece gümüşi ay ışığı geçişi */
    Vec3 t_sun   = atmos_eval_transmittance(p, 0.01f, m_max(p->sun_dir.y, -0.05f));
    f32  day_vis = smoothstep_f(-0.08f, 0.06f, p->sun_dir.y);
    Vec3 day_sun = vec3(
        m_clamp(t_sun.x * 1.35f, 0.15f, 1.0f),
        m_clamp(t_sun.y * 1.25f, 0.08f, 0.98f),
        m_clamp(t_sun.z * 1.15f, 0.04f, 0.92f)
    );
    Vec3 moon_col = vec3(0.32f, 0.42f, 0.58f);
    p->sun_color = vec3(
        m_lerp(moon_col.x, day_sun.x, day_vis),
        m_lerp(moon_col.y, day_sun.y, day_vis),
        m_lerp(moon_col.z, day_sun.z, day_vis)
    );
    p->fog_color = p->sky_horizon;
}

void atmos_set_sun_angles(AtmosParams* p, f32 sun_elev, f32 sun_azim) {
    p->sun_elev = m_clamp(sun_elev, -1.25f, 1.45f);
    p->sun_azim = sun_azim;
    atmos_update_lighting(p);
}

f32 atmos_beer_powder(f32 optical_depth) {
    f32 d      = m_max(optical_depth, 0.0f);
    f32 beer   = atmos_exp(-d);
    f32 powder = 1.0f - atmos_exp(-2.0f * d);
    return 2.0f * beer * powder;
}

/* --- 3B Periyodik Perlin-Worley + Worley Gürültü Üreticisi (Schneider 2015) --- */

static u32 hash3_u32(i32 x, i32 y, i32 z, i32 period, u32 seed) {
    u32 ux = (u32)(((x % period) + period) % period);
    u32 uy = (u32)(((y % period) + period) % period);
    u32 uz = (u32)(((z % period) + period) % period);
    u32 h  = ux * 374761393u + uy * 668265263u + uz * 2147483647u + seed * 1664525u;
    h = (h ^ (h >> 13u)) * 1274126177u;
    return h ^ (h >> 16u);
}

static Vec3 hash3_vec3(i32 x, i32 y, i32 z, i32 period, u32 seed) {
    u32 h = hash3_u32(x, y, z, period, seed);
    f32 rx = (f32)(h & 0x3FFu) * (1.0f / 1023.0f);
    f32 ry = (f32)((h >> 10u) & 0x3FFu) * (1.0f / 1023.0f);
    f32 rz = (f32)((h >> 20u) & 0x3FFu) * (1.0f / 1023.0f);
    return vec3(rx, ry, rz);
}

/* Periyodik 3B Hücresel Worley gürültüsü (ters çevrilmiş: 1 = hücre merkezi, 0 = sınır) */
static f32 worley3d_periodic(Vec3 uvw, i32 freq, u32 seed) {
    f32 px = uvw.x * (f32)freq;
    f32 py = uvw.y * (f32)freq;
    f32 pz = uvw.z * (f32)freq;
    i32 ix = (i32)m_floor(px);
    i32 iy = (i32)m_floor(py);
    i32 iz = (i32)m_floor(pz);
    f32 fx = px - (f32)ix;
    f32 fy = py - (f32)iy;
    f32 fz = pz - (f32)iz;

    f32 min_d2 = 4.0f;
    for (i32 dz = -1; dz <= 1; dz++) {
        for (i32 dy = -1; dy <= 1; dy++) {
            for (i32 dx = -1; dx <= 1; dx++) {
                Vec3 rnd = hash3_vec3(ix + dx, iy + dy, iz + dz, freq, seed);
                f32  ox  = (f32)dx + rnd.x - fx;
                f32  oy  = (f32)dy + rnd.y - fy;
                f32  oz  = (f32)dz + rnd.z - fz;
                f32  d2  = ox * ox + oy * oy + oz * oz;
                if (d2 < min_d2) min_d2 = d2;
            }
        }
    }
    return m_clamp(1.0f - m_sqrt(min_d2), 0.0f, 1.0f);
}

/* Periyodik 3B gradyan (Perlin) gürültüsü */
static f32 perlin3d_periodic(Vec3 uvw, i32 freq, u32 seed) {
    f32 px = uvw.x * (f32)freq;
    f32 py = uvw.y * (f32)freq;
    f32 pz = uvw.z * (f32)freq;
    i32 ix = (i32)m_floor(px);
    i32 iy = (i32)m_floor(py);
    i32 iz = (i32)m_floor(pz);
    f32 fx = px - (f32)ix;
    f32 fy = py - (f32)iy;
    f32 fz = pz - (f32)iz;

    f32 ux = fx * fx * fx * (fx * (fx * 6.0f - 15.0f) + 10.0f);
    f32 uy = fy * fy * fy * (fy * (fy * 6.0f - 15.0f) + 10.0f);
    f32 uz = fz * fz * fz * (fz * (fz * 6.0f - 15.0f) + 10.0f);

    f32 n[8];
    u32 idx = 0;
    for (i32 dz = 0; dz <= 1; dz++) {
        for (i32 dy = 0; dy <= 1; dy++) {
            for (i32 dx = 0; dx <= 1; dx++) {
                Vec3 g = vec3_sub(vec3_scale(hash3_vec3(ix + dx, iy + dy, iz + dz, freq, seed), 2.0f),
                                  vec3(1.0f, 1.0f, 1.0f));
                Vec3 d = vec3(fx - (f32)dx, fy - (f32)dy, fz - (f32)dz);
                n[idx++] = vec3_dot(g, d);
            }
        }
    }

    f32 nx00 = m_lerp(n[0], n[1], ux);
    f32 nx10 = m_lerp(n[2], n[3], ux);
    f32 nx01 = m_lerp(n[4], n[5], ux);
    f32 nx11 = m_lerp(n[6], n[7], ux);
    f32 nxy0 = m_lerp(nx00, nx10, uy);
    f32 nxy1 = m_lerp(nx01, nx11, uy);
    return m_clamp(0.5f + 0.65f * m_lerp(nxy0, nxy1, uz), 0.0f, 1.0f);
}

static f32 remap_f(f32 val, f32 o_min, f32 o_max, f32 n_min, f32 n_max) {
    f32 t = m_clamp((val - o_min) / m_max(o_max - o_min, 1e-5f), 0.0f, 1.0f);
    return n_min + t * (n_max - n_min);
}

void atmos_cloud_noise_texel(u32 x, u32 y, u32 z, u32 n, f32 out_rgba[4]) {
    f32 inv_n = 1.0f / (f32)m_max((f32)n, 1.0f);
    Vec3 uvw  = vec3(((f32)x + 0.5f) * inv_n,
                     ((f32)y + 0.5f) * inv_n,
                     ((f32)z + 0.5f) * inv_n);

    f32 p0 = perlin3d_periodic(uvw, 4,  11u);
    f32 p1 = perlin3d_periodic(uvw, 8,  23u);
    f32 p2 = perlin3d_periodic(uvw, 16, 37u);
    f32 perlin_fbm = m_clamp(p0 * 0.55f + p1 * 0.30f + p2 * 0.15f, 0.0f, 1.0f);

    f32 w0 = worley3d_periodic(uvw, 4,  101u);
    f32 w1 = worley3d_periodic(uvw, 8,  211u);
    f32 w2 = worley3d_periodic(uvw, 16, 307u);
    f32 w3 = worley3d_periodic(uvw, 32, 401u);
    f32 worley_fbm = w0 * 0.625f + w1 * 0.25f + w2 * 0.125f;

    /* R: Perlin-Worley taban kümülüs şekli, G/B/A: artan frekanslı Worley erozyon kanalları */
    out_rgba[0] = remap_f(perlin_fbm, 1.0f - worley_fbm, 1.0f, 0.0f, 1.0f);
    out_rgba[1] = m_clamp(w0 * 0.625f + w1 * 0.25f + w2 * 0.125f, 0.0f, 1.0f);
    out_rgba[2] = m_clamp(w1 * 0.625f + w2 * 0.25f + w3 * 0.125f, 0.0f, 1.0f);
    out_rgba[3] = m_clamp(w2 * 0.65f  + w3 * 0.35f,                0.0f, 1.0f);
}
