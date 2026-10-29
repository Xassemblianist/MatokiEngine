/* gpu/rhi.c — RHI tutamaç tablosu, ikili komut akışı kodlayıcısı ve hata halkası (D-108).
 * Tüm bellek Arena'dan ayrılır (malloc/libc yok). Her komut akış tamponuna 4-bayt hizalı
 * olarak yazılır; kapasite aşımında yarım komut yazılmaz ve akışın önceki kısmı geçerli kalır.
 */
#include "gpu/rhi.h"
#include "core/log.h"
#include "thread/atomic.h"
#include "plat/platform.h"

static Rhi*         g_active_rhi;
static b32          g_gpu_available;
static u32          g_gpu_viewport_w = 1920u;
static u32          g_gpu_viewport_h = 1080u;
static u32          g_gpu_frame_time_us;
static u32          g_gpu_sky_time_us;
static b32          g_gpu_has_timestamp;
static RhiErrorRing g_fallback_errors = { 0, 0, RHI_ERROR_RING_CAP, 0, { { 0 } } };

static void copy_entry_name(char dst[16], const char* src, const char* fallback) {
    const char* s = (src && src[0] != '\0') ? src : fallback;
    u32 i = 0;
    for (; i < 15u && s[i] != '\0'; i++) {
        dst[i] = s[i];
    }
    for (; i < 16u; i++) {
        dst[i] = '\0';
    }
}

static void rhi_sync_header(Rhi* rhi) {
    if (!rhi || !rhi->stream_buf || rhi->stream_cap < sizeof(RhiStreamHeader)) return;
    RhiStreamHeader* hdr = (RhiStreamHeader*)rhi->stream_buf;
    hdr->magic     = RHI_STREAM_MAGIC;
    hdr->version   = RHI_STREAM_VERSION;
    hdr->byte_len  = rhi->stream_used;
    hdr->cmd_count = rhi->cmd_count;
}

static void* rhi_alloc_cmd(Rhi* rhi, RhiCmdOp op, u32 total_size) {
    if (!rhi || !rhi->stream_buf) return NULL;
    if (total_size < sizeof(RhiCmdHeader) || (total_size & 3u) != 0u) {
        rhi->dropped_cmds++;
        return NULL;
    }
    if (total_size > rhi->stream_cap || rhi->stream_used > rhi->stream_cap - total_size) {
        rhi->overflow = TRUE;
        rhi->dropped_cmds++;
        return NULL;
    }
    u8* ptr = rhi->stream_buf + rhi->stream_used;
    __builtin_memset(ptr, 0, total_size);
    RhiCmdHeader* hdr = (RhiCmdHeader*)ptr;
    hdr->opcode = (u32)op;
    hdr->size   = total_size;
    rhi->stream_used += total_size;
    rhi->cmd_count++;
    rhi_sync_header(rhi);
    return ptr;
}

static b32 rhi_find_free_slot(const Rhi* rhi, u32* out_idx) {
    if (!rhi || !rhi->slots) return FALSE;
    for (u32 i = 1u; i <= rhi->max_resources; i++) {
        if (!rhi->slots[i].alive) {
            *out_idx = i;
            return TRUE;
        }
    }
    return FALSE;
}

static RhiHandle rhi_commit_slot(Rhi* rhi, u32 idx, RhiResourceType type) {
    RhiResourceSlot* s = &rhi->slots[idx];
    u8 next_gen = (u8)(s->gen + 1u);
    if (next_gen == 0u) next_gen = 1u;
    __builtin_memset(s, 0, sizeof(*s));
    s->type  = type;
    s->gen   = next_gen;
    s->alive = TRUE;
    rhi->live_count++;
    return RHI_HANDLE_MAKE(type, next_gen, idx);
}

b32 rhi_init(Rhi* rhi, Arena* arena, u32 max_resources, u32 stream_cap) {
    if (!rhi || !arena) return FALSE;
    if (max_resources == 0u) max_resources = RHI_DEFAULT_MAX_RES;
    if (max_resources > 0xFFFEu) max_resources = 0xFFFEu;
    if (stream_cap < sizeof(RhiStreamHeader)) return FALSE;
    stream_cap = (u32)ALIGN_UP(stream_cap, 4u);

    usize need_slots  = sizeof(RhiResourceSlot) * (usize)(max_resources + 1u);
    usize need_stream = (usize)stream_cap;
    if (arena->used + need_slots + need_stream + 32u > arena->cap) {
        return FALSE;
    }

    __builtin_memset(rhi, 0, sizeof(*rhi));
    rhi->slots         = ARENA_PUSH_ARRAY(arena, RhiResourceSlot, max_resources + 1u);
    rhi->max_resources = max_resources;
    rhi->stream_buf    = (u8*)arena_push(arena, stream_cap, 16u);
    rhi->stream_cap    = stream_cap;
    rhi->errors.capacity = RHI_ERROR_RING_CAP;

    rhi_begin_frame(rhi);
    if (!g_active_rhi) {
        g_active_rhi = rhi;
    }
    return TRUE;
}

void rhi_set_active(Rhi* rhi) {
    g_active_rhi = rhi;
}

