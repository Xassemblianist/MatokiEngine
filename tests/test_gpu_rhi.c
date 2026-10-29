/* tests/test_gpu_rhi.c — GPU RHI ikili komut akışı, tutamaç tablosu, sınır/taşma ve hata halkası testleri.
 * Host ortamında WebGPU olmadan çalışır; komut akışını yalnız ARCHITECTURE.md §2 ikili şartnamesine
 * göre okuyan bağımsız bir çözücü (decoder) ile doğrular.
 */
#include <stdio.h>
#include <string.h>
#include "test.h"
#include "core/arena.h"
#include "plat/platform.h"
#include "gpu/rhi.h"

/* --- Bağımsız İkili Akış Çözücüsü (yalnız ham baytları okur) --- */

static u32 rd_u32(const u8* p) {
    u32 v;
    memcpy(&v, p, 4);
    return v;
}

static i32 rd_i32(const u8* p) {
    i32 v;
    memcpy(&v, p, 4);
    return v;
}

static f32 rd_f32(const u8* p) {
    f32 v;
    memcpy(&v, p, 4);
    return v;
}

typedef struct {
    u32 cmd_count;
    u32 op_counts[RHI_CMD_COUNT];
    u32 total_bytes;
    u32 last_shader_len;
    char last_shader_prefix[32];
    u32 last_write_buf_len;
    u8  last_write_buf_head[16];
    u32 last_write_tex_w;
    u32 last_write_tex_h;
    u32 last_draw_verts;
    u32 last_draw_idx_count;
    i32 last_draw_base_vert;
    f32 last_clear_r;
    f32 last_clear_depth;
} DecodedStreamSummary;

