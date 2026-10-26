/* masm/mdis.c — WebAssembly ikili (binary) modül çözücüsü (disassembler).
 * Tüm standart bölümleri (type, import, func, table, memory, global, export,
 * elem, code, data) ve mbin_ops.h içindeki tüm skaler/SIMD komutları okuyarak
 * masm tarafından kayıpsız (round-trip) derlenebilen düz WAT metni üretir.
 */
#include "masm/mdis.h"
#include "mbin/mbin.h"
#include "core/log.h"

/* ============================================================================
 * 1. Metin Yazıcı (String Builder) ve İkili Okuyucu (Binary Reader)
 * ============================================================================ */

typedef struct {
    char* buf;
    u32   len;
    u32   cap;
} MdisOut;

static void mdis_memzero(void* dst, usize len) {
    u8* p = (u8*)dst;
    for (usize i = 0; i < len; i++) p[i] = 0;
}

static void mdis_putc(MdisOut* o, char c) {
    if (o->len + 1 < o->cap) {
        o->buf[o->len++] = c;
        o->buf[o->len] = '\0';
    }
}

static void mdis_puts(MdisOut* o, const char* s) {
    while (*s) mdis_putc(o, *s++);
}

static void mdis_printf(MdisOut* o, const char* fmt, ...) {
    char tmp[512];
    VaList ap;
    VA_START(ap, fmt);
    u32 n = fmt_vformat(tmp, sizeof(tmp), fmt, ap);
    VA_END(ap);
    for (u32 i = 0; i < n; i++) mdis_putc(o, tmp[i]);
}

static void mdis_indent(MdisOut* o, u32 depth) {
    for (u32 i = 0; i < depth; i++) {
        mdis_putc(o, ' ');
        mdis_putc(o, ' ');
    }
}

static void mdis_escaped_bytes(MdisOut* o, const u8* bytes, u32 len) {
    static const char hex[] = "0123456789abcdef";
    mdis_putc(o, '"');
    for (u32 i = 0; i < len; i++) {
        u8 b = bytes[i];
        if (b >= 32 && b < 127 && b != '"' && b != '\\') {
            mdis_putc(o, (char)b);
        } else {
            mdis_putc(o, '\\');
            mdis_putc(o, hex[(b >> 4) & 0xFu]);
            mdis_putc(o, hex[b & 0xFu]);
        }
    }
    mdis_putc(o, '"');
}

/* IEEE-754 f32 bitlerini tam (kayıpsız) onaltılık kayan nokta metnine çevirir */
static void mdis_fmt_f32_exact(MdisOut* o, u32 bits) {
    b32 sign = (bits >> 31) != 0;
    u32 exp  = (bits >> 23) & 0xFFu;
    u32 mant = bits & 0x7FFFFFu;
    if (sign) mdis_putc(o, '-');
    if (exp == 0xFFu) {
        if (mant == 0) {
            mdis_puts(o, "inf");
        } else if (mant == 0x400000u) {
            mdis_puts(o, "nan");
        } else {
            mdis_printf(o, "nan:0x%x", mant);
        }
        return;
    }
    if (exp == 0 && mant == 0) {
        mdis_puts(o, "0x0p+0");
        return;
    }
    if (exp == 0) {
        mdis_printf(o, "0x0.%06xp-126", mant << 1);
    } else {
        i32 e = (i32)exp - 127;
        const char* esign = (e >= 0) ? "+" : "";
        if (mant == 0) {
            mdis_printf(o, "0x1p%s%d", esign, e);
        } else {
            mdis_printf(o, "0x1.%06xp%s%d", mant << 1, esign, e);
        }
    }
}

/* IEEE-754 f64 bitlerini tam (kayıpsız) onaltılık kayan nokta metnine çevirir */
static void mdis_fmt_f64_exact(MdisOut* o, u64 bits) {
    b32 sign = (bits >> 63) != 0;
    u32 exp  = (u32)((bits >> 52) & 0x7FFull);
    u64 mant = bits & 0xFFFFFFFFFFFFFull;
    if (sign) mdis_putc(o, '-');
    if (exp == 0x7FFu) {
        if (mant == 0) {
            mdis_puts(o, "inf");
        } else if (mant == 0x8000000000000ull) {
            mdis_puts(o, "nan");
        } else {
            mdis_printf(o, "nan:0x%llx", (unsigned long long)mant);
        }
        return;
    }
    if (exp == 0 && mant == 0) {
        mdis_puts(o, "0x0p+0");
        return;
    }
    if (exp == 0) {
        mdis_printf(o, "0x0.%013llxp-1022", (unsigned long long)mant);
    } else {
        i32 e = (i32)exp - 1023;
        const char* esign = (e >= 0) ? "+" : "";
        if (mant == 0) {
            mdis_printf(o, "0x1p%s%d", esign, e);
        } else {
            mdis_printf(o, "0x1.%013llxp%s%d", (unsigned long long)mant, esign, e);
        }
    }
}

typedef struct {
    const u8* p;
    u32       len;
    u32       pos;
    b32       has_err;
    u32       err_off;
    char      err_msg[256];
} MdisReader;

static void mr_err(MdisReader* r, const char* fmt, ...) {
    if (r->has_err) return;
    r->has_err = TRUE;
    r->err_off = r->pos;
    u32 n = fmt_format(r->err_msg, sizeof(r->err_msg), "offset 0x%x: ", r->pos);
    VaList ap;
    VA_START(ap, fmt);
    fmt_vformat(r->err_msg + n, (u32)sizeof(r->err_msg) - n, fmt, ap);
    VA_END(ap);
}

static b32 mr_u8(MdisReader* r, u8* out) {
    if (r->pos >= r->len) {
        mr_err(r, "unexpected EOF reading byte");
        return FALSE;
    }
    *out = r->p[r->pos++];
    return TRUE;
}

static b32 mr_bytes(MdisReader* r, const u8** out_ptr, u32 count) {
    if (r->pos > r->len || count > r->len - r->pos) {
        mr_err(r, "unexpected EOF reading %u bytes", count);
        return FALSE;
    }
    *out_ptr = r->p + r->pos;
    r->pos += count;
    return TRUE;
}

static b32 mr_uleb(MdisReader* r, u64* out) {
    u64 res = 0;
    u32 shift = 0;
    while (TRUE) {
        if (r->pos >= r->len || shift >= 64) {
            mr_err(r, "invalid or truncated ULEB128");
            return FALSE;
        }
        u8 b = r->p[r->pos++];
        res |= (u64)(b & 0x7Fu) << shift;
        if ((b & 0x80u) == 0) break;
        shift += 7;
    }
    *out = res;
    return TRUE;
}

static b32 mr_u32(MdisReader* r, u32* out) {
    u64 v = 0;
    if (!mr_uleb(r, &v) || v > 0xFFFFFFFFull) {
        mr_err(r, "u32 ULEB128 overflow");
        return FALSE;
    }
    *out = (u32)v;
    return TRUE;
}

static b32 mr_sleb(MdisReader* r, i64* out) {
    u64 res = 0;
    u32 shift = 0;
    u8 b = 0;
    while (TRUE) {
        if (r->pos >= r->len || shift >= 64) {
            mr_err(r, "invalid or truncated SLEB128");
            return FALSE;
        }
        b = r->p[r->pos++];
        res |= (u64)(b & 0x7Fu) << shift;
        shift += 7;
        if ((b & 0x80u) == 0) break;
    }
    if (shift < 64 && (b & 0x40u) != 0) {
        res |= (~(u64)0) << shift;
    }
    *out = (i64)res;
    return TRUE;
}

static b32 mr_u32_le(MdisReader* r, u32* out) {
    const u8* b = NULL;
    if (!mr_bytes(r, &b, 4)) return FALSE;
    *out = (u32)b[0] | ((u32)b[1] << 8) | ((u32)b[2] << 16) | ((u32)b[3] << 24);
    return TRUE;
}

static b32 mr_u64_le(MdisReader* r, u64* out) {
    const u8* b = NULL;
    if (!mr_bytes(r, &b, 8)) return FALSE;
    u64 v = 0;
    for (u32 i = 0; i < 8; i++) v |= ((u64)b[i] << (i * 8));
    *out = v;
    return TRUE;
}

static const char* mdis_valtype_str(u8 vt) {
    switch (vt) {
    case MB_VAL_I32:  return "i32";
    case MB_VAL_I64:  return "i64";
    case MB_VAL_F32:  return "f32";
    case MB_VAL_F64:  return "f64";
    case MB_VAL_V128: return "v128";
    default:          return NULL;
    }
}

/* ============================================================================
 * 2. Komut Çözme Tablosu ve Gövde Çözücüsü
 * ============================================================================ */

typedef enum {
    DOP_SIMPLE = 0,
    DOP_MEM,
    DOP_MEM_ZERO,
    DOP_SIMD_SIMPLE,
    DOP_SIMD_MEM,
    DOP_SIMD_MEM_LANE,
    DOP_SIMD_LANE
} MdisOpKind;

typedef struct {
    u16         code;
    u8          kind;
    u8          def_align;
    const char* name;
} MdisOpEntry;

