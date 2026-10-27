/* app/r3d.h — Uygulama düzeyi 3D çizim katmanı: gün ışığı modeli, güneş gölge geçişi + ana geçiş
 * (pbr3d.mtk veya lit3d.mtk) ve kendinden ışıklı nesneler (unlit3d.mtk). Sahne bir çizim fonksiyonu verir;
 * r3d_render onu iki kez çağırır (gölge geçişi, ana geçiş). Demo sahnesi (scene3d) ve oyunlar paylaşır.
 */
#ifndef APP_R3D_H
#define APP_R3D_H

#include "core/types.h"
#include "core/arena.h"
#include "core/math.h"
#include "raster/raster.h"
#include "gfx3d/pipeline.h"
#include "gfx3d/shadow.h"
#include "gfx3d/camera.h"
#include "app/shaders.h"

typedef struct {
    Vec3 light_dir;    /* ışığa doğru (güneş veya ay), birim */
    Vec3 light_col;    /* doğrusal ışınım */
    Vec3 sky_lin, gnd_lin;
    Vec3 sky_srgb;     /* temizleme ve sis rengi */
    f32  day;          /* 0 gece, 1 gündüz */
} DayLight;

/* tod ∈ [0,1): 0 = gün doğumu (06:00), 0.25 öğle, 0.5 gün batımı, 0.75 gece yarısı */
DayLight r3d_daylight(f32 tod);

typedef struct {
    ShadowMap     shadow;
    ManagedShader pbr_nomip, lit_nomip;
    u32           shadow_size;
} R3dRenderer;

typedef struct {
    G3dContext*    ctx;
    Arena*         frame;
    G3dMaterial    mat;          /* geçişin temel materyali */
    G3dMaterial    emissive;     /* unlit3d materyali (ana geçişte) */
    b32            shadow_pass;
    b32            pbr;
    DayLight       dl;
    Vec3           eye;
    f32            shadow_bias;
    const Texture* shadow_tex;
    f32            fog_density;
} R3dPass;

typedef void (*R3dSceneFn)(R3dPass* pass, void* user);

typedef struct {
    Camera*  cam;
    DayLight dl;
    b32      pbr;          /* FALSE: Lambert (lit3d), gölge yok */
    b32      mipmap;
    b32      use_jit;
    Vec3     shadow_center;
    f32      shadow_radius;
    f32      fog_density;  /* 1/birim */
} R3dFrame;

/* shaders_init'ten sonra: @mipmap'siz varyantları derler, gölge haritasını kurar */
void r3d_init(R3dRenderer* rr, Arena* perm, Arena* scratch, u32 shadow_size);
/* Paralel ön uç (g3d_set_deferred + g3d_flush) açık/kapalı; varsayılan açık, çıktı aynı */
void r3d_set_parallel(b32 on);
/* Kareyi temizler, gölge + ana geçişi çizer, raster'ı boşaltır */
void r3d_render(R3dRenderer* rr, Rasterizer* r, Framebuffer* fb, Arena* frame, const ShaderManager* sm,
                const R3dFrame* f, R3dSceneFn scene, void* user, G3dStats* out_main, G3dStats* out_shadow);

/* Işıklı nesne (gölge geçişinde gölge düşürür, ana geçişte PBR/Lambert) */
void r3d_draw(R3dPass* p, const Mesh* mesh, const Mat4* model, const Texture* tex, Vec4 color,
              f32 metal, f32 rough, G3dCull cull);
/* Yalnız gölge alan (gölge düşürmeyen) nesne — ör. zemin */
void r3d_draw_receiver(R3dPass* p, const Mesh* mesh, const Mat4* model, const Texture* tex, Vec4 color,
                       f32 metal, f32 rough);
/* Kendinden ışıklı nesne: gölge düşürmez, ışıktan etkilenmez */
void r3d_draw_emissive(R3dPass* p, const Mesh* mesh, const Mat4* model, Vec4 color);

#endif
