/* gfx3d/sail.c — Görsel sınıf (D-112) XPBD yelken kumaşı simülasyonu.
 * Direk (luff, i=0) ve bumba/alt yaka (foot, j=0) kenarları pimli veya yarı-pimli tutulur;
 * görünür rüzgârın yelkene dik bileşeni aerodinamik şişkinlik (belly) oluşturur, ölü bölgede
 * firar kenarında (leech) sinüzoidal çırpınma dalgası üretilir.
 */
#include "gfx3d/sail.h"
#include "core/log.h"

static void add_constraint(SailCloth* sc, u32 a, u32 b, f32 compliance) {
    if (sc->constr_count >= SAIL_MAX_CONSTR) return;
    f32 dx = sc->rx[b] - sc->rx[a];
    f32 dy = sc->ry[b] - sc->ry[a];
    f32 dz = sc->rz[b] - sc->rz[a];
    f32 len = m_sqrt(dx * dx + dy * dy + dz * dz);
    if (len < 1e-5f) return;
    SailConstraint* c = &sc->constr[sc->constr_count++];
    c->a          = (u16)a;
    c->b          = (u16)b;
    c->rest_len   = len;
    c->compliance = compliance;
}

Mesh* sail_cloth_init(SailCloth* sc, Arena* a,
                      f32 z_bot0, f32 z_bot1, f32 z_top0, f32 z_top1,
                      f32 y_bot, f32 y_top, f32 max_belly) {
    ASSERT(sc != NULL && a != NULL);
    __builtin_memset(sc, 0, sizeof(*sc));

    sc->mast_z    = z_bot1;
    sc->z_bot0    = z_bot0;
    sc->z_bot1    = z_bot1;
    sc->z_top0    = z_top0;
    sc->z_top1    = z_top1;
    sc->y_bot     = y_bot;
    sc->y_top     = y_top;
    sc->max_belly = max_belly;

    const u32 div = SAIL_GRID_DIV;
    const u32 n   = SAIL_GRID_N;
    Mesh* m = mesh_alloc(a, SAIL_VERT_COUNT, div * div * 6u, FALSE);
    sc->mesh = m;

    for (u32 j = 0; j < n; j++) {
        f32 vt = (f32)j / (f32)div;
        f32 y  = y_bot + (y_top - y_bot) * vt;
        f32 z0 = z_bot0 + (z_top0 - z_bot0) * vt;
        f32 z1 = z_bot1 + (z_top1 - z_bot1) * vt;
        for (u32 i = 0; i < n; i++) {
            /* i=0 firar kenarı (z0), i=div direk/orman kenarı (z1) */
            f32 ut = (f32)i / (f32)div;
            f32 z  = z0 + (z1 - z0) * ut;
            u32 vi = j * n + i;
            sc->rx[vi] = 0.0f;
            sc->ry[vi] = y;
            sc->rz[vi] = z;
            /* i == div (direk/ön kenar) ve (j == 0, i == 0) ıskota köşesi pimli */
            if (i == div || (j == 0u && (i == 0u || i == div))) {
                sc->inv_m[vi] = 0.0f;
            } else if (j == 0u) {
                sc->inv_m[vi] = 0.35f;   /* bumba boyunca kısıtlı esneme */
            } else {
                sc->inv_m[vi] = 1.0f;
            }
            m->u[vi] = ut;
            m->v[vi] = 1.0f - vt;
        }
    }

    u32* ix = m->indices;
    for (u32 j = 0; j < div; j++) {
        for (u32 i = 0; i < div; i++) {
            u32 a0 = j * n + i, a1 = a0 + 1u, b0 = a0 + n, b1 = b0 + 1u;
            *ix++ = a0; *ix++ = b0; *ix++ = b1;
            *ix++ = a0; *ix++ = b1; *ix++ = a1;
        }
    }

    /* Yapısal (yatay + dikey) ve bükülme (2 adım) kısıtları */
    for (u32 j = 0; j < n; j++) {
        for (u32 i = 0; i < n; i++) {
            u32 v = j * n + i;
            if (i + 1u < n) add_constraint(sc, v, v + 1u, 0.0002f);
            if (j + 1u < n) add_constraint(sc, v, v + n,  0.0002f);
            if (i + 2u < n) add_constraint(sc, v, v + 2u, 0.0025f);
            if (j + 2u < n) add_constraint(sc, v, v + 2u * n, 0.0025f);
        }
    }

    sail_cloth_reset(sc);
    return m;
}

