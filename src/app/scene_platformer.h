/* app/scene_platformer.h — "Matoki Koşusu" platform oyunu sahnesi (M7_3).
 * Kontroller: ←/→ veya A/D koş, Boşluk/↑/W/Z zıpla (basılı tut = yüksek), ↓+zıpla platformdan in,
 * R yeniden başla, N müzik aç/kapa. 2D motor temellerinin (tilemap, kamera, animasyon, ses) vitrini.
 */
#ifndef APP_SCENE_PLATFORMER_H
#define APP_SCENE_PLATFORMER_H

#include "core/types.h"
#include "core/arena.h"
#include "plat/input.h"
#include "raster/raster.h"
#include "ui/ui.h"
#include "app/shaders.h"

void scene_plat_init(Arena* perm);
void scene_plat_frame(Rasterizer* r, Framebuffer* fb, Arena* frame, const ShaderManager* sm,
                      const InputState* in, f32 dt, b32 use_jit);
void scene_plat_draw_hud(UiContext* ui, const Framebuffer* fb);
/* Sahneden çıkılırken (müziği durdurur) */
void scene_plat_leave(void);

#endif
