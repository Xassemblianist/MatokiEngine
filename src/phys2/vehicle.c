/* phys2/vehicle.c — raycast tekerlekli araç.
 * Sıra: (1) her tekerlek için ışın + süspansiyon KUVVETİ (bağlantı noktasında, temas normali boyunca;
 *     kuvvet biriktiricisiyle alt adımlarda entegre — yerçekimiyle aynı zamanlama, titreşim yok)
 * (2) lastikler: küçük bir yinelemeli çözücü (4 tur, her turda sıra ters → sol/sağ asimetrisi yok):
 *     boyuna = sabit motor itkisi + birikimli fren kısıtı, yanal = kayma hızını sıfırlayan birikimli
 *     itki; ikisi birlikte |(L, S)| <= μ·Fn·dt sürtünme çemberinde.
 * Yanal itki temas noktasından kütle merkezi yüksekliğine doğru (roll_reduction) taşınır:
 * gerçek araçlardaki düşük ağırlık merkezi / viraj denge çubuğu etkisinin basit karşılığı.
 * SamurayBalgami/Matoki'den uyarlandı (izinle).
 */
#include "phys2/vehicle.h"

Phys2VehicleDesc phys2_vehicle_desc_default(Vec3 pos) {
    Phys2VehicleDesc d;
    d.pos = pos;
    d.rot = phys2_quat_identity();
    d.half = vec3(0.9f, 0.35f, 2.0f);
    d.mass = 1200.0f;
    d.spring = 30000.0f;
    d.damper = 2600.0f;
    d.engine_force = 9000.0f;
    d.brake_force = 16000.0f;
    d.max_steer = 0.55f;
    d.grip = 1.3f;
    d.roll_reduction = 0.6f;
    return d;
}

void phys2_vehicle_create(Phys2World* w, Phys2Vehicle* v, const Phys2VehicleDesc* d) {
    Phys2BodyDesc b = phys2_desc_box(d->pos, d->half, d->mass);
    b.rot = d->rot;
    b.friction = 0.4f;
    b.restitution = 0.05f;
    v->body = phys2_add_body(w, &b);
    w->bodies[v->body].flags |= PHYS2_BODY_NO_SLEEP;
    v->spring = d->spring;
    v->damper = d->damper;
    v->engine_force = d->engine_force;
    v->brake_force = d->brake_force;
    v->max_steer = d->max_steer;
    v->grip = d->grip;
    v->roll_reduction = d->roll_reduction;
    v->throttle = v->brake = v->steer = 0.0f;
    v->handbrake = FALSE;
    v->wheel_count = 4;
    const f32 x = d->half.x - 0.05f, y = -d->half.y + 0.1f, z = d->half.z - 0.65f;
    const f32 sx[4] = { -x, x, -x, x }, sz[4] = { -z, -z, z, z };   /* 0,1 ön (-z), 2,3 arka */
    for (u32 i = 0; i < 4; i++) {
        Phys2Wheel* wh = &v->wheels[i];
        *wh = (Phys2Wheel){ 0 };
        wh->local_pos = vec3(sx[i], y, sz[i]);
        wh->rest_length = 0.5f;
        wh->radius = 0.4f;
        wh->steer = i < 2;
        wh->drive = i >= 2;
        wh->handbrake = i >= 2;
        wh->ground_body = PHYS2_NO_BODY;
    }
}

Vec3 phys2_vehicle_forward(const Phys2World* w, const Phys2Vehicle* v) {
    return vec3_scale(phys2_mat3_col(&w->bodies[v->body].rot_m, 2), -1.0f);
}

