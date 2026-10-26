/* img/procgen.h — 2'nin kuvveti boyutlu, tam döşenebilir (tileable) prosedürel doku üreticileri.
 * Tuğla, ahşap, mermer, çim, taş, metal panel, lav, su kostiği ve bulut dokuları.
 * Her üretici seed + kendi parametre struct'ını alır (NULL verilirse varsayılan parametreler kullanılır).
 * Çıktı Texture yapısı Arena üzerinde ayrılır ve RGBA8 piksellerle doldurulur.
 */
#ifndef IMG_PROCGEN_H
#define IMG_PROCGEN_H

#include "core/types.h"
#include "core/arena.h"
#include "raster/shade_abi.h"

typedef struct {
    i32 cols;           /* yatay tuğla sayısı (ör. 4) */
    i32 rows;           /* dikey sıra sayısı, çift olmalı (ör. 8) */
    f32 mortar_width;   /* harç kalınlığı [0.02 .. 0.25] */
    u32 brick_color;    /* baz tuğla rengi (RGBA8) */
    u32 mortar_color;   /* harç rengi (RGBA8) */
    f32 color_var;      /* tuğla başına renk çeşitliliği [0..1] */
} ProcBrickParams;

typedef struct {
    f32 rings;          /* yıllık halka sıklığı (ör. 8.0) */
    f32 turb_strength;  /* damar bükülme şiddeti (ör. 0.35) */
    i32 freq;           /* döşeme frekansı (ör. 4) */
    u32 light_color;    /* açık ahşap rengi */
    u32 dark_color;     /* koyu damar rengi */
} ProcWoodParams;

typedef struct {
    i32 freq;           /* temel frekans (ör. 4) */
    f32 vein_freq;      /* damar sıklığı (ör. 3.0) */
    f32 turb;           /* türbülans şiddeti (ör. 5.0) */
    u32 base_color;     /* ana mermer rengi */
    u32 vein_color;     /* damar rengi */
} ProcMarbleParams;

typedef struct {
    i32 base_freq;      /* zemin dalga frekansı (ör. 8) */
    i32 blade_freq;     /* çim yaprağı frekansı (ör. 32) */
    u32 dark_green;     /* dip rengi */
    u32 bright_green;   /* uç rengi */
} ProcGrassParams;

typedef struct {
    i32 cells;          /* Worley hücre sayısı (ör. 6) */
    f32 crack_sharpness;/* çatlak keskinliği (ör. 12.0) */
    u32 stone_color;    /* taş ana rengi */
    u32 crack_color;    /* derz/çatlak rengi */
} ProcStoneParams;

typedef struct {
    i32 grid_x;         /* yatay panel sayısı (ör. 2) */
    i32 grid_y;         /* dikey panel sayısı (ör. 2) */
    f32 bevel_width;    /* pah/derz genişliği (ör. 0.06) */
    f32 brush_strength; /* fırçalanmış yüzey izi (ör. 0.12) */
    u32 base_color;     /* metal rengi */
} ProcMetalParams;

typedef struct {
    i32 cells;          /* kabuk hücre sıklığı (ör. 6) */
    f32 glow_width;     /* kor çatlak genişliği (ör. 0.35) */
    u32 crust_color;    /* soğumuş siyah/gri kabuk */
    u32 lava_color;     /* sıcak turuncu/sarı kor */
} ProcLavaParams;

typedef struct {
    i32 cells;          /* kostik hücre sıklığı (ör. 6) */
    f32 warp;           /* dalga bükülme şiddeti (ör. 0.45) */
    f32 sharpness;      /* ışık ağının keskinliği (ör. 3.5) */
    u32 deep_color;     /* derin su mavisi */
    u32 highlight_color;/* kostik parlama rengi */
} ProcCausticParams;

typedef struct {
    i32 freq;           /* bulut frekansı (ör. 4) */
    f32 coverage;       /* bulut yoğunluğu [0..1] (ör. 0.52) */
    f32 softness;       /* geçiş yumuşaklığı (ör. 0.35) */
    u32 sky_color;      /* gökyüzü rengi */
    u32 cloud_color;    /* bulut rengi */
} ProcCloudParams;

void procgen_brick(Texture* out, Arena* arena, u32 w, u32 h, u32 seed, const ProcBrickParams* p);
void procgen_wood(Texture* out, Arena* arena, u32 w, u32 h, u32 seed, const ProcWoodParams* p);
void procgen_marble(Texture* out, Arena* arena, u32 w, u32 h, u32 seed, const ProcMarbleParams* p);
void procgen_grass(Texture* out, Arena* arena, u32 w, u32 h, u32 seed, const ProcGrassParams* p);
void procgen_stone(Texture* out, Arena* arena, u32 w, u32 h, u32 seed, const ProcStoneParams* p);
void procgen_metal(Texture* out, Arena* arena, u32 w, u32 h, u32 seed, const ProcMetalParams* p);
void procgen_lava(Texture* out, Arena* arena, u32 w, u32 h, u32 seed, const ProcLavaParams* p);
void procgen_caustic(Texture* out, Arena* arena, u32 w, u32 h, u32 seed, const ProcCausticParams* p);
void procgen_cloud(Texture* out, Arena* arena, u32 w, u32 h, u32 seed, const ProcCloudParams* p);

#endif
