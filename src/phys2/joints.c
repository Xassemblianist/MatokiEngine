/* phys2/joints.c — eklemler: top-mafsal, menteşe (sınır + motor), mesafe (çubuk / ip / aralık).
 * Top-mafsal: 3 doğrusal kısıt tek blokta (K⁻¹, K = (mA+mB)I − [rA]×IA[rA]× − [rB]×IB[rB]×).
 * Menteşe: top + eksene dik 2 açısal kısıt (hata = a × b) + eksen boyunca sınır/motor.
 * Mesafe: d = |pB − pA| tek satır; min = max çubuk, aksi halde tek yönlü (ip/itme).
 * Kuvvet kolları (ra, rb) adım başında sabitlenir; konum hatası güncel dönüşlerle ölçülür.
 * SamurayBalgami/Matoki'den uyarlandı (izinle).
 */
#include "phys2/joints.h"
#include "phys2/pmath.h"

#define JOINT_HERTZ   60.0f
#define JOINT_DAMPING 2.0f

static inline Phys2Body* jb(Phys2World* w, u32 i) { return i == PHYS2_NO_BODY ? &w->world_body : &w->bodies[i]; }
static inline const Phys2Body* jbc(const Phys2World* w, u32 i) { return i == PHYS2_NO_BODY ? &w->world_body : &w->bodies[i]; }

/* ---- küçük 3x3 cebir (sütun-öncelikli) ---- */

static Mat3 skew(Vec3 r) {
    Mat3 s;
    s.m[0] = 0.0f;  s.m[1] = r.z;   s.m[2] = -r.y;
    s.m[3] = -r.z;  s.m[4] = 0.0f;  s.m[5] = r.x;
    s.m[6] = r.y;   s.m[7] = -r.x;  s.m[8] = 0.0f;
    return s;
}

static Mat3 mat3_mul_ptr(const Mat3* a, const Mat3* b) {
    Mat3 r;
    for (u32 c = 0; c < 3; c++) {
        for (u32 row = 0; row < 3; row++) {
            f32 s = 0.0f;
            for (u32 k = 0; k < 3; k++) s += a->m[k * 3 + row] * b->m[c * 3 + k];
            r.m[c * 3 + row] = s;
        }
    }
    return r;
}

static Mat3 mat3_inverse(const Mat3* a) {
    Vec3 c0 = vec3(a->m[0], a->m[1], a->m[2]), c1 = vec3(a->m[3], a->m[4], a->m[5]), c2 = vec3(a->m[6], a->m[7], a->m[8]);
    Vec3 r0 = vec3_cross(c1, c2), r1 = vec3_cross(c2, c0), r2 = vec3_cross(c0, c1);
    f32 det = vec3_dot(c0, r0);
    f32 inv = det != 0.0f ? 1.0f / det : 0.0f;
    /* tersin satırları r0, r1, r2 → sütun-öncelikli */
    Mat3 o;
    o.m[0] = r0.x * inv; o.m[3] = r0.y * inv; o.m[6] = r0.z * inv;
    o.m[1] = r1.x * inv; o.m[4] = r1.y * inv; o.m[7] = r1.z * inv;
    o.m[2] = r2.x * inv; o.m[5] = r2.y * inv; o.m[8] = r2.z * inv;
    return o;
}

/* ---- API ---- */

Phys2JointDesc phys2_joint_desc(Phys2JointType type, u32 a, u32 b, Vec3 anchor) {
    Phys2JointDesc d;
    phys2_mem_zero(&d, sizeof(d));
    d.type = type;
    d.a = a;
    d.b = b;
    d.anchor_a = d.anchor_b = anchor;
    d.axis = vec3(0, 1, 0);
    return d;
}