static const MdisOpEntry g_mdis_std_ops[] = {
    { 0x00u, DOP_SIMPLE,   0, "unreachable" },
    { 0x01u, DOP_SIMPLE,   0, "nop" },
    { 0x0Fu, DOP_SIMPLE,   0, "return" },
    { 0x1Au, DOP_SIMPLE,   0, "drop" },
    { 0x1Bu, DOP_SIMPLE,   0, "select" },

    { 0x28u, DOP_MEM,      2, "i32.load" },
    { 0x29u, DOP_MEM,      3, "i64.load" },
    { 0x2Au, DOP_MEM,      2, "f32.load" },
    { 0x2Bu, DOP_MEM,      3, "f64.load" },
    { 0x2Cu, DOP_MEM,      0, "i32.load8_s" },
    { 0x2Du, DOP_MEM,      0, "i32.load8_u" },
    { 0x2Eu, DOP_MEM,      1, "i32.load16_s" },
    { 0x2Fu, DOP_MEM,      1, "i32.load16_u" },
    { 0x30u, DOP_MEM,      0, "i64.load8_s" },
    { 0x31u, DOP_MEM,      0, "i64.load8_u" },
    { 0x32u, DOP_MEM,      1, "i64.load16_s" },
    { 0x33u, DOP_MEM,      1, "i64.load16_u" },
    { 0x34u, DOP_MEM,      2, "i64.load32_s" },
    { 0x35u, DOP_MEM,      2, "i64.load32_u" },
    { 0x36u, DOP_MEM,      2, "i32.store" },
    { 0x37u, DOP_MEM,      3, "i64.store" },
    { 0x38u, DOP_MEM,      2, "f32.store" },
    { 0x39u, DOP_MEM,      3, "f64.store" },
    { 0x3Au, DOP_MEM,      0, "i32.store8" },
    { 0x3Bu, DOP_MEM,      1, "i32.store16" },
    { 0x3Cu, DOP_MEM,      0, "i64.store8" },
    { 0x3Du, DOP_MEM,      1, "i64.store16" },
    { 0x3Eu, DOP_MEM,      2, "i64.store32" },
    { 0x3Fu, DOP_MEM_ZERO, 0, "memory.size" },
    { 0x40u, DOP_MEM_ZERO, 0, "memory.grow" },

    { 0x45u, DOP_SIMPLE,   0, "i32.eqz" },
    { 0x46u, DOP_SIMPLE,   0, "i32.eq" },
    { 0x47u, DOP_SIMPLE,   0, "i32.ne" },
    { 0x48u, DOP_SIMPLE,   0, "i32.lt_s" },
    { 0x49u, DOP_SIMPLE,   0, "i32.lt_u" },
    { 0x4Au, DOP_SIMPLE,   0, "i32.gt_s" },
    { 0x4Bu, DOP_SIMPLE,   0, "i32.gt_u" },
    { 0x4Cu, DOP_SIMPLE,   0, "i32.le_s" },
    { 0x4Du, DOP_SIMPLE,   0, "i32.le_u" },
    { 0x4Eu, DOP_SIMPLE,   0, "i32.ge_s" },
    { 0x4Fu, DOP_SIMPLE,   0, "i32.ge_u" },

    { 0x50u, DOP_SIMPLE,   0, "i64.eqz" },
    { 0x51u, DOP_SIMPLE,   0, "i64.eq" },
    { 0x52u, DOP_SIMPLE,   0, "i64.ne" },
    { 0x53u, DOP_SIMPLE,   0, "i64.lt_s" },
    { 0x54u, DOP_SIMPLE,   0, "i64.lt_u" },
    { 0x55u, DOP_SIMPLE,   0, "i64.gt_s" },
    { 0x56u, DOP_SIMPLE,   0, "i64.gt_u" },
    { 0x57u, DOP_SIMPLE,   0, "i64.le_s" },
    { 0x58u, DOP_SIMPLE,   0, "i64.le_u" },
    { 0x59u, DOP_SIMPLE,   0, "i64.ge_s" },
    { 0x5Au, DOP_SIMPLE,   0, "i64.ge_u" },

    { 0x5Bu, DOP_SIMPLE,   0, "f32.eq" },
    { 0x5Cu, DOP_SIMPLE,   0, "f32.ne" },
    { 0x5Du, DOP_SIMPLE,   0, "f32.lt" },
    { 0x5Eu, DOP_SIMPLE,   0, "f32.gt" },
    { 0x5Fu, DOP_SIMPLE,   0, "f32.le" },
    { 0x60u, DOP_SIMPLE,   0, "f32.ge" },
    { 0x61u, DOP_SIMPLE,   0, "f64.eq" },
    { 0x62u, DOP_SIMPLE,   0, "f64.ne" },
    { 0x63u, DOP_SIMPLE,   0, "f64.lt" },
    { 0x64u, DOP_SIMPLE,   0, "f64.gt" },
    { 0x65u, DOP_SIMPLE,   0, "f64.le" },
    { 0x66u, DOP_SIMPLE,   0, "f64.ge" },

    { 0x67u, DOP_SIMPLE,   0, "i32.clz" },
    { 0x68u, DOP_SIMPLE,   0, "i32.ctz" },
    { 0x69u, DOP_SIMPLE,   0, "i32.popcnt" },
    { 0x6Au, DOP_SIMPLE,   0, "i32.add" },
    { 0x6Bu, DOP_SIMPLE,   0, "i32.sub" },
    { 0x6Cu, DOP_SIMPLE,   0, "i32.mul" },
    { 0x6Du, DOP_SIMPLE,   0, "i32.div_s" },
    { 0x6Eu, DOP_SIMPLE,   0, "i32.div_u" },
    { 0x6Fu, DOP_SIMPLE,   0, "i32.rem_s" },
    { 0x70u, DOP_SIMPLE,   0, "i32.rem_u" },
    { 0x71u, DOP_SIMPLE,   0, "i32.and" },
    { 0x72u, DOP_SIMPLE,   0, "i32.or" },
    { 0x73u, DOP_SIMPLE,   0, "i32.xor" },
    { 0x74u, DOP_SIMPLE,   0, "i32.shl" },
    { 0x75u, DOP_SIMPLE,   0, "i32.shr_s" },
    { 0x76u, DOP_SIMPLE,   0, "i32.shr_u" },
    { 0x77u, DOP_SIMPLE,   0, "i32.rotl" },
    { 0x78u, DOP_SIMPLE,   0, "i32.rotr" },

    { 0x79u, DOP_SIMPLE,   0, "i64.clz" },
    { 0x7Au, DOP_SIMPLE,   0, "i64.ctz" },
    { 0x7Bu, DOP_SIMPLE,   0, "i64.popcnt" },
    { 0x7Cu, DOP_SIMPLE,   0, "i64.add" },
    { 0x7Du, DOP_SIMPLE,   0, "i64.sub" },
    { 0x7Eu, DOP_SIMPLE,   0, "i64.mul" },
    { 0x7Fu, DOP_SIMPLE,   0, "i64.div_s" },
    { 0x80u, DOP_SIMPLE,   0, "i64.div_u" },
    { 0x81u, DOP_SIMPLE,   0, "i64.rem_s" },
    { 0x82u, DOP_SIMPLE,   0, "i64.rem_u" },
    { 0x83u, DOP_SIMPLE,   0, "i64.and" },
    { 0x84u, DOP_SIMPLE,   0, "i64.or" },
    { 0x85u, DOP_SIMPLE,   0, "i64.xor" },
    { 0x86u, DOP_SIMPLE,   0, "i64.shl" },
    { 0x87u, DOP_SIMPLE,   0, "i64.shr_s" },
    { 0x88u, DOP_SIMPLE,   0, "i64.shr_u" },
    { 0x89u, DOP_SIMPLE,   0, "i64.rotl" },
    { 0x8Au, DOP_SIMPLE,   0, "i64.rotr" },

    { 0x8Bu, DOP_SIMPLE,   0, "f32.abs" },
    { 0x8Cu, DOP_SIMPLE,   0, "f32.neg" },
    { 0x8Du, DOP_SIMPLE,   0, "f32.ceil" },
    { 0x8Eu, DOP_SIMPLE,   0, "f32.floor" },
    { 0x8Fu, DOP_SIMPLE,   0, "f32.trunc" },
    { 0x90u, DOP_SIMPLE,   0, "f32.nearest" },
    { 0x91u, DOP_SIMPLE,   0, "f32.sqrt" },
    { 0x92u, DOP_SIMPLE,   0, "f32.add" },
    { 0x93u, DOP_SIMPLE,   0, "f32.sub" },
    { 0x94u, DOP_SIMPLE,   0, "f32.mul" },
    { 0x95u, DOP_SIMPLE,   0, "f32.div" },
    { 0x96u, DOP_SIMPLE,   0, "f32.min" },
    { 0x97u, DOP_SIMPLE,   0, "f32.max" },
    { 0x98u, DOP_SIMPLE,   0, "f32.copysign" },
    { 0x99u, DOP_SIMPLE,   0, "f64.abs" },
    { 0x9Au, DOP_SIMPLE,   0, "f64.neg" },
    { 0x9Bu, DOP_SIMPLE,   0, "f64.ceil" },
    { 0x9Cu, DOP_SIMPLE,   0, "f64.floor" },
    { 0x9Du, DOP_SIMPLE,   0, "f64.trunc" },
    { 0x9Eu, DOP_SIMPLE,   0, "f64.nearest" },
    { 0x9Fu, DOP_SIMPLE,   0, "f64.sqrt" },
    { 0xA0u, DOP_SIMPLE,   0, "f64.add" },
    { 0xA1u, DOP_SIMPLE,   0, "f64.sub" },
    { 0xA2u, DOP_SIMPLE,   0, "f64.mul" },
    { 0xA3u, DOP_SIMPLE,   0, "f64.div" },
    { 0xA4u, DOP_SIMPLE,   0, "f64.min" },
    { 0xA5u, DOP_SIMPLE,   0, "f64.max" },
    { 0xA6u, DOP_SIMPLE,   0, "f64.copysign" },

    { 0xA7u, DOP_SIMPLE,   0, "i32.wrap_i64" },
    { 0xA8u, DOP_SIMPLE,   0, "i32.trunc_f32_s" },
    { 0xA9u, DOP_SIMPLE,   0, "i32.trunc_f32_u" },
    { 0xAAu, DOP_SIMPLE,   0, "i32.trunc_f64_s" },
    { 0xABu, DOP_SIMPLE,   0, "i32.trunc_f64_u" },
    { 0xACu, DOP_SIMPLE,   0, "i64.extend_i32_s" },
    { 0xADu, DOP_SIMPLE,   0, "i64.extend_i32_u" },
    { 0xAEu, DOP_SIMPLE,   0, "i64.trunc_f32_s" },
    { 0xAFu, DOP_SIMPLE,   0, "i64.trunc_f32_u" },
    { 0xB0u, DOP_SIMPLE,   0, "i64.trunc_f64_s" },
    { 0xB1u, DOP_SIMPLE,   0, "i64.trunc_f64_u" },
    { 0xB2u, DOP_SIMPLE,   0, "f32.convert_i32_s" },
    { 0xB3u, DOP_SIMPLE,   0, "f32.convert_i32_u" },
    { 0xB4u, DOP_SIMPLE,   0, "f32.convert_i64_s" },
    { 0xB5u, DOP_SIMPLE,   0, "f32.convert_i64_u" },
    { 0xB6u, DOP_SIMPLE,   0, "f32.demote_f64" },
    { 0xB7u, DOP_SIMPLE,   0, "f64.convert_i32_s" },
    { 0xB8u, DOP_SIMPLE,   0, "f64.convert_i32_u" },
    { 0xB9u, DOP_SIMPLE,   0, "f64.convert_i64_s" },
    { 0xBAu, DOP_SIMPLE,   0, "f64.convert_i64_u" },
    { 0xBBu, DOP_SIMPLE,   0, "f64.promote_f32" },
    { 0xBCu, DOP_SIMPLE,   0, "i32.reinterpret_f32" },
    { 0xBDu, DOP_SIMPLE,   0, "i64.reinterpret_f64" },
    { 0xBEu, DOP_SIMPLE,   0, "f32.reinterpret_i32" },
    { 0xBFu, DOP_SIMPLE,   0, "f64.reinterpret_i64" },
    { 0xC0u, DOP_SIMPLE,   0, "i32.extend8_s" },
    { 0xC1u, DOP_SIMPLE,   0, "i32.extend16_s" },
    { 0xC2u, DOP_SIMPLE,   0, "i64.extend8_s" },
    { 0xC3u, DOP_SIMPLE,   0, "i64.extend16_s" },
    { 0xC4u, DOP_SIMPLE,   0, "i64.extend32_s" },
};