Rhi* rhi_active(void) {
    return g_active_rhi;
}

void rhi_begin_frame(Rhi* rhi) {
    if (!rhi || !rhi->stream_buf || rhi->stream_cap < sizeof(RhiStreamHeader)) return;
    rhi->stream_used     = (u32)sizeof(RhiStreamHeader);
    rhi->cmd_count       = 0u;
    rhi->in_pass         = FALSE;
    rhi->in_compute_pass = FALSE;
    rhi->overflow        = FALSE;
    rhi->dropped_cmds    = 0u;
    rhi_sync_header(rhi);
}

const u8* rhi_stream_data(const Rhi* rhi) {
    return rhi ? rhi->stream_buf : NULL;
}

u32 rhi_stream_bytes(const Rhi* rhi) {
    return rhi ? rhi->stream_used : 0u;
}

u32 rhi_stream_cmd_count(const Rhi* rhi) {
    return rhi ? rhi->cmd_count : 0u;
}

b32 rhi_handle_valid(const Rhi* rhi, RhiHandle h, RhiResourceType expected_type) {
    if (!rhi || !rhi->slots || h == RHI_INVALID_HANDLE) return FALSE;
    u32 idx = RHI_HANDLE_INDEX(h);
    u32 gen = RHI_HANDLE_GEN(h);
    RhiResourceType type = RHI_HANDLE_TYPE(h);
    if (idx == 0u || idx > rhi->max_resources || gen == 0u) return FALSE;
    const RhiResourceSlot* s = &rhi->slots[idx];
    if (!s->alive || s->gen != gen || s->type != type) return FALSE;
    if (expected_type != RHI_RES_NONE && type != expected_type) return FALSE;
    return TRUE;
}

const RhiResourceSlot* rhi_resource_slot(const Rhi* rhi, RhiHandle h) {
    if (!rhi_handle_valid(rhi, h, RHI_RES_NONE)) return NULL;
    return &rhi->slots[RHI_HANDLE_INDEX(h)];
}

/* --- Kaynak Oluşturma ve Yok Etme --- */

RhiBuffer rhi_create_buffer(Rhi* rhi, u32 byte_size, u32 usage) {
    if (!rhi || byte_size == 0u || usage == 0u) {
        if (rhi) rhi->dropped_cmds++;
        return RHI_INVALID_HANDLE;
    }
    u32 idx = 0u;
    if (!rhi_find_free_slot(rhi, &idx)) {
        rhi->dropped_cmds++;
        return RHI_INVALID_HANDLE;
    }
    RhiCmdCreateBuffer* cmd = (RhiCmdCreateBuffer*)rhi_alloc_cmd(
        rhi, RHI_CMD_CREATE_BUFFER, (u32)sizeof(RhiCmdCreateBuffer));
    if (!cmd) return RHI_INVALID_HANDLE;

    RhiBuffer h = rhi_commit_slot(rhi, idx, RHI_RES_BUFFER);
    rhi->slots[idx].byte_size = byte_size;
    rhi->slots[idx].usage     = usage;

    cmd->handle    = h;
    cmd->byte_size = byte_size;
    cmd->usage     = usage;
    return h;
}

RhiTexture rhi_create_texture(Rhi* rhi, const RhiTextureDesc* desc) {
    if (!rhi || !desc || desc->width == 0u || desc->height == 0u ||
        desc->format == RHI_FMT_SURFACE_DEFAULT || desc->usage == 0u) {
        if (rhi) rhi->dropped_cmds++;
        return RHI_INVALID_HANDLE;
    }
    u32 idx = 0u;
    if (!rhi_find_free_slot(rhi, &idx)) {
        rhi->dropped_cmds++;
        return RHI_INVALID_HANDLE;
    }
    RhiCmdCreateTexture* cmd = (RhiCmdCreateTexture*)rhi_alloc_cmd(
        rhi, RHI_CMD_CREATE_TEXTURE, (u32)sizeof(RhiCmdCreateTexture));
    if (!cmd) return RHI_INVALID_HANDLE;

    u32 mips = desc->mip_levels > 0u ? desc->mip_levels : 1u;
    RhiTexture h = rhi_commit_slot(rhi, idx, RHI_RES_TEXTURE);
    rhi->slots[idx].width      = desc->width;
    rhi->slots[idx].height     = desc->height;
    rhi->slots[idx].mip_levels = mips;
    rhi->slots[idx].format     = (u32)desc->format;
    rhi->slots[idx].usage      = desc->usage;

    cmd->handle     = h;
    cmd->width      = desc->width;
    cmd->height     = desc->height;
    cmd->mip_levels = mips;
    cmd->format     = (u32)desc->format;
    cmd->usage      = desc->usage;
    return h;
}