u32 phys2_add_joint(Phys2World* w, const Phys2JointDesc* d) {
    if (w->joint_count >= w->joint_cap) PANIC("phys: eklem kapasitesi dolu");
    u32 id = w->joint_count++;
    Phys2Joint* j = &w->joints[id];
    phys2_mem_zero(j, sizeof(*j));
    j->type = d->type;
    j->a = d->a;
    j->b = d->b;
    const Phys2Body* A = jbc(w, d->a);
    const Phys2Body* B = jbc(w, d->b);
    Vec3 pb = d->type == PHYS2_JOINT_DISTANCE ? d->anchor_b : d->anchor_a;
    j->la = phys2_mat3_tmul_v3(&A->rot_m, vec3_sub(d->anchor_a, A->pos));
    j->lb = phys2_mat3_tmul_v3(&B->rot_m, vec3_sub(pb, B->pos));
    Vec3 ax = phys2_v3_norm(d->axis);
    Vec3 t1, t2;
    phys2_ortho_basis(ax, &t1, &t2);
    j->axis_a = phys2_mat3_tmul_v3(&A->rot_m, ax);
    j->axis_b = phys2_mat3_tmul_v3(&B->rot_m, ax);
    j->ref_a = phys2_mat3_tmul_v3(&A->rot_m, t1);
    j->ref_b = phys2_mat3_tmul_v3(&B->rot_m, t1);
    j->min_length = d->min_length;
    j->max_length = d->max_length;
    if (d->type == PHYS2_JOINT_DISTANCE && d->min_length == 0.0f && d->max_length == 0.0f) {
        j->min_length = j->max_length = vec3_len(vec3_sub(d->anchor_b, d->anchor_a));
    }
    j->limit = d->limit;
    j->lower = d->lower;
    j->upper = d->upper;
    j->motor = d->motor;
    j->motor_speed = d->motor_speed;
    j->max_motor_torque = d->max_motor_torque;
    j->collide_connected = d->collide_connected;
    j->length = vec3_len(vec3_sub(pb, d->anchor_a));
    if (d->b != PHYS2_NO_BODY) phys2_wake_body(w, d->b);
    if (d->a != PHYS2_NO_BODY) phys2_wake_body(w, d->a);
    return id;
}

b32 phys2_joints_block_pair(const Phys2World* w, u32 a, u32 b) {
    for (u32 k = 0; k < w->joint_count; k++) {
        const Phys2Joint* j = &w->joints[k];
        if (j->collide_connected) continue;
        if ((j->a == a && j->b == b) || (j->a == b && j->b == a)) return TRUE;
    }
    return FALSE;
}

static inline Vec3 world_anchor(const Phys2Body* b, Vec3 l) { return vec3_add(b->pos, phys2_mat3_mul_v3(&b->rot_m, l)); }

f32 phys2_joint_error(const Phys2World* w, u32 id) {
    const Phys2Joint* j = &w->joints[id];
    const Phys2Body* A = jbc(w, j->a);
    const Phys2Body* B = jbc(w, j->b);
    f32 len = vec3_len(vec3_sub(world_anchor(B, j->lb), world_anchor(A, j->la)));
    if (j->type != PHYS2_JOINT_DISTANCE) return len;
    if (len > j->max_length) return len - j->max_length;
    if (len < j->min_length) return j->min_length - len;
    return 0.0f;
}

void phys2_joints_prepare(Phys2World* w, f32 h) {
    UNUSED(h);
    for (u32 k = 0; k < w->joint_count; k++) {
        Phys2Joint* j = &w->joints[k];
        Phys2Body* A = jb(w, j->a);
        Phys2Body* B = jb(w, j->b);
        j->ra = phys2_mat3_mul_v3(&A->rot_m, j->la);
        j->rb = phys2_mat3_mul_v3(&B->rot_m, j->lb);
        if (j->type != PHYS2_JOINT_DISTANCE) {
            Mat3 sa = skew(j->ra), sb = skew(j->rb);
            Mat3 ta = mat3_mul_ptr(&sa, &A->inv_inertia_world), tb = mat3_mul_ptr(&sb, &B->inv_inertia_world);
            Mat3 ka = mat3_mul_ptr(&ta, &sa), kb = mat3_mul_ptr(&tb, &sb);
            Mat3 K;
            f32 m = A->inv_mass + B->inv_mass;
            for (u32 i = 0; i < 9; i++) K.m[i] = -ka.m[i] - kb.m[i];
            K.m[0] += m; K.m[4] += m; K.m[8] += m;
            j->k_inv = mat3_inverse(&K);
        }
    }
}

static inline void apply_lin(Phys2Body* A, Phys2Body* B, Vec3 ra, Vec3 rb, Vec3 P) {
    A->vel = vec3_sub(A->vel, vec3_scale(P, A->inv_mass));
    A->ang_vel = vec3_sub(A->ang_vel, phys2_mat3_mul_v3(&A->inv_inertia_world, vec3_cross(ra, P)));
    B->vel = vec3_add(B->vel, vec3_scale(P, B->inv_mass));
    B->ang_vel = vec3_add(B->ang_vel, phys2_mat3_mul_v3(&B->inv_inertia_world, vec3_cross(rb, P)));
}

