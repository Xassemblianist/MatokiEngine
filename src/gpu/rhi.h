/* gpu/rhi.h — Donanım Soyutlama Arayüzü (RHI) ve ikili komut akışı (D-108).
 * C tarafı GPU kaynaklarını (buffer, texture, sampler, WGSL shader, render pipeline, bind group)
 * nesil sayaçlı 32-bit tutamaçlarla yönetir ve kare başına düz ikili komut akışı üretir.
 * web/gpu.js bu akışı kare sonunda tek geçişte WebGPU çağrılarına çevirir; JS'te motor mantığı yoktur.
 * WGSL derleme ve doğrulama hataları paylaşımlı bellekteki RhiErrorRing halkasıyla C'ye döner.
 */
#ifndef GPU_RHI_H
#define GPU_RHI_H

#include "core/types.h"
#include "core/arena.h"

/* --- Tutamaç (Handle) Kodlaması ---
 * Bit 0..15  : 1-tabanlı yuva indeksi (1..RHI_MAX_RESOURCES_LIMIT)
 * Bit 16..23 : nesil (generation) sayacı (1..255, sarmalı)
 * Bit 24..31 : kaynak türü (RhiResourceType)
 * 0 her zaman geçersiz tutamaçtır (RHI_INVALID_HANDLE).
 */
typedef u32 RhiHandle;
typedef u32 RhiBuffer;
typedef u32 RhiTexture;
typedef u32 RhiSampler;
typedef u32 RhiShader;
typedef u32 RhiPipeline;
typedef u32 RhiBindGroup;
typedef u32 RhiComputePipeline;

#define RHI_INVALID_HANDLE      0u
#define RHI_DEFAULT_MAX_RES     256u
#define RHI_DEFAULT_STREAM_CAP  KB(256)
#define RHI_MAX_VERTEX_ATTRS    8u
#define RHI_MAX_BINDINGS        8u

typedef enum {
    RHI_RES_NONE             = 0,
    RHI_RES_BUFFER           = 1,
    RHI_RES_TEXTURE          = 2,
    RHI_RES_SAMPLER          = 3,
    RHI_RES_SHADER           = 4,
    RHI_RES_PIPELINE         = 5,
    RHI_RES_BIND_GROUP       = 6,
    RHI_RES_COMPUTE_PIPELINE = 7
} RhiResourceType;

#define RHI_HANDLE_MAKE(type, gen, idx) \
    ((((u32)(type) & 0xFFu) << 24) | (((u32)(gen) & 0xFFu) << 16) | ((u32)(idx) & 0xFFFFu))
#define RHI_HANDLE_INDEX(h) ((u32)((h) & 0xFFFFu))
#define RHI_HANDLE_GEN(h)   ((u32)(((h) >> 16) & 0xFFu))
#define RHI_HANDLE_TYPE(h)  ((RhiResourceType)(((h) >> 24) & 0xFFu))

/* --- Kaynak ve Boru Hattı Sabitleri --- */

typedef enum {
    RHI_BUF_VERTEX   = 1u << 0,
    RHI_BUF_INDEX    = 1u << 1,
    RHI_BUF_UNIFORM  = 1u << 2,
    RHI_BUF_COPY_DST = 1u << 3,
    RHI_BUF_COPY_SRC = 1u << 4,
    RHI_BUF_STORAGE  = 1u << 5
} RhiBufferUsage;

typedef enum {
    RHI_FMT_SURFACE_DEFAULT = 0,   /* Tuvalin tercih edilen formatı (yalnız pipeline color_format için) */
    RHI_FMT_RGBA8_UNORM     = 1,
    RHI_FMT_BGRA8_UNORM     = 2,
    RHI_FMT_DEPTH24_PLUS    = 3,
    RHI_FMT_DEPTH32_FLOAT   = 4,
    RHI_FMT_RGBA16_FLOAT    = 5,
    RHI_FMT_RGBA32_FLOAT    = 6
} RhiTextureFormat;

typedef enum {
    RHI_TEX_SAMPLED           = 1u << 0,
    RHI_TEX_RENDER_ATTACHMENT = 1u << 1,
    RHI_TEX_COPY_DST          = 1u << 2,
    RHI_TEX_COPY_SRC          = 1u << 3,
    RHI_TEX_STORAGE           = 1u << 4
} RhiTextureUsage;

