/* app/scene_kuyu.h — "Kuyu": retro birinci şahıs nişancı. Eski bir kuyunun dibindeki zindandan,
 * mühürlü kapının ardındaki ışık bacasına ulaşarak çık. 320×180 iç çözünürlük + retro sunum,
 * nokta ışıklı zindan shader'ı (dungeon3d.mtk), hitscan + mermi silahları, iki düşman türü.
 * Kontroller: tıkla → fare kilidi, fare bak, WASD koş, Boşluk zıpla, sol tık/F ateş, 1-3 silah,
 * oklar bakış (klavye), V retro aç/kapa, G köşe titremesi (vertex snap), R yeniden, N müzik.
 * İki bölüm; düşmanlar BFS akış alanıyla yol bulur, Gözcü uçar.
 */
#ifndef APP_SCENE_KUYU_H
#define APP_SCENE_KUYU_H

#include "core/types.h"
#include "core/arena.h"
#include "plat/input.h"
#include "raster/raster.h"
#include "ui/ui.h"
#include "app/shaders.h"

/* shaders_init'ten sonra */
void scene_kuyu_init(Arena* perm, Arena* scratch);
/* pointer_locked: JS fare kilidi açık mı (açıksa mouse_dx/dy ham hareket) */
void scene_kuyu_frame(Rasterizer* r, Framebuffer* fb, Arena* frame, const ShaderManager* sm,
                      const InputState* in, f32 dt, b32 use_jit, b32 pointer_locked);
void scene_kuyu_draw_hud(UiContext* ui, Framebuffer* fb);
void scene_kuyu_leave(void);

/* Test/ölçüm: [x y z, yaw pitch, can, silah, saçma, kor, öldürülen, kalan düşman, durum, anahtar,
 * toplam düşman, çizim ms, sunum ms, üçgen, ışık, görünen en yakın düşman x y z uzaklık, bölüm, snap] */
const f32* scene_kuyu_debug(void);
/* cmd 1: bakış (a=yaw, b=pitch); 2: ışınla (a,b,c ayak); 3: ölümsüzlük (a != 0); 4: bölüm a'yı yükle */
void scene_kuyu_debug_cmd(u32 cmd, f32 a, f32 b, f32 c);

#endif
