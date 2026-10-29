/* app/scene3d.h — 3D demo sahnesi: dokulu zemin, 1000 küplük ızgara, dönen küp, küre ve parçacık sistemi.
 * gfx3d boru hattını (kırpma, reverse-Z derinlik, arka yüz ayıklama, frustum culling, ertelenmiş paralel ön uç),
 * pbr3d.mtk / lit3d.mtk, dairesel alfa harmanlı particle3d.mtk (ateş/duman + P ile kıvılcım patlaması) ve
 * TPT SVF filtre + yankı ses efektlerini gösterir.
 * Kamera: uçuş (WASD/QE + sürükle) / orbit (C). M: mipmap, L: PBR/Lambert, N: gün döngüsü, G: 20× hızlı, P: patlama.
 */
#ifndef APP_SCENE3D_H
#define APP_SCENE3D_H

#include "core/types.h"
#include "core/arena.h"
#include "plat/input.h"
#include "raster/raster.h"
#include "ui/ui.h"
#include "app/shaders.h"

/* shaders_init'ten sonra çağrılmalı (mipmap'siz lit3d varyantını derler; scratch geçici bellek) */
void scene3d_init(Arena* perm, Arena* scratch, Framebuffer* fb);
/* look_enabled: fare sürüklemesi kameraya gitsin mi (UI üzerindeyken FALSE) */
void scene3d_frame(Rasterizer* r, Framebuffer* fb, Arena* frame, const ShaderManager* sm,
                   const InputState* in, f32 dt, f32 time_sec, b32 use_jit, b32 look_enabled);
void scene3d_draw_hud(UiContext* ui, const Framebuffer* fb);
/* Ölçüm: aydınlatma modu ve sabit gün saati (tod < 0: değiştirme) */
void scene3d_bench_set(b32 pbr, f32 tod);

/* Sürükle-bırak: bir yükleme grubundaki dosyalar (.glb/.gltf/.obj + .mtl/.png/.bin) */
typedef struct {
    const char* name;
    const u8*   data;
    u32         len;
} DroppedFile;

/* Grubu yükler; out: model belleği (bir sonraki yüklemede sıfırlanır), scratch: geçici */
b32 scene3d_load_files(const DroppedFile* files, u32 count, Arena* out, Arena* scratch);

#endif