typedef enum {
    RHI_ADDR_REPEAT       = 0,
    RHI_ADDR_CLAMP        = 1
} RhiAddressMode;

typedef enum {
    RHI_FILTER_NEAREST = 0,
    RHI_FILTER_LINEAR  = 1
} RhiFilterMode;

typedef enum {
    RHI_VFMT_FLOAT32   = 1,
    RHI_VFMT_FLOAT32X2 = 2,
    RHI_VFMT_FLOAT32X3 = 3,
    RHI_VFMT_FLOAT32X4 = 4,
    RHI_VFMT_UINT32    = 5,
    RHI_VFMT_UNORM8X4  = 6
} RhiVertexFormat;

typedef enum {
    RHI_TOPOLOGY_TRIANGLE_LIST = 0,
    RHI_TOPOLOGY_LINE_LIST     = 1
} RhiTopology;

typedef enum {
    RHI_CULL_NONE  = 0,
    RHI_CULL_BACK  = 1,
    RHI_CULL_FRONT = 2
} RhiCullMode;

typedef enum {
    RHI_FRONT_CCW = 0,
    RHI_FRONT_CW  = 1
} RhiFrontFace;

typedef enum {
    RHI_BLEND_NONE  = 0,
    RHI_BLEND_ALPHA = 1,
    RHI_BLEND_ADD   = 2
} RhiBlendMode;

typedef enum {
    RHI_CMP_ALWAYS        = 0,
    RHI_CMP_LESS          = 1,
    RHI_CMP_LESS_EQUAL    = 2,
    RHI_CMP_GREATER       = 3,
    RHI_CMP_GREATER_EQUAL = 4,
    RHI_CMP_EQUAL         = 5,
    RHI_CMP_NOT_EQUAL     = 6,
    RHI_CMP_NEVER         = 7
} RhiCompareFunc;

typedef enum {
    RHI_INDEX_UINT16 = 0,
    RHI_INDEX_UINT32 = 1
} RhiIndexFormat;

typedef enum {
    RHI_BIND_BUFFER          = 1,
    RHI_BIND_TEXTURE         = 2,
    RHI_BIND_SAMPLER         = 3,
    RHI_BIND_STORAGE_BUFFER  = 4,
    RHI_BIND_STORAGE_TEXTURE = 5
} RhiBindingKind;

/* --- İkili Komut Akışı Biçimi (ARCHITECTURE.md §2) ---
 * Tüm başlıklar ve komutlar 4-bayt hizalıdır. Değişken uzunluklu yükler (WGSL metni,
 * buffer/doku verisi) komutun hemen ardından gelir ve 4 bayt katına sıfırla tamamlanır.
 */
#define RHI_STREAM_MAGIC   0x4D475055u  /* "MGPU" (little-endian) */
#define RHI_STREAM_VERSION 1u

typedef struct {
    u32 magic;       /* RHI_STREAM_MAGIC */
    u32 version;     /* RHI_STREAM_VERSION */
    u32 byte_len;    /* Başlık dahil toplam kullanılan bayt (>= 16, 4'ün katı) */
    u32 cmd_count;   /* Akıştaki komut sayısı */
} RhiStreamHeader;

_Static_assert(sizeof(RhiStreamHeader) == 16, "RhiStreamHeader 16 bayt olmali");