static b32 decode_and_verify_stream(const u8* buf, u32 len, DecodedStreamSummary* out) {
    if (!buf || len < 16u || (len & 3u) != 0u) return FALSE;
    if (out) memset(out, 0, sizeof(*out));

    u32 magic     = rd_u32(buf + 0);
    u32 version   = rd_u32(buf + 4);
    u32 byte_len  = rd_u32(buf + 8);
    u32 cmd_count = rd_u32(buf + 12);

    if (magic != 0x4D475055u || version != 1u) return FALSE;
    if (byte_len != len) return FALSE;

    u32 off = 16u;
    u32 seen = 0u;
    while (off < byte_len) {
        if (byte_len - off < 8u) return FALSE;
        u32 op   = rd_u32(buf + off + 0);
        u32 size = rd_u32(buf + off + 4);
        if (size < 8u || (size & 3u) != 0u || size > byte_len - off) return FALSE;
        if (op == 0u || op >= (u32)RHI_CMD_COUNT) return FALSE;

        const u8* p = buf + off;
        switch (op) {
        case RHI_CMD_CREATE_BUFFER:
            if (size != 20u) return FALSE;
            if (rd_u32(p + 8) == 0u || rd_u32(p + 12) == 0u || rd_u32(p + 16) == 0u) return FALSE;
            break;
        case RHI_CMD_CREATE_TEXTURE:
            if (size != 32u) return FALSE;
            if (rd_u32(p + 8) == 0u || rd_u32(p + 12) == 0u || rd_u32(p + 16) == 0u) return FALSE;
            break;
        case RHI_CMD_CREATE_SAMPLER:
            if (size != 32u) return FALSE;
            if (rd_u32(p + 8) == 0u) return FALSE;
            break;
        case RHI_CMD_CREATE_SHADER: {
            if (size < 20u) return FALSE;
            u32 code_len = rd_u32(p + 12);
            if (code_len == 0u || 16u + ALIGN_UP(code_len, 4u) != size) return FALSE;
            if (out) {
                out->last_shader_len = code_len;
                u32 n = code_len < 31u ? code_len : 31u;
                memcpy(out->last_shader_prefix, p + 16, n);
                out->last_shader_prefix[n] = '\0';
            }
            break;
        }
        case RHI_CMD_CREATE_PIPELINE: {
            if (size < 96u) return FALSE;
            u32 attr_count = rd_u32(p + 56);
            if (attr_count > RHI_MAX_VERTEX_ATTRS) return FALSE;
            if (96u + attr_count * 12u != size) return FALSE;
            break;
        }
        case RHI_CMD_CREATE_BIND_GROUP: {
            if (size < 44u) return FALSE;
            u32 entry_count = rd_u32(p + 20);
            if (entry_count == 0u || entry_count > RHI_MAX_BINDINGS) return FALSE;
            if (24u + entry_count * 20u != size) return FALSE;
            break;
        }
        case RHI_CMD_DESTROY_RESOURCE:
            if (size != 12u || rd_u32(p + 8) == 0u) return FALSE;
            break;
        case RHI_CMD_WRITE_BUFFER: {
            if (size < 24u) return FALSE;
            u32 dlen = rd_u32(p + 16);
            if (dlen == 0u || 20u + ALIGN_UP(dlen, 4u) != size) return FALSE;
            if (out) {
                out->last_write_buf_len = dlen;
                u32 n = dlen < 16u ? dlen : 16u;
                memcpy(out->last_write_buf_head, p + 20, n);
            }
            break;
        }
        case RHI_CMD_WRITE_TEXTURE: {
            if (size < 44u) return FALSE;
            u32 w    = rd_u32(p + 24);
            u32 h    = rd_u32(p + 28);
            u32 dlen = rd_u32(p + 36);
            if (w == 0u || h == 0u || dlen == 0u || 40u + ALIGN_UP(dlen, 4u) != size) return FALSE;
            if (out) {
                out->last_write_tex_w = w;
                out->last_write_tex_h = h;
            }
            break;
        }
        case RHI_CMD_BEGIN_PASS:
            if (size != 40u) return FALSE;
            if (out) {
                out->last_clear_r     = rd_f32(p + 20);
                out->last_clear_depth = rd_f32(p + 36);
            }
            break;
        case RHI_CMD_SET_PIPELINE:
            if (size != 12u || rd_u32(p + 8) == 0u) return FALSE;
            break;
        case RHI_CMD_SET_BIND_GROUP:
            if (size != 16u || rd_u32(p + 12) == 0u) return FALSE;
            break;
        case RHI_CMD_SET_VERTEX_BUFFER:
            if (size != 24u || rd_u32(p + 12) == 0u) return FALSE;
            break;
        case RHI_CMD_SET_INDEX_BUFFER:
            if (size != 24u || rd_u32(p + 8) == 0u) return FALSE;
            break;
        case RHI_CMD_DRAW:
            if (size != 24u) return FALSE;
            if (out) out->last_draw_verts = rd_u32(p + 8);
            break;
        case RHI_CMD_DRAW_INDEXED:
            if (size != 28u) return FALSE;
            if (out) {
                out->last_draw_idx_count = rd_u32(p + 8);
                out->last_draw_base_vert = rd_i32(p + 20);
            }
            break;
        case RHI_CMD_SET_VIEWPORT:
            if (size != 32u) return FALSE;
            break;
        case RHI_CMD_END_PASS:
        case RHI_CMD_PRESENT:
            if (size != 8u) return FALSE;
            break;
        default:
            return FALSE;
        }

        if (out) out->op_counts[op]++;
        off += size;
        seen++;
    }

    if (off != byte_len || seen != cmd_count) return FALSE;
    if (out) {
        out->cmd_count   = seen;
        out->total_bytes = byte_len;
    }
    return TRUE;
}

/* --- Test 1: Tüm komutların kodlanması ve bağımsız çözücüyle doğrulanması --- */

