/* app/scene3d.c — 3D demo sahnesinin kurulumu, çizimi ve HUD'u.
 * Tüm kalıcı varlıklar (mesh'ler, dokular, gölge haritası) perm arena'da; materyal uniform blokları ve
 * doku dizileri her kare frame arena'dan ayrılır (raster flush'a kadar geçerli kalmaları gerekir).
 * Kare: (1) güneşten gölge haritası (yalnız derinlik), (2) PBR ana geçiş (pbr3d.mtk) veya eski Lambert (lit3d.mtk).
 */
#include "app/scene3d.h"
#include "core/math.h"
#include "core/log.h"
#include "raster/texture.h"
#include "core/embed.h"
#include "asset/gltf.h"
#include "asset/obj.h"
#include "img/png.h"
#include "img/image.h"
#include "plat/platform.h"
#include "gfx3d/mesh.h"
#include "gfx3d/camera.h"
#include "gfx3d/pipeline.h"
#include "gfx3d/shadow.h"
#include "app/r3d.h"
#include "core/profiler.h"
#include "ui/font.h"

#define KEY_C 67
#define KEY_G 71
#define KEY_M 77
#define KEY_L 76
#define KEY_N 78
#define GRID_X 40
#define GRID_Z 25
#define SHADOW_SIZE 1024u
#define DAY_SECONDS 90.0f

static Mesh*    g_cube;
static Mesh*    g_ground;
static Mesh*    g_sphere;
static Texture  g_tex[3];      /* 0: zemin damalı (mip zincirli), 1: küp, 2: beyaz */
static Texture  g_tex_ground_nomip;   /* aynı pikseller, MIPMAP bayrağı yok (C referans yolu için) */
static R3dRenderer g_r3d;
static b32      g_mipmap_on;
static b32      g_pbr_on;
static b32      g_day_paused;
static f32      g_tod;                /* gün saati [0,1): 0.25 öğle, 0.75 gece yarısı */
static u8       g_prev_m, g_prev_p, g_prev_t;
static Camera   g_cam;
static G3dStats g_last_stats;
static G3dStats g_shadow_stats;

/* Yüklenen model (sürükle-bırak) */
typedef struct {
    Mesh*          mesh;
    Mat4           world;
    const Texture* tex;
    Vec4           color;
    b32            double_sided;
} ModelDraw;
#define MAX_MODEL_DRAWS 512
static ModelDraw g_model[MAX_MODEL_DRAWS];
static u32       g_model_count;
static Mat4      g_model_fit;
static char      g_model_info[160];
static u8       g_prev_c;

void scene3d_init(Arena* perm, Arena* scratch, Framebuffer* fb) {
    if (fb->depth == NULL) fb_init_depth(fb, perm);

    g_cube   = mesh_cube(perm, 0.5f);
    g_ground = mesh_plane(perm, 240.0f, 24, 60.0f);   /* 4 m'lik hücreler; düzlemi kesen üçgenler */
    g_sphere = mesh_sphere(perm, 1.0f, 32, 16);

    tex_init_mipmapped(&g_tex[0], perm, 64, 64, TEX_FLAG_BILINEAR);
    tex_gen_checker(&g_tex[0], 8, FB_RGBA(235, 235, 228, 255), FB_RGBA(70, 82, 96, 255));
    tex_build_mips(&g_tex[0]);
    g_tex_ground_nomip        = g_tex[0];
    g_tex_ground_nomip.flags &= ~TEX_FLAG_MIPMAP;
    g_tex_ground_nomip.levels = 1u;
    g_mipmap_on = TRUE;
    g_pbr_on    = TRUE;
    g_day_paused = FALSE;
    g_tod       = 0.13f;     /* sabah */
    g_prev_m = g_prev_p = g_prev_t = 0;
    r3d_init(&g_r3d, perm, scratch, SHADOW_SIZE);
    tex_init(&g_tex[1], perm, 32, 32, TEX_FLAG_BILINEAR);
    tex_gen_gradient(&g_tex[1], FB_RGBA(255, 220, 90, 255), FB_RGBA(255, 120, 60, 255),
                     FB_RGBA(90, 200, 255, 255), FB_RGBA(150, 90, 255, 255));
    tex_init(&g_tex[2], perm, 4, 4, 0);
    tex_gen_checker(&g_tex[2], 4, FB_RGBA(255, 255, 255, 255), FB_RGBA(255, 255, 255, 255));

    camera_init(&g_cam, CAM_FLY);
    g_cam.pos   = vec3(0.0f, 3.0f, 14.0f);
    g_cam.pitch = -0.18f;
    g_prev_c    = 0;
}

