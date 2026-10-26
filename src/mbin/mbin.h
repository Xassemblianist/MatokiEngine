/* mbin/mbin.h — WebAssembly ikili (binary) modül üretici arayüzü.
 * Matoki JIT derleyicisinin arka ucu: bellek tamponu, LEB128 kodlama ve
 * WebAssembly Core Spec §5 bölüm (section) serileştiricisini içerir.
 * libc veya dinamik bellek (malloc) kullanmaz; tüm bellek dışarıdan verilir.
 */
#ifndef MBIN_MBIN_H
#define MBIN_MBIN_H

#include "core/types.h"
#include "core/log.h"

/* WebAssembly değer ve blok tipleri (Core Spec §5.3) */
typedef enum {
    MB_VAL_I32  = 0x7F,
    MB_VAL_I64  = 0x7E,
    MB_VAL_F32  = 0x7D,
    MB_VAL_F64  = 0x7C,
    MB_VAL_V128 = 0x7B,
    MB_FUNCREF  = 0x70,
    MB_BLOCK_VOID = 0x40
} MbValType;

/* WebAssembly bölüm (section) kimlikleri (Core Spec §5.5.2) */
typedef enum {
    MB_SEC_CUSTOM   = 0,
    MB_SEC_TYPE     = 1,
    MB_SEC_IMPORT   = 2,
    MB_SEC_FUNC     = 3,
    MB_SEC_TABLE    = 4,
    MB_SEC_MEMORY   = 5,
    MB_SEC_GLOBAL   = 6,
    MB_SEC_EXPORT   = 7,
    MB_SEC_START    = 8,
    MB_SEC_ELEM     = 9,
    MB_SEC_CODE     = 10,
    MB_SEC_DATA     = 11
} MbSectionId;

/* WebAssembly import/export tanımlayıcı türleri */
typedef enum {
    MB_EXT_FUNC   = 0x00,
    MB_EXT_TABLE  = 0x01,
    MB_EXT_MEMORY = 0x02,
    MB_EXT_GLOBAL = 0x03
} MbExternalKind;

/* Sabit kapasite sınırları */
#define MB_MAX_PARAMS       16
#define MB_MAX_RESULTS      8
#define MB_MAX_TYPES        64
#define MB_MAX_IMPORTS      32
#define MB_MAX_FUNCS        64
#define MB_MAX_EXPORTS      64
#define MB_MAX_GLOBALS      32
#define MB_MAX_ELEMS        16
#define MB_MAX_ELEM_FUNCS   64
#define MB_MAX_DATAS        16
#define MB_MAX_LOCAL_GROUPS 32
#define MB_MAX_NAME_LEN     64

/* Bayt tamponu: dışarıdan verilen bellek üzerinde çalışır, taşmada PANIC */
typedef struct {
    u8* p;
    u32 len;
    u32 cap;
} MbBuf;

/* Fonksiyon imzası: (params) -> (results) */
typedef struct {
    u8  params[MB_MAX_PARAMS];
    u32 param_count;
    u8  results[MB_MAX_RESULTS];
    u32 result_count;
} MbFuncType;

/* Import kaydı */
typedef struct {
    char mod_name[MB_MAX_NAME_LEN];
    u32  mod_len;
    char field_name[MB_MAX_NAME_LEN];
    u32  field_len;
    u8   kind;       /* MbExternalKind */
    u32  type_idx;   /* MB_EXT_FUNC için */
    u32  min_pages;  /* MB_EXT_MEMORY / MB_EXT_TABLE için */
    u32  max_pages;  /* MB_EXT_MEMORY / MB_EXT_TABLE için (0 = üst sınır yok) */
    b32  has_max;    /* MB_EXT_MEMORY / MB_EXT_TABLE için üst sınır var mı */
    b32  shared;     /* MB_EXT_MEMORY için paylaşımlı bellek mi */
    u8   valtype;    /* MB_EXT_GLOBAL için */
    b32  mutable_;   /* MB_EXT_GLOBAL için */
} MbImport;

/* Global değişken kaydı (Section 6) */
typedef struct {
    u8  valtype;
    b32 mutable_;
    u8  init_type;   /* MB_VAL_I32, MB_VAL_I64, MB_VAL_F32, MB_VAL_F64, MB_VAL_V128 veya 0x23 (global.get) */
    i64 init_i64;
    f32 init_f32;
    f64 init_f64;
    u8  init_v128[16];
} MbGlobal;

/* Element segmenti kaydı (Section 9) */
typedef struct {
    i32 offset;
    u32 func_indices[MB_MAX_ELEM_FUNCS];
    u32 count;
} MbElem;

/* Data segmenti kaydı (Section 11) */
typedef struct {
    i32 offset;
    u8* data;
    u32 len;
} MbData;