TEST(gpu_rhi_encode_decode_all_cmds) {
    static u8 mem[KB(128)];
    Arena arena;
    arena_init(&arena, mem, sizeof(mem));

    Rhi rhi;
    CHECK(rhi_init(&rhi, &arena, 32u, KB(64)));

    const char* wgsl = "@vertex fn vs_main() -> @builtin(position) vec4<f32> { return vec4<f32>(0.0); }";
    u32 wgsl_len = (u32)strlen(wgsl); /* 79 bayt -> 4-bayt hizalama test edilir */

    RhiShader   sh  = rhi_create_shader(&rhi, wgsl, wgsl_len);
    RhiBuffer   vb  = rhi_create_buffer(&rhi, 128u, RHI_BUF_VERTEX | RHI_BUF_COPY_DST);
    RhiBuffer   ib  = rhi_create_buffer(&rhi, 64u, RHI_BUF_INDEX | RHI_BUF_COPY_DST);
    RhiBuffer   ub  = rhi_create_buffer(&rhi, 64u, RHI_BUF_UNIFORM | RHI_BUF_COPY_DST);

    RhiTextureDesc tdesc = {
        .width = 4u, .height = 4u, .mip_levels = 1u,
        .format = RHI_FMT_RGBA8_UNORM, .usage = RHI_TEX_SAMPLED | RHI_TEX_COPY_DST
    };
    RhiTexture tex = rhi_create_texture(&rhi, &tdesc);

    RhiSamplerDesc sdesc = {
        .address_u = RHI_ADDR_REPEAT, .address_v = RHI_ADDR_CLAMP,
        .mag_filter = RHI_FILTER_LINEAR, .min_filter = RHI_FILTER_NEAREST,
        .mipmap_filter = RHI_FILTER_NEAREST
    };
    RhiSampler samp = rhi_create_sampler(&rhi, &sdesc);

    RhiPipelineDesc pdesc = {
        .vertex_shader   = sh,
        .fragment_shader = sh,
        .vs_entry        = "vs_main",
        .fs_entry        = "fs_main",
        .vertex_stride   = 12u,
        .attrs = {
            { .shader_location = 0u, .offset = 0u, .format = RHI_VFMT_FLOAT32X3 }
        },
        .attr_count    = 1u,
        .topology      = RHI_TOPOLOGY_TRIANGLE_LIST,
        .cull_mode     = RHI_CULL_BACK,
        .front_face    = RHI_FRONT_CCW,
        .color_format  = RHI_FMT_RGBA8_UNORM,
        .blend_mode    = RHI_BLEND_ALPHA,
        .depth_format  = RHI_FMT_DEPTH24_PLUS,
        .depth_test    = TRUE,
        .depth_write   = TRUE,
        .depth_compare = RHI_CMP_LESS
    };
    RhiPipeline pipe = rhi_create_pipeline(&rhi, &pdesc);

    RhiBindGroupDesc bgdesc = {
        .pipeline    = pipe,
        .group_index = 0u,
        .entries = {
            { .binding = 0u, .kind = RHI_BIND_BUFFER,  .resource = ub,   .offset = 0u, .size = 64u },
            { .binding = 1u, .kind = RHI_BIND_TEXTURE, .resource = tex,  .offset = 0u, .size = 0u },
            { .binding = 2u, .kind = RHI_BIND_SAMPLER, .resource = samp, .offset = 0u, .size = 0u }
        },
        .entry_count = 3u
    };
    RhiBindGroup bg = rhi_create_bind_group(&rhi, &bgdesc);

    CHECK(sh != RHI_INVALID_HANDLE);
    CHECK(vb != RHI_INVALID_HANDLE);
    CHECK(ib != RHI_INVALID_HANDLE);
    CHECK(ub != RHI_INVALID_HANDLE);
    CHECK(tex != RHI_INVALID_HANDLE);
    CHECK(samp != RHI_INVALID_HANDLE);
    CHECK(pipe != RHI_INVALID_HANDLE);
    CHECK(bg != RHI_INVALID_HANDLE);

    u8 raw_data[13] = { 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13 }; /* 4'ün katı olmayan uzunluk */
    CHECK(rhi_write_buffer(&rhi, vb, 0u, raw_data, 13u));

    u32 pixels[16];
    for (u32 i = 0u; i < 16u; i++) pixels[i] = 0xFF00FF00u + i;
    CHECK(rhi_write_texture(&rhi, tex, 0u, 0u, 0u, 4u, 4u, 16u, pixels, sizeof(pixels)));

    RhiPassDesc pass = {
        .color_texture   = RHI_INVALID_HANDLE,
        .depth_texture   = RHI_INVALID_HANDLE,
        .clear_color     = TRUE,
        .clear_r         = 0.25f,
        .clear_g         = 0.50f,
        .clear_b         = 0.75f,
        .clear_a         = 1.00f,
        .clear_depth     = TRUE,
        .clear_depth_val = 0.0f
    };
    CHECK(rhi_begin_pass(&rhi, &pass));
    CHECK(rhi_set_viewport(&rhi, 0.0f, 0.0f, 640.0f, 360.0f, 0.0f, 1.0f));
    CHECK(rhi_set_pipeline(&rhi, pipe));
    CHECK(rhi_set_bind_group(&rhi, 0u, bg));
    CHECK(rhi_set_vertex_buffer(&rhi, 0u, vb, 0u, 0u));
    CHECK(rhi_set_index_buffer(&rhi, ib, RHI_INDEX_UINT16, 0u, 0u));
    CHECK(rhi_draw(&rhi, 6u, 1u, 0u, 0u));
    CHECK(rhi_draw_indexed(&rhi, 12u, 2u, 0u, -3, 0u));
    CHECK(rhi_end_pass(&rhi));
    CHECK(rhi_destroy_resource(&rhi, samp));
    CHECK(rhi_present(&rhi));

    DecodedStreamSummary sum;
    CHECK(decode_and_verify_stream(rhi_stream_data(&rhi), rhi_stream_bytes(&rhi), &sum));
    CHECK(sum.cmd_count == 21u);
    CHECK(sum.op_counts[RHI_CMD_CREATE_SHADER]     == 1u);
    CHECK(sum.op_counts[RHI_CMD_CREATE_BUFFER]     == 3u);
    CHECK(sum.op_counts[RHI_CMD_CREATE_TEXTURE]    == 1u);
    CHECK(sum.op_counts[RHI_CMD_CREATE_SAMPLER]    == 1u);
    CHECK(sum.op_counts[RHI_CMD_CREATE_PIPELINE]   == 1u);
    CHECK(sum.op_counts[RHI_CMD_CREATE_BIND_GROUP] == 1u);
    CHECK(sum.op_counts[RHI_CMD_WRITE_BUFFER]      == 1u);
    CHECK(sum.op_counts[RHI_CMD_WRITE_TEXTURE]     == 1u);
    CHECK(sum.op_counts[RHI_CMD_BEGIN_PASS]        == 1u);
    CHECK(sum.op_counts[RHI_CMD_SET_VIEWPORT]      == 1u);
    CHECK(sum.op_counts[RHI_CMD_SET_PIPELINE]      == 1u);
    CHECK(sum.op_counts[RHI_CMD_SET_BIND_GROUP]    == 1u);
    CHECK(sum.op_counts[RHI_CMD_SET_VERTEX_BUFFER] == 1u);
    CHECK(sum.op_counts[RHI_CMD_SET_INDEX_BUFFER]  == 1u);
    CHECK(sum.op_counts[RHI_CMD_DRAW]              == 1u);
    CHECK(sum.op_counts[RHI_CMD_DRAW_INDEXED]      == 1u);
    CHECK(sum.op_counts[RHI_CMD_END_PASS]          == 1u);
    CHECK(sum.op_counts[RHI_CMD_DESTROY_RESOURCE]  == 1u);
    CHECK(sum.op_counts[RHI_CMD_PRESENT]           == 1u);

    CHECK(sum.last_shader_len == wgsl_len);
    CHECK(strncmp(sum.last_shader_prefix, "@vertex fn vs_main", 18) == 0);
    CHECK(sum.last_write_buf_len == 13u);
    CHECK(sum.last_write_buf_head[0] == 1u && sum.last_write_buf_head[12] == 13u);
    CHECK(sum.last_write_tex_w == 4u && sum.last_write_tex_h == 4u);
    CHECK(sum.last_draw_verts == 6u);
    CHECK(sum.last_draw_idx_count == 12u);
    CHECK(sum.last_draw_base_vert == -3);
    CHECK_NEAR(sum.last_clear_r, 0.25f, 1e-6);
    CHECK_NEAR(sum.last_clear_depth, 0.0f, 1e-6);
}