static Vec4 hue_color(f32 h) {
    f32 r = m_clamp(m_abs(h * 6.0f - 3.0f) - 1.0f, 0.0f, 1.0f);
    f32 g = m_clamp(2.0f - m_abs(h * 6.0f - 2.0f), 0.0f, 1.0f);
    f32 b = m_clamp(2.0f - m_abs(h * 6.0f - 4.0f), 0.0f, 1.0f);
    return vec4(0.35f + 0.65f * r, 0.35f + 0.65f * g, 0.35f + 0.65f * b, 1.0f);
}

/* Sahnedeki tüm nesneler (gölge ve ana geçiş aynı listeyi kullanır). Zemin gölge düşürmez. */
static void draw_scene(R3dPass* p, void* user) {
    f32 time_sec = *(const f32*)user;
    /* Önden arkaya kaba sıra early-Z'ye yardım eder: önce yakın nesneler, zemin en son */
    Mat4 m = mat4_trs(vec3(0.0f, 1.6f, 0.0f), time_sec * 0.7f, time_sec * 0.45f, vec3(2.2f, 2.2f, 2.2f));
    r3d_draw(p, g_cube, &m, &g_tex[1], vec4(1, 1, 1, 1), 0.0f, 0.45f, G3D_CULL_BACK);

    m = mat4_trs(vec3(4.0f, 1.2f + 0.4f * m_sin(time_sec * 1.3f), -1.0f), 0.0f, 0.0f, vec3(1.2f, 1.2f, 1.2f));
    r3d_draw(p, g_sphere, &m, &g_tex[2], vec4(0.95f, 0.75f, 0.35f, 1.0f), 1.0f, 0.25f, G3D_CULL_BACK);

    /* 1000 küplük ızgara: sütunlar boyunca metallik (5 adım), satırlar boyunca pürüzlülük (5 adım) */
    for (u32 j = 0; j < GRID_Z; j++) {
        for (u32 i = 0; i < GRID_X; i++) {
            f32 x = ((f32)i - (f32)(GRID_X - 1) * 0.5f) * 3.0f;
            f32 z = -10.0f - (f32)j * 3.0f;
            f32 ph = (f32)(i * 7u + j * 13u) * 0.37f;
            f32 h = 0.6f + 0.5f * m_sin(time_sec * 0.9f + ph);
            m = mat4_trs(vec3(x, h, z), time_sec * 0.5f + ph, 0.0f, vec3(1.0f, 1.0f, 1.0f));
            f32 metal = (f32)(i % 5u) * 0.25f;
            f32 rough = 0.1f + (f32)(j % 5u) * 0.2f;
            r3d_draw(p, g_cube, &m, &g_tex[2], hue_color((f32)((i * 37u + j * 11u) % 64u) / 64.0f),
                     metal, rough, G3D_CULL_BACK);
        }
    }

    /* Sürükle-bırak ile yüklenen model */
    for (u32 i = 0; i < g_model_count; i++) {
        const ModelDraw* d = &g_model[i];
        m = mat4_mul(g_model_fit, d->world);
        r3d_draw(p, d->mesh, &m, d->tex, d->color, 0.0f, 0.6f, d->double_sided ? G3D_CULL_NONE : G3D_CULL_BACK);
    }

    m = mat4_identity();
    r3d_draw_receiver(p, g_ground, &m, g_mipmap_on ? &g_tex[0] : &g_tex_ground_nomip, vec4(1, 1, 1, 1), 0.0f, 0.85f);
}

