/* app/shaders.h — Gömülü ve canlı Matoki shader'larını derleyen ve yöneten modül.
 * Açılışta g_embedded_shaders içindeki .mtk kaynaklarını IR + wasm olarak derler,
 * jit_load_shade ile paylaşılan tabloya yükler, derleme süresini (ms), IR komut
 * sayısını ve wasm ikili boyutunu kaydeder; derleme hatasında C referansa düşer.
 */
#ifndef APP_SHADERS_H
#define APP_SHADERS_H

#include "core/types.h"
#include "core/arena.h"
#include "raster/raster.h"
#include "raster/shade_abi.h"
#include "matoki/matoki.h"

/* shaders/ ve shaders/compute/ altındaki tüm gömülü .mtk dosyalarına yetmeli (tests/test_app_shaders.c) */
#define APP_MAX_SHADERS 32

typedef struct {
    char      name[32];
    ShadeFn   jit_fn[4];        /* 0: NONE, 1: REPLACE(BLEND_NONE), 2: ALPHA, 3: ADD */
    b32       has_fused[4];     /* ilgili varyant füzyonlu olarak başarıyla derlendi mi */
    ShadeFn   ref_fn;           /* C referans yedek fonksiyonu */
    MtkLayout layout;
    u32       wasm_bytes;       /* temel (NONE) modülün wasm byte boyutu */
    u32       ir_inst_count;    /* optimize edilmiş IR komut sayısı */
    f64       compile_ms;       /* derleme + JIT yükleme süresi (ms) */
    b32       ok;               /* JIT derlemesi başarılı mı */
} ManagedShader;

typedef struct {
    ManagedShader items[APP_MAX_SHADERS];
    u32           count;
} ShaderManager;

/* Gömülü tüm shader'ları derler ve JIT tablosuna yükler. */
void                 shaders_init(ShaderManager* sm, Arena* scratch);

/* İsimle ("sprite.mtk" veya "sprite") yönetilen shader kaydını bulur. */
const ManagedShader* shaders_find(const ShaderManager* sm, const char* name);

/* Verilen shader için istenen moda (use_jit) ve blend moduna uygun ShadeFn döner.
 * *out_fused (NULL değilse) seçilen fonksiyonun blend füzyonlu olup olmadığını alır. */
ShadeFn              shaders_get_fn(const ManagedShader* sh, b32 use_jit,
                                    BlendMode blend, b32* out_fused);

/* Tek bir kaynağı derleyip ManagedShader yapısını doldurur (canlı editör için). */
b32                  shaders_compile_one(ManagedShader* out, const char* name,
                                         const char* src, u32 len,
                                         ShadeFn ref_fallback, Arena* scratch,
                                         MtkDiag* diag);

/* C referans yedek shader'ları (plasma, branchy, lit3d, pbr3d, unlit3d, dungeon3d, ocean, sky) */
void ref_shade_plasma(ShadeBatch* batch);
void ref_shade_branchy(ShadeBatch* batch);
void ref_shade_lit3d(ShadeBatch* batch);
void ref_shade_pbr3d(ShadeBatch* batch);
void ref_shade_unlit3d(ShadeBatch* batch);
void ref_shade_dungeon3d(ShadeBatch* batch);
void ref_shade_ocean(ShadeBatch* batch);
void ref_shade_sky(ShadeBatch* batch);

#endif
