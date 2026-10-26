/* gfx3d/camera.c — Kamera güncellemesi ve görüş/projeksiyon matrisleri.
 * Uçuş: W/S ileri-geri, A/D yan, E/Q yukarı-aşağı, Shift hızlı; sol veya sağ tuşla sürükleme bakış.
 * Orbit: sürükleme hedef etrafında döndürür, tekerlek yakınlaştırır.
 */
#include "gfx3d/camera.h"

#define KEY_SHIFT 16
#define KEY_A 65
#define KEY_D 68
#define KEY_E 69
#define KEY_Q 81
#define KEY_S 83
#define KEY_W 87

void camera_init(Camera* c, CameraMode mode) {
    c->mode      = mode;
    c->pos       = vec3(0.0f, 2.0f, 6.0f);
    c->yaw       = 0.0f;
    c->pitch     = -0.2f;
    c->target    = vec3(0.0f, 0.5f, 0.0f);
    c->dist      = 8.0f;
    c->fov_y     = 60.0f * M_DEG2RAD;
    c->near_z    = 0.05f;
    c->speed     = 6.0f;
    c->look_sens = 0.005f;
}

static Vec3 dir_from_angles(f32 yaw, f32 pitch) {
    f32 cp = m_cos(pitch);
    return vec3(-m_sin(yaw) * cp, m_sin(pitch), -m_cos(yaw) * cp);
}

Vec3 camera_forward(const Camera* c) {
    return dir_from_angles(c->yaw, c->pitch);
}

Vec3 camera_eye(const Camera* c) {
    if (c->mode == CAM_ORBIT) {
        return vec3_sub(c->target, vec3_scale(camera_forward(c), c->dist));
    }
    return c->pos;
}

void camera_update(Camera* c, const InputState* in, f32 dt, b32 look_enabled) {
    if (look_enabled && (in->mouse_buttons & 3u)) {
        c->yaw   -= (f32)in->mouse_dx * c->look_sens;
        c->pitch -= (f32)in->mouse_dy * c->look_sens;
    }
    const f32 lim = 89.0f * M_DEG2RAD;
    c->pitch = m_clamp(c->pitch, -lim, lim);
    if (c->yaw >  M_PI) c->yaw -= M_TAU;
    if (c->yaw < -M_PI) c->yaw += M_TAU;

    if (c->mode == CAM_ORBIT) {
        if (in->wheel != 0) {
            c->dist *= (in->wheel > 0) ? 1.1f : (1.0f / 1.1f);
            c->dist = m_clamp(c->dist, 0.5f, 500.0f);
        }
        c->pos = camera_eye(c);
        return;
    }

    Vec3 f = camera_forward(c);
    Vec3 right = vec3_norm(vec3_cross(f, vec3(0.0f, 1.0f, 0.0f)));
    Vec3 move = vec3(0.0f, 0.0f, 0.0f);
    if (in->keys[KEY_W]) move = vec3_add(move, f);
    if (in->keys[KEY_S]) move = vec3_sub(move, f);
    if (in->keys[KEY_D]) move = vec3_add(move, right);
    if (in->keys[KEY_A]) move = vec3_sub(move, right);
    if (in->keys[KEY_E]) move.y += 1.0f;
    if (in->keys[KEY_Q]) move.y -= 1.0f;
    f32 len = vec3_len(move);
    if (len > 0.0f) {
        f32 sp = c->speed * (in->keys[KEY_SHIFT] ? 4.0f : 1.0f);
        c->pos = vec3_add(c->pos, vec3_scale(move, sp * dt / len));
    }
    if (in->wheel != 0) {
        c->speed = m_clamp(c->speed * ((in->wheel > 0) ? 1.25f : 0.8f), 0.5f, 200.0f);
    }
}

Mat4 camera_view(const Camera* c) {
    Vec3 eye = camera_eye(c);
    return mat4_look_at(eye, vec3_add(eye, camera_forward(c)), vec3(0.0f, 1.0f, 0.0f));
}

Mat4 camera_proj(const Camera* c, f32 aspect) {
    return mat4_perspective_rz(c->fov_y, aspect, c->near_z);
}

Mat4 mat4_perspective_rz(f32 fov_y_rad, f32 aspect, f32 near_z) {
    f32 f = 1.0f / m_tan(0.5f * fov_y_rad);
    Mat4 r = {{
        f / aspect, 0.0f, 0.0f,    0.0f,
        0.0f,       f,    0.0f,    0.0f,
        0.0f,       0.0f, 0.0f,   -1.0f,
        0.0f,       0.0f, near_z,  0.0f
    }};
    return r;
}
