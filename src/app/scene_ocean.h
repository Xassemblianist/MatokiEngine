/* app/scene_ocean.h — "Okyanus" (M17_A): 3 kademeli Tessendorf FFT okyanus sahnesi.
 * JONSWAP spektrumu (L = 500/60/8 m, N = 128/128/64), Jacobian köpüğü, mipmap'li eğim+köpük dokuları,
 * ocean.mtk (Fresnel + analitik gökyüzü yansıması + güneş yakamozu + dalga tepesi SSS + köpük + sis),
 * sky.mtk (Rayleigh/Mie gök kubbesi) ve 8 noktalı kaldırma kuvvetiyle dalgalarda yüzen yelkenli tekne.
 */
#ifndef APP_SCENE_OCEAN_H
#define APP_SCENE_OCEAN_H

#include "core/types.h"
#include "core/arena.h"
#include "plat/input.h"
#include "raster/raster.h"
#include "ui/ui.h"
#include "app/shaders.h"

/* shaders_init'ten sonra: 3 kademeli FFT okyanusu, ızgara ağını ve tekne modellerini kurar */
void scene_ocean_init(Arena* perm, Arena* scratch);

/* Okyanus FFT kademelerini, tekne fiziğini ve 3D sahneyi günceller ve çizer */
void scene_ocean_frame(Rasterizer* r, Framebuffer* fb, Arena* frame, const ShaderManager* sm,
                       const InputState* in, f32 dt, b32 use_jit, b32 look_enabled);

/* HUD: FFT ms, mesh ms, shade ms, Hs, rüzgâr hızı, choppiness, zaman ölçeği ve tuş ipuçları */
void scene_ocean_draw_hud(UiContext* ui, const Framebuffer* fb);

/* HUD görünürlüğünü aç/kapat (JIT <-> C-Ref piksel karşılaştırması için) */
void scene_ocean_set_hud(b32 show);

/* Test/ölçüm: [0]=fft_ms, [1]=mesh_ms, [2]=shade_ms, [3]=hs, [4]=theory_hs,
 * [5]=wind_speed, [6]=choppy, [7]=time_scale, [8]=boat_y, [9]=boat_pitch, [10]=boat_roll, [11]=tris */
const f32* scene_ocean_debug(void);

/* Test/komut: cmd=0 sıfırla, cmd=1 (t, wind, choppy) sabit zamana kur, cmd=2 (yaw, pitch, dist) kamera,
 * cmd=3 (sun_elev, time_scale, wireframe) görünüm ayarları */
void scene_ocean_debug_cmd(u32 cmd, f32 a, f32 b, f32 c);

#endif