static const MdisOpEntry g_mdis_simd_ops[] = {
    { 0x00u, DOP_SIMD_MEM,      4, "v128.load" },
    { 0x01u, DOP_SIMD_MEM,      3, "v128.load8x8_s" },
    { 0x02u, DOP_SIMD_MEM,      3, "v128.load8x8_u" },
    { 0x03u, DOP_SIMD_MEM,      3, "v128.load16x4_s" },
    { 0x04u, DOP_SIMD_MEM,      3, "v128.load16x4_u" },
    { 0x05u, DOP_SIMD_MEM,      3, "v128.load32x2_s" },
    { 0x06u, DOP_SIMD_MEM,      3, "v128.load32x2_u" },
    { 0x07u, DOP_SIMD_MEM,      0, "v128.load8_splat" },
    { 0x08u, DOP_SIMD_MEM,      1, "v128.load16_splat" },
    { 0x09u, DOP_SIMD_MEM,      2, "v128.load32_splat" },
    { 0x0Au, DOP_SIMD_MEM,      3, "v128.load64_splat" },
    { 0x0Bu, DOP_SIMD_MEM,      4, "v128.store" },
    { 0x0Eu, DOP_SIMD_SIMPLE,   0, "i8x16.swizzle" },
    { 0x0Fu, DOP_SIMD_SIMPLE,   0, "i8x16.splat" },
    { 0x10u, DOP_SIMD_SIMPLE,   0, "i16x8.splat" },
    { 0x11u, DOP_SIMD_SIMPLE,   0, "i32x4.splat" },
    { 0x12u, DOP_SIMD_SIMPLE,   0, "i64x2.splat" },
    { 0x13u, DOP_SIMD_SIMPLE,   0, "f32x4.splat" },
    { 0x14u, DOP_SIMD_SIMPLE,   0, "f64x2.splat" },
    { 0x15u, DOP_SIMD_LANE,     0, "i8x16.extract_lane_s" },
    { 0x16u, DOP_SIMD_LANE,     0, "i8x16.extract_lane_u" },
    { 0x17u, DOP_SIMD_LANE,     0, "i8x16.replace_lane" },
    { 0x18u, DOP_SIMD_LANE,     0, "i16x8.extract_lane_s" },
    { 0x19u, DOP_SIMD_LANE,     0, "i16x8.extract_lane_u" },
    { 0x1Au, DOP_SIMD_LANE,     0, "i16x8.replace_lane" },
    { 0x1Bu, DOP_SIMD_LANE,     0, "i32x4.extract_lane" },
    { 0x1Cu, DOP_SIMD_LANE,     0, "i32x4.replace_lane" },
    { 0x1Du, DOP_SIMD_LANE,     0, "i64x2.extract_lane" },
    { 0x1Eu, DOP_SIMD_LANE,     0, "i64x2.replace_lane" },
    { 0x1Fu, DOP_SIMD_LANE,     0, "f32x4.extract_lane" },
    { 0x20u, DOP_SIMD_LANE,     0, "f32x4.replace_lane" },
    { 0x21u, DOP_SIMD_LANE,     0, "f64x2.extract_lane" },
    { 0x22u, DOP_SIMD_LANE,     0, "f64x2.replace_lane" },

    { 0x23u, DOP_SIMD_SIMPLE,   0, "i8x16.eq" },
    { 0x24u, DOP_SIMD_SIMPLE,   0, "i8x16.ne" },
    { 0x25u, DOP_SIMD_SIMPLE,   0, "i8x16.lt_s" },
    { 0x26u, DOP_SIMD_SIMPLE,   0, "i8x16.lt_u" },
    { 0x27u, DOP_SIMD_SIMPLE,   0, "i8x16.gt_s" },
    { 0x28u, DOP_SIMD_SIMPLE,   0, "i8x16.gt_u" },
    { 0x29u, DOP_SIMD_SIMPLE,   0, "i8x16.le_s" },
    { 0x2Au, DOP_SIMD_SIMPLE,   0, "i8x16.le_u" },
    { 0x2Bu, DOP_SIMD_SIMPLE,   0, "i8x16.ge_s" },
    { 0x2Cu, DOP_SIMD_SIMPLE,   0, "i8x16.ge_u" },

    { 0x2Du, DOP_SIMD_SIMPLE,   0, "i16x8.eq" },
    { 0x2Eu, DOP_SIMD_SIMPLE,   0, "i16x8.ne" },
    { 0x2Fu, DOP_SIMD_SIMPLE,   0, "i16x8.lt_s" },
    { 0x30u, DOP_SIMD_SIMPLE,   0, "i16x8.lt_u" },
    { 0x31u, DOP_SIMD_SIMPLE,   0, "i16x8.gt_s" },
    { 0x32u, DOP_SIMD_SIMPLE,   0, "i16x8.gt_u" },
    { 0x33u, DOP_SIMD_SIMPLE,   0, "i16x8.le_s" },
    { 0x34u, DOP_SIMD_SIMPLE,   0, "i16x8.le_u" },
    { 0x35u, DOP_SIMD_SIMPLE,   0, "i16x8.ge_s" },
    { 0x36u, DOP_SIMD_SIMPLE,   0, "i16x8.ge_u" },

    { 0x37u, DOP_SIMD_SIMPLE,   0, "i32x4.eq" },
    { 0x38u, DOP_SIMD_SIMPLE,   0, "i32x4.ne" },
    { 0x39u, DOP_SIMD_SIMPLE,   0, "i32x4.lt_s" },
    { 0x3Au, DOP_SIMD_SIMPLE,   0, "i32x4.lt_u" },
    { 0x3Bu, DOP_SIMD_SIMPLE,   0, "i32x4.gt_s" },
    { 0x3Cu, DOP_SIMD_SIMPLE,   0, "i32x4.gt_u" },
    { 0x3Du, DOP_SIMD_SIMPLE,   0, "i32x4.le_s" },
    { 0x3Eu, DOP_SIMD_SIMPLE,   0, "i32x4.le_u" },
    { 0x3Fu, DOP_SIMD_SIMPLE,   0, "i32x4.ge_s" },
    { 0x40u, DOP_SIMD_SIMPLE,   0, "i32x4.ge_u" },

    { 0x41u, DOP_SIMD_SIMPLE,   0, "f32x4.eq" },
    { 0x42u, DOP_SIMD_SIMPLE,   0, "f32x4.ne" },
    { 0x43u, DOP_SIMD_SIMPLE,   0, "f32x4.lt" },
    { 0x44u, DOP_SIMD_SIMPLE,   0, "f32x4.gt" },
    { 0x45u, DOP_SIMD_SIMPLE,   0, "f32x4.le" },
    { 0x46u, DOP_SIMD_SIMPLE,   0, "f32x4.ge" },
    { 0x47u, DOP_SIMD_SIMPLE,   0, "f64x2.eq" },
    { 0x48u, DOP_SIMD_SIMPLE,   0, "f64x2.ne" },
    { 0x49u, DOP_SIMD_SIMPLE,   0, "f64x2.lt" },
    { 0x4Au, DOP_SIMD_SIMPLE,   0, "f64x2.gt" },
    { 0x4Bu, DOP_SIMD_SIMPLE,   0, "f64x2.le" },
    { 0x4Cu, DOP_SIMD_SIMPLE,   0, "f64x2.ge" },

    { 0x4Du, DOP_SIMD_SIMPLE,   0, "v128.not" },
    { 0x4Eu, DOP_SIMD_SIMPLE,   0, "v128.and" },
    { 0x4Fu, DOP_SIMD_SIMPLE,   0, "v128.andnot" },
    { 0x50u, DOP_SIMD_SIMPLE,   0, "v128.or" },
    { 0x51u, DOP_SIMD_SIMPLE,   0, "v128.xor" },
    { 0x52u, DOP_SIMD_SIMPLE,   0, "v128.bitselect" },
    { 0x53u, DOP_SIMD_SIMPLE,   0, "v128.any_true" },

    { 0x54u, DOP_SIMD_MEM_LANE, 0, "v128.load8_lane" },
    { 0x55u, DOP_SIMD_MEM_LANE, 1, "v128.load16_lane" },
    { 0x56u, DOP_SIMD_MEM_LANE, 2, "v128.load32_lane" },
    { 0x57u, DOP_SIMD_MEM_LANE, 3, "v128.load64_lane" },
    { 0x58u, DOP_SIMD_MEM_LANE, 0, "v128.store8_lane" },
    { 0x59u, DOP_SIMD_MEM_LANE, 1, "v128.store16_lane" },
    { 0x5Au, DOP_SIMD_MEM_LANE, 2, "v128.store32_lane" },
    { 0x5Bu, DOP_SIMD_MEM_LANE, 3, "v128.store64_lane" },
    { 0x5Cu, DOP_SIMD_MEM,      2, "v128.load32_zero" },
    { 0x5Du, DOP_SIMD_MEM,      3, "v128.load64_zero" },
    { 0x5Eu, DOP_SIMD_SIMPLE,   0, "f32x4.demote_f64x2_zero" },
    { 0x5Fu, DOP_SIMD_SIMPLE,   0, "f64x2.promote_low_f32x4" },

    { 0x60u, DOP_SIMD_SIMPLE,   0, "i8x16.abs" },
    { 0x61u, DOP_SIMD_SIMPLE,   0, "i8x16.neg" },
    { 0x62u, DOP_SIMD_SIMPLE,   0, "i8x16.popcnt" },
    { 0x63u, DOP_SIMD_SIMPLE,   0, "i8x16.all_true" },
    { 0x64u, DOP_SIMD_SIMPLE,   0, "i8x16.bitmask" },
    { 0x65u, DOP_SIMD_SIMPLE,   0, "i8x16.narrow_i16x8_s" },
    { 0x66u, DOP_SIMD_SIMPLE,   0, "i8x16.narrow_i16x8_u" },
    { 0x67u, DOP_SIMD_SIMPLE,   0, "f32x4.ceil" },
    { 0x68u, DOP_SIMD_SIMPLE,   0, "f32x4.floor" },
    { 0x69u, DOP_SIMD_SIMPLE,   0, "f32x4.trunc" },
    { 0x6Au, DOP_SIMD_SIMPLE,   0, "f32x4.nearest" },
    { 0x6Bu, DOP_SIMD_SIMPLE,   0, "i8x16.shl" },
    { 0x6Cu, DOP_SIMD_SIMPLE,   0, "i8x16.shr_s" },
    { 0x6Du, DOP_SIMD_SIMPLE,   0, "i8x16.shr_u" },
    { 0x6Eu, DOP_SIMD_SIMPLE,   0, "i8x16.add" },
    { 0x6Fu, DOP_SIMD_SIMPLE,   0, "i8x16.add_sat_s" },
    { 0x70u, DOP_SIMD_SIMPLE,   0, "i8x16.add_sat_u" },
    { 0x71u, DOP_SIMD_SIMPLE,   0, "i8x16.sub" },
    { 0x72u, DOP_SIMD_SIMPLE,   0, "i8x16.sub_sat_s" },
    { 0x73u, DOP_SIMD_SIMPLE,   0, "i8x16.sub_sat_u" },
    { 0x74u, DOP_SIMD_SIMPLE,   0, "f64x2.ceil" },
    { 0x75u, DOP_SIMD_SIMPLE,   0, "f64x2.floor" },
    { 0x76u, DOP_SIMD_SIMPLE,   0, "i8x16.min_s" },
    { 0x77u, DOP_SIMD_SIMPLE,   0, "i8x16.min_u" },
    { 0x78u, DOP_SIMD_SIMPLE,   0, "i8x16.max_s" },
    { 0x79u, DOP_SIMD_SIMPLE,   0, "i8x16.max_u" },
    { 0x7Au, DOP_SIMD_SIMPLE,   0, "f64x2.trunc" },
    { 0x7Bu, DOP_SIMD_SIMPLE,   0, "i8x16.avgr_u" },
    { 0x7Cu, DOP_SIMD_SIMPLE,   0, "i16x8.extadd_pairwise_i8x16_s" },
    { 0x7Du, DOP_SIMD_SIMPLE,   0, "i16x8.extadd_pairwise_i8x16_u" },
    { 0x7Eu, DOP_SIMD_SIMPLE,   0, "i32x4.extadd_pairwise_i16x8_s" },
    { 0x7Fu, DOP_SIMD_SIMPLE,   0, "i32x4.extadd_pairwise_i16x8_u" },

    { 0x80u, DOP_SIMD_SIMPLE,   0, "i16x8.abs" },
    { 0x81u, DOP_SIMD_SIMPLE,   0, "i16x8.neg" },
    { 0x82u, DOP_SIMD_SIMPLE,   0, "i16x8.q15mulr_sat_s" },
    { 0x83u, DOP_SIMD_SIMPLE,   0, "i16x8.all_true" },
    { 0x84u, DOP_SIMD_SIMPLE,   0, "i16x8.bitmask" },
    { 0x85u, DOP_SIMD_SIMPLE,   0, "i16x8.narrow_i32x4_s" },
    { 0x86u, DOP_SIMD_SIMPLE,   0, "i16x8.narrow_i32x4_u" },
    { 0x87u, DOP_SIMD_SIMPLE,   0, "i16x8.extend_low_i8x16_s" },
    { 0x88u, DOP_SIMD_SIMPLE,   0, "i16x8.extend_high_i8x16_s" },
    { 0x89u, DOP_SIMD_SIMPLE,   0, "i16x8.extend_low_i8x16_u" },
    { 0x8Au, DOP_SIMD_SIMPLE,   0, "i16x8.extend_high_i8x16_u" },
    { 0x8Bu, DOP_SIMD_SIMPLE,   0, "i16x8.shl" },
    { 0x8Cu, DOP_SIMD_SIMPLE,   0, "i16x8.shr_s" },
    { 0x8Du, DOP_SIMD_SIMPLE,   0, "i16x8.shr_u" },
    { 0x8Eu, DOP_SIMD_SIMPLE,   0, "i16x8.add" },
    { 0x8Fu, DOP_SIMD_SIMPLE,   0, "i16x8.add_sat_s" },
    { 0x90u, DOP_SIMD_SIMPLE,   0, "i16x8.add_sat_u" },
    { 0x91u, DOP_SIMD_SIMPLE,   0, "i16x8.sub" },
    { 0x92u, DOP_SIMD_SIMPLE,   0, "i16x8.sub_sat_s" },
    { 0x93u, DOP_SIMD_SIMPLE,   0, "i16x8.sub_sat_u" },
    { 0x94u, DOP_SIMD_SIMPLE,   0, "f64x2.nearest" },
    { 0x95u, DOP_SIMD_SIMPLE,   0, "i16x8.mul" },
    { 0x96u, DOP_SIMD_SIMPLE,   0, "i16x8.min_s" },
    { 0x97u, DOP_SIMD_SIMPLE,   0, "i16x8.min_u" },
    { 0x98u, DOP_SIMD_SIMPLE,   0, "i16x8.max_s" },
    { 0x99u, DOP_SIMD_SIMPLE,   0, "i16x8.max_u" },
    { 0x9Bu, DOP_SIMD_SIMPLE,   0, "i16x8.avgr_u" },
    { 0x9Cu, DOP_SIMD_SIMPLE,   0, "i16x8.extmul_low_i8x16_s" },
    { 0x9Du, DOP_SIMD_SIMPLE,   0, "i16x8.extmul_high_i8x16_s" },
    { 0x9Eu, DOP_SIMD_SIMPLE,   0, "i16x8.extmul_low_i8x16_u" },
    { 0x9Fu, DOP_SIMD_SIMPLE,   0, "i16x8.extmul_high_i8x16_u" },

    { 0xA0u, DOP_SIMD_SIMPLE,   0, "i32x4.abs" },
    { 0xA1u, DOP_SIMD_SIMPLE,   0, "i32x4.neg" },
    { 0xA3u, DOP_SIMD_SIMPLE,   0, "i32x4.all_true" },
    { 0xA4u, DOP_SIMD_SIMPLE,   0, "i32x4.bitmask" },
    { 0xA7u, DOP_SIMD_SIMPLE,   0, "i32x4.extend_low_i16x8_s" },
    { 0xA8u, DOP_SIMD_SIMPLE,   0, "i32x4.extend_high_i16x8_s" },
    { 0xA9u, DOP_SIMD_SIMPLE,   0, "i32x4.extend_low_i16x8_u" },
    { 0xAAu, DOP_SIMD_SIMPLE,   0, "i32x4.extend_high_i16x8_u" },
    { 0xABu, DOP_SIMD_SIMPLE,   0, "i32x4.shl" },
    { 0xACu, DOP_SIMD_SIMPLE,   0, "i32x4.shr_s" },
    { 0xADu, DOP_SIMD_SIMPLE,   0, "i32x4.shr_u" },
    { 0xAEu, DOP_SIMD_SIMPLE,   0, "i32x4.add" },
    { 0xB1u, DOP_SIMD_SIMPLE,   0, "i32x4.sub" },
    { 0xB5u, DOP_SIMD_SIMPLE,   0, "i32x4.mul" },
    { 0xB6u, DOP_SIMD_SIMPLE,   0, "i32x4.min_s" },
    { 0xB7u, DOP_SIMD_SIMPLE,   0, "i32x4.min_u" },
    { 0xB8u, DOP_SIMD_SIMPLE,   0, "i32x4.max_s" },
    { 0xB9u, DOP_SIMD_SIMPLE,   0, "i32x4.max_u" },
    { 0xBAu, DOP_SIMD_SIMPLE,   0, "i32x4.dot_i16x8_s" },
    { 0xBCu, DOP_SIMD_SIMPLE,   0, "i32x4.extmul_low_i16x8_s" },
    { 0xBDu, DOP_SIMD_SIMPLE,   0, "i32x4.extmul_high_i16x8_s" },
    { 0xBEu, DOP_SIMD_SIMPLE,   0, "i32x4.extmul_low_i16x8_u" },
    { 0xBFu, DOP_SIMD_SIMPLE,   0, "i32x4.extmul_high_i16x8_u" },

    { 0xC0u, DOP_SIMD_SIMPLE,   0, "i64x2.abs" },
    { 0xC1u, DOP_SIMD_SIMPLE,   0, "i64x2.neg" },
    { 0xC3u, DOP_SIMD_SIMPLE,   0, "i64x2.all_true" },
    { 0xC4u, DOP_SIMD_SIMPLE,   0, "i64x2.bitmask" },
    { 0xC7u, DOP_SIMD_SIMPLE,   0, "i64x2.extend_low_i32x4_s" },
    { 0xC8u, DOP_SIMD_SIMPLE,   0, "i64x2.extend_high_i32x4_s" },
    { 0xC9u, DOP_SIMD_SIMPLE,   0, "i64x2.extend_low_i32x4_u" },
    { 0xCAu, DOP_SIMD_SIMPLE,   0, "i64x2.extend_high_i32x4_u" },
    { 0xCBu, DOP_SIMD_SIMPLE,   0, "i64x2.shl" },
    { 0xCCu, DOP_SIMD_SIMPLE,   0, "i64x2.shr_s" },
    { 0xCDu, DOP_SIMD_SIMPLE,   0, "i64x2.shr_u" },
    { 0xCEu, DOP_SIMD_SIMPLE,   0, "i64x2.add" },
    { 0xD1u, DOP_SIMD_SIMPLE,   0, "i64x2.sub" },
    { 0xD5u, DOP_SIMD_SIMPLE,   0, "i64x2.mul" },
    { 0xD6u, DOP_SIMD_SIMPLE,   0, "i64x2.eq" },
    { 0xD7u, DOP_SIMD_SIMPLE,   0, "i64x2.ne" },
    { 0xD8u, DOP_SIMD_SIMPLE,   0, "i64x2.lt_s" },
    { 0xD9u, DOP_SIMD_SIMPLE,   0, "i64x2.gt_s" },
    { 0xDAu, DOP_SIMD_SIMPLE,   0, "i64x2.le_s" },
    { 0xDBu, DOP_SIMD_SIMPLE,   0, "i64x2.ge_s" },
    { 0xDCu, DOP_SIMD_SIMPLE,   0, "i64x2.extmul_low_i32x4_s" },
    { 0xDDu, DOP_SIMD_SIMPLE,   0, "i64x2.extmul_high_i32x4_s" },
    { 0xDEu, DOP_SIMD_SIMPLE,   0, "i64x2.extmul_low_i32x4_u" },
    { 0xDFu, DOP_SIMD_SIMPLE,   0, "i64x2.extmul_high_i32x4_u" },

    { 0xE0u, DOP_SIMD_SIMPLE,   0, "f32x4.abs" },
    { 0xE1u, DOP_SIMD_SIMPLE,   0, "f32x4.neg" },
    { 0xE3u, DOP_SIMD_SIMPLE,   0, "f32x4.sqrt" },
    { 0xE4u, DOP_SIMD_SIMPLE,   0, "f32x4.add" },
    { 0xE5u, DOP_SIMD_SIMPLE,   0, "f32x4.sub" },
    { 0xE6u, DOP_SIMD_SIMPLE,   0, "f32x4.mul" },
    { 0xE7u, DOP_SIMD_SIMPLE,   0, "f32x4.div" },
    { 0xE8u, DOP_SIMD_SIMPLE,   0, "f32x4.min" },
    { 0xE9u, DOP_SIMD_SIMPLE,   0, "f32x4.max" },
    { 0xEAu, DOP_SIMD_SIMPLE,   0, "f32x4.pmin" },
    { 0xEBu, DOP_SIMD_SIMPLE,   0, "f32x4.pmax" },
    { 0xECu, DOP_SIMD_SIMPLE,   0, "f64x2.abs" },
    { 0xEDu, DOP_SIMD_SIMPLE,   0, "f64x2.neg" },
    { 0xEFu, DOP_SIMD_SIMPLE,   0, "f64x2.sqrt" },
    { 0xF0u, DOP_SIMD_SIMPLE,   0, "f64x2.add" },
    { 0xF1u, DOP_SIMD_SIMPLE,   0, "f64x2.sub" },
    { 0xF2u, DOP_SIMD_SIMPLE,   0, "f64x2.mul" },
    { 0xF3u, DOP_SIMD_SIMPLE,   0, "f64x2.div" },
    { 0xF4u, DOP_SIMD_SIMPLE,   0, "f64x2.min" },
    { 0xF5u, DOP_SIMD_SIMPLE,   0, "f64x2.max" },
    { 0xF6u, DOP_SIMD_SIMPLE,   0, "f64x2.pmin" },
    { 0xF7u, DOP_SIMD_SIMPLE,   0, "f64x2.pmax" },
    { 0xF8u, DOP_SIMD_SIMPLE,   0, "i32x4.trunc_sat_f32x4_s" },
    { 0xF9u, DOP_SIMD_SIMPLE,   0, "i32x4.trunc_sat_f32x4_u" },
    { 0xFAu, DOP_SIMD_SIMPLE,   0, "f32x4.convert_i32x4_s" },
    { 0xFBu, DOP_SIMD_SIMPLE,   0, "f32x4.convert_i32x4_u" },
    { 0xFCu, DOP_SIMD_SIMPLE,   0, "i32x4.trunc_sat_f64x2_s_zero" },
    { 0xFDu, DOP_SIMD_SIMPLE,   0, "i32x4.trunc_sat_f64x2_u_zero" },
    { 0xFEu, DOP_SIMD_SIMPLE,   0, "f64x2.convert_low_i32x4_s" },
    { 0xFFu, DOP_SIMD_SIMPLE,   0, "f64x2.convert_low_i32x4_u" },
};