static inline void apply_ang(Phys2Body* A, Phys2Body* B, Vec3 L) {
    A->ang_vel = vec3_sub(A->ang_vel, phys2_mat3_mul_v3(&A->inv_inertia_world, L));
    B->ang_vel = vec3_add(B->ang_vel, phys2_mat3_mul_v3(&B->inv_inertia_world, L));
}

static Vec3 distance_dir(const Phys2Body* A, const Phys2Body* B, const Phys2Joint* j, f32* len) {
    Vec3 d = vec3_sub(world_anchor(B, j->lb), world_anchor(A, j->la));
    *len = vec3_len(d);
    return *len > 1e-6f ? vec3_scale(d, 1.0f / *len) : vec3(0, 1, 0);
}

void phys2_joints_warm_start(Phys2World* w) {
    for (u32 k = 0; k < w->joint_count; k++) {
        Phys2Joint* j = &w->joints[k];
        Phys2Body* A = jb(w, j->a);
        Phys2Body* B = jb(w, j->b);
        if (j->type == PHYS2_JOINT_DISTANCE) {
            f32 len;
            Vec3 u = distance_dir(A, B, j, &len);
            apply_lin(A, B, j->ra, j->rb, vec3_scale(u, j->len_imp));
            continue;
        }
        apply_lin(A, B, j->ra, j->rb, j->lin_imp);
        if (j->type == PHYS2_JOINT_HINGE) {
            Vec3 ax = phys2_mat3_mul_v3(&A->rot_m, j->axis_a);
            apply_ang(A, B, vec3_add(j->ang_imp, vec3_scale(ax, j->motor_imp + j->limit_imp)));
        }
    }
}

typedef struct Soft {
    f32 bias_rate, mass_scale, impulse_scale;
} Soft;

static Soft joint_soft(f32 h) {
    Soft s;
    f32 omega = M_TAU * JOINT_HERTZ;
    f32 a1 = 2.0f * JOINT_DAMPING + h * omega;
    f32 a2 = h * omega * a1;
    f32 a3 = 1.0f / (1.0f + a2);
    s.bias_rate = omega / a1;
    s.mass_scale = a2 * a3;
    s.impulse_scale = a3;
    return s;
}

/* C konum hatası için (bias, kütle ölçeği, itki ölçeği) */
static inline void coeffs(Soft s, b32 use_bias, b32 soft, f32 h, f32* bias_rate, f32* ms, f32* is) {
    if (!use_bias) { *bias_rate = 0.0f; *ms = 1.0f; *is = 0.0f; return; }
    if (soft) { *bias_rate = s.bias_rate; *ms = s.mass_scale; *is = s.impulse_scale; return; }
    *bias_rate = 0.2f / h;   /* Baumgarte */
    *ms = 1.0f;
    *is = 0.0f;
}

static inline f32 ang_mass(const Phys2Body* A, const Phys2Body* B, Vec3 t) {
    f32 k = vec3_dot(t, phys2_mat3_mul_v3(&A->inv_inertia_world, t)) + vec3_dot(t, phys2_mat3_mul_v3(&B->inv_inertia_world, t));
    return k > 0.0f ? 1.0f / k : 0.0f;
}

