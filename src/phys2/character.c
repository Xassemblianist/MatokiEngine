/* phys2/character.c — kinematik karakter kontrolcüsü.
 * Çözme: kapsülün 3 küresi her gövdeyle phys2_collide ile sınanır; en çok 4 tur girişim
 * giderilir. Yürünebilir yüzey (normal·yukarı ≥ cos(eğim)) → yalnızca yukarı itilir ve
 * yere basılı sayılır; dik yüzey → normal boyunca itilir ve hızın yüzeye giren bileşeni silinir.
 * Basamak: dik bir yüzey alt küreyi durdurduysa yukarı-ileri-aşağı dene; daha çok ilerleme
 * sağlıyor ve yürünebilir zeminde bitiyorsa kabul et.
 * SamurayBalgami/Matoki'den uyarlandı (izinle).
 */
#include "phys2/character.h"

#define CHAR_SKIN 0.002f
#define CHAR_MAX_CAND 256        /* karakter kutusundaki en çok aday gövde (düzlemler dahil) */
#define CHAR_MESH_MANIFOLDS 16   /* mesh × küre: tek sorguda en çok üçgen manifoldu */

Phys2CharacterDesc phys2_character_desc_default(Vec3 pos) {
    Phys2CharacterDesc d;
    d.pos = pos;
    d.radius = 0.35f;
    d.height = 1.8f;
    d.step_height = 0.4f;
    d.max_slope_deg = 45.0f;
    d.speed = 5.0f;
    d.jump_speed = 5.5f;
    d.mass = 80.0f;
    return d;
}

void phys2_character_init(Phys2Character* c, const Phys2CharacterDesc* d) {
    c->pos = d->pos;
    c->vel = vec3(0, 0, 0);
    c->radius = d->radius;
    c->height = d->height;
    c->step_height = d->step_height;
    c->slope_cos = m_cos(d->max_slope_deg * (M_PI / 180.0f));
    c->speed = d->speed;
    c->jump_speed = d->jump_speed;
    c->mass = d->mass;
    c->grounded = FALSE;
    c->ground_normal = vec3(0, 1, 0);
    c->ground_body = PHYS2_NO_BODY;
    c->last_contacts = 0;
    c->stepped = FALSE;
    c->last_tests = 0;
}

Vec3 phys2_character_eye(const Phys2Character* c) { return vec3_add(c->pos, vec3(0, c->height - 0.15f, 0)); }

typedef struct Resolve {
    b32 grounded;
    b32 blocked;      /* alt küre dik bir yüzeye takıldı */
    Vec3 ground_normal;
    u32 ground_body;
    u32 contacts;
    u32 tests;        /* dar fazda sınanan gövde sayısı (geniş faz sorgusu O(N) taramıyor mu: test) */
} Resolve;

/* Karakter kutusuyla kesişen gövdeler artan indeks sırasıyla (düzlemler dahil): geniş faz sorgusu
 * (BVH / SAP) + düzlem listesi birleştirilir; tüm gövdeler taranmaz (D-111). Dönüş = aday sayısı. */
static u32 gather_candidates(Phys2World* w, Vec3 mn, Vec3 mx, u32* out, u32 cap) {
    u32 nq = phys2_query_aabb(w, mn, mx, w->query_buf, w->query_cap);
    u32 n = 0, qi = 0, pi = 0;
    while ((qi < nq || pi < w->plane_count) && n < cap) {
        if (pi < w->plane_count && (qi >= nq || w->plane_ids[pi] < w->query_buf[qi])) out[n++] = w->plane_ids[pi++];
        else out[n++] = w->query_buf[qi++];
    }
    return n;
}

