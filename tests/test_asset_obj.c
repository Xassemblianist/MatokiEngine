/* tests/test_asset_obj.c — OBJ/MTL yükleyici testleri: dörtgen yüzlü küp (v/vt/vn, tekilleştirme),
 * negatif indeksler + normal üretimi, v//vn biçimi, CRLF + yorumlar, hatalı indekste satır numarası,
 * MTL Kd/map_Kd ve rastgele mutasyon fuzz'u (ASan altında çökme yok).
 */
#include "test.h"
#include "core/arena.h"
#include "core/rng.h"
#include "core/math.h"
#include "asset/obj.h"
#include <string.h>

static u8 g_mem[MB(8)];

static const char* k_cube =
    "# kup\n"
    "mtllib kup.mtl\n"
    "v -1 -1 1\nv 1 -1 1\nv 1 1 1\nv -1 1 1\nv -1 -1 -1\nv 1 -1 -1\nv 1 1 -1\nv -1 1 -1\n"
    "vt 0 0\nvt 1 0\nvt 1 1\nvt 0 1\n"
    "vn 0 0 1\nvn 0 0 -1\nvn 1 0 0\nvn -1 0 0\nvn 0 1 0\nvn 0 -1 0\n"
    "f 1/1/1 2/2/1 3/3/1 4/4/1\n"
    "f 6/1/2 5/2/2 8/3/2 7/4/2\n"
    "f 2/1/3 6/2/3 7/3/3 3/4/3\n"
    "f 5/1/4 1/2/4 4/3/4 8/4/4\n"
    "f 4/1/5 3/2/5 7/3/5 8/4/5\n"
    "f 5/1/6 6/2/6 2/3/6 1/4/6\n";

TEST(obj_cube_quads_dedup) {
    Arena a, s;
    arena_init(&a, g_mem, MB(4));
    arena_init(&s, g_mem + MB(4), MB(4));
    ObjInfo info;
    Mesh* m = obj_load(k_cube, (u32)strlen(k_cube), &a, &s, &info);
    CHECK(m != NULL);
    if (!m) return;
    CHECK(info.positions == 8 && info.texcoords == 4 && info.normals == 6);
    CHECK(info.faces == 6 && info.triangles == 12);
    CHECK(m->vertex_count == 24);          /* her yüz kendi normaliyle 4 köşe */
    CHECK(m->index_count == 36);
    CHECK(!info.generated_normals);
    CHECK(strcmp(info.mtllib, "kup.mtl") == 0);
    CHECK_NEAR(m->radius, m_sqrt(3.0f), 1e-5);
    /* UV v ekseni çevrildi: vt 0 0 → (0, 1) */
    CHECK_NEAR(m->u[0], 0.0f, 1e-6);
    CHECK_NEAR(m->v[0], 1.0f, 1e-6);
    /* Ön yüz üçgenleri CCW (dışarıdan +z'ye bakınca) */
    u32 i0 = m->indices[0], i1 = m->indices[1], i2 = m->indices[2];
    Vec3 e1 = vec3(m->px[i1] - m->px[i0], m->py[i1] - m->py[i0], m->pz[i1] - m->pz[i0]);
    Vec3 e2 = vec3(m->px[i2] - m->px[i0], m->py[i2] - m->py[i0], m->pz[i2] - m->pz[i0]);
    CHECK(vec3_cross(e1, e2).z > 0.0f);
}

TEST(obj_negative_indices_and_generated_normals) {
    static const char* src =
        "v 0 0 0\r\nv 1 0 0\r\nv 0 1 0\r\n"
        "f -3 -2 -1   # goreli\r\n"
        "v 0 0 -1.5e0\n"
        "f 1 3 4\n";
    Arena a, s;
    arena_init(&a, g_mem, MB(4));
    arena_init(&s, g_mem + MB(4), MB(4));
    ObjInfo info;
    Mesh* m = obj_load(src, (u32)strlen(src), &a, &s, &info);
    CHECK(m != NULL);
    if (!m) return;
    CHECK(info.triangles == 2);
    CHECK(m->vertex_count == 4);
    CHECK(info.generated_normals);
    /* İlk üçgen z=0 düzleminde CCW → normal +z (ikinci üçgenin katkısı x'te) */
    f32 nl = m_sqrt(m->nx[1] * m->nx[1] + m->ny[1] * m->ny[1] + m->nz[1] * m->nz[1]);
    CHECK_NEAR(nl, 1.0f, 1e-5);
    CHECK_NEAR(m->nz[1], 1.0f, 1e-5);
    CHECK_NEAR(m->pz[3], -1.5f, 1e-6);
}

TEST(obj_formats_and_errors) {
    Arena a, s;
    ObjInfo info;
    static const char* vn_only = "v 0 0 0\nv 1 0 0\nv 0 1 0\nvn 0 0 1\nf 1//1 2//1 3//1\n";
    arena_init(&a, g_mem, MB(4));
    arena_init(&s, g_mem + MB(4), MB(4));
    Mesh* m = obj_load(vn_only, (u32)strlen(vn_only), &a, &s, &info);
    CHECK(m && m->vertex_count == 3 && !info.generated_normals);

    static const char* bad = "v 0 0 0\nv 1 0 0\nv 0 1 0\nf 1 2 3\nf 1 2 9\n";
    arena_init(&a, g_mem, MB(4));
    arena_init(&s, g_mem + MB(4), MB(4));
    CHECK(obj_load(bad, (u32)strlen(bad), &a, &s, &info) == NULL);
    CHECK(info.err_line == 5);

    static const char* nofaces = "v 0 0 0\n# bos\n";
    CHECK(obj_load(nofaces, (u32)strlen(nofaces), &a, &s, &info) == NULL);

    static const char* mtl = "# m\nnewmtl kirmizi\nKd 0.8 0.1 0.05\nmap_Kd doku_1.png\nnewmtl diger\nKd 0 0 0\n";
    MtlInfo mi;
    CHECK(mtl_load_first(mtl, (u32)strlen(mtl), &mi));
    CHECK_NEAR(mi.kd[0], 0.8f, 1e-6);
    CHECK_NEAR(mi.kd[2], 0.05f, 1e-6);
    CHECK(strcmp(mi.map_kd, "doku_1.png") == 0);
}

TEST(obj_fuzz) {
    static char buf[1024];
    u32 len = (u32)strlen(k_cube);
    Rng r;
    rng_seed(&r, 9);
    Arena a, s;
    u32 ok = 0;
    for (u32 it = 0; it < 20000; it++) {
        memcpy(buf, k_cube, len);
        for (u32 k = 0; k < 3; k++) buf[rng_u32(&r) % len] = "0123456789-/ \nfv.e#"[rng_u32(&r) % 19u];
        arena_init(&a, g_mem, MB(4));
        arena_init(&s, g_mem + MB(4), MB(4));
        ObjInfo info;
        Mesh* m = obj_load(buf, len, &a, &s, &info);
        if (m) {
            ok++;
            for (u32 i = 0; i < m->index_count; i++) CHECK(m->indices[i] < m->vertex_count);
        }
    }
    CHECK(ok > 0);
}