void phys2_vehicle_update(Phys2World* w, Phys2Vehicle* v, f32 dt) {
    Phys2Body* ch = &w->bodies[v->body];
    Vec3 up = phys2_mat3_col(&ch->rot_m, 1);
    Vec3 fwd = vec3_scale(phys2_mat3_col(&ch->rot_m, 2), -1.0f);
    Vec3 left = vec3_cross(up, fwd);
    Vec3 down = vec3_scale(up, -1.0f);
    v->speed = vec3_dot(ch->vel, fwd);
    v->wheels_on_ground = 0;

    /* 1) süspansiyon */
    u32 drive_count = 0;
    for (u32 i = 0; i < v->wheel_count; i++) {
        Phys2Wheel* wh = &v->wheels[i];
        if (wh->drive) drive_count++;
        Vec3 attach = vec3_add(ch->pos, phys2_mat3_mul_v3(&ch->rot_m, wh->local_pos));
        Phys2RayHit hit;
        wh->contact = phys2_raycast(w, attach, down, wh->rest_length + wh->radius, v->body, &hit);
        if (!wh->contact) {
            wh->compression = wh->prev_compression = 0.0f;
            wh->load = 0.0f;
            wh->ground_body = PHYS2_NO_BODY;
            continue;
        }
        v->wheels_on_ground++;
        wh->ground_body = hit.body;
        wh->contact_point = hit.point;
        wh->contact_normal = hit.normal;
        wh->compression = m_clamp(wh->rest_length - (hit.t - wh->radius), 0.0f, wh->rest_length);
        f32 comp_vel = (wh->compression - wh->prev_compression) / dt;
        wh->prev_compression = wh->compression;
        f32 f = m_max(v->spring * wh->compression + v->damper * comp_vel, 0.0f);
        wh->load = f;
        Vec3 F = vec3_scale(hit.normal, f);
        phys2_apply_force_at(w, v->body, F, attach);
        if (w->bodies[hit.body].inv_mass > 0.0f) phys2_apply_force_at(w, hit.body, vec3_scale(F, -1.0f), hit.point);
    }

    /* 2) lastikler: ön hesap */
    typedef struct Tire {
        Vec3 wf, side, p;
        f32 mf, ms, drive, brake_lim, max_imp;
        f32 acc_l, acc_s, acc_brake;
    } Tire;
    Tire tires[PHYS2_VEHICLE_MAX_WHEELS];
    for (u32 i = 0; i < v->wheel_count; i++) {
        Phys2Wheel* wh = &v->wheels[i];
        Tire* t = &tires[i];
        if (!wh->contact) continue;
        Vec3 n = wh->contact_normal;
        f32 a = wh->steer ? v->steer * v->max_steer : 0.0f;
        Vec3 wf = vec3_add(vec3_scale(fwd, m_cos(a)), vec3_scale(left, m_sin(a)));
        t->wf = phys2_v3_norm(vec3_sub(wf, vec3_scale(n, vec3_dot(wf, n))));
        t->side = vec3_cross(n, t->wf);
        t->p = vec3_add(wh->contact_point, vec3_scale(up, v->roll_reduction * vec3_dot(vec3_sub(ch->pos, wh->contact_point), up)));
        t->mf = phys2_effective_mass(ch, t->p, t->wf);
        t->ms = phys2_effective_mass(ch, t->p, t->side);
        t->drive = (wh->drive && drive_count) ? v->engine_force * v->throttle / (f32)drive_count * dt : 0.0f;
        f32 brake = (v->handbrake && wh->handbrake) ? 1.0f : v->brake;
        t->brake_lim = v->brake_force * brake / (f32)v->wheel_count * dt;
        t->max_imp = v->grip * wh->load * dt * ((v->handbrake && wh->handbrake) ? 0.4f : 1.0f);
        t->acc_l = t->acc_s = t->acc_brake = 0.0f;
    }
    /* yinelemeli çözüm (tur başına sıra ters) */
    for (u32 it = 0; it < 4; it++) {
        for (u32 k = 0; k < v->wheel_count; k++) {
            u32 i = (it & 1) ? v->wheel_count - 1 - k : k;
            Phys2Wheel* wh = &v->wheels[i];
            if (!wh->contact) continue;
            Tire* t = &tires[i];
            const Phys2Body* g = &w->bodies[wh->ground_body];
            Vec3 vp = phys2_point_velocity(ch, t->p);
            if (g->inv_mass > 0.0f) vp = vec3_sub(vp, phys2_point_velocity(g, wh->contact_point));
            f32 vf = vec3_dot(vp, t->wf), vs = vec3_dot(vp, t->side);
            f32 nb = m_clamp(t->acc_brake - vf * t->mf, -t->brake_lim, t->brake_lim);
            f32 L = t->drive + nb;
            f32 S = t->acc_s - vs * t->ms;
            f32 len = m_sqrt(L * L + S * S);
            if (len > t->max_imp && len > 0.0f) {
                f32 s = t->max_imp / len;
                L *= s;
                S *= s;
            }
            t->acc_brake = L - t->drive;
            Vec3 J = vec3_add(vec3_scale(t->wf, L - t->acc_l), vec3_scale(t->side, S - t->acc_s));
            t->acc_l = L;
            t->acc_s = S;
            phys2_apply_impulse_at(w, v->body, J, t->p);
            if (g->inv_mass > 0.0f) phys2_apply_impulse_at(w, wh->ground_body, vec3_scale(J, -1.0f), wh->contact_point);
        }
    }
    /* yuvarlanma direnci, çizim için dönüş, kayma */
    for (u32 i = 0; i < v->wheel_count; i++) {
        Phys2Wheel* wh = &v->wheels[i];
        f32 vf = 0.0f;
        if (wh->contact) {
            Tire* t = &tires[i];
            Vec3 vp = phys2_point_velocity(ch, t->p);
            vf = vec3_dot(vp, t->wf);
            wh->slip = vec3_dot(vp, t->side);
            phys2_apply_impulse_at(w, v->body, vec3_scale(t->wf, -vf * t->mf * 0.002f), t->p);
        }
        wh->spin -= vf / wh->radius * dt;
        if (wh->spin > M_TAU) wh->spin -= M_TAU;
        if (wh->spin < -M_TAU) wh->spin += M_TAU;
    }
}

Mat4 phys2_vehicle_wheel_matrix(const Phys2World* w, const Phys2Vehicle* v, u32 i) {
    const Phys2Body* ch = &w->bodies[v->body];
    const Phys2Wheel* wh = &v->wheels[i];
    Vec3 up = phys2_mat3_col(&ch->rot_m, 1);
    Vec3 attach = vec3_add(ch->pos, phys2_mat3_mul_v3(&ch->rot_m, wh->local_pos));
    Vec3 c = vec3_sub(attach, vec3_scale(up, wh->rest_length - wh->compression));
    f32 a = wh->steer ? v->steer * v->max_steer : 0.0f;
    Phys2Quat q = phys2_quat_mul(ch->rot, phys2_quat_mul(phys2_quat_from_axis_angle(vec3(0, 1, 0), a), phys2_quat_from_axis_angle(vec3(1, 0, 0), wh->spin)));
    return phys2_mat4_from_pos_rot_scale(c, q, vec3(0.28f, wh->radius, wh->radius));
}