static void resolve(Phys2World* w, Phys2Character* c, Vec3* pos, Vec3* vel, f32 dt, b32 push_bodies, Resolve* r) {
    const f32 offs[3] = { c->radius, c->height * 0.5f, c->height - c->radius };
    Phys2Body s;
    s = (Phys2Body){ 0 };
    s.shape = PHYS2_SHAPE_SPHERE;
    s.radius = c->radius;
    s.rot = phys2_quat_identity();
    s.rot_m = phys2_mat3_from_quat(s.rot);
    u32 cand[CHAR_MAX_CAND];
    for (u32 iter = 0; iter < 4; iter++) {
        b32 any = FALSE;
        Vec3 mn = vec3(pos->x - c->radius, pos->y, pos->z - c->radius);
        Vec3 mx = vec3(pos->x + c->radius, pos->y + c->height, pos->z + c->radius);
        u32 nc = gather_candidates(w, mn, mx, cand, CHAR_MAX_CAND);
        for (u32 ci = 0; ci < nc; ci++) {
            u32 i = cand[ci];
            const Phys2Body* b = &w->bodies[i];
            r->tests++;
            for (u32 k = 0; k < 3; k++) {
                s.pos = vec3_add(*pos, vec3(0, offs[k], 0));
                Phys2Manifold m;
                if (b->shape == PHYS2_SHAPE_MESH) {
                    /* mesh: üçgen başına manifold; en derin olanı çöz (komşu eş düzlemli üçgenler aynı girişimi
                     * iki kez itmesin), kalanı sonraki turda yeniden sorgulanır. Normal sphere(A) → mesh(B). */
                    Phys2Manifold mm[CHAR_MESH_MANIFOLDS];
                    f32 e = c->radius + PHYS2_MARGIN;   /* collide_mesh gövdenin AABB'siyle üçgenleri seçer */
                    s.aabb_min = vec3(s.pos.x - e, s.pos.y - e, s.pos.z - e);
                    s.aabb_max = vec3(s.pos.x + e, s.pos.y + e, s.pos.z + e);
                    u32 nm = phys2_collide_mesh(&s, b, mm, CHAR_MESH_MANIFOLDS, w->mesh_edge_fix);
                    u32 best = nm;
                    f32 bd = 0.0f;
                    for (u32 q = 0; q < nm; q++) {
                        if (mm[q].count && mm[q].p[0].depth > bd) { bd = mm[q].p[0].depth; best = q; }
                    }
                    if (best == nm) continue;
                    m = mm[best];
                } else if (!phys2_collide(&s, b, &m)) {
                    continue;
                }
                f32 depth = m.p[0].depth;
                if (depth <= 0.0f) continue;
                Vec3 out = vec3_scale(m.normal, -1.0f);   /* gövdeden dışarı */
                r->contacts++;
                any = TRUE;
                if (out.y >= c->slope_cos) {
                    pos->y += depth / out.y + CHAR_SKIN;
                    if (vel->y < 0.0f) vel->y = 0.0f;
                    r->grounded = TRUE;
                    r->ground_normal = out;
                    r->ground_body = i;
                } else {
                    *pos = vec3_add(*pos, vec3_scale(out, depth + CHAR_SKIN));
                    f32 vn = vec3_dot(*vel, out);
                    if (vn < 0.0f) {
                        *vel = vec3_sub(*vel, vec3_scale(out, vn));
                        if (push_bodies && b->inv_mass > 0.0f) {
                            /* karakter kütlesinin yarısıyla it (esnek olmayan, kaba) */
                            phys2_apply_impulse_at(w, i, vec3_scale(out, vn * c->mass * 0.5f), m.p[0].pos);
                        }
                    }
                    if (k == 0 && m_abs(out.y) < 0.7f) r->blocked = TRUE;
                }
                s.pos = vec3_add(*pos, vec3(0, offs[k], 0));
            }
        }
        if (!any) break;
    }
    UNUSED(dt);
}