static const MdisOpEntry* mdis_find_std_op(u8 op) {
    for (usize i = 0; i < ARRAY_LEN(g_mdis_std_ops); i++) {
        if (g_mdis_std_ops[i].code == op) return &g_mdis_std_ops[i];
    }
    return NULL;
}

static const MdisOpEntry* mdis_find_simd_op(u32 subop) {
    for (usize i = 0; i < ARRAY_LEN(g_mdis_simd_ops); i++) {
        if (g_mdis_simd_ops[i].code == subop) return &g_mdis_simd_ops[i];
    }
    return NULL;
}

static b32 mdis_emit_memarg(MdisReader* r, MdisOut* o, u32 def_align) {
    u32 align_lg = 0, offset = 0;
    if (!mr_u32(r, &align_lg) || !mr_u32(r, &offset)) return FALSE;
    if (offset != 0) {
        mdis_printf(o, " offset=%u", offset);
    }
    if (align_lg != def_align) {
        mdis_printf(o, " align=%u", 1u << align_lg);
    }
    return TRUE;
}

static b32 mdis_disassemble_code_body(MdisReader* r, u32 end_pos, MdisOut* o) {
    u32 depth = 0;
    u32 indent = 2;

    while (r->pos < end_pos) {
        u8 op = 0;
        if (!mr_u8(r, &op)) return FALSE;

        if (op == 0x0Bu) { /* end */
            if (depth == 0) {
                /* Fonksiyon gövdesi sonu */
                if (r->pos != end_pos) {
                    mr_err(r, "extra bytes after function end");
                    return FALSE;
                }
                return TRUE;
            }
            depth--;
            indent--;
            mdis_indent(o, indent);
            mdis_puts(o, "end\n");
            continue;
        }

        if (op == 0x05u) { /* else */
            if (depth == 0) {
                mr_err(r, "unexpected else at depth 0");
                return FALSE;
            }
            mdis_indent(o, indent - 1);
            mdis_puts(o, "else\n");
            continue;
        }

        mdis_indent(o, indent);

        if (op == 0x02u || op == 0x03u || op == 0x04u) {
            if (r->pos >= end_pos) {
                mr_err(r, "unexpected EOF reading blocktype");
                return FALSE;
            }
            if (op == 0x02u) mdis_puts(o, "block");
            else if (op == 0x03u) mdis_puts(o, "loop");
            else mdis_puts(o, "if");

            u8 peek_bt = r->p[r->pos];
            if ((peek_bt & 0xC0u) == 0x40u) {
                u8 bt = 0;
                if (!mr_u8(r, &bt)) return FALSE;
                if (bt != MB_BLOCK_VOID) {
                    const char* ts = mdis_valtype_str(bt);
                    if (!ts) {
                        mr_err(r, "unsupported blocktype 0x%x", bt);
                        return FALSE;
                    }
                    mdis_printf(o, " (result %s)", ts);
                }
            } else {
                i64 tidx = 0;
                if (!mr_sleb(r, &tidx) || tidx < 0) {
                    mr_err(r, "invalid block typeidx");
                    return FALSE;
                }
                mdis_printf(o, " (type $t%u)", (u32)tidx);
            }
            mdis_putc(o, '\n');
            depth++;
            indent++;
            continue;
        }

        if (op == 0x0Cu || op == 0x0Du) {
            u32 target = 0;
            if (!mr_u32(r, &target)) return FALSE;
            mdis_printf(o, "%s %u\n", (op == 0x0Cu) ? "br" : "br_if", target);
            continue;
        }

        if (op == 0x0Eu) {
            u32 cnt = 0;
            if (!mr_u32(r, &cnt)) return FALSE;
            mdis_puts(o, "br_table");
            for (u32 i = 0; i <= cnt; i++) {
                u32 target = 0;
                if (!mr_u32(r, &target)) return FALSE;
                mdis_printf(o, " %u", target);
            }
            mdis_putc(o, '\n');
            continue;
        }

        if (op == 0x10u) {
            u32 fidx = 0;
            if (!mr_u32(r, &fidx)) return FALSE;
            mdis_printf(o, "call $f%u\n", fidx);
            continue;
        }

        if (op == 0x11u) {
            u32 tidx = 0;
            u8 tbl = 0;
            if (!mr_u32(r, &tidx) || !mr_u8(r, &tbl)) return FALSE;
            mdis_printf(o, "call_indirect (type $t%u)\n", tidx);
            continue;
        }

        if (op == 0x20u || op == 0x21u || op == 0x22u) {
            u32 lidx = 0;
            if (!mr_u32(r, &lidx)) return FALSE;
            const char* mn = (op == 0x20u) ? "local.get" : (op == 0x21u) ? "local.set" : "local.tee";
            mdis_printf(o, "%s %u\n", mn, lidx);
            continue;
        }

        if (op == 0x23u || op == 0x24u) {
            u32 gidx = 0;
            if (!mr_u32(r, &gidx)) return FALSE;
            mdis_printf(o, "%s $g%u\n", (op == 0x23u) ? "global.get" : "global.set", gidx);
            continue;
        }

        if (op == 0x41u) {
            i64 v = 0;
            if (!mr_sleb(r, &v)) return FALSE;
            mdis_printf(o, "i32.const %d\n", (i32)v);
            continue;
        }

        if (op == 0x42u) {
            i64 v = 0;
            if (!mr_sleb(r, &v)) return FALSE;
            mdis_printf(o, "i64.const %lld\n", (long long)v);
            continue;
        }

        if (op == 0x43u) {
            u32 bits = 0;
            if (!mr_u32_le(r, &bits)) return FALSE;
            mdis_puts(o, "f32.const ");
            mdis_fmt_f32_exact(o, bits);
            mdis_putc(o, '\n');
            continue;
        }

        if (op == 0x44u) {
            u64 bits = 0;
            if (!mr_u64_le(r, &bits)) return FALSE;
            mdis_puts(o, "f64.const ");
            mdis_fmt_f64_exact(o, bits);
            mdis_putc(o, '\n');
            continue;
        }

        if (op == 0xFCu) {
            u32 subop = 0;
            if (!mr_u32(r, &subop)) return FALSE;
            if (subop == 0u)      mdis_puts(o, "i32.trunc_sat_f32_s\n");
            else if (subop == 1u) mdis_puts(o, "i32.trunc_sat_f32_u\n");
            else if (subop == 2u) mdis_puts(o, "i32.trunc_sat_f64_s\n");
            else if (subop == 3u) mdis_puts(o, "i32.trunc_sat_f64_u\n");
            else if (subop == 4u) mdis_puts(o, "i64.trunc_sat_f32_s\n");
            else if (subop == 5u) mdis_puts(o, "i64.trunc_sat_f32_u\n");
            else if (subop == 6u) mdis_puts(o, "i64.trunc_sat_f64_s\n");
            else if (subop == 7u) mdis_puts(o, "i64.trunc_sat_f64_u\n");
            else if (subop == 10u) {
                u8 m0 = 0, m1 = 0;
                if (!mr_u8(r, &m0) || !mr_u8(r, &m1)) return FALSE;
                mdis_puts(o, "memory.copy\n");
            } else if (subop == 11u) {
                u8 m0 = 0;
                if (!mr_u8(r, &m0)) return FALSE;
                mdis_puts(o, "memory.fill\n");
            } else {
                mr_err(r, "unsupported 0xFC subopcode %u", subop);
                return FALSE;
            }
            continue;
        }

        if (op == 0xFDu) {
            u32 subop = 0;
            if (!mr_u32(r, &subop)) return FALSE;
            if (subop == 0x0Cu) { /* v128.const */
                mdis_puts(o, "v128.const i32x4");
                for (u32 lane = 0; lane < 4; lane++) {
                    u32 u = 0;
                    if (!mr_u32_le(r, &u)) return FALSE;
                    mdis_printf(o, " %d", (i32)u);
                }
                mdis_putc(o, '\n');
                continue;
            }
            if (subop == 0x0Du) { /* i8x16.shuffle */
                mdis_puts(o, "i8x16.shuffle");
                for (u32 lane = 0; lane < 16; lane++) {
                    u8 l = 0;
                    if (!mr_u8(r, &l)) return FALSE;
                    mdis_printf(o, " %u", (u32)l);
                }
                mdis_putc(o, '\n');
                continue;
            }
            const MdisOpEntry* se = mdis_find_simd_op(subop);
            if (!se) {
                mr_err(r, "unsupported SIMD subopcode 0x%x", subop);
                return FALSE;
            }
            mdis_puts(o, se->name);
            if (se->kind == DOP_SIMD_MEM) {
                if (!mdis_emit_memarg(r, o, se->def_align)) return FALSE;
            } else if (se->kind == DOP_SIMD_MEM_LANE) {
                if (!mdis_emit_memarg(r, o, se->def_align)) return FALSE;
                u8 lane = 0;
                if (!mr_u8(r, &lane)) return FALSE;
                mdis_printf(o, " %u", (u32)lane);
            } else if (se->kind == DOP_SIMD_LANE) {
                u8 lane = 0;
                if (!mr_u8(r, &lane)) return FALSE;
                mdis_printf(o, " %u", (u32)lane);
            }
            mdis_putc(o, '\n');
            continue;
        }

        const MdisOpEntry* e = mdis_find_std_op(op);
        if (!e) {
            mr_err(r, "unsupported opcode 0x%02x", op);
            return FALSE;
        }
        mdis_puts(o, e->name);
        if (e->kind == DOP_MEM) {
            if (!mdis_emit_memarg(r, o, e->def_align)) return FALSE;
        } else if (e->kind == DOP_MEM_ZERO) {
            u8 mem_idx = 0;
            if (!mr_u8(r, &mem_idx)) return FALSE;
        }
        mdis_putc(o, '\n');
    }

    mr_err(r, "missing function end (0x0B)");
    return FALSE;
}