RhiSampler rhi_create_sampler(Rhi* rhi, const RhiSamplerDesc* desc) {
    if (!rhi || !desc) {
        if (rhi) rhi->dropped_cmds++;
        return RHI_INVALID_HANDLE;
    }
    u32 idx = 0u;
    if (!rhi_find_free_slot(rhi, &idx)) {
        rhi->dropped_cmds++;
        return RHI_INVALID_HANDLE;
    }
    RhiCmdCreateSampler* cmd = (RhiCmdCreateSampler*)rhi_alloc_cmd(
        rhi, RHI_CMD_CREATE_SAMPLER, (u32)sizeof(RhiCmdCreateSampler));
    if (!cmd) return RHI_INVALID_HANDLE;

    RhiSampler h = rhi_commit_slot(rhi, idx, RHI_RES_SAMPLER);
    cmd->handle        = h;
    cmd->address_u     = (u32)desc->address_u;
    cmd->address_v     = (u32)desc->address_v;
    cmd->mag_filter    = (u32)desc->mag_filter;
    cmd->min_filter    = (u32)desc->min_filter;
    cmd->mipmap_filter = (u32)desc->mipmap_filter;
    return h;
}

RhiShader rhi_create_shader(Rhi* rhi, const char* wgsl_code, u32 code_len) {
    if (!rhi || !wgsl_code || code_len == 0u || code_len > MB(16)) {
        if (rhi) rhi->dropped_cmds++;
        return RHI_INVALID_HANDLE;
    }
    u32 idx = 0u;
    if (!rhi_find_free_slot(rhi, &idx)) {
        rhi->dropped_cmds++;
        return RHI_INVALID_HANDLE;
    }
    u32 aligned_len = (u32)ALIGN_UP(code_len, 4u);
    u32 total_size  = (u32)sizeof(RhiCmdCreateShader) + aligned_len;
    RhiCmdCreateShader* cmd = (RhiCmdCreateShader*)rhi_alloc_cmd(
        rhi, RHI_CMD_CREATE_SHADER, total_size);
    if (!cmd) return RHI_INVALID_HANDLE;

    RhiShader h = rhi_commit_slot(rhi, idx, RHI_RES_SHADER);
    rhi->slots[idx].byte_size = code_len;

    cmd->handle   = h;
    cmd->code_len = code_len;
    __builtin_memcpy((u8*)cmd + sizeof(RhiCmdCreateShader), wgsl_code, code_len);
    return h;
}

RhiPipeline rhi_create_pipeline(Rhi* rhi, const RhiPipelineDesc* desc) {
    if (!rhi || !desc || desc->attr_count > RHI_MAX_VERTEX_ATTRS) {
        if (rhi) rhi->dropped_cmds++;
        return RHI_INVALID_HANDLE;
    }
    if (!rhi_handle_valid(rhi, desc->vertex_shader, RHI_RES_SHADER)) {
        rhi->dropped_cmds++;
        return RHI_INVALID_HANDLE;
    }
    if (desc->fragment_shader != RHI_INVALID_HANDLE &&
        !rhi_handle_valid(rhi, desc->fragment_shader, RHI_RES_SHADER)) {
        rhi->dropped_cmds++;
        return RHI_INVALID_HANDLE;
    }
    u32 idx = 0u;
    if (!rhi_find_free_slot(rhi, &idx)) {
        rhi->dropped_cmds++;
        return RHI_INVALID_HANDLE;
    }
    u32 attr_bytes = desc->attr_count * (u32)sizeof(RhiVertexAttr);
    u32 total_size = (u32)sizeof(RhiCmdCreatePipeline) + attr_bytes;
    RhiCmdCreatePipeline* cmd = (RhiCmdCreatePipeline*)rhi_alloc_cmd(
        rhi, RHI_CMD_CREATE_PIPELINE, total_size);
    if (!cmd) return RHI_INVALID_HANDLE;

    RhiPipeline h = rhi_commit_slot(rhi, idx, RHI_RES_PIPELINE);
    cmd->handle          = h;
    cmd->vertex_shader   = desc->vertex_shader;
    cmd->fragment_shader = desc->fragment_shader;
    copy_entry_name(cmd->vs_entry, desc->vs_entry, "vs_main");
    copy_entry_name(cmd->fs_entry, desc->fs_entry, "fs_main");
    cmd->vertex_stride   = desc->vertex_stride;
    cmd->attr_count      = desc->attr_count;
    cmd->topology        = (u32)desc->topology;
    cmd->cull_mode       = (u32)desc->cull_mode;
    cmd->front_face      = (u32)desc->front_face;
    cmd->color_format    = (u32)desc->color_format;
    cmd->blend_mode      = (u32)desc->blend_mode;
    cmd->depth_format    = (u32)desc->depth_format;
    cmd->depth_test      = (desc->depth_test ? 1u : 0u) | ((desc->sample_count & 0xFFu) << 8);
    cmd->depth_write     = desc->depth_write ? 1u : 0u;
    cmd->depth_compare   = (u32)desc->depth_compare;

    if (attr_bytes > 0u) {
        __builtin_memcpy((u8*)cmd + sizeof(RhiCmdCreatePipeline), desc->attrs, attr_bytes);
    }
    return h;
}

