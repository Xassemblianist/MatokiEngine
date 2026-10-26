/* app/scene3d.h — 3D demo sahnesi: dokulu zemin, 1000 küplük ızgara, dönen küp ve küre.
 * gfx3d boru hattını (kırpma, reverse-Z derinlik, arka yüz ayıklama, frustum culling) ve
 * lit3d.mtk shader'ını çalıştırır. Kamera: uçuş (WASD/QE + sürükle) / orbit (C ile geçiş).
 */
#ifndef APP_SCENE3D_H
#define APP_SCENE3D_H

#include "core/types.h"
#include "core/arena.h"
#include "plat/input.h"
#include "raster/raster.h"
#include "ui/ui.h"
#include "app/shaders.h"

void scene3d_init(Arena* perm, Framebuffer* fb);
/* look_enabled: fare sürüklemesi kameraya gitsin mi (UI üzerindeyken FALSE) */
void scene3d_frame(Rasterizer* r, Framebuffer* fb, Arena* frame, const ShaderManager* sm,
                   const InputState* in, f32 dt, f32 time_sec, b32 use_jit, b32 look_enabled);
void scene3d_draw_hud(UiContext* ui, const Framebuffer* fb);

#endif