/* --- Test 2: Tutamaç yaşam döngüsü, nesil sayacı ve tip denetimi --- */

TEST(gpu_rhi_handle_generation_and_type_safety) {
    static u8 mem[KB(64)];
    Arena arena;
    arena_init(&arena, mem, sizeof(mem));

    Rhi rhi;
    /* Yalnız 2 kaynak yuvası olan RHI */
    CHECK(rhi_init(&rhi, &arena, 2u, KB(16)));

    RhiBuffer b1 = rhi_create_buffer(&rhi, 32u, RHI_BUF_VERTEX);
    RhiBuffer b2 = rhi_create_buffer(&rhi, 64u, RHI_BUF_UNIFORM);
    /* 3. kaynak kapasite sınırından dolayı reddedilmeli */
    RhiBuffer b3 = rhi_create_buffer(&rhi, 64u, RHI_BUF_UNIFORM);
    CHECK(b1 != RHI_INVALID_HANDLE);
    CHECK(b2 != RHI_INVALID_HANDLE);
    CHECK(b3 == RHI_INVALID_HANDLE);
    CHECK(rhi.live_count == 2u);

    /* Yanlış kaynak tipi olarak sorgulama reddedilmeli */
    CHECK(rhi_handle_valid(&rhi, b1, RHI_RES_BUFFER));
    CHECK(!rhi_handle_valid(&rhi, b1, RHI_RES_TEXTURE));
    CHECK(!rhi_handle_valid(&rhi, b1, RHI_RES_SHADER));

    /* b1'i yok et -> yuva 1 boşalır, eski tutamaç geçersiz olur */
    CHECK(rhi_destroy_resource(&rhi, b1));
    CHECK(!rhi_handle_valid(&rhi, b1, RHI_RES_BUFFER));
    CHECK(!rhi_destroy_resource(&rhi, b1)); /* Çifte yok etme reddedilmeli */

    /* Aynı yuvayı yeniden tahsis et -> nesil (generation) artmalı */
    RhiBuffer b1_reused = rhi_create_buffer(&rhi, 128u, RHI_BUF_VERTEX);
    CHECK(b1_reused != RHI_INVALID_HANDLE);
    CHECK(RHI_HANDLE_INDEX(b1_reused) == RHI_HANDLE_INDEX(b1));
    CHECK(RHI_HANDLE_GEN(b1_reused) == RHI_HANDLE_GEN(b1) + 1u);
    CHECK(!rhi_handle_valid(&rhi, b1, RHI_RES_BUFFER));
    CHECK(rhi_handle_valid(&rhi, b1_reused, RHI_RES_BUFFER));
}