RhiComputePipeline rhi_create_compute_pipeline(Rhi* rhi, const RhiComputePipelineDesc* desc) {
    if (!rhi || !desc || !rhi_handle_valid(rhi, desc->shader, RHI_RES_SHADER)) {
        if (rhi) rhi->dropped_cmds++;
        return RHI_INVALID_HANDLE;
    }
    u32 idx = 0u;
    if (!rhi_find_free_slot(rhi, &idx)) {
        rhi->dropped_cmds++;
        return RHI_INVALID_HANDLE;
    }
    RhiCmdCreateComputePipeline* cmd = (RhiCmdCreateComputePipeline*)rhi_alloc_cmd(
        rhi, RHI_CMD_CREATE_COMPUTE_PIPELINE, (u32)sizeof(RhiCmdCreateComputePipeline));
    if (!cmd) return RHI_INVALID_HANDLE;

    RhiComputePipeline h = rhi_commit_slot(rhi, idx, RHI_RES_COMPUTE_PIPELINE);
    cmd->handle = h;
    cmd->shader = desc->shader;
    copy_entry_name(cmd->entry, desc->entry, "main");
    return h;
}

RhiBindGroup rhi_create_bind_group(Rhi* rhi, const RhiBindGroupDesc* desc) {
    if (!rhi || !desc || desc->entry_count == 0u || desc->entry_count > RHI_MAX_BINDINGS ||
        desc->group_index > 3u) {
        if (rhi) rhi->dropped_cmds++;
        return RHI_INVALID_HANDLE;
    }
    if (!rhi_handle_valid(rhi, desc->pipeline, RHI_RES_PIPELINE) &&
        !rhi_handle_valid(rhi, desc->pipeline, RHI_RES_COMPUTE_PIPELINE)) {
        rhi->dropped_cmds++;
        return RHI_INVALID_HANDLE;
    }
    for (u32 i = 0u; i < desc->entry_count; i++) {
        const RhiBindEntry* e = &desc->entries[i];
        RhiResourceType expected = RHI_RES_NONE;
        if (e->kind == RHI_BIND_BUFFER || e->kind == RHI_BIND_STORAGE_BUFFER) expected = RHI_RES_BUFFER;
        else if (e->kind == RHI_BIND_TEXTURE || e->kind == RHI_BIND_STORAGE_TEXTURE) expected = RHI_RES_TEXTURE;
        else if (e->kind == RHI_BIND_SAMPLER) expected = RHI_RES_SAMPLER;
        else {
            rhi->dropped_cmds++;
            return RHI_INVALID_HANDLE;
        }
        if (!rhi_handle_valid(rhi, e->resource, expected)) {
            rhi->dropped_cmds++;
            return RHI_INVALID_HANDLE;
        }
    }
    u32 idx = 0u;
    if (!rhi_find_free_slot(rhi, &idx)) {
        rhi->dropped_cmds++;
        return RHI_INVALID_HANDLE;
    }
    u32 entries_bytes = desc->entry_count * (u32)sizeof(RhiBindEntry);
    u32 total_size    = (u32)sizeof(RhiCmdCreateBindGroup) + entries_bytes;
    RhiCmdCreateBindGroup* cmd = (RhiCmdCreateBindGroup*)rhi_alloc_cmd(
        rhi, RHI_CMD_CREATE_BIND_GROUP, total_size);
    if (!cmd) return RHI_INVALID_HANDLE;

    RhiBindGroup h = rhi_commit_slot(rhi, idx, RHI_RES_BIND_GROUP);
    cmd->handle      = h;
    cmd->pipeline    = desc->pipeline;
    cmd->group_index = desc->group_index;
    cmd->entry_count = desc->entry_count;
    __builtin_memcpy((u8*)cmd + sizeof(RhiCmdCreateBindGroup), desc->entries, entries_bytes);
    return h;
}

b32 rhi_destroy_resource(Rhi* rhi, RhiHandle handle) {
    if (!rhi_handle_valid(rhi, handle, RHI_RES_NONE)) {
        if (rhi) rhi->dropped_cmds++;
        return FALSE;
    }
    RhiCmdDestroyResource* cmd = (RhiCmdDestroyResource*)rhi_alloc_cmd(
        rhi, RHI_CMD_DESTROY_RESOURCE, (u32)sizeof(RhiCmdDestroyResource));
    if (!cmd) return FALSE;

    cmd->handle = handle;
    u32 idx = RHI_HANDLE_INDEX(handle);
    rhi->slots[idx].alive = FALSE;
    rhi->slots[idx].type  = RHI_RES_NONE;
    if (rhi->live_count > 0u) rhi->live_count--;
    return TRUE;
}

/* --- Veri Yükleme ve Çizim Komutları --- */