void sail_cloth_reset(SailCloth* sc) {
    ASSERT(sc != NULL);
    const u32 div = SAIL_GRID_DIV;
    const u32 n   = SAIL_GRID_N;
    for (u32 j = 0; j < n; j++) {
        f32 vt = (f32)j / (f32)div;
        f32 vy_arch = 4.0f * vt * (1.0f - vt);
        for (u32 i = 0; i < n; i++) {
            f32 ut = (f32)i / (f32)div;
            f32 ux_arch = 4.0f * ut * (1.0f - ut);
            f32 belly = sc->max_belly * ux_arch * (0.65f + 0.35f * vy_arch);
            u32 vi = j * n + i;
            sc->px[vi] = (sc->inv_m[vi] > 0.0f) ? belly : 0.0f;
            sc->py[vi] = sc->ry[vi];
            sc->pz[vi] = sc->rz[vi];
            sc->vx[vi] = 0.0f;
            sc->vy[vi] = 0.0f;
            sc->vz[vi] = 0.0f;
        }
    }
    sc->boom_angle    = 0.0f;
    sc->flutter_phase = 0.0f;
    sail_cloth_step(sc, vec3(4.0f, 0.0f, 0.0f), 0.0f, 0.0f, 0.0f);
}

void sail_cloth_step(SailCloth* sc, Vec3 local_app_wind, f32 target_boom_angle,
                     f32 luffing, f32 dt) {
    ASSERT(sc != NULL && sc->mesh != NULL);
    const u32 div = SAIL_GRID_DIV;
    const u32 n   = SAIL_GRID_N;

    if (dt > 0.0f) {
        f32 h = m_min(dt, 1.0f / 30.0f);
        f32 boom_lerp = m_min(h * 10.0f, 1.0f);
        sc->boom_angle += (target_boom_angle - sc->boom_angle) * boom_lerp;

        f32 wind_spd = vec3_len(local_app_wind);
        sc->flutter_phase += h * (12.0f + wind_spd * 1.8f);
        if (sc->flutter_phase > M_TAU * 100.0f) sc->flutter_phase -= M_TAU * 100.0f;

        /* Yelkene dik rüzgâr bileşeni -> şişme yönü ve büyüklüğü */
        f32 cb = m_cos(sc->boom_angle);
        f32 sb = m_sin(sc->boom_angle);
        /* Yelken normali yakınsaması: (cos(boom), 0, -sin(boom)) */
        f32 w_norm = local_app_wind.x * cb - local_app_wind.z * sb;
        f32 sign_w = (w_norm >= 0.0f) ? 1.0f : -1.0f;
        f32 pressure = m_clamp(m_abs(w_norm) * 0.14f, 0.15f, 1.35f) * sign_w;
        if (luffing > 0.5f) {
            pressure *= (1.0f - (luffing - 0.5f) * 1.4f);
        }

        f32 ox[SAIL_VERT_COUNT];
        f32 oy[SAIL_VERT_COUNT];
        f32 oz[SAIL_VERT_COUNT];

        for (u32 j = 0; j < n; j++) {
            f32 vt = (f32)j / (f32)div;
            f32 vy_arch = 4.0f * vt * (1.0f - vt);
            for (u32 i = 0; i < n; i++) {
                u32 vi = j * n + i;
                ox[vi] = sc->px[vi];
                oy[vi] = sc->py[vi];
                oz[vi] = sc->pz[vi];
                if (sc->inv_m[vi] <= 0.0f) continue;

                f32 ut = (f32)i / (f32)div;
                f32 ux_arch = 4.0f * ut * (1.0f - ut);
                f32 shape = (0.68f * ux_arch + 0.32f * vy_arch) * m_sqrt(ut);
                f32 target_x = sc->max_belly * pressure * shape;

                /* Ölü bölgede (luffing) firar kenarına doğru (ut -> 0) çırpınma dalgası */
                if (luffing > 0.05f) {
                    f32 leech_w = (1.0f - ut);
                    f32 wave = m_sin(sc->flutter_phase - (1.0f - ut) * 9.5f + vt * 4.0f);
                    target_x += wave * luffing * 0.22f * leech_w;
                }

                f32 fx = (target_x - sc->px[vi]) * 42.0f - sc->vx[vi] * 7.5f;
                f32 fy = (sc->ry[vi] - sc->py[vi]) * 65.0f - sc->vy[vi] * 9.0f;
                f32 fz = (sc->rz[vi] - sc->pz[vi]) * 65.0f - sc->vz[vi] * 9.0f;

                sc->px[vi] += (sc->vx[vi] + fx * h) * h;
                sc->py[vi] += (sc->vy[vi] + fy * h) * h;
                sc->pz[vi] += (sc->vz[vi] + fz * h) * h;
            }
        }

        /* 3 XPBD Gauss-Seidel kısıt iterasyonu */
        f32 inv_h2 = 1.0f / (h * h);
        for (u32 it = 0; it < 3u; it++) {
            for (u32 ci = 0; ci < sc->constr_count; ci++) {
                const SailConstraint* c = &sc->constr[ci];
                u32 a = c->a, b = c->b;
                f32 wa = sc->inv_m[a], wb = sc->inv_m[b];
                f32 w_sum = wa + wb;
                if (w_sum <= 0.0f) continue;

                f32 dx = sc->px[b] - sc->px[a];
                f32 dy = sc->py[b] - sc->py[a];
                f32 dz = sc->pz[b] - sc->pz[a];
                f32 d  = m_sqrt(dx * dx + dy * dy + dz * dz);
                if (d < 1e-6f) continue;

                f32 err = d - c->rest_len;
                f32 alpha_tilde = c->compliance * inv_h2;
                f32 corr = err / (w_sum + alpha_tilde);
                f32 nx = dx / d, ny = dy / d, nz = dz / d;

                sc->px[a] += wa * corr * nx;
                sc->py[a] += wa * corr * ny;
                sc->pz[a] += wa * corr * nz;
                sc->px[b] -= wb * corr * nx;
                sc->py[b] -= wb * corr * ny;
                sc->pz[b] -= wb * corr * nz;
            }
        }

        f32 inv_h = 1.0f / h;
        for (u32 vi = 0; vi < SAIL_VERT_COUNT; vi++) {
            sc->px[vi] = m_clamp(sc->px[vi], -sc->max_belly * 1.45f, sc->max_belly * 1.45f);
            sc->vx[vi] = (sc->px[vi] - ox[vi]) * inv_h;
            sc->vy[vi] = (sc->py[vi] - oy[vi]) * inv_h;
            sc->vz[vi] = (sc->pz[vi] - oz[vi]) * inv_h;
        }
    }

    /* Bumba açısıyla direk menteşesi (mast_z) etrafında döndür ve Mesh'e yaz */
    Mesh* m = sc->mesh;
    f32 cb = m_cos(sc->boom_angle);
    f32 sb = m_sin(sc->boom_angle);
    f32 mz = sc->mast_z;

    for (u32 vi = 0; vi < SAIL_VERT_COUNT; vi++) {
        f32 lx = sc->px[vi];
        f32 ly = sc->py[vi];
        f32 lz = sc->pz[vi] - mz;
        m->px[vi] =  lx * cb + lz * sb;
        m->py[vi] =  ly;
        m->pz[vi] = -lx * sb + lz * cb + mz;
    }

    mesh_compute_normals(m);
    mesh_compute_bounds(m);
}