static b32 key_pressed(const InputState* in, u32 key, u8* prev) {
    u8 down = in->keys[key];
    b32 hit = down && !*prev;
    *prev = down;
    return hit;
}

void scene3d_frame(Rasterizer* r, Framebuffer* fb, Arena* frame, const ShaderManager* sm,
                   const InputState* in, f32 dt, f32 time_sec, b32 use_jit, b32 look_enabled) {
    if (key_pressed(in, KEY_C, &g_prev_c)) {
        if (g_cam.mode == CAM_FLY) {
            g_cam.mode   = CAM_ORBIT;
            g_cam.target = vec3_add(g_cam.pos, vec3_scale(camera_forward(&g_cam), g_cam.dist));
        } else {
            g_cam.pos  = camera_eye(&g_cam);
            g_cam.mode = CAM_FLY;
        }
    }
    if (key_pressed(in, KEY_M, &g_prev_m)) {
        g_mipmap_on = !g_mipmap_on;
        LOG("Mipmap: %s", g_mipmap_on ? "acik" : "kapali");
    }
    if (key_pressed(in, KEY_L, &g_prev_p)) {
        g_pbr_on = !g_pbr_on;
        LOG("Aydinlatma: %s", g_pbr_on ? "PBR + golge" : "Lambert");
    }
    if (key_pressed(in, KEY_N, &g_prev_t)) g_day_paused = !g_day_paused;
    f32 speed = in->keys[KEY_G] ? 20.0f : 1.0f;
    if (!g_day_paused || in->keys[KEY_G]) {
        g_tod += dt * speed / DAY_SECONDS;
        g_tod -= m_floor(g_tod);
    }
    camera_update(&g_cam, in, dt, look_enabled);

    /* Gölge: kameranın önündeki 36 birimlik küre */
    Vec3 eye = camera_eye(&g_cam);
    Vec3 fwd = camera_forward(&g_cam);
    Vec3 c = vec3_add(eye, vec3_scale(vec3(fwd.x, 0.0f, fwd.z), 22.0f));
    c.y = 1.0f;

    R3dFrame f;
    f.cam           = &g_cam;
    f.dl            = r3d_daylight(g_tod);
    f.pbr           = g_pbr_on;
    f.mipmap        = g_mipmap_on;
    f.use_jit       = use_jit;
    f.shadow_center = c;
    f.shadow_radius = 36.0f;
    f.fog_density   = 1.0f / 160.0f;
    r3d_render(&g_r3d, r, fb, frame, sm, &f, draw_scene, &time_sec, &g_last_stats, &g_shadow_stats);
}

void scene3d_bench_set(b32 pbr, f32 tod) {
    g_pbr_on = pbr;
    if (tod >= 0.0f) { g_tod = tod; g_day_paused = TRUE; }
}

void scene3d_draw_hud(UiContext* ui, const Framebuffer* fb) {
    char line[160];
    const G3dStats* s = &g_last_stats;
    fmt_format(line, sizeof(line),
               "Nesne %llu (culled %llu) | Ucgen %llu -> ciz %llu, kirp %llu, arka %llu, red %llu | golge %llu",
               (unsigned long long)s->draws, (unsigned long long)s->draws_culled,
               (unsigned long long)s->tris_in, (unsigned long long)s->tris_out,
               (unsigned long long)s->tris_clipped, (unsigned long long)s->tris_backface,
               (unsigned long long)s->tris_rejected,
               (unsigned long long)(g_pbr_on ? g_shadow_stats.tris_out : 0));
    i32 y = (i32)fb->h - 22;
    ui_draw_rect(ui, 0, y - 2, (i32)fb->w, 24, FB_RGBA(15, 18, 26, 190));
    ui_draw_text(ui, 4, y, line, FB_RGBA(225, 235, 248, 255), 1);
    u32 hh = (u32)(g_tod * 24.0f + 6.0f) % 24u;   /* tod 0 = 06:00 gün doğumu */
    f32 hf = g_tod * 24.0f;
    u32 mm = (u32)((hf - m_floor(hf)) * 60.0f);
    fmt_format(line, sizeof(line), "[C] %s [M] mip %s [L] %s [N] %s [G] hizli %02u:%02u | (%.1f, %.1f, %.1f)",
               g_cam.mode == CAM_FLY ? "Ucus" : "Orbit", g_mipmap_on ? "acik" : "kapali",
               g_pbr_on ? "PBR+golge" : "Lambert", g_day_paused ? "durdu" : "gun", hh, mm,
               (f64)camera_eye(&g_cam).x, (f64)camera_eye(&g_cam).y, (f64)camera_eye(&g_cam).z);
    ui_draw_text(ui, 4, y + 11, line, FB_RGBA(150, 200, 255, 255), 1);
    const char* hint = g_model_info[0] ? g_model_info : ".glb / .gltf / .obj dosyasini pencereye surukle-birak";
    i32 hw = font_measure_width(hint, 1);
    ui_draw_rect(ui, (i32)fb->w - hw - 10, 22, hw + 8, 12, FB_RGBA(15, 18, 26, 190));
    ui_draw_text(ui, (i32)fb->w - hw - 6, 24, hint, FB_RGBA(255, 236, 140, 255), 1);
}

