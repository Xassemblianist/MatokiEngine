/* asset/obj.h — Wavefront OBJ yükleyici (sıfırdan): v / vt / vn / f (çokgenler yelpaze üçgenlenir),
 * v, v/vt, v//vn, v/vt/vn köşe biçimleri, negatif (göreli) indeksler, köşe tekilleştirme (hash),
 * eksik normallerde alan ağırlıklı normal üretimi. MTL'den yalnız ilk malzemenin Kd rengi ve map_Kd adı.
 * UV: OBJ'de v aşağıdan yukarı → dokularımızda yukarıdan aşağı (v' = 1 - v).
 */
#ifndef ASSET_OBJ_H
#define ASSET_OBJ_H

#include "core/types.h"
#include "core/arena.h"
#include "gfx3d/mesh.h"

typedef struct {
    u32  positions, texcoords, normals;   /* dosyadaki kayıt sayıları */
    u32  faces, triangles;
    u32  unique_vertices;                 /* tekilleştirme sonrası */
    b32  generated_normals;
    char mtllib[64];                      /* "mtllib" adı (varsa) */
    char err[96];
    u32  err_line;
} ObjInfo;

/* Başarıda mesh döner (out arena'da); scratch geçici tablolar içindir. */
Mesh* obj_load(const char* text, u32 len, Arena* out, Arena* scratch, ObjInfo* info);

typedef struct {
    f32  kd[3];          /* dağınık renk (varsayılan 1,1,1) */
    char map_kd[64];     /* doku dosyası adı (varsa) */
} MtlInfo;

/* MTL dosyasından ilk malzemeyi okur */
b32 mtl_load_first(const char* text, u32 len, MtlInfo* out);

#endif