void phys2_joints_solve(Phys2World* w, f32 h, b32 use_bias, b32 soft) {
    Soft s = joint_soft(h);
    f32 br, ms, is;
    coeffs(s, use_bias, soft, h, &br, &ms, &is);
    for (u32 k = 0; k < w->joint_count; k++) {
        Phys2Joint* j = &w->joints[k];
        Phys2Body* A = jb(w, j->a);
        Phys2Body* B = jb(w, j->b);
        if (A->inv_mass == 0.0f && B->inv_mass == 0.0f) continue;

        if (j->type == PHYS2_JOINT_DISTANCE) {
            f32 len;
            Vec3 u = distance_dir(A, B, j, &len);
            j->length = len;
            b32 rigid = j->min_length == j->max_length;
            f32 C = 0.0f;
            i32 side = 0;   /* 0 çubuk, +1 alt sınır (itki >= 0), -1 üst sınır (itki <= 0) */
            if (rigid) C = len - j->min_length;
            else if (len < j->min_length) { C = len - j->min_length; side = 1; }
            else if (len > j->max_length) { C = len - j->max_length; side = -1; }
            else {
                /* aralıkta: serbest, ama yaklaşma hızına sınırı geçmeyecek kadar izin ver (spekülatif) */
                j->len_imp = 0.0f;
                continue;
            }
            Vec3 ca = vec3_cross(j->ra, u), cb = vec3_cross(j->rb, u);
            f32 kk = A->inv_mass + B->inv_mass + vec3_dot(ca, phys2_mat3_mul_v3(&A->inv_inertia_world, ca)) +
                     vec3_dot(cb, phys2_mat3_mul_v3(&B->inv_inertia_world, cb));
            f32 mass = kk > 0.0f ? 1.0f / kk : 0.0f;
            Vec3 dv = vec3_sub(vec3_add(B->vel, vec3_cross(B->ang_vel, j->rb)), vec3_add(A->vel, vec3_cross(A->ang_vel, j->ra)));
            f32 cdot = vec3_dot(u, dv);
            f32 lam = -mass * ms * (cdot + br * C) - is * j->len_imp;
            f32 nw = j->len_imp + lam;
            if (side > 0) nw = m_max(nw, 0.0f);
            if (side < 0) nw = m_min(nw, 0.0f);
            lam = nw - j->len_imp;
            j->len_imp = nw;
            apply_lin(A, B, j->ra, j->rb, vec3_scale(u, lam));
            continue;
        }

        /* menteşe: önce açısal kısıtlar (motor, sınır, eksen hizası), sonra nokta */
        if (j->type == PHYS2_JOINT_HINGE) {
            Vec3 aw = phys2_mat3_mul_v3(&A->rot_m, j->axis_a);
            Vec3 bw = phys2_mat3_mul_v3(&B->rot_m, j->axis_b);
            Vec3 refa = phys2_mat3_mul_v3(&A->rot_m, j->ref_a), refb = phys2_mat3_mul_v3(&B->rot_m, j->ref_b);
            j->angle = m_atan2(vec3_dot(vec3_cross(refa, refb), aw), vec3_dot(refa, refb));
            f32 am = ang_mass(A, B, aw);
            if (j->motor) {
                f32 cdot = vec3_dot(vec3_sub(B->ang_vel, A->ang_vel), aw);
                f32 lam = -am * (cdot - j->motor_speed);
                f32 maxi = j->max_motor_torque * h;
                f32 nw = m_clamp(j->motor_imp + lam, -maxi, maxi);
                lam = nw - j->motor_imp;
                j->motor_imp = nw;
                apply_ang(A, B, vec3_scale(aw, lam));
            }
            if (j->limit) {
                f32 C = 0.0f;
                i32 side = 0;
                if (j->angle <= j->lower) { C = j->angle - j->lower; side = 1; }
                else if (j->angle >= j->upper) { C = j->angle - j->upper; side = -1; }
                if (side) {
                    f32 cdot = vec3_dot(vec3_sub(B->ang_vel, A->ang_vel), aw);
                    f32 lam = -am * ms * (cdot + br * C) - is * j->limit_imp;
                    f32 nw = j->limit_imp + lam;
                    nw = side > 0 ? m_max(nw, 0.0f) : m_min(nw, 0.0f);
                    lam = nw - j->limit_imp;
                    j->limit_imp = nw;
                    apply_ang(A, B, vec3_scale(aw, lam));
                } else {
                    j->limit_imp = 0.0f;
                }
            }
            Vec3 t1, t2;
            phys2_ortho_basis(aw, &t1, &t2);
            Vec3 err = vec3_cross(aw, bw);
            const Vec3 ts[2] = { t1, t2 };
            for (u32 i = 0; i < 2; i++) {
                Vec3 t = ts[i];
                f32 cdot = vec3_dot(vec3_sub(B->ang_vel, A->ang_vel), t);
                f32 acc = vec3_dot(j->ang_imp, t);
                f32 lam = -ang_mass(A, B, t) * ms * (cdot + br * vec3_dot(err, t)) - is * acc;
                j->ang_imp = vec3_add(j->ang_imp, vec3_scale(t, lam));
                apply_ang(A, B, vec3_scale(t, lam));
            }
        }

        /* nokta kısıtı (top-mafsal ve menteşe) */
        Vec3 C = vec3_sub(world_anchor(B, j->lb), world_anchor(A, j->la));
        Vec3 dv = vec3_sub(vec3_add(B->vel, vec3_cross(B->ang_vel, j->rb)), vec3_add(A->vel, vec3_cross(A->ang_vel, j->ra)));
        Vec3 rhs = vec3_add(dv, vec3_scale(C, br));
        Vec3 lam = vec3_sub(vec3_scale(phys2_mat3_mul_v3(&j->k_inv, rhs), -ms), vec3_scale(j->lin_imp, is));
        j->lin_imp = vec3_add(j->lin_imp, lam);
        apply_lin(A, B, j->ra, j->rb, lam);
    }
}