typedef enum {
    RHI_CMD_NOP                     = 0,
    RHI_CMD_CREATE_BUFFER           = 1,
    RHI_CMD_CREATE_TEXTURE          = 2,
    RHI_CMD_CREATE_SAMPLER          = 3,
    RHI_CMD_CREATE_SHADER           = 4,
    RHI_CMD_CREATE_PIPELINE         = 5,
    RHI_CMD_CREATE_BIND_GROUP       = 6,
    RHI_CMD_DESTROY_RESOURCE        = 7,
    RHI_CMD_WRITE_BUFFER            = 8,
    RHI_CMD_WRITE_TEXTURE           = 9,
    RHI_CMD_BEGIN_PASS              = 10,
    RHI_CMD_SET_PIPELINE            = 11,
    RHI_CMD_SET_BIND_GROUP          = 12,
    RHI_CMD_SET_VERTEX_BUFFER       = 13,
    RHI_CMD_SET_INDEX_BUFFER        = 14,
    RHI_CMD_DRAW                    = 15,
    RHI_CMD_DRAW_INDEXED            = 16,
    RHI_CMD_SET_VIEWPORT            = 17,
    RHI_CMD_END_PASS                = 18,
    RHI_CMD_PRESENT                 = 19,
    RHI_CMD_CREATE_COMPUTE_PIPELINE = 20,
    RHI_CMD_BEGIN_COMPUTE_PASS      = 21,
    RHI_CMD_SET_COMPUTE_PIPELINE    = 22,
    RHI_CMD_DISPATCH                = 23,
    RHI_CMD_END_COMPUTE_PASS        = 24,
    RHI_CMD_COUNT                   = 25
} RhiCmdOp;

typedef struct {
    u32 opcode;      /* RhiCmdOp */
    u32 size;        /* Bu başlık (8 bayt) dahil komutun toplam bayt uzunluğu (4'ün katı) */
} RhiCmdHeader;

_Static_assert(sizeof(RhiCmdHeader) == 8, "RhiCmdHeader 8 bayt olmali");

typedef struct {
    u32 shader_location; /* @location(N) */
    u32 offset;          /* Köşe içi bayt ofseti */
    u32 format;          /* RhiVertexFormat */
} RhiVertexAttr;

_Static_assert(sizeof(RhiVertexAttr) == 12, "RhiVertexAttr 12 bayt olmali");

typedef struct {
    u32 binding;   /* @binding(N) */
    u32 kind;      /* RhiBindingKind */
    u32 resource;  /* RhiHandle */
    u32 offset;    /* Buffer için bayt ofseti (diğerleri 0) */
    u32 size;      /* Buffer için bayt boyutu (0 = kalan tamamı) */
} RhiBindEntry;

_Static_assert(sizeof(RhiBindEntry) == 20, "RhiBindEntry 20 bayt olmali");

typedef struct {
    RhiCmdHeader hdr;        /* opcode=RHI_CMD_CREATE_BUFFER, size=20 */
    u32          handle;
    u32          byte_size;
    u32          usage;
} RhiCmdCreateBuffer;

typedef struct {
    RhiCmdHeader hdr;        /* opcode=RHI_CMD_CREATE_TEXTURE, size=32 */
    u32          handle;
    u32          width;
    u32          height;
    u32          mip_levels;
    u32          format;
    u32          usage;
} RhiCmdCreateTexture;

typedef struct {
    RhiCmdHeader hdr;        /* opcode=RHI_CMD_CREATE_SAMPLER, size=32 */
    u32          handle;
    u32          address_u;
    u32          address_v;
    u32          mag_filter;
    u32          min_filter;
    u32          mipmap_filter;
} RhiCmdCreateSampler;

typedef struct {
    RhiCmdHeader hdr;        /* opcode=RHI_CMD_CREATE_SHADER, size=16 + ALIGN_UP(code_len, 4) */
    u32          handle;
    u32          code_len;
    /* Ardından: u8 code[ALIGN_UP(code_len, 4)] */
} RhiCmdCreateShader;

typedef struct {
    RhiCmdHeader hdr;            /* opcode=RHI_CMD_CREATE_PIPELINE, size=96 + attr_count * 12 */
    u32          handle;
    u32          vertex_shader;
    u32          fragment_shader;
    char         vs_entry[16];
    char         fs_entry[16];
    u32          vertex_stride;
    u32          attr_count;
    u32          topology;
    u32          cull_mode;
    u32          front_face;
    u32          color_format;
    u32          blend_mode;
    u32          depth_format;
    u32          depth_test;
    u32          depth_write;
    u32          depth_compare;
    /* Ardından: RhiVertexAttr attrs[attr_count] */
} RhiCmdCreatePipeline;

typedef struct {
    RhiCmdHeader hdr;         /* opcode=RHI_CMD_CREATE_BIND_GROUP, size=24 + entry_count * 20 */
    u32          handle;
    u32          pipeline;
    u32          group_index;
    u32          entry_count;
    /* Ardından: RhiBindEntry entries[entry_count] */
} RhiCmdCreateBindGroup;