/* --- Test 3: Sınır denetimleri ve geçiş (pass) durum makinesi --- */

TEST(gpu_rhi_bounds_and_state_validation) {
    static u8 mem[KB(64)];
    Arena arena;
    arena_init(&arena, mem, sizeof(mem));

    Rhi rhi;
    CHECK(rhi_init(&rhi, &arena, 16u, KB(16)));

    RhiBuffer buf = rhi_create_buffer(&rhi, 32u, RHI_BUF_VERTEX | RHI_BUF_COPY_DST);
    u8 data[32] = { 0 };

    /* Buffer sınır aşımı, hizasız ofset ve u32 taşması reddedilmeli */
    CHECK(!rhi_write_buffer(&rhi, buf, 0u, data, 33u));
    CHECK(!rhi_write_buffer(&rhi, buf, 16u, data, 20u));
    CHECK(!rhi_write_buffer(&rhi, buf, 2u, data, 4u)); /* 4-bayt hizasız ofset */
    CHECK(!rhi_write_buffer(&rhi, buf, 0xFFFFFFF0u, data, 32u));
    CHECK(rhi_write_buffer(&rhi, buf, 16u, data, 16u));

    /* Doku sınır aşımı ve geçersiz mip seviyesi reddedilmeli */
    RhiTextureDesc tdesc = {
        .width = 8u, .height = 8u, .mip_levels = 2u,
        .format = RHI_FMT_RGBA8_UNORM, .usage = RHI_TEX_SAMPLED | RHI_TEX_COPY_DST
    };
    RhiTexture tex = rhi_create_texture(&rhi, &tdesc);
    u32 texels[64] = { 0 };
    CHECK(!rhi_write_texture(&rhi, tex, 2u, 0u, 0u, 4u, 4u, 16u, texels, 64u)); /* mip 2 yok */
    CHECK(!rhi_write_texture(&rhi, tex, 1u, 0u, 0u, 8u, 8u, 32u, texels, 256u)); /* mip 1 boyutu 4x4 */
    CHECK(!rhi_write_texture(&rhi, tex, 0u, 0u, 0u, 8u, 8u, 16u, texels, 256u)); /* bytes_per_row < 8*4 */
    CHECK(rhi_write_texture(&rhi, tex, 1u, 0u, 0u, 4u, 4u, 16u, texels, 64u));

    /* Pass dışında çizim komutları reddedilmeli */
    CHECK(!rhi_draw(&rhi, 3u, 1u, 0u, 0u));
    CHECK(!rhi_end_pass(&rhi));

    RhiPassDesc pass = { .clear_color = TRUE, .clear_a = 1.0f };
    CHECK(rhi_begin_pass(&rhi, &pass));
    CHECK(!rhi_begin_pass(&rhi, &pass)); /* İç içe pass yasak */
    CHECK(!rhi_present(&rhi));           /* Pass açıkken present yasak */
    CHECK(rhi_end_pass(&rhi));
    CHECK(rhi_present(&rhi));

    /* Reddedilen tüm hatalı çağrılara rağmen akış %100 geçerli olmalı */
    CHECK(decode_and_verify_stream(rhi_stream_data(&rhi), rhi_stream_bytes(&rhi), NULL));
}