b32 rhi_write_buffer(Rhi* rhi, RhiBuffer dst, u32 dst_offset, const void* data, u32 data_len) {
    if (!rhi_handle_valid(rhi, dst, RHI_RES_BUFFER) || !data || data_len == 0u ||
        (dst_offset & 3u) != 0u || data_len > MB(64)) {
        if (rhi) rhi->dropped_cmds++;
        return FALSE;
    }
    const RhiResourceSlot* s = &rhi->slots[RHI_HANDLE_INDEX(dst)];
    if (dst_offset > s->byte_size || data_len > s->byte_size - dst_offset) {
        rhi->dropped_cmds++;
        return FALSE;
    }
    u32 aligned_len = (u32)ALIGN_UP(data_len, 4u);
    u32 total_size  = (u32)sizeof(RhiCmdWriteBuffer) + aligned_len;
    RhiCmdWriteBuffer* cmd = (RhiCmdWriteBuffer*)rhi_alloc_cmd(
        rhi, RHI_CMD_WRITE_BUFFER, total_size);
    if (!cmd) return FALSE;

    cmd->dst_buffer = dst;
    cmd->dst_offset = dst_offset;
    cmd->data_len   = data_len;
    __builtin_memcpy((u8*)cmd + sizeof(RhiCmdWriteBuffer), data, data_len);
    return TRUE;
}

b32 rhi_write_texture(Rhi* rhi, RhiTexture dst, u32 mip_level,
                      u32 x, u32 y, u32 width, u32 height,
                      u32 bytes_per_row, const void* data, u32 data_len) {
    if (!rhi_handle_valid(rhi, dst, RHI_RES_TEXTURE) || !data ||
        width == 0u || height == 0u || data_len == 0u || data_len > MB(64)) {
        if (rhi) rhi->dropped_cmds++;
        return FALSE;
    }
    const RhiResourceSlot* s = &rhi->slots[RHI_HANDLE_INDEX(dst)];
    if (mip_level >= s->mip_levels) {
        rhi->dropped_cmds++;
        return FALSE;
    }
    u32 mip_w = s->width >> mip_level;
    u32 mip_h = s->height >> mip_level;
    if (mip_w == 0u) mip_w = 1u;
    if (mip_h == 0u) mip_h = 1u;
    if (x > mip_w || width > mip_w - x || y > mip_h || height > mip_h - y) {
        rhi->dropped_cmds++;
        return FALSE;
    }
    u32 bpp = 4u;
    if (s->format == (u32)RHI_FMT_RGBA16_FLOAT) bpp = 8u;
    else if (s->format == (u32)RHI_FMT_RGBA32_FLOAT) bpp = 16u;
    u32 min_row = width * bpp;
    if (bytes_per_row < min_row) {
        rhi->dropped_cmds++;
        return FALSE;
    }
    u64 min_bytes = (u64)bytes_per_row * (u64)(height - 1u) + (u64)min_row;
    if ((u64)data_len < min_bytes) {
        rhi->dropped_cmds++;
        return FALSE;
    }

    u32 aligned_len = (u32)ALIGN_UP(data_len, 4u);
    u32 total_size  = (u32)sizeof(RhiCmdWriteTexture) + aligned_len;
    RhiCmdWriteTexture* cmd = (RhiCmdWriteTexture*)rhi_alloc_cmd(
        rhi, RHI_CMD_WRITE_TEXTURE, total_size);
    if (!cmd) return FALSE;

    cmd->dst_texture   = dst;
    cmd->mip_level     = mip_level;
    cmd->x             = x;
    cmd->y             = y;
    cmd->width         = width;
    cmd->height        = height;
    cmd->bytes_per_row = bytes_per_row;
    cmd->data_len      = data_len;
    __builtin_memcpy((u8*)cmd + sizeof(RhiCmdWriteTexture), data, data_len);
    return TRUE;
}

b32 rhi_begin_pass(Rhi* rhi, const RhiPassDesc* desc) {
    if (!rhi || !desc || rhi->in_pass || rhi->in_compute_pass) {
        if (rhi) rhi->dropped_cmds++;
        return FALSE;
    }
    if (desc->color_texture != RHI_INVALID_HANDLE &&
        !rhi_handle_valid(rhi, desc->color_texture, RHI_RES_TEXTURE)) {
        rhi->dropped_cmds++;
        return FALSE;
    }
    if (desc->depth_texture != RHI_INVALID_HANDLE &&
        !rhi_handle_valid(rhi, desc->depth_texture, RHI_RES_TEXTURE)) {
        rhi->dropped_cmds++;
        return FALSE;
    }
    RhiCmdBeginPass* cmd = (RhiCmdBeginPass*)rhi_alloc_cmd(
        rhi, RHI_CMD_BEGIN_PASS, (u32)sizeof(RhiCmdBeginPass));
    if (!cmd) return FALSE;

    u32 flags = (desc->sample_count & 0xFFFFu) << 8;
    if (desc->clear_color) flags |= RHI_PASS_CLEAR_COLOR;
    if (desc->clear_depth) flags |= RHI_PASS_CLEAR_DEPTH;

    cmd->color_texture = desc->color_texture;
    cmd->depth_texture = desc->depth_texture;
    cmd->flags         = flags;
    cmd->clear_r       = desc->clear_r;
    cmd->clear_g       = desc->clear_g;
    cmd->clear_b       = desc->clear_b;
    cmd->clear_a       = desc->clear_a;
    cmd->clear_depth   = desc->clear_depth_val;
    rhi->in_pass       = TRUE;
    return TRUE;
}