/* ---------- Model yükleme ---------- */

static b32 name_has_ext(const char* n, const char* ext) {
    u32 ln = 0, le = 0;
    while (n[ln]) ln++;
    while (ext[le]) le++;
    if (ln < le) return FALSE;
    for (u32 i = 0; i < le; i++) {
        char a = n[ln - le + i], b = ext[i];
        if (a >= 'A' && a <= 'Z') a = (char)(a - 'A' + 'a');
        if (a != b) return FALSE;
    }
    return TRUE;
}

/* "dizin/ad.png" ile "ad.png" eşleşsin: yalnız son yol bileşeni karşılaştırılır */
static const char* base_name(const char* s) {
    const char* b = s;
    for (const char* p = s; *p; p++) if (*p == '/' || *p == '\\') b = p + 1;
    return b;
}

static b32 same_name(const char* a, const char* b) {
    a = base_name(a);
    b = base_name(b);
    while (*a && *a == *b) { a++; b++; }
    return *a == *b;
}

typedef struct { const DroppedFile* files; u32 count; } FileSet;

static const u8* resolve_file(void* user, const char* uri, u32* out_len) {
    const FileSet* fs = (const FileSet*)user;
    for (u32 i = 0; i < fs->count; i++) {
        if (same_name(fs->files[i].name, uri)) { *out_len = fs->files[i].len; return fs->files[i].data; }
    }
    return NULL;
}

static b32 png_to_texture(const u8* data, u32 len, Arena* out, Arena* scratch, Texture* t) {
    u32 w = 0, h = 0;
    u32* px = png_decode(data, len, &w, &h, scratch);
    if (!px || !w || !h) return FALSE;
    u32 nw = 1, nh = 1;
    while (nw < w && nw < 1024) nw <<= 1;
    while (nh < h && nh < 1024) nh <<= 1;
    tex_init_mipmapped(t, out, nw, nh, TEX_FLAG_BILINEAR);
    if (nw == w && nh == h) { for (u32 i = 0; i < w * h; i++) t->px[i] = px[i]; }
    else img_resize_bilinear(px, w, h, t->px, nw, nh);
    tex_build_mips(t);
    return TRUE;
}

/* Modeli zemine oturt, en büyük boyutu 4 birime ölçekle, (-6, 0, 2) civarına yerleştir */
static void fit_model(Vec3 lo, Vec3 hi) {
    Vec3 ext = vec3_sub(hi, lo);
    f32 big = m_max(ext.x, m_max(ext.y, ext.z));
    f32 s = (big > 0.0f) ? 4.0f / big : 1.0f;
    Vec3 c = vec3((lo.x + hi.x) * 0.5f, lo.y, (lo.z + hi.z) * 0.5f);
    g_model_fit = mat4_mul(mat4_translate(vec3(-6.0f, 0.0f, 2.0f)),
                           mat4_mul(mat4_scale(vec3(s, s, s)), mat4_translate(vec3_scale(c, -1.0f))));
}