typedef struct {
    RhiCmdHeader hdr;        /* opcode=RHI_CMD_DESTROY_RESOURCE, size=12 */
    u32          handle;
} RhiCmdDestroyResource;

typedef struct {
    RhiCmdHeader hdr;        /* opcode=RHI_CMD_WRITE_BUFFER, size=20 + ALIGN_UP(data_len, 4) */
    u32          dst_buffer;
    u32          dst_offset;
    u32          data_len;
    /* Ardından: u8 data[ALIGN_UP(data_len, 4)] */
} RhiCmdWriteBuffer;

typedef struct {
    RhiCmdHeader hdr;           /* opcode=RHI_CMD_WRITE_TEXTURE, size=40 + ALIGN_UP(data_len, 4) */
    u32          dst_texture;
    u32          mip_level;
    u32          x;
    u32          y;
    u32          width;
    u32          height;
    u32          bytes_per_row;
    u32          data_len;
    /* Ardından: u8 data[ALIGN_UP(data_len, 4)] */
} RhiCmdWriteTexture;

#define RHI_PASS_CLEAR_COLOR 1u
#define RHI_PASS_CLEAR_DEPTH 2u

typedef struct {
    RhiCmdHeader hdr;           /* opcode=RHI_CMD_BEGIN_PASS, size=40 */
    u32          color_texture; /* 0 = tuval yüzeyi, >0 = RhiTexture */
    u32          depth_texture; /* 0 = derinlik eki yok, >0 = RhiTexture */
    u32          flags;         /* RHI_PASS_CLEAR_* */
    f32          clear_r;
    f32          clear_g;
    f32          clear_b;
    f32          clear_a;
    f32          clear_depth;
} RhiCmdBeginPass;

typedef struct {
    RhiCmdHeader hdr;      /* opcode=RHI_CMD_SET_PIPELINE, size=12 */
    u32          pipeline;
} RhiCmdSetPipeline;

typedef struct {
    RhiCmdHeader hdr;         /* opcode=RHI_CMD_SET_BIND_GROUP, size=16 */
    u32          group_index;
    u32          bind_group;
} RhiCmdSetBindGroup;

typedef struct {
    RhiCmdHeader hdr;    /* opcode=RHI_CMD_SET_VERTEX_BUFFER, size=24 */
    u32          slot;
    u32          buffer;
    u32          offset;
    u32          size;
} RhiCmdSetVertexBuffer;

typedef struct {
    RhiCmdHeader hdr;          /* opcode=RHI_CMD_SET_INDEX_BUFFER, size=24 */
    u32          buffer;
    u32          index_format;
    u32          offset;
    u32          size;
} RhiCmdSetIndexBuffer;

typedef struct {
    RhiCmdHeader hdr;            /* opcode=RHI_CMD_DRAW, size=24 */
    u32          vertex_count;
    u32          instance_count;
    u32          first_vertex;
    u32          first_instance;
} RhiCmdDraw;

typedef struct {
    RhiCmdHeader hdr;            /* opcode=RHI_CMD_DRAW_INDEXED, size=28 */
    u32          index_count;
    u32          instance_count;
    u32          first_index;
    i32          base_vertex;
    u32          first_instance;
} RhiCmdDrawIndexed;

typedef struct {
    RhiCmdHeader hdr;       /* opcode=RHI_CMD_SET_VIEWPORT, size=32 */
    f32          x;
    f32          y;
    f32          w;
    f32          h;
    f32          min_depth;
    f32          max_depth;
} RhiCmdSetViewport;

typedef struct {
    RhiCmdHeader hdr; /* opcode=RHI_CMD_END_PASS, size=8 */
} RhiCmdEndPass;

typedef struct {
    RhiCmdHeader hdr; /* opcode=RHI_CMD_PRESENT, size=8 */
} RhiCmdPresent;

typedef struct {
    RhiCmdHeader hdr;          /* opcode=RHI_CMD_CREATE_COMPUTE_PIPELINE, size=32 */
    u32          handle;
    u32          shader;
    char         entry[16];
} RhiCmdCreateComputePipeline;