b32 rhi_set_pipeline(Rhi* rhi, RhiPipeline pipeline) {
    if (!rhi || !rhi->in_pass || !rhi_handle_valid(rhi, pipeline, RHI_RES_PIPELINE)) {
        if (rhi) rhi->dropped_cmds++;
        return FALSE;
    }
    RhiCmdSetPipeline* cmd = (RhiCmdSetPipeline*)rhi_alloc_cmd(
        rhi, RHI_CMD_SET_PIPELINE, (u32)sizeof(RhiCmdSetPipeline));
    if (!cmd) return FALSE;
    cmd->pipeline = pipeline;
    return TRUE;
}

b32 rhi_set_bind_group(Rhi* rhi, u32 group_index, RhiBindGroup bind_group) {
    if (!rhi || (!rhi->in_pass && !rhi->in_compute_pass) || group_index > 3u ||
        !rhi_handle_valid(rhi, bind_group, RHI_RES_BIND_GROUP)) {
        if (rhi) rhi->dropped_cmds++;
        return FALSE;
    }
    RhiCmdSetBindGroup* cmd = (RhiCmdSetBindGroup*)rhi_alloc_cmd(
        rhi, RHI_CMD_SET_BIND_GROUP, (u32)sizeof(RhiCmdSetBindGroup));
    if (!cmd) return FALSE;
    cmd->group_index = group_index;
    cmd->bind_group  = bind_group;
    return TRUE;
}

b32 rhi_set_vertex_buffer(Rhi* rhi, u32 slot, RhiBuffer buffer, u32 offset, u32 size) {
    if (!rhi || !rhi->in_pass || slot > 3u || !rhi_handle_valid(rhi, buffer, RHI_RES_BUFFER)) {
        if (rhi) rhi->dropped_cmds++;
        return FALSE;
    }
    const RhiResourceSlot* s = &rhi->slots[RHI_HANDLE_INDEX(buffer)];
    if (offset > s->byte_size || size > s->byte_size - offset) {
        rhi->dropped_cmds++;
        return FALSE;
    }
    RhiCmdSetVertexBuffer* cmd = (RhiCmdSetVertexBuffer*)rhi_alloc_cmd(
        rhi, RHI_CMD_SET_VERTEX_BUFFER, (u32)sizeof(RhiCmdSetVertexBuffer));
    if (!cmd) return FALSE;
    cmd->slot   = slot;
    cmd->buffer = buffer;
    cmd->offset = offset;
    cmd->size   = size;
    return TRUE;
}

b32 rhi_set_index_buffer(Rhi* rhi, RhiBuffer buffer, RhiIndexFormat format, u32 offset, u32 size) {
    if (!rhi || !rhi->in_pass || !rhi_handle_valid(rhi, buffer, RHI_RES_BUFFER)) {
        if (rhi) rhi->dropped_cmds++;
        return FALSE;
    }
    const RhiResourceSlot* s = &rhi->slots[RHI_HANDLE_INDEX(buffer)];
    if (offset > s->byte_size || size > s->byte_size - offset) {
        rhi->dropped_cmds++;
        return FALSE;
    }
    RhiCmdSetIndexBuffer* cmd = (RhiCmdSetIndexBuffer*)rhi_alloc_cmd(
        rhi, RHI_CMD_SET_INDEX_BUFFER, (u32)sizeof(RhiCmdSetIndexBuffer));
    if (!cmd) return FALSE;
    cmd->buffer       = buffer;
    cmd->index_format = (u32)format;
    cmd->offset       = offset;
    cmd->size         = size;
    return TRUE;
}

b32 rhi_set_viewport(Rhi* rhi, f32 x, f32 y, f32 w, f32 h, f32 min_depth, f32 max_depth) {
    if (!rhi || !rhi->in_pass || w <= 0.0f || h <= 0.0f) {
        if (rhi) rhi->dropped_cmds++;
        return FALSE;
    }
    RhiCmdSetViewport* cmd = (RhiCmdSetViewport*)rhi_alloc_cmd(
        rhi, RHI_CMD_SET_VIEWPORT, (u32)sizeof(RhiCmdSetViewport));
    if (!cmd) return FALSE;
    cmd->x         = x;
    cmd->y         = y;
    cmd->w         = w;
    cmd->h         = h;
    cmd->min_depth = min_depth;
    cmd->max_depth = max_depth;
    return TRUE;
}

b32 rhi_draw(Rhi* rhi, u32 vertex_count, u32 instance_count, u32 first_vertex, u32 first_instance) {
    if (!rhi || !rhi->in_pass || vertex_count == 0u || instance_count == 0u) {
        if (rhi) rhi->dropped_cmds++;
        return FALSE;
    }
    RhiCmdDraw* cmd = (RhiCmdDraw*)rhi_alloc_cmd(
        rhi, RHI_CMD_DRAW, (u32)sizeof(RhiCmdDraw));
    if (!cmd) return FALSE;
    cmd->vertex_count   = vertex_count;
    cmd->instance_count = instance_count;
    cmd->first_vertex   = first_vertex;
    cmd->first_instance = first_instance;
    return TRUE;
}