b32 scene3d_load_files(const DroppedFile* files, u32 count, Arena* out, Arena* scratch) {
    g_model_count = 0;
    g_model_info[0] = 0;
    FileSet fs = { files, count };
    f64 t0 = plat_time_ms();
    for (u32 i = 0; i < count; i++) {
        const DroppedFile* f = &files[i];
        if (name_has_ext(f->name, ".glb") || name_has_ext(f->name, ".gltf")) {
            GltfModel gm;
            if (!gltf_load(f->data, f->len, out, scratch, resolve_file, &fs, &gm)) {
                fmt_format(g_model_info, sizeof(g_model_info), "%s: HATA %s", base_name(f->name), gm.err);
                LOG("glTF yuklenemedi: %s", gm.err);
                return FALSE;
            }
            for (u32 k = 0; k < gm.draw_count && g_model_count < MAX_MODEL_DRAWS; k++) {
                const GltfDraw* gd = &gm.draws[k];
                ModelDraw* d = &g_model[g_model_count++];
                d->mesh = gd->mesh;
                d->world = gd->world;
                d->tex = &g_tex[2];
                d->color = vec4(1, 1, 1, 1);
                d->double_sided = FALSE;
                if (gd->material >= 0) {
                    const GltfMaterial* mt = &gm.materials[gd->material];
                    d->color = mt->base_color;
                    d->double_sided = mt->double_sided;
                    if (mt->base_texture >= 0) d->tex = &gm.textures[mt->base_texture];
                }
            }
            fit_model(gm.bounds_min, gm.bounds_max);
            fmt_format(g_model_info, sizeof(g_model_info), "%s: %u cizim, %u ucgen, %u uyari, %.1f ms",
                       base_name(f->name), gm.draw_count, gm.triangles, gm.warnings, plat_time_ms() - t0);
            if (gm.warnings) LOG("glTF uyarilari: %s", gm.warn);
            LOG("glTF sureleri: JSON %.2f ms, doku (PNG+mip) %.2f ms, mesh %.2f ms", gm.t_json_ms, gm.t_tex_ms, gm.t_mesh_ms);
            LOG("%s", g_model_info);
            return TRUE;
        }
        if (name_has_ext(f->name, ".obj")) {
            ObjInfo oi;
            Mesh* mesh = obj_load((const char*)f->data, f->len, out, scratch, &oi);
            if (!mesh) {
                fmt_format(g_model_info, sizeof(g_model_info), "%s: HATA satir %u: %s", base_name(f->name), oi.err_line, oi.err);
                return FALSE;
            }
            ModelDraw* d = &g_model[g_model_count++];
            d->mesh = mesh;
            d->world = mat4_identity();
            d->tex = &g_tex[2];
            d->color = vec4(1, 1, 1, 1);
            d->double_sided = FALSE;
            u32 mlen = 0;
            const u8* mtl = oi.mtllib[0] ? resolve_file(&fs, oi.mtllib, &mlen) : NULL;
            if (mtl) {
                MtlInfo mi;
                if (mtl_load_first((const char*)mtl, mlen, &mi)) {
                    d->color = vec4(mi.kd[0], mi.kd[1], mi.kd[2], 1.0f);
                    u32 plen = 0;
                    const u8* png = mi.map_kd[0] ? resolve_file(&fs, mi.map_kd, &plen) : NULL;
                    Texture* t = ARENA_PUSH_STRUCT(out, Texture);
                    if (png && png_to_texture(png, plen, out, scratch, t)) d->tex = t;
                }
            }
            fit_model(mesh->aabb_min, mesh->aabb_max);
            fmt_format(g_model_info, sizeof(g_model_info), "%s: %u kose, %u ucgen%s, %.1f ms", base_name(f->name),
                       mesh->vertex_count, oi.triangles, oi.generated_normals ? " (normal uretildi)" : "",
                       plat_time_ms() - t0);
            LOG("%s", g_model_info);
            return TRUE;
        }
    }
    fmt_format(g_model_info, sizeof(g_model_info), "Model bulunamadi (.glb/.gltf/.obj bekleniyor)");
    return FALSE;
}
