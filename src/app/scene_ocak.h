/* app/scene_ocak.h — "Ocak": küçük 3D oyun. Vadide yüzen adacıklara dağılmış kıvılcımları topla,
 * meşalen sönmeden ortadaki ocağa taşı, güneş batmadan ateşi yak.
 * Kontroller: WASD koş (kameraya göre), Boşluk zıpla, fare sürükle kamerayı döndür, tekerlek yakınlaştır,
 * R yeniden başla, N müzik aç/kapa. Motorun 3D parçalarının (fizik, ECS, PBR + gölge, ses) vitrini.
 */
#ifndef APP_SCENE_OCAK_H
#define APP_SCENE_OCAK_H

#include "core/types.h"
#include "core/arena.h"
#include "plat/input.h"
#include "raster/raster.h"
#include "ui/ui.h"
#include "app/shaders.h"

/* shaders_init'ten sonra */
void scene_ocak_init(Arena* perm, Arena* scratch);
void scene_ocak_frame(Rasterizer* r, Framebuffer* fb, Arena* frame, const ShaderManager* sm,
                      const InputState* in, f32 dt, b32 use_jit, b32 look_enabled);
void scene_ocak_draw_hud(UiContext* ui, const Framebuffer* fb);
/* Sahneden çıkılırken (müziği durdurur) */
void scene_ocak_leave(void);
/* Test/ölçüm: [ayak x,y,z, hız y, taşınan, yakılan, meşale, gün saati, durum, kamera yaw] */
const f32* scene_ocak_debug(void);
void       scene_ocak_debug_set_yaw(f32 yaw);

#endif