void phys2_character_update(Phys2World* w, Phys2Character* c, Vec3 wish, b32 jump, f32 dt) {
    Vec3 target = vec3(wish.x * c->speed, 0.0f, wish.z * c->speed);
    b32 was_grounded = c->grounded;
    if (was_grounded) {
        c->vel.x = target.x;
        c->vel.z = target.z;
        if (jump) {
            c->vel.y = c->jump_speed;
            was_grounded = FALSE;
        }
    } else {
        f32 k = m_min(1.0f, 2.0f * dt);
        c->vel.x += (target.x - c->vel.x) * k;
        c->vel.z += (target.z - c->vel.z) * k;
    }
    c->vel = vec3_add(c->vel, vec3_scale(w->gravity, dt));

    Vec3 start = c->pos;
    Vec3 pos = vec3_add(start, vec3_scale(c->vel, dt));
    Vec3 vel = c->vel;
    Resolve r = { 0 };
    resolve(w, c, &pos, &vel, dt, TRUE, &r);
    c->stepped = FALSE;

    /* basamak: önü kapandıysa, hareket yönünde (yarıçap + 0,1) ileride yukarıdan aşağı ışınla
     * basamağın üst yüzünü yokla; yürünebilir ve en çok step_height yüksekse oraya çık ve
     * yatay hareketi o yükseklikten yeniden çöz. (Çarpışma çözmeyle "yukarı-ileri-aşağı" denemesi,
     * yarıçap basamak derinliğinden büyükken kenardan geri itildiği için güvenilir değil.) */
    Vec3 hmove = vec3(c->vel.x * dt, 0.0f, c->vel.z * dt);
    f32 hlen = m_sqrt(hmove.x * hmove.x + hmove.z * hmove.z);
    if (r.blocked && was_grounded && hlen > 1e-6f) {
        Vec3 dir = vec3_scale(hmove, 1.0f / hlen);
        Vec3 probe = vec3_add(vec3_add(start, vec3_scale(dir, c->radius + 0.1f)), vec3(0, c->step_height + 0.05f, 0));
        Phys2RayHit hit;
        if (phys2_raycast(w, probe, vec3(0, -1, 0), c->step_height + 0.05f, PHYS2_NO_BODY, &hit) &&
            hit.normal.y >= c->slope_cos) {
            f32 rise = hit.point.y - start.y;
            if (rise > 0.01f && rise <= c->step_height + 0.001f) {
                Vec3 p2 = vec3_add(start, vec3(0, rise + CHAR_SKIN, 0));
                Vec3 v2 = vec3(c->vel.x, 0.0f, c->vel.z);
                p2 = vec3_add(p2, hmove);
                Resolve r2 = { 0 };
                resolve(w, c, &p2, &v2, dt, TRUE, &r2);
                Vec3 d1 = vec3_sub(pos, start), d2 = vec3_sub(p2, start);
                if (d2.x * d2.x + d2.z * d2.z > d1.x * d1.x + d1.z * d1.z + 1e-8f) {
                    u32 tests = r.tests + r2.tests;
                    pos = p2;
                    vel = v2;
                    r = r2;
                    r.tests = tests;
                    r.grounded = TRUE;   /* yürünebilir basamak üstüne çıktı */
                    r.ground_normal = hit.normal;
                    r.ground_body = hit.body;
                    c->stepped = TRUE;
                }
            }
        }
    }

    /* zemine yapış / zemin denetimi: zıplarken değil, ve çözme zaten yürünebilir bir destek
     * bulduysa (ör. basamak kenarına basıyor) hiç değil — merkez ışını kenarın aşağısındaki
     * zemini görüp karakteri basamaktan geri çekerdi */
    if (vel.y <= 0.0f && !r.grounded) {
        f32 snap = was_grounded ? c->step_height : 0.05f;
        Phys2RayHit hit;
        Vec3 o = vec3_add(pos, vec3(0, c->radius, 0));
        if (phys2_raycast(w, o, vec3(0, -1, 0), c->radius + snap, PHYS2_NO_BODY, &hit) && hit.normal.y >= c->slope_cos) {
            f32 gap = pos.y - hit.point.y;
            if (gap <= snap) {
                pos.y = hit.point.y + CHAR_SKIN;
                vel.y = 0.0f;
                r.grounded = TRUE;
                r.ground_normal = hit.normal;
                r.ground_body = hit.body;
            }
        }
    }

    c->pos = pos;
    c->vel = vel;
    c->grounded = r.grounded;
    c->ground_normal = r.grounded ? r.ground_normal : vec3(0, 1, 0);
    c->ground_body = r.grounded ? r.ground_body : PHYS2_NO_BODY;
    c->last_contacts = r.contacts;
    c->last_tests = r.tests;
}