/* --- Test 4: Komut akışı kapasite taşması ve kanarya (bellek taşması) testi --- */

TEST(gpu_rhi_stream_overflow_safety) {
    static u8 mem[KB(16)];
    Arena arena;
    arena_init(&arena, mem, sizeof(mem));

    /* Yalnız 64 baytlık çok küçük komut akışı:
     * 16B başlık + 20B create_buffer = 36B (kalan 28B). */
    Rhi rhi;
    CHECK(rhi_init(&rhi, &arena, 16u, 64u));

    /* Akış tamponunun hemen ardına kanarya baytları yerleştir */
    u8* canary = (u8*)arena_push(&arena, 16u, 1u);
    for (u32 i = 0u; i < 16u; i++) canary[i] = 0xA5u;

    RhiBuffer b1 = rhi_create_buffer(&rhi, 64u, RHI_BUF_VERTEX | RHI_BUF_COPY_DST);
    CHECK(b1 != RHI_INVALID_HANDLE);
    CHECK(rhi_stream_bytes(&rhi) == 36u);
    CHECK(!rhi.overflow);

    /* 28B kalan alana 32B'lik create_texture sığmaz -> taşma bayrağı kalkmalı ve yuva sızmamalı */
    RhiTextureDesc tdesc = {
        .width = 16u, .height = 16u, .mip_levels = 1u,
        .format = RHI_FMT_RGBA8_UNORM, .usage = RHI_TEX_SAMPLED
    };
    RhiTexture tex = rhi_create_texture(&rhi, &tdesc);
    CHECK(tex == RHI_INVALID_HANDLE);
    CHECK(rhi.overflow);
    CHECK(rhi.live_count == 1u);
    CHECK(rhi_stream_bytes(&rhi) == 36u);

    /* 20B'lik ikinci create_buffer ise kalan 28B'ye tam sığar (36 + 20 = 56B <= 64B) */
    RhiBuffer b2 = rhi_create_buffer(&rhi, 32u, RHI_BUF_INDEX);
    CHECK(b2 != RHI_INVALID_HANDLE);
    CHECK(rhi_stream_bytes(&rhi) == 56u);

    /* Kanarya baytları hiç bozulmamış olmalı ve akış çözücüden geçmeli */
    if (rhi.stream_buf + rhi.stream_cap == canary) {
        for (u32 i = 0u; i < 16u; i++) CHECK(canary[i] == 0xA5u);
    }
    DecodedStreamSummary sum;
    CHECK(decode_and_verify_stream(rhi_stream_data(&rhi), rhi_stream_bytes(&rhi), &sum));
    CHECK(sum.cmd_count == 2u);
}

/* --- Test 5: Hata halkası (RhiErrorRing) taşma, sarma ve kırpma testleri --- */

