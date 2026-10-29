/* app/scene_fluid.h — "Akışkan" (P1_F) sahnesi: GPU Position Based Fluids (256k parçacık) + SSFR.
 * Macklin 2013 PBF (3B hücre öbeği, yoğunluk kısıtı, s_corr, girdap hapsi, XSPH viskozite),
 * ekran-uzayı akışkan görüntüleme (derinlik + ikili süzgeç + normal + kırılma/soğurma/köpük)
 * ve katı cisimlerle (küreler, kutu, eğilebilir kova) tek-yönlü etkileşim.
 */
#ifndef APP_SCENE_FLUID_H
#define APP_SCENE_FLUID_H

#include "core/arena.h"
#include "plat/input.h"
#include "raster/framebuffer.h"
#include "raster/raster.h"
#include "ui/ui.h"
#include "app/shaders.h"

void scene_fluid_init(Arena* perm, Arena* scratch);

void scene_fluid_frame(Rasterizer* r, Framebuffer* fb, Arena* frame,
                       const ShaderManager* sm, const InputState* in,
                       f32 dt, b32 use_jit, b32 look_enabled);

void scene_fluid_draw_hud(UiContext* ui, const Framebuffer* fb);

void scene_fluid_set_hud(b32 show);

/* Tarayıcı / doğrulama betiği için 12 adet f32 durum özeti:
 * [0]=gpu_ms, [1]=cpu_ms, [2]=particle_count, [3]=substeps,
 * [4]=viscosity, [5]=vorticity, [6]=tilt_x, [7]=tilt_z,
 * [8]=sphere_x, [9]=sphere_z, [10]=sun_elev, [11]=sim_time
 */
const f32* scene_fluid_debug(void);

/* Test/doğrulama komutu:
 * cmd=0 -> sahneyi sıfırla (dam-break başlangıcı)
 * cmd=1 -> sim_time=a, tilt_x=b, tilt_z=c
 * cmd=2 -> kamera açısını/uzaklığını ayarla (a=yaw, b=pitch, c=dist)
 * cmd=3 -> fizik parametreleri (a=vorticity, b=viscosity, c=sun_elev)
 */
void scene_fluid_debug_cmd(u32 cmd, f32 a, f32 b, f32 c);

#endif
