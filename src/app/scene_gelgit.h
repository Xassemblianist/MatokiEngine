/* app/scene_gelgit.h — "Gelgit" (OYUN1): Fırtınalı okyanusta yelkenli oyun sahnesi.
 * 60 Hz deterministik Sim çekirdeği (src/game/gelgit.c), 3 kademeli Tessendorf FFT okyanus,
 * XPBD kumaş yelkenler (src/gfx3d/sail.c), prosedürel deniz/fırtına ses sentezi (src/audio/synth.c),
 * 4 takımada + liman iskelesi + 2 deniz feneri + şamandıralar + Günün Denizi hayalet teknesi,
 * rüzgâr gülü / hız / sörf / kargo HUD'u ve canlı fizik ayar kaydırıcılarını (F2) birleştirir.
 */
#ifndef APP_SCENE_GELGIT_H
#define APP_SCENE_GELGIT_H

#include "core/types.h"
#include "core/arena.h"
#include "plat/input.h"
#include "raster/raster.h"
#include "ui/ui.h"
#include "app/shaders.h"

/* Sahneyi, Sim + Replay tamponlarını, FFT okyanusu, XPBD yelkenleri ve dokuları ilklendirir */
void scene_gelgit_init(Arena* perm, Arena* scratch);

/* 60 Hz deterministik simülasyonu, XPBD yelkenleri, sesleri ve 3D/2D sahneyi günceller ve çizer */
void scene_gelgit_frame(Rasterizer* r, Framebuffer* fb, Arena* frame, const ShaderManager* sm,
                        const InputState* in, f32 dt, b32 use_jit, b32 look_enabled);

/* Minimal denizcilik HUD'u, görev/tersane menüsü (M) ve canlı fizik ayar panelini (F2) çizer */
void scene_gelgit_draw_hud(UiContext* ui, const Framebuffer* fb);

/* HUD görünürlüğünü aç/kapat */
void scene_gelgit_set_hud(b32 show);

/* Sahneden çıkarken prosedürel sesleri susturur */
void scene_gelgit_leave(void);

/* Test/ölçüm telemetrisi (16 x f32):
 * [0]=sim_ms, [1]=fft_ms, [2]=shade_ms, [3]=hs, [4]=wind_speed, [5]=boat_speed,
 * [6]=surf_intensity, [7]=heel_deg, [8]=tw_angle_deg, [9]=vmg_upwind,
 * [10]=mission_id, [11]=mission_stage, [12]=capsized, [13]=cargo_health,
 * [14]=tris_drawn, [15]=state_hash_lo32 */
const f32* scene_gelgit_debug(void);

/* Test/komut:
 * cmd=0: mevcut görevi sıfırla
 * cmd=1: görevi başlat (a = mission_id 0..7, b = upgrades_mask)
 * cmd=2: kamera ayarla (a = yaw, b = pitch, c = dist)
 * cmd=3: zaman ölçeği / duraklatma (a = time_scale, b = paused, c = wireframe)
 * cmd=10..14: ekran görüntüsü sahne kurulumları (10=sunset, 11=storm, 12=surf, 13=capsize, 14=harbor) */
void scene_gelgit_debug_cmd(u32 cmd, f32 a, f32 b, f32 c);

#endif /* APP_SCENE_GELGIT_H */