TEST(gpu_rhi_error_ring) {
    static u8 mem[KB(32)];
    Arena arena;
    arena_init(&arena, mem, sizeof(mem));

    Rhi rhi;
    CHECK(rhi_init(&rhi, &arena, 16u, KB(8)));
    rhi_set_active(&rhi);

    /* Halka kapasitesi (16) kadar hata it */
    for (u32 i = 0u; i < RHI_ERROR_RING_CAP; i++) {
        const char* m = "WGSL syntax error";
        CHECK(rhi_push_error(&rhi, RHI_ERR_WGSL_COMPILE, i + 1u, i + 10u, 4u, m, (u32)strlen(m)));
    }
    /* 17. hata düşürülmeli (dropped == 1) */
    CHECK(!rhi_push_error(&rhi, RHI_ERR_VALIDATION, 99u, 0u, 0u, "overflow", 8u));
    CHECK(rhi.errors.dropped == 1u);

    /* İlk 8 hatayı çek */
    for (u32 i = 0u; i < 8u; i++) {
        RhiErrorEntry e;
        CHECK(rhi_pop_error(&rhi, &e));
        CHECK(e.kind == RHI_ERR_WGSL_COMPILE);
        CHECK(e.handle == i + 1u);
        CHECK(e.line == i + 10u);
        CHECK(e.col == 4u);
        CHECK(strcmp(e.msg, "WGSL syntax error") == 0);
    }

    /* Çok uzun bir hata mesajı it (RHI_ERROR_MSG_MAX - 1 sınırında kırpılmalı) */
    char long_msg[400];
    memset(long_msg, 'X', sizeof(long_msg));
    CHECK(engine_gpu_push_error(RHI_ERR_VALIDATION, 0x1234u, 7u, 9u, long_msg, sizeof(long_msg)));

    /* Kalan 8 eski hatayı çek, ardından uzun mesajlı hatayı doğrula */
    for (u32 i = 0u; i < 8u; i++) {
        RhiErrorEntry e;
        CHECK(rhi_pop_error(&rhi, &e));
    }
    RhiErrorEntry last;
    CHECK(rhi_pop_error(&rhi, &last));
    CHECK(last.kind == RHI_ERR_VALIDATION);
    CHECK(last.handle == 0x1234u);
    CHECK(last.msg_len == RHI_ERROR_MSG_MAX - 1u);
    CHECK(last.msg[RHI_ERROR_MSG_MAX - 1u] == '\0');
    CHECK(!rhi_pop_error(&rhi, &last));
}

/* --- Test 6: engine_gpu_test_frame() uçtan uca komut akışı doğrulaması --- */

TEST(gpu_rhi_demo_test_frame_stream) {
    /* 1. kare: tüm kaynakların oluşturulması + ilk çizim */
    u32 bytes_f1 = engine_gpu_test_frame(1000.0, 0.016f);
    CHECK(bytes_f1 > 16384u); /* 64x64 RGBA8 doku + shader + geometri + çizim komutları */
    CHECK(engine_gpu_cmd_bytes() == bytes_f1);
    CHECK(engine_gpu_cmd_stream() != NULL);

    DecodedStreamSummary s1;
    CHECK(decode_and_verify_stream(engine_gpu_cmd_stream(), bytes_f1, &s1));
    CHECK(s1.op_counts[RHI_CMD_CREATE_SHADER]     == 2u);
    CHECK(s1.op_counts[RHI_CMD_CREATE_PIPELINE]   == 2u);
    CHECK(s1.op_counts[RHI_CMD_CREATE_BIND_GROUP] == 2u);
    CHECK(s1.op_counts[RHI_CMD_CREATE_TEXTURE]    == 2u); /* derinlik + dama dokusu */
    CHECK(s1.op_counts[RHI_CMD_CREATE_SAMPLER]    == 1u);
    CHECK(s1.op_counts[RHI_CMD_WRITE_TEXTURE]     == 1u);
    CHECK(s1.op_counts[RHI_CMD_BEGIN_PASS]        == 1u);
    CHECK(s1.op_counts[RHI_CMD_DRAW_INDEXED]      == 2u); /* üçgen (3) + küp (36) */
    CHECK(s1.op_counts[RHI_CMD_END_PASS]          == 1u);
    CHECK(s1.op_counts[RHI_CMD_PRESENT]           == 1u);
    CHECK(s1.last_draw_idx_count == 36u);

    /* 2. kare: yalnız uniform güncellemesi + render geçişi (yeni kaynak oluşturulmaz) */
    u32 bytes_f2 = engine_gpu_test_frame(1016.0, 0.016f);
    CHECK(bytes_f2 > sizeof(RhiStreamHeader) && bytes_f2 < 1024u);

    DecodedStreamSummary s2;
    CHECK(decode_and_verify_stream(engine_gpu_cmd_stream(), bytes_f2, &s2));
    CHECK(s2.op_counts[RHI_CMD_CREATE_SHADER]   == 0u);
    CHECK(s2.op_counts[RHI_CMD_CREATE_PIPELINE] == 0u);
    CHECK(s2.op_counts[RHI_CMD_WRITE_BUFFER]    == 2u); /* tri_ub + cube_ub */
    CHECK(s2.op_counts[RHI_CMD_BEGIN_PASS]      == 1u);
    CHECK(s2.op_counts[RHI_CMD_DRAW_INDEXED]    == 2u);
    CHECK(s2.op_counts[RHI_CMD_END_PASS]        == 1u);
    CHECK(s2.op_counts[RHI_CMD_PRESENT]         == 1u);

    /* Kararlı durum (steady-state) 1000 kare kodlama + çözme ölçümü */
    f64 t0 = plat_time_ms();
    for (u32 f = 0u; f < 1000u; f++) {
        u32 nb = engine_gpu_test_frame(1032.0 + (f64)f * 16.0, 0.016f);
        CHECK(nb == bytes_f2);
        CHECK(decode_and_verify_stream(engine_gpu_cmd_stream(), nb, NULL));
    }
    f64 elapsed_ms = plat_time_ms() - t0;
    printf("  [bench M15_B] gpu_test akis: kare1=%u B (%u komut), kare2+=%u B (%u komut), "
           "1000 kare kodla+coz=%.3f ms (%.3f us/kare)\n",
           bytes_f1, s1.cmd_count, bytes_f2, s2.cmd_count,
           elapsed_ms, elapsed_ms);
}

