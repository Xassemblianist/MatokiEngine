/* tests/test_app_shaders.c — uygulama shader yöneticisinin kapasitesi.
 * shaders_init gömülü listeyi APP_MAX_SHADERS ile keser; sınır aşılırsa alfabetik
 * sıradaki son shader'lar (sky, sprite, unlit3d ...) yüklenmez ve CPU sahneleri bozulur.
 */
#include "test.h"
#include "core/embed.h"
#include "app/shaders.h"

TEST(app_shaders_capacity) {
    CHECK(g_embedded_shader_count > 0u);
    CHECK(g_embedded_shader_count <= APP_MAX_SHADERS);
}