typedef struct {
    RhiCmdHeader hdr; /* opcode=RHI_CMD_BEGIN_COMPUTE_PASS, size=8 */
} RhiCmdBeginComputePass;

typedef struct {
    RhiCmdHeader hdr;      /* opcode=RHI_CMD_SET_COMPUTE_PIPELINE, size=12 */
    u32          pipeline;
} RhiCmdSetComputePipeline;

typedef struct {
    RhiCmdHeader hdr; /* opcode=RHI_CMD_DISPATCH, size=20 */
    u32          x;
    u32          y;
    u32          z;
} RhiCmdDispatch;

typedef struct {
    RhiCmdHeader hdr; /* opcode=RHI_CMD_END_COMPUTE_PASS, size=8 */
} RhiCmdEndComputePass;

_Static_assert(sizeof(RhiCmdCreateBuffer)          == 20, "RhiCmdCreateBuffer");
_Static_assert(sizeof(RhiCmdCreateTexture)         == 32, "RhiCmdCreateTexture");
_Static_assert(sizeof(RhiCmdCreateSampler)         == 32, "RhiCmdCreateSampler");
_Static_assert(sizeof(RhiCmdCreateShader)          == 16, "RhiCmdCreateShader");
_Static_assert(sizeof(RhiCmdCreatePipeline)        == 96, "RhiCmdCreatePipeline");
_Static_assert(sizeof(RhiCmdCreateBindGroup)       == 24, "RhiCmdCreateBindGroup");
_Static_assert(sizeof(RhiCmdDestroyResource)       == 12, "RhiCmdDestroyResource");
_Static_assert(sizeof(RhiCmdWriteBuffer)           == 20, "RhiCmdWriteBuffer");
_Static_assert(sizeof(RhiCmdWriteTexture)          == 40, "RhiCmdWriteTexture");
_Static_assert(sizeof(RhiCmdBeginPass)             == 40, "RhiCmdBeginPass");
_Static_assert(sizeof(RhiCmdSetPipeline)           == 12, "RhiCmdSetPipeline");
_Static_assert(sizeof(RhiCmdSetBindGroup)          == 16, "RhiCmdSetBindGroup");
_Static_assert(sizeof(RhiCmdSetVertexBuffer)       == 24, "RhiCmdSetVertexBuffer");
_Static_assert(sizeof(RhiCmdSetIndexBuffer)        == 24, "RhiCmdSetIndexBuffer");
_Static_assert(sizeof(RhiCmdDraw)                  == 24, "RhiCmdDraw");
_Static_assert(sizeof(RhiCmdDrawIndexed)           == 28, "RhiCmdDrawIndexed");
_Static_assert(sizeof(RhiCmdSetViewport)           == 32, "RhiCmdSetViewport");
_Static_assert(sizeof(RhiCmdEndPass)               == 8,  "RhiCmdEndPass");
_Static_assert(sizeof(RhiCmdPresent)               == 8,  "RhiCmdPresent");
_Static_assert(sizeof(RhiCmdCreateComputePipeline) == 32, "RhiCmdCreateComputePipeline");
_Static_assert(sizeof(RhiCmdBeginComputePass)      == 8,  "RhiCmdBeginComputePass");
_Static_assert(sizeof(RhiCmdSetComputePipeline)    == 12, "RhiCmdSetComputePipeline");
_Static_assert(sizeof(RhiCmdDispatch)              == 20, "RhiCmdDispatch");
_Static_assert(sizeof(RhiCmdEndComputePass)        == 8,  "RhiCmdEndComputePass");

/* --- Hata Halkası (JS → C geri bildirim, paylaşımlı bellek) --- */

#define RHI_ERROR_RING_CAP 16u   /* 2'nin kuvveti */
#define RHI_ERROR_MSG_MAX  236u

typedef enum {
    RHI_ERR_NONE          = 0,
    RHI_ERR_WGSL_COMPILE  = 1,
    RHI_ERR_VALIDATION    = 2,
    RHI_ERR_OUT_OF_MEMORY = 3,
    RHI_ERR_DEVICE_LOST   = 4,
    RHI_ERR_STREAM        = 5
} RhiErrorKind;

