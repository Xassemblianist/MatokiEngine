/* gfx3d/camera.h — Perspektif kamera: serbest uçuş (WASD + fare sürükleme) ve orbit modları.
 * Projeksiyon reverse-Z + sonsuz uzak düzlem: derinlik near'da 1, sonsuzda 0 (DEPTH_GREATER).
 * Sağ-el koordinat sistemi, +Y yukarı, kamera -Z'ye bakar (yaw=0).
 */
#ifndef GFX3D_CAMERA_H
#define GFX3D_CAMERA_H

#include "core/types.h"
#include "core/math.h"
#include "plat/input.h"

typedef enum {
    CAM_FLY   = 0,
    CAM_ORBIT = 1
} CameraMode;

typedef struct {
    CameraMode mode;
    Vec3 pos;          /* fly: konum; orbit: hesaplanan göz konumu */
    f32  yaw, pitch;   /* radyan; yaw=0 → -Z, pitch>0 → yukarı */
    Vec3 target;       /* orbit merkezi */
    f32  dist;         /* orbit uzaklığı */
    f32  fov_y;        /* radyan */
    f32  near_z;
    f32  speed;        /* uçuş hızı (birim/s) */
    f32  look_sens;    /* radyan / piksel */
} Camera;

void camera_init(Camera* c, CameraMode mode);
/* Girdiyi uygular. look_enabled: fare sürüklemesi bakış açısını değiştirsin mi (UI üstündeyken kapat). */
void camera_update(Camera* c, const InputState* in, f32 dt, b32 look_enabled);
Vec3 camera_forward(const Camera* c);
Vec3 camera_eye(const Camera* c);
Mat4 camera_view(const Camera* c);
Mat4 camera_proj(const Camera* c, f32 aspect);

/* Reverse-Z, sonsuz uzak düzlemli perspektif: z_clip = near, w_clip = -z_view.
 * Görünür bölge: 0 < z/w <= 1 (near'da 1). */
Mat4 mat4_perspective_rz(f32 fov_y_rad, f32 aspect, f32 near_z);

#endif