/* Export kaydı */
typedef struct {
    char name[MB_MAX_NAME_LEN];
    u32  name_len;
    u8   kind;       /* MbExternalKind */
    u32  index;
} MbExport;

/* Fonksiyon yerel değişken grubu: (count, valtype) */
typedef struct {
    u32 count;
    u8  valtype;
} MbLocalGroup;

typedef struct MbModule MbModule;

/* Fonksiyon kurucu durumu */
typedef struct {
    MbModule*    mod;
    u32          type_idx;
    u32          func_idx;
    u32          next_local_idx;
    MbLocalGroup locals[MB_MAX_LOCAL_GROUPS];
    u32          local_group_count;
    MbBuf        code;
    u32          depth;
    b32          ended;
} MbFunc;

/* Modül kurucu durumu (malloc yok, çalışma tamponu dışarıdan verilir) */
struct MbModule {
    u8*        work_mem;
    u32        work_cap;
    u32        work_used;

    MbFuncType types[MB_MAX_TYPES];
    u32        type_count;

    MbImport   imports[MB_MAX_IMPORTS];
    u32        import_count;
    u32        imported_func_count;
    u32        imported_global_count;

    MbFunc     funcs[MB_MAX_FUNCS];
    u32        func_count;
    b32        func_active;

    b32        has_defined_table;
    u32        table_min_elems;
    u32        table_max_elems;
    b32        table_has_max;

    b32        has_defined_memory;
    u32        mem_min_pages;
    u32        mem_max_pages;
    b32        mem_has_max;
    b32        mem_shared;

    MbGlobal   globals[MB_MAX_GLOBALS];
    u32        global_count;

    MbExport   exports[MB_MAX_EXPORTS];
    u32        export_count;

    MbElem     elems[MB_MAX_ELEMS];
    u32        elem_count;

    MbData     datas[MB_MAX_DATAS];
    u32        data_count;
};

/* Bayt tamponu işlemleri */
void mb_buf_init(MbBuf* b, u8* mem, u32 cap);
void mb_u8(MbBuf* b, u8 v);
void mb_bytes(MbBuf* b, const void* src, u32 len);
void mb_uleb(MbBuf* b, u64 v);
void mb_sleb(MbBuf* b, i64 v);
void mb_f32(MbBuf* b, f32 v);
void mb_f64(MbBuf* b, f64 v);
void mb_name(MbBuf* b, const char* str, u32 len);

/* LEB128 çözme yardımcıları (test ve doğrulama için) */
u64  mb_decode_uleb(const u8* p, u32 max_len, u32* out_bytes_read);
i64  mb_decode_sleb(const u8* p, u32 max_len, u32* out_bytes_read);

/* Modül kurucu işlemleri */
void    mb_module_init(MbModule* m, u8* work_mem, u32 work_cap);
u32     mb_type(MbModule* m, const u8* params, u32 np, const u8* results, u32 nr);
u32     mb_type_add(MbModule* m, const u8* params, u32 np, const u8* results, u32 nr);
u32     mb_import_func(MbModule* m, const char* mod, const char* name, u32 type_idx);
void    mb_import_memory(MbModule* m, const char* mod, const char* name,
                         u32 min_pages, u32 max_pages, b32 shared);
void    mb_import_table(MbModule* m, const char* mod, const char* name, u32 min_elems);
void    mb_import_table_limits(MbModule* m, const char* mod, const char* name,
                               u32 min_elems, u32 max_elems, b32 has_max);
u32     mb_import_global(MbModule* m, const char* mod, const char* name,
                         u8 valtype, b32 mutable_);
void    mb_table(MbModule* m, u32 min_elems, u32 max_elems, b32 has_max);
void    mb_memory(MbModule* m, u32 min_pages, u32 max_pages, b32 shared);
u32     mb_global_i32(MbModule* m, b32 mutable_, i32 init_val);
u32     mb_global_i64(MbModule* m, b32 mutable_, i64 init_val);
u32     mb_global_f32(MbModule* m, b32 mutable_, f32 init_val);
u32     mb_global_f64(MbModule* m, b32 mutable_, f64 init_val);
u32     mb_global_v128(MbModule* m, b32 mutable_, const u8 init_bytes[16]);
MbFunc* mb_func_begin(MbModule* m, u32 type_idx);
u32     mb_local(MbFunc* f, u8 valtype, u32 count);
void    mb_func_end(MbFunc* f);
void    mb_export(MbModule* m, const char* name, u8 kind, u32 index);
void    mb_export_func(MbModule* m, const char* name, u32 func_idx);
void    mb_elem(MbModule* m, i32 offset, const u32* func_indices, u32 count);
void    mb_data(MbModule* m, i32 offset, const u8* bytes, u32 len);
u32     mb_module_finish(MbModule* m, MbBuf* out_buf);

#endif /* MBIN_MBIN_H */