typedef struct {
    u32  kind;                   /* RhiErrorKind */
    u32  handle;                 /* İlgili kaynak tutamacı veya 0 */
    u32  line;                   /* WGSL satır numarası (1-tabanlı) veya 0 */
    u32  col;                    /* WGSL sütun numarası (1-tabanlı) veya 0 */
    u32  msg_len;                /* Mesaj bayt uzunluğu (< RHI_ERROR_MSG_MAX) */
    char msg[RHI_ERROR_MSG_MAX]; /* NUL-sonlu UTF-8 hata metni */
} RhiErrorEntry;

_Static_assert(sizeof(RhiErrorEntry) == 256, "RhiErrorEntry 256 bayt olmali");

typedef struct {
    u32           write_pos;     /* Üretici (JS / rhi_push_error) artırır */
    u32           read_pos;      /* Tüketici (C rhi_pop_error) artırır */
    u32           capacity;      /* RHI_ERROR_RING_CAP (16) */
    u32           dropped;       /* Halka doluyken düşürülen hata sayısı */
    RhiErrorEntry entries[RHI_ERROR_RING_CAP];
} RhiErrorRing;

#define RHI_ERR_RING_OFF_WRITE   0
#define RHI_ERR_RING_OFF_READ    4
#define RHI_ERR_RING_OFF_CAP     8
#define RHI_ERR_RING_OFF_DROPPED 12
#define RHI_ERR_RING_OFF_ENTRIES 16
#define RHI_ERR_ENTRY_SIZE       256

_Static_assert(__builtin_offsetof(RhiErrorRing, write_pos) == RHI_ERR_RING_OFF_WRITE, "err ring abi");
_Static_assert(__builtin_offsetof(RhiErrorRing, read_pos)  == RHI_ERR_RING_OFF_READ, "err ring abi");
_Static_assert(__builtin_offsetof(RhiErrorRing, capacity)  == RHI_ERR_RING_OFF_CAP, "err ring abi");
_Static_assert(__builtin_offsetof(RhiErrorRing, dropped)   == RHI_ERR_RING_OFF_DROPPED, "err ring abi");
_Static_assert(__builtin_offsetof(RhiErrorRing, entries)   == RHI_ERR_RING_OFF_ENTRIES, "err ring abi");

/* --- C Tarafı Kaynak Tanımlayıcıları (Descriptors) --- */

/* 3B doku derinliğini mip_levels üst 16 bitinde ikili akış uyumlu kodlar */
#define RHI_MIP_DEPTH(mips, depth) (((u32)(depth) << 16) | ((u32)(mips) & 0xFFFFu))

/* Atmosfer/bulut timestamp-query ölçüm bayrağı (compute flags veya pass sample_count/load_op) */
#define RHI_PASS_TIMED_SKY 0x100u

typedef struct {
    u32            width;
    u32            height;
    u32            mip_levels;   /* 0 ise 1 kabul edilir; üst 16 bit > 1 ise 3B doku derinliği */
    RhiTextureFormat format;
    u32            usage;        /* RHI_TEX_* */
} RhiTextureDesc;

typedef struct {
    RhiAddressMode address_u;
    RhiAddressMode address_v;
    RhiFilterMode  mag_filter;
    RhiFilterMode  min_filter;
    RhiFilterMode  mipmap_filter;
} RhiSamplerDesc;

typedef struct {
    RhiShader        vertex_shader;
    RhiShader        fragment_shader; /* 0 = yalnız derinlik */
    const char*      vs_entry;        /* NULL ise "vs_main" */
    const char*      fs_entry;        /* NULL ise "fs_main" */
    u32              vertex_stride;
    RhiVertexAttr    attrs[RHI_MAX_VERTEX_ATTRS];
    u32              attr_count;
    RhiTopology      topology;
    RhiCullMode      cull_mode;
    RhiFrontFace     front_face;
    RhiTextureFormat color_format;    /* 0 = tuval varsayılanı */
    RhiBlendMode     blend_mode;
    RhiTextureFormat depth_format;    /* 0 = derinlik eki yok */
    b32              depth_test;
    b32              depth_write;
    RhiCompareFunc   depth_compare;
    u32              sample_count;    /* 0 veya 1 = 1x, 4 = 4x MSAA */
} RhiPipelineDesc;