b32 rhi_draw_indexed(Rhi* rhi, u32 index_count, u32 instance_count,
                     u32 first_index, i32 base_vertex, u32 first_instance) {
    if (!rhi || !rhi->in_pass || index_count == 0u || instance_count == 0u) {
        if (rhi) rhi->dropped_cmds++;
        return FALSE;
    }
    RhiCmdDrawIndexed* cmd = (RhiCmdDrawIndexed*)rhi_alloc_cmd(
        rhi, RHI_CMD_DRAW_INDEXED, (u32)sizeof(RhiCmdDrawIndexed));
    if (!cmd) return FALSE;
    cmd->index_count    = index_count;
    cmd->instance_count = instance_count;
    cmd->first_index    = first_index;
    cmd->base_vertex    = base_vertex;
    cmd->first_instance = first_instance;
    return TRUE;
}

b32 rhi_end_pass(Rhi* rhi) {
    if (!rhi || !rhi->in_pass) {
        if (rhi) rhi->dropped_cmds++;
        return FALSE;
    }
    RhiCmdEndPass* cmd = (RhiCmdEndPass*)rhi_alloc_cmd(
        rhi, RHI_CMD_END_PASS, (u32)sizeof(RhiCmdEndPass));
    if (!cmd) return FALSE;
    rhi->in_pass = FALSE;
    return TRUE;
}

b32 rhi_begin_compute_pass_ex(Rhi* rhi, u32 flags) {
    if (!rhi || rhi->in_pass || rhi->in_compute_pass) {
        if (rhi) rhi->dropped_cmds++;
        return FALSE;
    }
    u32 sz = (flags != 0u) ? 12u : (u32)sizeof(RhiCmdBeginComputePass);
    RhiCmdBeginComputePass* cmd = (RhiCmdBeginComputePass*)rhi_alloc_cmd(
        rhi, RHI_CMD_BEGIN_COMPUTE_PASS, sz);
    if (!cmd) return FALSE;
    if (flags != 0u) {
        ((u32*)(void*)cmd)[2] = flags;
    }
    rhi->in_compute_pass = TRUE;
    return TRUE;
}

b32 rhi_begin_compute_pass(Rhi* rhi) {
    return rhi_begin_compute_pass_ex(rhi, 0u);
}

b32 rhi_set_compute_pipeline(Rhi* rhi, RhiComputePipeline pipeline) {
    if (!rhi || !rhi->in_compute_pass ||
        !rhi_handle_valid(rhi, pipeline, RHI_RES_COMPUTE_PIPELINE)) {
        if (rhi) rhi->dropped_cmds++;
        return FALSE;
    }
    RhiCmdSetComputePipeline* cmd = (RhiCmdSetComputePipeline*)rhi_alloc_cmd(
        rhi, RHI_CMD_SET_COMPUTE_PIPELINE, (u32)sizeof(RhiCmdSetComputePipeline));
    if (!cmd) return FALSE;
    cmd->pipeline = pipeline;
    return TRUE;
}

b32 rhi_dispatch(Rhi* rhi, u32 groups_x, u32 groups_y, u32 groups_z) {
    if (!rhi || !rhi->in_compute_pass || groups_x == 0u || groups_y == 0u || groups_z == 0u) {
        if (rhi) rhi->dropped_cmds++;
        return FALSE;
    }
    RhiCmdDispatch* cmd = (RhiCmdDispatch*)rhi_alloc_cmd(
        rhi, RHI_CMD_DISPATCH, (u32)sizeof(RhiCmdDispatch));
    if (!cmd) return FALSE;
    cmd->x = groups_x;
    cmd->y = groups_y;
    cmd->z = groups_z;
    return TRUE;
}

b32 rhi_end_compute_pass(Rhi* rhi) {
    if (!rhi || !rhi->in_compute_pass) {
        if (rhi) rhi->dropped_cmds++;
        return FALSE;
    }
    RhiCmdEndComputePass* cmd = (RhiCmdEndComputePass*)rhi_alloc_cmd(
        rhi, RHI_CMD_END_COMPUTE_PASS, (u32)sizeof(RhiCmdEndComputePass));
    if (!cmd) return FALSE;
    rhi->in_compute_pass = FALSE;
    return TRUE;
}

b32 rhi_present(Rhi* rhi) {
    if (!rhi || rhi->in_pass || rhi->in_compute_pass) {
        if (rhi) rhi->dropped_cmds++;
        return FALSE;
    }
    RhiCmdPresent* cmd = (RhiCmdPresent*)rhi_alloc_cmd(
        rhi, RHI_CMD_PRESENT, (u32)sizeof(RhiCmdPresent));
    return cmd != NULL;
}

/* --- Hata Halkası --- */

static b32 ring_push(RhiErrorRing* ring, RhiErrorKind kind, RhiHandle handle,
                     u32 line, u32 col, const char* msg, u32 msg_len) {
    if (!ring) return FALSE;
    if (ring->capacity == 0u) ring->capacity = RHI_ERROR_RING_CAP;
    u32 w = atomic_load_relaxed_u32(&ring->write_pos);
    u32 r = atomic_load_acquire_u32(&ring->read_pos);
    if (w - r >= ring->capacity) {
        ring->dropped++;
        return FALSE;
    }
    RhiErrorEntry* e = &ring->entries[w & (ring->capacity - 1u)];
    __builtin_memset(e, 0, sizeof(*e));
    e->kind   = (u32)kind;
    e->handle = handle;
    e->line   = line;
    e->col    = col;
    if (msg && msg_len > 0u) {
        u32 n = MIN(msg_len, RHI_ERROR_MSG_MAX - 1u);
        __builtin_memcpy(e->msg, msg, n);
        e->msg[n]  = '\0';
        e->msg_len = n;
    }
    atomic_store_release_u32(&ring->write_pos, w + 1u);
    return TRUE;
}