/* --- Test 7: Bozuk/düşmanca akış baytlarının çözücü tarafından reddi ve CPU yedek durumu --- */

TEST(gpu_rhi_corrupted_stream_and_availability) {
    /* Varsayılan olarak WebGPU cihazı yokken engine_gpu_available() == 0 olmalı */
    engine_gpu_set_available(FALSE);
    CHECK(engine_gpu_available() == FALSE);
    engine_gpu_set_available(TRUE);
    CHECK(engine_gpu_available() == TRUE);
    engine_gpu_set_available(FALSE);
    CHECK(engine_gpu_available() == FALSE);

    u8 bad[64];
    memset(bad, 0, sizeof(bad));

    /* Çok kısa veya hizasız akış */
    CHECK(!decode_and_verify_stream(NULL, 16u, NULL));
    CHECK(!decode_and_verify_stream(bad, 12u, NULL));
    CHECK(!decode_and_verify_stream(bad, 18u, NULL));

    /* Geçerli başlık + 1 END_PASS (toplam 24 bayt) kur, sonra alanları bozarak dene */
    u32 magic = 0x4D475055u, ver = 1u, blen = 24u, cnt = 1u;
    u32 op = RHI_CMD_END_PASS, csize = 8u;
    memcpy(bad + 0,  &magic, 4);
    memcpy(bad + 4,  &ver,   4);
    memcpy(bad + 8,  &blen,  4);
    memcpy(bad + 12, &cnt,   4);
    memcpy(bad + 16, &op,    4);
    memcpy(bad + 20, &csize, 4);
    CHECK(decode_and_verify_stream(bad, 24u, NULL));

    /* Yanlış magic */
    u32 bad_magic = 0x12345678u;
    memcpy(bad + 0, &bad_magic, 4);
    CHECK(!decode_and_verify_stream(bad, 24u, NULL));
    memcpy(bad + 0, &magic, 4);

    /* Yanlış sürüm */
    u32 bad_ver = 2u;
    memcpy(bad + 4, &bad_ver, 4);
    CHECK(!decode_and_verify_stream(bad, 24u, NULL));
    memcpy(bad + 4, &ver, 4);

    /* Komut sayısı uyuşmazlığı */
    u32 bad_cnt = 2u;
    memcpy(bad + 12, &bad_cnt, 4);
    CHECK(!decode_and_verify_stream(bad, 24u, NULL));
    memcpy(bad + 12, &cnt, 4);

    /* Taşan komut boyutu */
    u32 bad_csize = 16u;
    memcpy(bad + 20, &bad_csize, 4);
    CHECK(!decode_and_verify_stream(bad, 24u, NULL));
    memcpy(bad + 20, &csize, 4);

    /* Geçersiz opcode */
    u32 bad_op = 99u;
    memcpy(bad + 16, &bad_op, 4);
    CHECK(!decode_and_verify_stream(bad, 24u, NULL));
}