typedef struct {
    RhiShader   shader;
    const char* entry; /* NULL ise "compute" */
} RhiComputePipelineDesc;

typedef struct {
    RhiHandle    pipeline; /* RhiPipeline veya RhiComputePipeline */
    u32          group_index;
    RhiBindEntry entries[RHI_MAX_BINDINGS];
    u32          entry_count;
} RhiBindGroupDesc;

typedef struct {
    RhiTexture color_texture; /* 0 = tuval yüzeyi */
    RhiTexture depth_texture; /* 0 = derinlik eki yok */
    b32        clear_color;
    f32        clear_r, clear_g, clear_b, clear_a;
    b32        clear_depth;
    f32        clear_depth_val;
    u32        sample_count;  /* 0 veya 1 = 1x, 4 = 4x MSAA */
} RhiPassDesc;

/* --- Kaynak Yuvası ve RHI Bağlamı --- */

typedef struct {
    RhiResourceType type;
    u8              gen;
    b32             alive;
    u32             byte_size;   /* Buffer için boyut */
    u32             width;       /* Texture için genişlik */
    u32             height;      /* Texture için yükseklik */
    u32             mip_levels;  /* Texture için mip sayısı */
    u32             format;      /* Texture için RhiTextureFormat */
    u32             usage;       /* Buffer/Texture kullanım bayrakları */
} RhiResourceSlot;

typedef struct {
    RhiResourceSlot* slots;       /* [max_resources + 1], 1-tabanlı */
    u32              max_resources;
    u32              live_count;
    u8*              stream_buf;  /* Komut akışı tamponu (Arena'dan, 16-bayt hizalı) */
    u32              stream_cap;  /* Tampon kapasitesi (bayt, >= sizeof(RhiStreamHeader)) */
    u32              stream_used; /* Yazılan bayt sayısı */
    u32              cmd_count;   /* Akıştaki komut sayısı */
    b32              in_pass;     /* begin_pass..end_pass arasında TRUE */
    b32              in_compute_pass; /* begin_compute_pass..end_compute_pass arasında TRUE */
    b32              overflow;    /* Akış kapasitesi aşıldıysa TRUE */
    u32              dropped_cmds;/* Kapasite aşımı veya geçersiz durum yüzünden reddedilen komutlar */
    RhiErrorRing     errors;      /* JS/C hata halkası */
} Rhi;

/* --- RHI Yaşam Döngüsü ve Akış Yönetimi --- */

b32        rhi_init(Rhi* rhi, Arena* arena, u32 max_resources, u32 stream_cap);
void       rhi_set_active(Rhi* rhi);
Rhi*       rhi_active(void);

void       rhi_begin_frame(Rhi* rhi);
const u8*  rhi_stream_data(const Rhi* rhi);
u32        rhi_stream_bytes(const Rhi* rhi);
u32        rhi_stream_cmd_count(const Rhi* rhi);
b32        rhi_handle_valid(const Rhi* rhi, RhiHandle h, RhiResourceType expected_type);
const RhiResourceSlot* rhi_resource_slot(const Rhi* rhi, RhiHandle h);

/* --- Kaynak Oluşturma ve Yok Etme --- */

RhiBuffer          rhi_create_buffer(Rhi* rhi, u32 byte_size, u32 usage);
RhiTexture         rhi_create_texture(Rhi* rhi, const RhiTextureDesc* desc);
RhiSampler         rhi_create_sampler(Rhi* rhi, const RhiSamplerDesc* desc);
RhiShader          rhi_create_shader(Rhi* rhi, const char* wgsl_code, u32 code_len);
RhiPipeline        rhi_create_pipeline(Rhi* rhi, const RhiPipelineDesc* desc);
RhiComputePipeline rhi_create_compute_pipeline(Rhi* rhi, const RhiComputePipelineDesc* desc);
RhiBindGroup       rhi_create_bind_group(Rhi* rhi, const RhiBindGroupDesc* desc);
b32                rhi_destroy_resource(Rhi* rhi, RhiHandle handle);

/* --- Veri Yükleme ve Çizim / Compute Komutları --- */

