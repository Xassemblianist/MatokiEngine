/* core/embed.h — build sırasında gömülen dosyalar (tools/embed.py üretir).
 * shaders/NAME.mtk → g_embedded_shaders. src NUL ile biter, len NUL hariç.
 * Yalnızca wasm derlemesine girer (host araçları dosyayı diskten okur).
 */
#ifndef CORE_EMBED_H
#define CORE_EMBED_H

#include "core/types.h"

typedef struct {
    const char* name;   /* "sprite.mtk" */
    const char* src;
    u32         len;
} EmbeddedFile;

extern const EmbeddedFile g_embedded_shaders[];
extern const u32          g_embedded_shader_count;

#endif