/* ============================================================================
 * 3. Ana Çözücü (mdis_disassemble)
 * ============================================================================ */

MdisResult mdis_disassemble(Arena* arena, const u8* wasm, u32 wasm_len) {
    ASSERT(arena != NULL);
    MdisResult res;
    mdis_memzero(&res, sizeof(res));

    usize mark = arena_mark(arena);
    MdisReader r;
    mdis_memzero(&r, sizeof(r));
    r.p = wasm;
    r.len = wasm_len;

    u32 out_cap = 131072;
    char* out_buf = ARENA_PUSH_ARRAY(arena, char, out_cap);
    MdisOut o = { out_buf, 0, out_cap };

    /* 1. Sihirli başlık ve sürüm kontrolü */
    const u8* hdr = NULL;
    if (!mr_bytes(&r, &hdr, 8)) goto fail;
    if (hdr[0] != 0x00u || hdr[1] != 0x61u || hdr[2] != 0x73u || hdr[3] != 0x6Du ||
        hdr[4] != 0x01u || hdr[5] != 0x00u || hdr[6] != 0x00u || hdr[7] != 0x00u) {
        mr_err(&r, "invalid WebAssembly magic header or version");
        goto fail;
    }

    /* Bölüm verilerini tutacak ara yapılar */
    MbFuncType* types = ARENA_PUSH_ARRAY(arena, MbFuncType, MB_MAX_TYPES);
    u32 type_count = 0;

    u32* func_types = ARENA_PUSH_ARRAY(arena, u32, MB_MAX_FUNCS);
    u32 def_func_count = 0;
    u32 imp_func_count = 0;
    u32 imp_global_count = 0;

    u32 code_sec_pos = 0;
    u32 code_sec_end = 0;

    mdis_puts(&o, "(module\n");

    /* 2. Bölümleri sırayla oku */
    while (r.pos < r.len) {
        u8 sec_id = 0;
        u32 sec_len = 0;
        if (!mr_u8(&r, &sec_id) || !mr_u32(&r, &sec_len)) goto fail;
        if (sec_len > r.len - r.pos) {
            mr_err(&r, "section %u length exceeds module size", sec_id);
            goto fail;
        }
        u32 sec_end = r.pos + sec_len;

        switch (sec_id) {
        case MB_SEC_CUSTOM:
            r.pos = sec_end;
            break;

        case MB_SEC_TYPE: {
            if (!mr_u32(&r, &type_count) || type_count > MB_MAX_TYPES) {
                mr_err(&r, "invalid type count");
                goto fail;
            }
            for (u32 i = 0; i < type_count; i++) {
                u8 form = 0;
                if (!mr_u8(&r, &form) || form != 0x60u) {
                    mr_err(&r, "expected functype 0x60");
                    goto fail;
                }
                MbFuncType* t = &types[i];
                if (!mr_u32(&r, &t->param_count) || t->param_count > MB_MAX_PARAMS) goto fail;
                for (u32 j = 0; j < t->param_count; j++) {
                    if (!mr_u8(&r, &t->params[j])) goto fail;
                }
                if (!mr_u32(&r, &t->result_count) || t->result_count > MB_MAX_RESULTS) goto fail;
                for (u32 j = 0; j < t->result_count; j++) {
                    if (!mr_u8(&r, &t->results[j])) goto fail;
                }

                mdis_printf(&o, "  (type $t%u (func", i);
                if (t->param_count > 0) {
                    mdis_puts(&o, " (param");
                    for (u32 j = 0; j < t->param_count; j++) {
                        const char* ts = mdis_valtype_str(t->params[j]);
                        if (!ts) { mr_err(&r, "invalid param valtype"); goto fail; }
                        mdis_printf(&o, " %s", ts);
                    }
                    mdis_putc(&o, ')');
                }
                if (t->result_count > 0) {
                    mdis_puts(&o, " (result");
                    for (u32 j = 0; j < t->result_count; j++) {
                        const char* ts = mdis_valtype_str(t->results[j]);
                        if (!ts) { mr_err(&r, "invalid result valtype"); goto fail; }
                        mdis_printf(&o, " %s", ts);
                    }
                    mdis_putc(&o, ')');
                }
                mdis_puts(&o, "))\n");
            }
        } break;

        case MB_SEC_IMPORT: {
            u32 imp_count = 0;
            if (!mr_u32(&r, &imp_count) || imp_count > MB_MAX_IMPORTS) goto fail;
            for (u32 i = 0; i < imp_count; i++) {
                u32 mlen = 0, flen = 0;
                const u8* mptr = NULL;
                const u8* fptr = NULL;
                u8 kind = 0;
                if (!mr_u32(&r, &mlen) || !mr_bytes(&r, &mptr, mlen)) goto fail;
                if (!mr_u32(&r, &flen) || !mr_bytes(&r, &fptr, flen)) goto fail;
                if (!mr_u8(&r, &kind)) goto fail;

                mdis_puts(&o, "  (import ");
                mdis_escaped_bytes(&o, mptr, mlen);
                mdis_putc(&o, ' ');
                mdis_escaped_bytes(&o, fptr, flen);
                mdis_putc(&o, ' ');

                if (kind == MB_EXT_FUNC) {
                    u32 tidx = 0;
                    if (!mr_u32(&r, &tidx) || tidx >= type_count) {
                        mr_err(&r, "invalid import func type_idx");
                        goto fail;
                    }
                    mdis_printf(&o, "(func $f%u (type $t%u)))\n", imp_func_count++, tidx);
                } else if (kind == MB_EXT_TABLE) {
                    u8 elem_type = 0, flags = 0;
                    u32 min_v = 0, max_v = 0;
                    if (!mr_u8(&r, &elem_type) || !mr_u8(&r, &flags) || !mr_u32(&r, &min_v)) goto fail;
                    if (flags & 0x01u) {
                        if (!mr_u32(&r, &max_v)) goto fail;
                        mdis_printf(&o, "(table %u %u funcref))\n", min_v, max_v);
                    } else {
                        mdis_printf(&o, "(table %u funcref))\n", min_v);
                    }
                } else if (kind == MB_EXT_MEMORY) {
                    u8 flags = 0;
                    u32 min_v = 0, max_v = 0;
                    if (!mr_u8(&r, &flags) || !mr_u32(&r, &min_v)) goto fail;
                    if (flags & 0x01u) {
                        if (!mr_u32(&r, &max_v)) goto fail;
                        mdis_printf(&o, "(memory %u %u%s))\n", min_v, max_v,
                                    (flags & 0x02u) ? " shared" : "");
                    } else {
                        mdis_printf(&o, "(memory %u))\n", min_v);
                    }
                } else if (kind == MB_EXT_GLOBAL) {
                    u8 vt = 0, mut = 0;
                    if (!mr_u8(&r, &vt) || !mr_u8(&r, &mut)) goto fail;
                    const char* ts = mdis_valtype_str(vt);
                    if (!ts) { mr_err(&r, "invalid global valtype"); goto fail; }
                    if (mut) {
                        mdis_printf(&o, "(global $g%u (mut %s)))\n", imp_global_count++, ts);
                    } else {
                        mdis_printf(&o, "(global $g%u %s))\n", imp_global_count++, ts);
                    }
                } else {
                    mr_err(&r, "unsupported import kind %u", kind);
                    goto fail;
                }
            }
        } break;

        case MB_SEC_FUNC: {
            if (!mr_u32(&r, &def_func_count) || def_func_count > MB_MAX_FUNCS) goto fail;
            for (u32 i = 0; i < def_func_count; i++) {
                if (!mr_u32(&r, &func_types[i]) || func_types[i] >= type_count) {
                    mr_err(&r, "invalid func type index");
                    goto fail;
                }
            }
        } break;

        case MB_SEC_TABLE: {
            u32 tcount = 0;
            if (!mr_u32(&r, &tcount) || tcount != 1) goto fail;
            u8 et = 0, flags = 0;
            u32 min_v = 0, max_v = 0;
            if (!mr_u8(&r, &et) || !mr_u8(&r, &flags) || !mr_u32(&r, &min_v)) goto fail;
            if (flags & 0x01u) {
                if (!mr_u32(&r, &max_v)) goto fail;
                mdis_printf(&o, "  (table %u %u funcref)\n", min_v, max_v);
            } else {
                mdis_printf(&o, "  (table %u funcref)\n", min_v);
            }
        } break;

        case MB_SEC_MEMORY: {
            u32 mcount = 0;
            if (!mr_u32(&r, &mcount) || mcount != 1) goto fail;
            u8 flags = 0;
            u32 min_v = 0, max_v = 0;
            if (!mr_u8(&r, &flags) || !mr_u32(&r, &min_v)) goto fail;
            if (flags & 0x01u) {
                if (!mr_u32(&r, &max_v)) goto fail;
                mdis_printf(&o, "  (memory %u %u%s)\n", min_v, max_v,
                            (flags & 0x02u) ? " shared" : "");
            } else {
                mdis_printf(&o, "  (memory %u)\n", min_v);
            }
        } break;

        case MB_SEC_GLOBAL: {
            u32 gcount = 0;
            if (!mr_u32(&r, &gcount) || gcount > MB_MAX_GLOBALS) goto fail;
            for (u32 i = 0; i < gcount; i++) {
                u8 vt = 0, mut = 0, init_op = 0;
                if (!mr_u8(&r, &vt) || !mr_u8(&r, &mut) || !mr_u8(&r, &init_op)) goto fail;
                const char* ts = mdis_valtype_str(vt);
                if (!ts) { mr_err(&r, "invalid global valtype"); goto fail; }
                u32 gidx = imp_global_count + i;
                if (mut) {
                    mdis_printf(&o, "  (global $g%u (mut %s) ", gidx, ts);
                } else {
                    mdis_printf(&o, "  (global $g%u %s ", gidx, ts);
                }

                if (init_op == 0x41u) {
                    i64 v = 0;
                    if (!mr_sleb(&r, &v)) goto fail;
                    mdis_printf(&o, "(i32.const %d)", (i32)v);
                } else if (init_op == 0x42u) {
                    i64 v = 0;
                    if (!mr_sleb(&r, &v)) goto fail;
                    mdis_printf(&o, "(i64.const %lld)", (long long)v);
                } else if (init_op == 0x43u) {
                    u32 bits = 0;
                    if (!mr_u32_le(&r, &bits)) goto fail;
                    mdis_puts(&o, "(f32.const ");
                    mdis_fmt_f32_exact(&o, bits);
                    mdis_putc(&o, ')');
                } else if (init_op == 0x44u) {
                    u64 bits = 0;
                    if (!mr_u64_le(&r, &bits)) goto fail;
                    mdis_puts(&o, "(f64.const ");
                    mdis_fmt_f64_exact(&o, bits);
                    mdis_putc(&o, ')');
                } else {
                    mr_err(&r, "unsupported global init opcode 0x%x", init_op);
                    goto fail;
                }
                u8 end_op = 0;
                if (!mr_u8(&r, &end_op) || end_op != 0x0Bu) {
                    mr_err(&r, "expected 0x0B after global init expr");
                    goto fail;
                }
                mdis_puts(&o, ")\n");
            }
        } break;

        case MB_SEC_EXPORT: {
            u32 ecount = 0;
            if (!mr_u32(&r, &ecount) || ecount > MB_MAX_EXPORTS) goto fail;
            for (u32 i = 0; i < ecount; i++) {
                u32 nlen = 0;
                const u8* nptr = NULL;
                u8 kind = 0;
                u32 idx = 0;
                if (!mr_u32(&r, &nlen) || !mr_bytes(&r, &nptr, nlen)) goto fail;
                if (!mr_u8(&r, &kind) || !mr_u32(&r, &idx)) goto fail;

                mdis_puts(&o, "  (export ");
                mdis_escaped_bytes(&o, nptr, nlen);
                if (kind == MB_EXT_FUNC) mdis_printf(&o, " (func $f%u))\n", idx);
                else if (kind == MB_EXT_TABLE) mdis_printf(&o, " (table %u))\n", idx);
                else if (kind == MB_EXT_MEMORY) mdis_printf(&o, " (memory %u))\n", idx);
                else if (kind == MB_EXT_GLOBAL) mdis_printf(&o, " (global $g%u))\n", idx);
                else { mr_err(&r, "invalid export kind"); goto fail; }
            }
        } break;

        case MB_SEC_ELEM: {
            u32 el_count = 0;
            if (!mr_u32(&r, &el_count) || el_count > MB_MAX_ELEMS) goto fail;
            for (u32 i = 0; i < el_count; i++) {
                u8 flags = 0, op = 0, end_op = 0;
                i64 off = 0;
                u32 fcount = 0;
                if (!mr_u8(&r, &flags) || flags != 0x00u) {
                    mr_err(&r, "unsupported elem segment flags");
                    goto fail;
                }
                if (!mr_u8(&r, &op) || op != 0x41u || !mr_sleb(&r, &off) ||
                    !mr_u8(&r, &end_op) || end_op != 0x0Bu || !mr_u32(&r, &fcount)) {
                    goto fail;
                }
                mdis_printf(&o, "  (elem (i32.const %d) func", (i32)off);
                for (u32 j = 0; j < fcount; j++) {
                    u32 fidx = 0;
                    if (!mr_u32(&r, &fidx)) goto fail;
                    mdis_printf(&o, " $f%u", fidx);
                }
                mdis_puts(&o, ")\n");
            }
        } break;

        case MB_SEC_CODE:
            code_sec_pos = r.pos;
            code_sec_end = sec_end;
            r.pos = sec_end;
            break;

        case MB_SEC_DATA: {
            /* Eğer Code bölümü varsa Data'dan önce fonksiyonları yazdır */
            if (code_sec_pos != 0) {
                u32 saved_pos = r.pos;
                r.pos = code_sec_pos;
                u32 ccount = 0;
                if (!mr_u32(&r, &ccount) || ccount != def_func_count) {
                    mr_err(&r, "code count mismatch with func section");
                    goto fail;
                }
                for (u32 i = 0; i < ccount; i++) {
                    u32 body_size = 0;
                    if (!mr_u32(&r, &body_size) || body_size > code_sec_end - r.pos) goto fail;
                    u32 body_end = r.pos + body_size;
                    u32 fidx = imp_func_count + i;
                    u32 tidx = func_types[i];
                    mdis_printf(&o, "  (func $f%u (type $t%u)\n", fidx, tidx);

                    u32 lgroup_count = 0;
                    if (!mr_u32(&r, &lgroup_count)) goto fail;
                    for (u32 g = 0; g < lgroup_count; g++) {
                        u32 cnt = 0;
                        u8 vt = 0;
                        if (!mr_u32(&r, &cnt) || !mr_u8(&r, &vt)) goto fail;
                        const char* ts = mdis_valtype_str(vt);
                        if (!ts) { mr_err(&r, "invalid local valtype"); goto fail; }
                        for (u32 k = 0; k < cnt; k++) {
                            mdis_printf(&o, "    (local %s)\n", ts);
                        }
                    }
                    if (!mdis_disassemble_code_body(&r, body_end, &o)) goto fail;
                    mdis_puts(&o, "  )\n");
                }
                code_sec_pos = 0;
                r.pos = saved_pos;
            }

            u32 dcount = 0;
            if (!mr_u32(&r, &dcount) || dcount > MB_MAX_DATAS) goto fail;
            for (u32 i = 0; i < dcount; i++) {
                u8 flags = 0, op = 0, end_op = 0;
                i64 off = 0;
                u32 dlen = 0;
                const u8* dptr = NULL;
                if (!mr_u8(&r, &flags) || flags != 0x00u) {
                    mr_err(&r, "unsupported data segment flags");
                    goto fail;
                }
                if (!mr_u8(&r, &op) || op != 0x41u || !mr_sleb(&r, &off) ||
                    !mr_u8(&r, &end_op) || end_op != 0x0Bu ||
                    !mr_u32(&r, &dlen) || !mr_bytes(&r, &dptr, dlen)) {
                    goto fail;
                }
                mdis_printf(&o, "  (data (i32.const %d) ", (i32)off);
                mdis_escaped_bytes(&o, dptr, dlen);
                mdis_puts(&o, ")\n");
            }
        } break;

        default:
            mr_err(&r, "unknown section id %u", sec_id);
            goto fail;
        }

        if (r.pos != sec_end) {
            mr_err(&r, "section %u not fully consumed", sec_id);
            goto fail;
        }
    }

    /* Data bölümü yoksa Code bölümünü burada yazdır */
    if (code_sec_pos != 0) {
        r.pos = code_sec_pos;
        u32 ccount = 0;
        if (!mr_u32(&r, &ccount) || ccount != def_func_count) {
            mr_err(&r, "code count mismatch with func section");
            goto fail;
        }
        for (u32 i = 0; i < ccount; i++) {
            u32 body_size = 0;
            if (!mr_u32(&r, &body_size) || body_size > code_sec_end - r.pos) goto fail;
            u32 body_end = r.pos + body_size;
            u32 fidx = imp_func_count + i;
            u32 tidx = func_types[i];
            mdis_printf(&o, "  (func $f%u (type $t%u)\n", fidx, tidx);

            u32 lgroup_count = 0;
            if (!mr_u32(&r, &lgroup_count)) goto fail;
            for (u32 g = 0; g < lgroup_count; g++) {
                u32 cnt = 0;
                u8 vt = 0;
                if (!mr_u32(&r, &cnt) || !mr_u8(&r, &vt)) goto fail;
                const char* ts = mdis_valtype_str(vt);
                if (!ts) { mr_err(&r, "invalid local valtype"); goto fail; }
                for (u32 k = 0; k < cnt; k++) {
                    mdis_printf(&o, "    (local %s)\n", ts);
                }
            }
            if (!mdis_disassemble_code_body(&r, body_end, &o)) goto fail;
            mdis_puts(&o, "  )\n");
        }
    }

    mdis_puts(&o, ")\n");

    res.ok = TRUE;
    res.wat = o.buf;
    res.len = o.len;
    return res;

fail:
    res.ok = FALSE;
    res.offset = r.err_off;
    for (usize i = 0; i < sizeof(res.msg); i++) {
        res.msg[i] = r.err_msg[i];
    }
    arena_pop_to(arena, mark);
    return res;
}