b32 rhi_write_buffer(Rhi* rhi, RhiBuffer dst, u32 dst_offset, const void* data, u32 data_len);
b32 rhi_write_texture(Rhi* rhi, RhiTexture dst, u32 mip_level,
                      u32 x, u32 y, u32 width, u32 height,
                      u32 bytes_per_row, const void* data, u32 data_len);

b32 rhi_begin_compute_pass(Rhi* rhi);
b32 rhi_begin_compute_pass_ex(Rhi* rhi, u32 flags);
b32 rhi_set_compute_pipeline(Rhi* rhi, RhiComputePipeline pipeline);
b32 rhi_dispatch(Rhi* rhi, u32 x, u32 y, u32 z);
b32 rhi_end_compute_pass(Rhi* rhi);

b32 rhi_begin_pass(Rhi* rhi, const RhiPassDesc* desc);
b32 rhi_set_pipeline(Rhi* rhi, RhiPipeline pipeline);
b32 rhi_set_bind_group(Rhi* rhi, u32 group_index, RhiBindGroup bind_group);
b32 rhi_set_vertex_buffer(Rhi* rhi, u32 slot, RhiBuffer buffer, u32 offset, u32 size);
b32 rhi_set_index_buffer(Rhi* rhi, RhiBuffer buffer, RhiIndexFormat format, u32 offset, u32 size);
b32 rhi_set_viewport(Rhi* rhi, f32 x, f32 y, f32 w, f32 h, f32 min_depth, f32 max_depth);
b32 rhi_draw(Rhi* rhi, u32 vertex_count, u32 instance_count, u32 first_vertex, u32 first_instance);
b32 rhi_draw_indexed(Rhi* rhi, u32 index_count, u32 instance_count,
                     u32 first_index, i32 base_vertex, u32 first_instance);
b32 rhi_end_pass(Rhi* rhi);
b32 rhi_present(Rhi* rhi);

/* --- Hata Halkası İşlemleri --- */

b32 rhi_push_error(Rhi* rhi, RhiErrorKind kind, RhiHandle handle,
                   u32 line, u32 col, const char* msg, u32 msg_len);
b32 rhi_pop_error(Rhi* rhi, RhiErrorEntry* out);
u32 rhi_poll_errors(Rhi* rhi);   /* Tüm bekleyen hataları LOG'a yazar, sayısını döner */

/* --- Wasm ↔ JS Sözleşmesi Dışa Aktarımları (ARCHITECTURE.md §2) --- */

b32           engine_gpu_available(void);
void          engine_gpu_set_available(b32 available);
void          engine_gpu_set_viewport_size(u32 width, u32 height);
u32           engine_gpu_viewport_width(void);
u32           engine_gpu_viewport_height(void);
void          engine_gpu_set_frame_time_us(u32 us);
u32           engine_gpu_frame_time_us(void);
void          engine_gpu_set_sky_time_us(u32 us);
u32           engine_gpu_sky_time_us(void);
void          engine_gpu_set_has_timestamp(b32 has);
b32           engine_gpu_has_timestamp(void);
const u8*     engine_gpu_cmd_stream(void);
u32           engine_gpu_cmd_bytes(void);
void          engine_gpu_clear_cmds(void);
RhiErrorRing* engine_gpu_error_ring(void);
b32           engine_gpu_push_error(u32 kind, u32 handle, u32 line, u32 col,
                                    const char* msg, u32 msg_len);

/* Bağımsız GPU demo girişi ve piksel doğrulaması (src/gpu/gpu_test.c) */
typedef struct {
    u32 width;
    u32 height;
    u32 bg_pixels;
    u32 tri_pixels;
    u32 tri_red_pixels;
    u32 tri_green_pixels;
    u32 tri_blue_pixels;
    u32 cube_pixels;
    u32 cube_warm_pixels;
    u32 cube_cool_pixels;
    u32 gpu_errors;
    b32 passed;
} GpuTestSummary;

_Static_assert(sizeof(GpuTestSummary) == 48, "GpuTestSummary 48 bayt olmali");

u32                   engine_gpu_test_frame(f64 now_ms, f32 dt);
u8*                   engine_gpu_test_readback_buf(void);
u32                   engine_gpu_test_readback_bytes(void);
b32                   engine_gpu_test_analyze_readback(void);
const GpuTestSummary* engine_gpu_test_summary(void);

#endif
