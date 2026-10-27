/* gfx3d/billboard.h — Kameraya bakan 3D dörtgen (billboard) toplayıcı ve dinamik Mesh üretici.
 * Dünya konumu, boyut, dönüş, RGBA8 köşe rengi ve doku/atlas UV bilgisiyle verilen dörtgenleri
 * tek bir dinamik Mesh'te (G3D_ATTR_COLOR destekli) birleştirir; tek g3d_draw ile çizilir.
 * Alfa harmanlı çizim için kameraya uzaklığa göre malloc'suz radix sıralama (arkadan öne) içerir.
 */
#ifndef GFX3D_BILLBOARD_H
#define GFX3D_BILLBOARD_H

#include "core/types.h"
#include "core/arena.h"
#include "core/math.h"
#include "gfx3d/mesh.h"
#include "gfx3d/pipeline.h"

#define BILLBOARD_MAX_CAPACITY 65536u

typedef struct {
    f32 u0, v0;
    f32 u1, v1;
} BillboardUv;

typedef struct {
    Vec3        pos;    /* Dünya uzayı merkez konumu */
    f32         w, h;   /* Dünya uzayı genişlik ve yükseklik (> 0) */
    f32         rot;    /* Kamera düzleminde saat yönünün tersi dönüş açısı (radyan) */
    u32         color;  /* RGBA8 köşe rengi (FB_RGBA ile aynı paketleme) */
    BillboardUv uv;     /* Doku koordinat dikdörtgeni */
} Billboard;

typedef struct {
    Mesh*      mesh;           /* Dinamik üçgen ağı (capacity*4 köşe, capacity*6 indeks, köşe renkli) */
    Billboard* items;          /* Toplanan dörtgen tanımları [capacity] */
    u32*       order;          /* Çizim sırası indeksleri [capacity] */
    u32*       sort_idx_tmp;   /* Radix sıralama geçici indeks tamponu [capacity] */
    u32*       sort_keys;      /* Radix sıralama uzaklık anahtarları [capacity] */
    u32*       sort_keys_tmp;  /* Radix sıralama geçici anahtar tamponu [capacity] */
    u32        capacity;       /* Maksimum dörtgen kapasitesi */
    u32        count;          /* Geçerli karede eklenen dörtgen sayısı */
    u32        overflow_count; /* Kapasite doluyken atlanan geçerli dörtgen sayısı */
    u32        skipped_count;  /* Sıfır/negatif/NaN boyut veya NaN/Inf konum nedeniyle atlanan sayısı */
    Vec3       cam_right;      /* Kamera sağ birim vektörü (view matrisinden) */
    Vec3       cam_up;         /* Kamera yukarı birim vektörü (view matrisinden) */
    Vec3       cam_forward;    /* Kamera ileri birim vektörü (view matrisinden) */
    Vec3       normal;         /* Billboard yüzey normali (= -cam_forward) */
    Vec3       eye;            /* Kamera dünya konumu (view matrisinden çıkarılan veya verilen) */
    b32        in_begin;       /* billboard_begin .. billboard_end arasında mı */
    b32        mesh_dirty;     /* Mesh köşeleri yeniden üretilmeli mi */
    b32        initialized;    /* Başarıyla ayrıldı mı */
} BillboardBatch;

/* Tam doku [0,0]..[1,1] UV aralığı döner */
static inline BillboardUv billboard_uv_full(void) {
    BillboardUv uv = { 0.0f, 0.0f, 1.0f, 1.0f };
    return uv;
}

/* Özel dikdörtgen [u0,v0]..[u1,v1] UV aralığı döner */
static inline BillboardUv billboard_uv_rect(f32 u0, f32 v0, f32 u1, f32 v1) {
    BillboardUv uv = { u0, v0, u1, v1 };
    return uv;
}

/* cols × rows ızgaralı doku atlasındaki frame karesi için UV dikdörtgeni hesaplar */
BillboardUv billboard_uv_atlas(u32 cols, u32 rows, u32 frame);

/* View matrisinden birim sağ, yukarı, ileri vektörlerini ve kamera göz konumunu çıkarır */
void billboard_extract_camera(Mat4 view, Vec3* out_right, Vec3* out_up,
                              Vec3* out_forward, Vec3* out_eye);

/* Sabit kapasiteli billboard toplayıcısını verilen Arena üzerinde ayırır (taşma denetimli) */
b32  billboard_init(BillboardBatch* b, Arena* a, u32 capacity);

/* Yeni kare/geçiş toplamasını başlatır; kamera eksenlerini ve göz konumunu view matrisinden alır */
void billboard_begin(BillboardBatch* b, Mat4 view);

/* Yeni kare/geçiş toplamasını açık göz konumu ile başlatır */
void billboard_begin_ex(BillboardBatch* b, Mat4 view, Vec3 eye);

/* Kare boyutlu (size × size), tam UV'li (0..1) bir billboard ekler */
b32  billboard_push(BillboardBatch* b, Vec3 pos, f32 size, f32 rot, u32 color);

/* Dikdörtgen boyutlu (size.x × size.y) ve özel UV aralıklı bir billboard ekler */
b32  billboard_push_ex(BillboardBatch* b, Vec3 pos, Vec2 size, f32 rot, u32 color, BillboardUv uv);

/* Doku atlası karesi kullanan kare boyutlu bir billboard ekler */
b32  billboard_push_atlas(BillboardBatch* b, Vec3 pos, f32 size, f32 rot, u32 color,
                          u32 cols, u32 rows, u32 frame);

/* Hazır Billboard yapısını ekler */
b32  billboard_push_item(BillboardBatch* b, const Billboard* item);

/* Toplanan dörtgenleri kameraya uzaklığa göre arkadan öne (azalan uzaklık) radix ile sıralar */
void billboard_sort(BillboardBatch* b);

/* Verilen göz konumuna olan uzaklığa göre arkadan öne radix ile sıralar */
void billboard_sort_by_eye(BillboardBatch* b, Vec3 eye);

/* Toplanan dörtgenlerden dinamik Mesh'i üretir ve döner */
Mesh* billboard_build_mesh(BillboardBatch* b);

/* Toplamayı kapatır, dinamik Mesh'i günceller ve döner */
Mesh* billboard_end(BillboardBatch* b);

/* Dinamik Mesh'i günceller ve birim model matrisiyle tek bir g3d_draw çağrısı yapar */
b32  billboard_draw(BillboardBatch* b, G3dContext* ctx, const G3dMaterial* mat);

#endif