static b32 ring_pop(RhiErrorRing* ring, RhiErrorEntry* out) {
    if (!ring) return FALSE;
    u32 r = atomic_load_relaxed_u32(&ring->read_pos);
    u32 w = atomic_load_acquire_u32(&ring->write_pos);
    if (w == r) return FALSE;
    u32 cap = ring->capacity ? ring->capacity : RHI_ERROR_RING_CAP;
    const RhiErrorEntry* src = &ring->entries[r & (cap - 1u)];
    if (out) {
        *out = *src;
        out->msg[RHI_ERROR_MSG_MAX - 1u] = '\0';
    }
    atomic_store_release_u32(&ring->read_pos, r + 1u);
    return TRUE;
}

b32 rhi_push_error(Rhi* rhi, RhiErrorKind kind, RhiHandle handle,
                   u32 line, u32 col, const char* msg, u32 msg_len) {
    RhiErrorRing* ring = rhi ? &rhi->errors : &g_fallback_errors;
    return ring_push(ring, kind, handle, line, col, msg, msg_len);
}

b32 rhi_pop_error(Rhi* rhi, RhiErrorEntry* out) {
    RhiErrorRing* ring = rhi ? &rhi->errors : &g_fallback_errors;
    return ring_pop(ring, out);
}

u32 rhi_poll_errors(Rhi* rhi) {
    u32 count = 0u;
    RhiErrorEntry e;
    while (rhi_pop_error(rhi, &e)) {
        LOG("[GPU HATA kind=%u h=0x%08x %u:%u] %s",
            e.kind, e.handle, e.line, e.col, e.msg);
        count++;
    }
    return count;
}

/* --- Wasm ↔ JS Sözleşmesi Dışa Aktarımları --- */

EXPORT(engine_gpu_available) b32 engine_gpu_available(void) {
    return g_gpu_available;
}

EXPORT(engine_gpu_set_available) void engine_gpu_set_available(b32 available) {
    g_gpu_available = available ? TRUE : FALSE;
}

EXPORT(engine_gpu_cmd_stream) const u8* engine_gpu_cmd_stream(void) {
    return g_active_rhi ? g_active_rhi->stream_buf : NULL;
}

EXPORT(engine_gpu_cmd_bytes) u32 engine_gpu_cmd_bytes(void) {
    return g_active_rhi ? g_active_rhi->stream_used : 0u;
}

EXPORT(engine_gpu_clear_cmds) void engine_gpu_clear_cmds(void) {
    if (g_active_rhi) rhi_begin_frame(g_active_rhi);
}

EXPORT(engine_gpu_error_ring) RhiErrorRing* engine_gpu_error_ring(void) {
    return g_active_rhi ? &g_active_rhi->errors : &g_fallback_errors;
}

EXPORT(engine_gpu_push_error) b32 engine_gpu_push_error(
    u32 kind, u32 handle, u32 line, u32 col, const char* msg, u32 msg_len) {
    return rhi_push_error(g_active_rhi, (RhiErrorKind)kind, handle, line, col, msg, msg_len);
}

EXPORT(engine_gpu_set_viewport_size) void engine_gpu_set_viewport_size(u32 width, u32 height) {
    if (width > 0u && height > 0u) {
        g_gpu_viewport_w = width;
        g_gpu_viewport_h = height;
    }
}

EXPORT(engine_gpu_viewport_width) u32 engine_gpu_viewport_width(void) {
    return g_gpu_viewport_w;
}

EXPORT(engine_gpu_viewport_height) u32 engine_gpu_viewport_height(void) {
    return g_gpu_viewport_h;
}

EXPORT(engine_gpu_set_frame_time_us) void engine_gpu_set_frame_time_us(u32 us) {
    g_gpu_frame_time_us = us;
}

EXPORT(engine_gpu_frame_time_us) u32 engine_gpu_frame_time_us(void) {
    return g_gpu_frame_time_us;
}

EXPORT(engine_gpu_set_sky_time_us) void engine_gpu_set_sky_time_us(u32 us) {
    g_gpu_sky_time_us = us;
}

EXPORT(engine_gpu_sky_time_us) u32 engine_gpu_sky_time_us(void) {
    return g_gpu_sky_time_us;
}

EXPORT(engine_gpu_set_has_timestamp) void engine_gpu_set_has_timestamp(b32 has_ts) {
    g_gpu_has_timestamp = has_ts ? TRUE : FALSE;
}

EXPORT(engine_gpu_has_timestamp) b32 engine_gpu_has_timestamp(void) {
    return g_gpu_has_timestamp;
}
