/* mbin/mbin_ops.h — WebAssembly komut (opcode) yayıcıları.
 * Tüm fonksiyonlar static inline olarak tanımlanmıştır; kontrol akışı,
 * yerel/global erişim, bellek, skaler aritmetik ve 128-bit SIMD (0xFD)
 * komutlarını MbFunc kod tamponuna yazar.
 */
#ifndef MBIN_MBIN_OPS_H
#define MBIN_MBIN_OPS_H

#include "mbin/mbin.h"

/* Yardımcı: tek byte opcode */
static inline void mb_op(MbFunc* f, u8 opcode) {
    mb_u8(&f->code, opcode);
}

/* Yardımcı: bellek argümanı (align = log2(byte_align), offset = byte_offset) */
static inline void mb_memop(MbFunc* f, u8 opcode, u32 align, u32 offset) {
    mb_u8(&f->code, opcode);
    mb_uleb(&f->code, (u64)align);
    mb_uleb(&f->code, (u64)offset);
}

/* Yardımcı: 0xFD önekli SIMD alt-opcode (ULEB128 ile kodlanır) */
static inline void mb_simd_op(MbFunc* f, u32 subop) {
    mb_u8(&f->code, 0xFDu);
    mb_uleb(&f->code, (u64)subop);
}

/* ============================================================================
 * 1. Kontrol Akışı Komutları (Core Spec §5.4.1)
 * ============================================================================ */

static inline void mb_unreachable(MbFunc* f) { mb_op(f, 0x00u); }
static inline void mb_nop(MbFunc* f)         { mb_op(f, 0x01u); }

static inline void mb_block(MbFunc* f, u8 blocktype) {
    mb_op(f, 0x02u);
    mb_u8(&f->code, blocktype);
    f->depth++;
}

static inline void mb_loop(MbFunc* f, u8 blocktype) {
    mb_op(f, 0x03u);
    mb_u8(&f->code, blocktype);
    f->depth++;
}

static inline void mb_if(MbFunc* f, u8 blocktype) {
    mb_op(f, 0x04u);
    mb_u8(&f->code, blocktype);
    f->depth++;
}

static inline void mb_else(MbFunc* f) {
    if (f->depth == 0) {
        PANIC("mb_else: called outside of if block");
    }
    mb_op(f, 0x05u);
}

static inline void mb_end(MbFunc* f) {
    if (f->depth == 0) {
        PANIC("mb_end: block depth underflow (extra mb_end call)");
    }
    f->depth--;
    mb_op(f, 0x0Bu);
}

static inline void mb_br(MbFunc* f, u32 depth) {
    if (depth > f->depth) {
        PANIC("mb_br: branch depth %u exceeds current block depth %u", depth, f->depth);
    }
    mb_op(f, 0x0Cu);
    mb_uleb(&f->code, (u64)depth);
}

static inline void mb_br_if(MbFunc* f, u32 depth) {
    if (depth > f->depth) {
        PANIC("mb_br_if: branch depth %u exceeds current block depth %u", depth, f->depth);
    }
    mb_op(f, 0x0Du);
    mb_uleb(&f->code, (u64)depth);
}

static inline void mb_return(MbFunc* f) {
    mb_op(f, 0x0Fu);
}

static inline void mb_call(MbFunc* f, u32 func_idx) {
    mb_op(f, 0x10u);
    mb_uleb(&f->code, (u64)func_idx);
}

static inline void mb_call_indirect(MbFunc* f, u32 type_idx) {
    mb_op(f, 0x11u);
    mb_uleb(&f->code, (u64)type_idx);
    mb_u8(&f->code, 0x00u); /* tableidx = 0 */
}

/* ============================================================================
 * 2. Parametrik ve Değişken Komutları (Core Spec §5.4.2 - §5.4.3)
 * ============================================================================ */

static inline void mb_drop(MbFunc* f)   { mb_op(f, 0x1Au); }
static inline void mb_select(MbFunc* f) { mb_op(f, 0x1Bu); }

static inline void mb_local_get(MbFunc* f, u32 local_idx) {
    mb_op(f, 0x20u);
    mb_uleb(&f->code, (u64)local_idx);
}

static inline void mb_local_set(MbFunc* f, u32 local_idx) {
    mb_op(f, 0x21u);
    mb_uleb(&f->code, (u64)local_idx);
}

static inline void mb_local_tee(MbFunc* f, u32 local_idx) {
    mb_op(f, 0x22u);
    mb_uleb(&f->code, (u64)local_idx);
}

static inline void mb_global_get(MbFunc* f, u32 global_idx) {
    mb_op(f, 0x23u);
    mb_uleb(&f->code, (u64)global_idx);
}

static inline void mb_global_set(MbFunc* f, u32 global_idx) {
    mb_op(f, 0x24u);
    mb_uleb(&f->code, (u64)global_idx);
}

/* ============================================================================
 * 3. Bellek Komutları (Core Spec §5.4.4)
 * Not: align parametresi log2(byte_hizalama) değeridir (0=1B, 1=2B, 2=4B, 3=8B, 4=16B).
 * ============================================================================ */

static inline void mb_i32_load(MbFunc* f, u32 align, u32 offset)    { mb_memop(f, 0x28u, align, offset); }
static inline void mb_i64_load(MbFunc* f, u32 align, u32 offset)    { mb_memop(f, 0x29u, align, offset); }
static inline void mb_f32_load(MbFunc* f, u32 align, u32 offset)    { mb_memop(f, 0x2Au, align, offset); }
static inline void mb_f64_load(MbFunc* f, u32 align, u32 offset)    { mb_memop(f, 0x2Bu, align, offset); }
static inline void mb_i32_load8_s(MbFunc* f, u32 align, u32 offset) { mb_memop(f, 0x2Cu, align, offset); }
static inline void mb_i32_load8_u(MbFunc* f, u32 align, u32 offset) { mb_memop(f, 0x2Du, align, offset); }
static inline void mb_i32_load16_s(MbFunc* f, u32 align, u32 offset){ mb_memop(f, 0x2Eu, align, offset); }
static inline void mb_i32_load16_u(MbFunc* f, u32 align, u32 offset){ mb_memop(f, 0x2Fu, align, offset); }

static inline void mb_i32_store(MbFunc* f, u32 align, u32 offset)   { mb_memop(f, 0x36u, align, offset); }
static inline void mb_i64_store(MbFunc* f, u32 align, u32 offset)   { mb_memop(f, 0x37u, align, offset); }
static inline void mb_f32_store(MbFunc* f, u32 align, u32 offset)   { mb_memop(f, 0x38u, align, offset); }
static inline void mb_f64_store(MbFunc* f, u32 align, u32 offset)   { mb_memop(f, 0x39u, align, offset); }
static inline void mb_i32_store8(MbFunc* f, u32 align, u32 offset)  { mb_memop(f, 0x3Au, align, offset); }
static inline void mb_i32_store16(MbFunc* f, u32 align, u32 offset) { mb_memop(f, 0x3Bu, align, offset); }

/* ============================================================================
 * 4. Sayısal Komutlar: Sabitler, Karşılaştırma, Aritmetik, Dönüşüm (§5.4.5)
 * ============================================================================ */

static inline void mb_i32_const(MbFunc* f, i32 v) {
    mb_op(f, 0x41u);
    mb_sleb(&f->code, (i64)v);
}

static inline void mb_i64_const(MbFunc* f, i64 v) {
    mb_op(f, 0x42u);
    mb_sleb(&f->code, v);
}

static inline void mb_f32_const(MbFunc* f, f32 v) {
    mb_op(f, 0x43u);
    mb_f32(&f->code, v);
}

static inline void mb_f64_const(MbFunc* f, f64 v) {
    mb_op(f, 0x44u);
    mb_f64(&f->code, v);
}

/* i32 karşılaştırma */
static inline void mb_i32_eqz(MbFunc* f)  { mb_op(f, 0x45u); }
static inline void mb_i32_eq(MbFunc* f)   { mb_op(f, 0x46u); }
static inline void mb_i32_ne(MbFunc* f)   { mb_op(f, 0x47u); }
static inline void mb_i32_lt_s(MbFunc* f) { mb_op(f, 0x48u); }
static inline void mb_i32_lt_u(MbFunc* f) { mb_op(f, 0x49u); }
static inline void mb_i32_gt_s(MbFunc* f) { mb_op(f, 0x4Au); }
static inline void mb_i32_gt_u(MbFunc* f) { mb_op(f, 0x4Bu); }
static inline void mb_i32_le_s(MbFunc* f) { mb_op(f, 0x4Cu); }
static inline void mb_i32_le_u(MbFunc* f) { mb_op(f, 0x4Du); }
static inline void mb_i32_ge_s(MbFunc* f) { mb_op(f, 0x4Eu); }
static inline void mb_i32_ge_u(MbFunc* f) { mb_op(f, 0x4Fu); }

/* i64 karşılaştırma */
static inline void mb_i64_eqz(MbFunc* f)  { mb_op(f, 0x50u); }
static inline void mb_i64_eq(MbFunc* f)   { mb_op(f, 0x51u); }
static inline void mb_i64_ne(MbFunc* f)   { mb_op(f, 0x52u); }
static inline void mb_i64_lt_s(MbFunc* f) { mb_op(f, 0x53u); }
static inline void mb_i64_lt_u(MbFunc* f) { mb_op(f, 0x54u); }
static inline void mb_i64_gt_s(MbFunc* f) { mb_op(f, 0x55u); }
static inline void mb_i64_gt_u(MbFunc* f) { mb_op(f, 0x56u); }
static inline void mb_i64_le_s(MbFunc* f) { mb_op(f, 0x57u); }
static inline void mb_i64_le_u(MbFunc* f) { mb_op(f, 0x58u); }
static inline void mb_i64_ge_s(MbFunc* f) { mb_op(f, 0x59u); }
static inline void mb_i64_ge_u(MbFunc* f) { mb_op(f, 0x5Au); }

/* f32 karşılaştırma */
static inline void mb_f32_eq(MbFunc* f)   { mb_op(f, 0x5Bu); }
static inline void mb_f32_ne(MbFunc* f)   { mb_op(f, 0x5Cu); }
static inline void mb_f32_lt(MbFunc* f)   { mb_op(f, 0x5Du); }
static inline void mb_f32_gt(MbFunc* f)   { mb_op(f, 0x5Eu); }
static inline void mb_f32_le(MbFunc* f)   { mb_op(f, 0x5Fu); }
static inline void mb_f32_ge(MbFunc* f)   { mb_op(f, 0x60u); }

/* i32 aritmetik ve bit işlemleri */
static inline void mb_i32_clz(MbFunc* f)    { mb_op(f, 0x67u); }
static inline void mb_i32_ctz(MbFunc* f)    { mb_op(f, 0x68u); }
static inline void mb_i32_popcnt(MbFunc* f) { mb_op(f, 0x69u); }
static inline void mb_i32_add(MbFunc* f)    { mb_op(f, 0x6Au); }
static inline void mb_i32_sub(MbFunc* f)    { mb_op(f, 0x6Bu); }
static inline void mb_i32_mul(MbFunc* f)    { mb_op(f, 0x6Cu); }
static inline void mb_i32_div_s(MbFunc* f)  { mb_op(f, 0x6Du); }
static inline void mb_i32_div_u(MbFunc* f)  { mb_op(f, 0x6Eu); }
static inline void mb_i32_rem_s(MbFunc* f)  { mb_op(f, 0x6Fu); }
static inline void mb_i32_rem_u(MbFunc* f)  { mb_op(f, 0x70u); }
static inline void mb_i32_and(MbFunc* f)    { mb_op(f, 0x71u); }
static inline void mb_i32_or(MbFunc* f)     { mb_op(f, 0x72u); }
static inline void mb_i32_xor(MbFunc* f)    { mb_op(f, 0x73u); }
static inline void mb_i32_shl(MbFunc* f)    { mb_op(f, 0x74u); }
static inline void mb_i32_shr_s(MbFunc* f)  { mb_op(f, 0x75u); }
static inline void mb_i32_shr_u(MbFunc* f)  { mb_op(f, 0x76u); }
static inline void mb_i32_rotl(MbFunc* f)   { mb_op(f, 0x77u); }
static inline void mb_i32_rotr(MbFunc* f)   { mb_op(f, 0x78u); }

/* i64 aritmetik ve bit işlemleri */
static inline void mb_i64_clz(MbFunc* f)    { mb_op(f, 0x79u); }
static inline void mb_i64_ctz(MbFunc* f)    { mb_op(f, 0x7Au); }
static inline void mb_i64_popcnt(MbFunc* f) { mb_op(f, 0x7Bu); }
static inline void mb_i64_add(MbFunc* f)    { mb_op(f, 0x7Cu); }
static inline void mb_i64_sub(MbFunc* f)    { mb_op(f, 0x7Du); }
static inline void mb_i64_mul(MbFunc* f)    { mb_op(f, 0x7Eu); }
static inline void mb_i64_div_s(MbFunc* f)  { mb_op(f, 0x7Fu); }
static inline void mb_i64_div_u(MbFunc* f)  { mb_op(f, 0x80u); }
static inline void mb_i64_rem_s(MbFunc* f)  { mb_op(f, 0x81u); }
static inline void mb_i64_rem_u(MbFunc* f)  { mb_op(f, 0x82u); }
static inline void mb_i64_and(MbFunc* f)    { mb_op(f, 0x83u); }
static inline void mb_i64_or(MbFunc* f)     { mb_op(f, 0x84u); }
static inline void mb_i64_xor(MbFunc* f)    { mb_op(f, 0x85u); }
static inline void mb_i64_shl(MbFunc* f)    { mb_op(f, 0x86u); }
static inline void mb_i64_shr_s(MbFunc* f)  { mb_op(f, 0x87u); }
static inline void mb_i64_shr_u(MbFunc* f)  { mb_op(f, 0x88u); }

/* f32 aritmetik ve matematik işlemleri */
static inline void mb_f32_abs(MbFunc* f)      { mb_op(f, 0x8Bu); }
static inline void mb_f32_neg(MbFunc* f)      { mb_op(f, 0x8Cu); }
static inline void mb_f32_ceil(MbFunc* f)     { mb_op(f, 0x8Du); }
static inline void mb_f32_floor(MbFunc* f)    { mb_op(f, 0x8Eu); }
static inline void mb_f32_trunc(MbFunc* f)    { mb_op(f, 0x8Fu); }
static inline void mb_f32_nearest(MbFunc* f)  { mb_op(f, 0x90u); }
static inline void mb_f32_sqrt(MbFunc* f)     { mb_op(f, 0x91u); }
static inline void mb_f32_add(MbFunc* f)      { mb_op(f, 0x92u); }
static inline void mb_f32_sub(MbFunc* f)      { mb_op(f, 0x93u); }
static inline void mb_f32_mul(MbFunc* f)      { mb_op(f, 0x94u); }
static inline void mb_f32_div(MbFunc* f)      { mb_op(f, 0x95u); }
static inline void mb_f32_min(MbFunc* f)      { mb_op(f, 0x96u); }
static inline void mb_f32_max(MbFunc* f)      { mb_op(f, 0x97u); }
static inline void mb_f32_copysign(MbFunc* f) { mb_op(f, 0x98u); }

/* Tip dönüşümleri */
static inline void mb_i32_wrap_i64(MbFunc* f)        { mb_op(f, 0xA7u); }
static inline void mb_i32_trunc_f32_s(MbFunc* f)     { mb_op(f, 0xA8u); }
static inline void mb_i32_trunc_f32_u(MbFunc* f)     { mb_op(f, 0xA9u); }
static inline void mb_i64_extend_i32_s(MbFunc* f)    { mb_op(f, 0xACu); }
static inline void mb_i64_extend_i32_u(MbFunc* f)    { mb_op(f, 0xADu); }
static inline void mb_f32_convert_i32_s(MbFunc* f)   { mb_op(f, 0xB2u); }
static inline void mb_f32_convert_i32_u(MbFunc* f)   { mb_op(f, 0xB3u); }
static inline void mb_f32_convert_i64_s(MbFunc* f)   { mb_op(f, 0xB4u); }
static inline void mb_f32_convert_i64_u(MbFunc* f)   { mb_op(f, 0xB5u); }
static inline void mb_i32_reinterpret_f32(MbFunc* f) { mb_op(f, 0xBCu); }
static inline void mb_f32_reinterpret_i32(MbFunc* f) { mb_op(f, 0xBEu); }

static inline void mb_i32_trunc_sat_f32_s(MbFunc* f) {
    mb_op(f, 0xFCu);
    mb_uleb(&f->code, 0u);
}

static inline void mb_i32_trunc_sat_f32_u(MbFunc* f) {
    mb_op(f, 0xFCu);
    mb_uleb(&f->code, 1u);
}

/* ============================================================================
 * 5. SIMD Komutları (0xFD Önek + ULEB128 Alt-Opcode, Core Spec §5.4.6)
 * ============================================================================ */

static inline void mb_v128_load(MbFunc* f, u32 align, u32 offset) {
    mb_simd_op(f, 0x00u);
    mb_uleb(&f->code, (u64)align);
    mb_uleb(&f->code, (u64)offset);
}

static inline void mb_v128_store(MbFunc* f, u32 align, u32 offset) {
    mb_simd_op(f, 0x0Bu);
    mb_uleb(&f->code, (u64)align);
    mb_uleb(&f->code, (u64)offset);
}

static inline void mb_v128_const(MbFunc* f, const u8 bytes[16]) {
    mb_simd_op(f, 0x0Cu);
    mb_bytes(&f->code, bytes, 16);
}

static inline void mb_f32x4_const(MbFunc* f, f32 x0, f32 x1, f32 x2, f32 x3) {
    mb_simd_op(f, 0x0Cu);
    mb_f32(&f->code, x0);
    mb_f32(&f->code, x1);
    mb_f32(&f->code, x2);
    mb_f32(&f->code, x3);
}

static inline void mb_i32x4_const(MbFunc* f, i32 x0, i32 x1, i32 x2, i32 x3) {
    mb_simd_op(f, 0x0Cu);
    i32 lanes[4] = { x0, x1, x2, x3 };
    for (u32 i = 0; i < 4; i++) {
        u32 u = (u32)lanes[i];
        mb_u8(&f->code, (u8)(u & 0xFFu));
        mb_u8(&f->code, (u8)((u >> 8) & 0xFFu));
        mb_u8(&f->code, (u8)((u >> 16) & 0xFFu));
        mb_u8(&f->code, (u8)((u >> 24) & 0xFFu));
    }
}

static inline void mb_i8x16_shuffle(MbFunc* f, const u8 lanes[16]) {
    mb_simd_op(f, 0x0Du);
    for (u32 i = 0; i < 16; i++) {
        ASSERT(lanes[i] < 32u);
        mb_u8(&f->code, lanes[i]);
    }
}

/* Splat ve Lane erişimi */
static inline void mb_i8x16_splat(MbFunc* f) { mb_simd_op(f, 0x0Fu); }
static inline void mb_i16x8_splat(MbFunc* f) { mb_simd_op(f, 0x10u); }
static inline void mb_i32x4_splat(MbFunc* f) { mb_simd_op(f, 0x11u); }
static inline void mb_i64x2_splat(MbFunc* f) { mb_simd_op(f, 0x12u); }
static inline void mb_f32x4_splat(MbFunc* f) { mb_simd_op(f, 0x13u); }
static inline void mb_f64x2_splat(MbFunc* f) { mb_simd_op(f, 0x14u); }

static inline void mb_i32x4_extract_lane(MbFunc* f, u8 lane) {
    ASSERT(lane < 4u);
    mb_simd_op(f, 0x1Bu);
    mb_u8(&f->code, lane);
}

static inline void mb_i32x4_replace_lane(MbFunc* f, u8 lane) {
    ASSERT(lane < 4u);
    mb_simd_op(f, 0x1Cu);
    mb_u8(&f->code, lane);
}

static inline void mb_f32x4_extract_lane(MbFunc* f, u8 lane) {
    ASSERT(lane < 4u);
    mb_simd_op(f, 0x1Fu);
    mb_u8(&f->code, lane);
}

static inline void mb_f32x4_replace_lane(MbFunc* f, u8 lane) {
    ASSERT(lane < 4u);
    mb_simd_op(f, 0x20u);
    mb_u8(&f->code, lane);
}

/* i32x4 karşılaştırma */
static inline void mb_i32x4_eq(MbFunc* f)   { mb_simd_op(f, 0x37u); }
static inline void mb_i32x4_ne(MbFunc* f)   { mb_simd_op(f, 0x38u); }
static inline void mb_i32x4_lt_s(MbFunc* f) { mb_simd_op(f, 0x39u); }
static inline void mb_i32x4_lt_u(MbFunc* f) { mb_simd_op(f, 0x3Au); }
static inline void mb_i32x4_gt_s(MbFunc* f) { mb_simd_op(f, 0x3Bu); }
static inline void mb_i32x4_gt_u(MbFunc* f) { mb_simd_op(f, 0x3Cu); }
static inline void mb_i32x4_le_s(MbFunc* f) { mb_simd_op(f, 0x3Du); }
static inline void mb_i32x4_le_u(MbFunc* f) { mb_simd_op(f, 0x3Eu); }
static inline void mb_i32x4_ge_s(MbFunc* f) { mb_simd_op(f, 0x3Fu); }
static inline void mb_i32x4_ge_u(MbFunc* f) { mb_simd_op(f, 0x40u); }

/* f32x4 karşılaştırma */
static inline void mb_f32x4_eq(MbFunc* f)   { mb_simd_op(f, 0x41u); }
static inline void mb_f32x4_ne(MbFunc* f)   { mb_simd_op(f, 0x42u); }
static inline void mb_f32x4_lt(MbFunc* f)   { mb_simd_op(f, 0x43u); }
static inline void mb_f32x4_gt(MbFunc* f)   { mb_simd_op(f, 0x44u); }
static inline void mb_f32x4_le(MbFunc* f)   { mb_simd_op(f, 0x45u); }
static inline void mb_f32x4_ge(MbFunc* f)   { mb_simd_op(f, 0x46u); }

/* v128 bit ve maske işlemleri */
static inline void mb_v128_not(MbFunc* f)       { mb_simd_op(f, 0x4Du); }
static inline void mb_v128_and(MbFunc* f)       { mb_simd_op(f, 0x4Eu); }
static inline void mb_v128_andnot(MbFunc* f)    { mb_simd_op(f, 0x4Fu); }
static inline void mb_v128_or(MbFunc* f)        { mb_simd_op(f, 0x50u); }
static inline void mb_v128_xor(MbFunc* f)       { mb_simd_op(f, 0x51u); }
static inline void mb_v128_bitselect(MbFunc* f) { mb_simd_op(f, 0x52u); }
static inline void mb_v128_any_true(MbFunc* f)  { mb_simd_op(f, 0x53u); }

/* Daraltma (narrow) ve f32x4 yuvarlama */
static inline void mb_i8x16_narrow_i16x8_s(MbFunc* f) { mb_simd_op(f, 0x65u); }
static inline void mb_i8x16_narrow_i16x8_u(MbFunc* f) { mb_simd_op(f, 0x66u); }
static inline void mb_f32x4_ceil(MbFunc* f)           { mb_simd_op(f, 0x67u); }
static inline void mb_f32x4_floor(MbFunc* f)          { mb_simd_op(f, 0x68u); }
static inline void mb_f32x4_trunc(MbFunc* f)          { mb_simd_op(f, 0x69u); }
static inline void mb_f32x4_nearest(MbFunc* f)        { mb_simd_op(f, 0x6Au); }
static inline void mb_i16x8_narrow_i32x4_s(MbFunc* f) { mb_simd_op(f, 0x85u); }
static inline void mb_i16x8_narrow_i32x4_u(MbFunc* f) { mb_simd_op(f, 0x86u); }

/* i32x4 aritmetik, kaydırma ve maske */
static inline void mb_i32x4_abs(MbFunc* f)      { mb_simd_op(f, 0xA0u); }
static inline void mb_i32x4_neg(MbFunc* f)      { mb_simd_op(f, 0xA1u); }
static inline void mb_i32x4_all_true(MbFunc* f) { mb_simd_op(f, 0xA3u); }
static inline void mb_i32x4_bitmask(MbFunc* f)  { mb_simd_op(f, 0xA4u); }
static inline void mb_i32x4_shl(MbFunc* f)      { mb_simd_op(f, 0xABu); }
static inline void mb_i32x4_shr_s(MbFunc* f)    { mb_simd_op(f, 0xACu); }
static inline void mb_i32x4_shr_u(MbFunc* f)    { mb_simd_op(f, 0xADu); }
static inline void mb_i32x4_add(MbFunc* f)      { mb_simd_op(f, 0xAEu); }
static inline void mb_i32x4_sub(MbFunc* f)      { mb_simd_op(f, 0xB1u); }
static inline void mb_i32x4_mul(MbFunc* f)      { mb_simd_op(f, 0xB5u); }
static inline void mb_i32x4_min_s(MbFunc* f)    { mb_simd_op(f, 0xB6u); }
static inline void mb_i32x4_min_u(MbFunc* f)    { mb_simd_op(f, 0xB7u); }
static inline void mb_i32x4_max_s(MbFunc* f)    { mb_simd_op(f, 0xB8u); }
static inline void mb_i32x4_max_u(MbFunc* f)    { mb_simd_op(f, 0xB9u); }

/* f32x4 aritmetik ve dönüşümler */
static inline void mb_f32x4_abs(MbFunc* f)  { mb_simd_op(f, 0xE0u); }
static inline void mb_f32x4_neg(MbFunc* f)  { mb_simd_op(f, 0xE1u); }
static inline void mb_f32x4_sqrt(MbFunc* f) { mb_simd_op(f, 0xE3u); }
static inline void mb_f32x4_add(MbFunc* f)  { mb_simd_op(f, 0xE4u); }
static inline void mb_f32x4_sub(MbFunc* f)  { mb_simd_op(f, 0xE5u); }
static inline void mb_f32x4_mul(MbFunc* f)  { mb_simd_op(f, 0xE6u); }
static inline void mb_f32x4_div(MbFunc* f)  { mb_simd_op(f, 0xE7u); }
static inline void mb_f32x4_min(MbFunc* f)  { mb_simd_op(f, 0xE8u); }
static inline void mb_f32x4_max(MbFunc* f)  { mb_simd_op(f, 0xE9u); }

static inline void mb_i32x4_trunc_sat_f32x4_s(MbFunc* f) { mb_simd_op(f, 0xF8u); }
static inline void mb_i32x4_trunc_sat_f32x4_u(MbFunc* f) { mb_simd_op(f, 0xF9u); }
static inline void mb_f32x4_convert_i32x4_s(MbFunc* f)   { mb_simd_op(f, 0xFAu); }
static inline void mb_f32x4_convert_i32x4_u(MbFunc* f)   { mb_simd_op(f, 0xFBu); }

/* ============================================================================
 * 6. M5_3 Genişletmeleri: Multi-value blok, br_table, memory.copy/fill ve tam SIMD128
 * ============================================================================ */

static inline void mb_block_type(MbFunc* f, u32 type_idx) {
    mb_op(f, 0x02u);
    mb_sleb(&f->code, (i64)type_idx);
    f->depth++;
}

static inline void mb_loop_type(MbFunc* f, u32 type_idx) {
    mb_op(f, 0x03u);
    mb_sleb(&f->code, (i64)type_idx);
    f->depth++;
}

static inline void mb_if_type(MbFunc* f, u32 type_idx) {
    mb_op(f, 0x04u);
    mb_sleb(&f->code, (i64)type_idx);
    f->depth++;
}

static inline void mb_br_table(MbFunc* f, const u32* targets, u32 count, u32 default_target) {
    mb_op(f, 0x0Eu);
    mb_uleb(&f->code, (u64)count);
    for (u32 i = 0; i < count; i++) {
        if (targets[i] > f->depth) {
            PANIC("mb_br_table: target depth %u exceeds block depth %u", targets[i], f->depth);
        }
        mb_uleb(&f->code, (u64)targets[i]);
    }
    if (default_target > f->depth) {
        PANIC("mb_br_table: default depth %u exceeds block depth %u", default_target, f->depth);
    }
    mb_uleb(&f->code, (u64)default_target);
}

static inline void mb_memory_size(MbFunc* f) {
    mb_op(f, 0x3Fu);
    mb_u8(&f->code, 0x00u);
}

static inline void mb_memory_grow(MbFunc* f) {
    mb_op(f, 0x40u);
    mb_u8(&f->code, 0x00u);
}

static inline void mb_memory_copy(MbFunc* f) {
    mb_op(f, 0xFCu);
    mb_uleb(&f->code, 10u);
    mb_u8(&f->code, 0x00u);
    mb_u8(&f->code, 0x00u);
}

static inline void mb_memory_fill(MbFunc* f) {
    mb_op(f, 0xFCu);
    mb_uleb(&f->code, 11u);
    mb_u8(&f->code, 0x00u);
}

/* Ek skaler bellek, karşılaştırma, f64 ve dönüşüm komutları */
static inline void mb_i64_load8_s(MbFunc* f, u32 align, u32 offset)  { mb_memop(f, 0x30u, align, offset); }
static inline void mb_i64_load8_u(MbFunc* f, u32 align, u32 offset)  { mb_memop(f, 0x31u, align, offset); }
static inline void mb_i64_load16_s(MbFunc* f, u32 align, u32 offset) { mb_memop(f, 0x32u, align, offset); }
static inline void mb_i64_load16_u(MbFunc* f, u32 align, u32 offset) { mb_memop(f, 0x33u, align, offset); }
static inline void mb_i64_load32_s(MbFunc* f, u32 align, u32 offset) { mb_memop(f, 0x34u, align, offset); }
static inline void mb_i64_load32_u(MbFunc* f, u32 align, u32 offset) { mb_memop(f, 0x35u, align, offset); }
static inline void mb_i64_store8(MbFunc* f, u32 align, u32 offset)   { mb_memop(f, 0x3Cu, align, offset); }
static inline void mb_i64_store16(MbFunc* f, u32 align, u32 offset)  { mb_memop(f, 0x3Du, align, offset); }
static inline void mb_i64_store32(MbFunc* f, u32 align, u32 offset)  { mb_memop(f, 0x3Eu, align, offset); }

static inline void mb_f64_eq(MbFunc* f)       { mb_op(f, 0x61u); }
static inline void mb_f64_ne(MbFunc* f)       { mb_op(f, 0x62u); }
static inline void mb_f64_lt(MbFunc* f)       { mb_op(f, 0x63u); }
static inline void mb_f64_gt(MbFunc* f)       { mb_op(f, 0x64u); }
static inline void mb_f64_le(MbFunc* f)       { mb_op(f, 0x65u); }
static inline void mb_f64_ge(MbFunc* f)       { mb_op(f, 0x66u); }
static inline void mb_i64_rotl(MbFunc* f)     { mb_op(f, 0x89u); }
static inline void mb_i64_rotr(MbFunc* f)     { mb_op(f, 0x8Au); }
static inline void mb_f64_abs(MbFunc* f)      { mb_op(f, 0x99u); }
static inline void mb_f64_neg(MbFunc* f)      { mb_op(f, 0x9Au); }
static inline void mb_f64_ceil(MbFunc* f)     { mb_op(f, 0x9Bu); }
static inline void mb_f64_floor(MbFunc* f)    { mb_op(f, 0x9Cu); }
static inline void mb_f64_trunc(MbFunc* f)    { mb_op(f, 0x9Du); }
static inline void mb_f64_nearest(MbFunc* f)  { mb_op(f, 0x9Eu); }
static inline void mb_f64_sqrt(MbFunc* f)     { mb_op(f, 0x9Fu); }
static inline void mb_f64_add(MbFunc* f)      { mb_op(f, 0xA0u); }
static inline void mb_f64_sub(MbFunc* f)      { mb_op(f, 0xA1u); }
static inline void mb_f64_mul(MbFunc* f)      { mb_op(f, 0xA2u); }
static inline void mb_f64_div(MbFunc* f)      { mb_op(f, 0xA3u); }
static inline void mb_f64_min(MbFunc* f)      { mb_op(f, 0xA4u); }
static inline void mb_f64_max(MbFunc* f)      { mb_op(f, 0xA5u); }
static inline void mb_f64_copysign(MbFunc* f) { mb_op(f, 0xA6u); }

static inline void mb_i32_trunc_f64_s(MbFunc* f)     { mb_op(f, 0xAAu); }
static inline void mb_i32_trunc_f64_u(MbFunc* f)     { mb_op(f, 0xABu); }
static inline void mb_i64_trunc_f32_s(MbFunc* f)     { mb_op(f, 0xAEu); }
static inline void mb_i64_trunc_f32_u(MbFunc* f)     { mb_op(f, 0xAFu); }
static inline void mb_i64_trunc_f64_s(MbFunc* f)     { mb_op(f, 0xB0u); }
static inline void mb_i64_trunc_f64_u(MbFunc* f)     { mb_op(f, 0xB1u); }
static inline void mb_f32_demote_f64(MbFunc* f)      { mb_op(f, 0xB6u); }
static inline void mb_f64_convert_i32_s(MbFunc* f)   { mb_op(f, 0xB7u); }
static inline void mb_f64_convert_i32_u(MbFunc* f)   { mb_op(f, 0xB8u); }
static inline void mb_f64_convert_i64_s(MbFunc* f)   { mb_op(f, 0xB9u); }
static inline void mb_f64_convert_i64_u(MbFunc* f)   { mb_op(f, 0xBAu); }
static inline void mb_f64_promote_f32(MbFunc* f)     { mb_op(f, 0xBBu); }
static inline void mb_i64_reinterpret_f64(MbFunc* f) { mb_op(f, 0xBDu); }
static inline void mb_f64_reinterpret_i64(MbFunc* f) { mb_op(f, 0xBFu); }
static inline void mb_i32_extend8_s(MbFunc* f)       { mb_op(f, 0xC0u); }
static inline void mb_i32_extend16_s(MbFunc* f)      { mb_op(f, 0xC1u); }
static inline void mb_i64_extend8_s(MbFunc* f)       { mb_op(f, 0xC2u); }
static inline void mb_i64_extend16_s(MbFunc* f)      { mb_op(f, 0xC3u); }
static inline void mb_i64_extend32_s(MbFunc* f)      { mb_op(f, 0xC4u); }

/* Genişleten SIMD yükler ve lane yük/sakla */
static inline void mb_simd_memop(MbFunc* f, u32 subop, u32 align, u32 offset) {
    mb_simd_op(f, subop);
    mb_uleb(&f->code, (u64)align);
    mb_uleb(&f->code, (u64)offset);
}

static inline void mb_simd_mem_lane(MbFunc* f, u32 subop, u32 align, u32 offset, u8 lane) {
    mb_simd_op(f, subop);
    mb_uleb(&f->code, (u64)align);
    mb_uleb(&f->code, (u64)offset);
    mb_u8(&f->code, lane);
}

static inline void mb_v128_load8x8_s(MbFunc* f, u32 align, u32 offset)    { mb_simd_memop(f, 0x01u, align, offset); }
static inline void mb_v128_load8x8_u(MbFunc* f, u32 align, u32 offset)    { mb_simd_memop(f, 0x02u, align, offset); }
static inline void mb_v128_load16x4_s(MbFunc* f, u32 align, u32 offset)   { mb_simd_memop(f, 0x03u, align, offset); }
static inline void mb_v128_load16x4_u(MbFunc* f, u32 align, u32 offset)   { mb_simd_memop(f, 0x04u, align, offset); }
static inline void mb_v128_load32x2_s(MbFunc* f, u32 align, u32 offset)   { mb_simd_memop(f, 0x05u, align, offset); }
static inline void mb_v128_load32x2_u(MbFunc* f, u32 align, u32 offset)   { mb_simd_memop(f, 0x06u, align, offset); }
static inline void mb_v128_load8_splat(MbFunc* f, u32 align, u32 offset)  { mb_simd_memop(f, 0x07u, align, offset); }
static inline void mb_v128_load16_splat(MbFunc* f, u32 align, u32 offset) { mb_simd_memop(f, 0x08u, align, offset); }
static inline void mb_v128_load32_splat(MbFunc* f, u32 align, u32 offset) { mb_simd_memop(f, 0x09u, align, offset); }
static inline void mb_v128_load64_splat(MbFunc* f, u32 align, u32 offset) { mb_simd_memop(f, 0x0Au, align, offset); }
static inline void mb_v128_load32_zero(MbFunc* f, u32 align, u32 offset)  { mb_simd_memop(f, 0x5Cu, align, offset); }
static inline void mb_v128_load64_zero(MbFunc* f, u32 align, u32 offset)  { mb_simd_memop(f, 0x5Du, align, offset); }

static inline void mb_v128_load8_lane(MbFunc* f, u32 align, u32 offset, u8 lane)  { mb_simd_mem_lane(f, 0x54u, align, offset, lane); }
static inline void mb_v128_load16_lane(MbFunc* f, u32 align, u32 offset, u8 lane) { mb_simd_mem_lane(f, 0x55u, align, offset, lane); }
static inline void mb_v128_load32_lane(MbFunc* f, u32 align, u32 offset, u8 lane) { mb_simd_mem_lane(f, 0x56u, align, offset, lane); }
static inline void mb_v128_load64_lane(MbFunc* f, u32 align, u32 offset, u8 lane) { mb_simd_mem_lane(f, 0x57u, align, offset, lane); }
static inline void mb_v128_store8_lane(MbFunc* f, u32 align, u32 offset, u8 lane) { mb_simd_mem_lane(f, 0x58u, align, offset, lane); }
static inline void mb_v128_store16_lane(MbFunc* f, u32 align, u32 offset, u8 lane){ mb_simd_mem_lane(f, 0x59u, align, offset, lane); }
static inline void mb_v128_store32_lane(MbFunc* f, u32 align, u32 offset, u8 lane){ mb_simd_mem_lane(f, 0x5Au, align, offset, lane); }
static inline void mb_v128_store64_lane(MbFunc* f, u32 align, u32 offset, u8 lane){ mb_simd_mem_lane(f, 0x5Bu, align, offset, lane); }

/* Ek SIMD lane, swizzle ve i8x16/i16x8/i64x2/f64x2 aritmetik komutları */
static inline void mb_i8x16_swizzle(MbFunc* f)                  { mb_simd_op(f, 0x0Eu); }
static inline void mb_i8x16_extract_lane_s(MbFunc* f, u8 lane)  { ASSERT(lane < 16u); mb_simd_op(f, 0x15u); mb_u8(&f->code, lane); }
static inline void mb_i8x16_extract_lane_u(MbFunc* f, u8 lane)  { ASSERT(lane < 16u); mb_simd_op(f, 0x16u); mb_u8(&f->code, lane); }
static inline void mb_i8x16_replace_lane(MbFunc* f, u8 lane)    { ASSERT(lane < 16u); mb_simd_op(f, 0x17u); mb_u8(&f->code, lane); }
static inline void mb_i16x8_extract_lane_s(MbFunc* f, u8 lane)  { ASSERT(lane < 8u);  mb_simd_op(f, 0x18u); mb_u8(&f->code, lane); }
static inline void mb_i16x8_extract_lane_u(MbFunc* f, u8 lane)  { ASSERT(lane < 8u);  mb_simd_op(f, 0x19u); mb_u8(&f->code, lane); }
static inline void mb_i16x8_replace_lane(MbFunc* f, u8 lane)    { ASSERT(lane < 8u);  mb_simd_op(f, 0x1Au); mb_u8(&f->code, lane); }
static inline void mb_i64x2_extract_lane(MbFunc* f, u8 lane)    { ASSERT(lane < 2u);  mb_simd_op(f, 0x1Du); mb_u8(&f->code, lane); }
static inline void mb_i64x2_replace_lane(MbFunc* f, u8 lane)    { ASSERT(lane < 2u);  mb_simd_op(f, 0x1Eu); mb_u8(&f->code, lane); }
static inline void mb_f64x2_extract_lane(MbFunc* f, u8 lane)    { ASSERT(lane < 2u);  mb_simd_op(f, 0x21u); mb_u8(&f->code, lane); }
static inline void mb_f64x2_replace_lane(MbFunc* f, u8 lane)    { ASSERT(lane < 2u);  mb_simd_op(f, 0x22u); mb_u8(&f->code, lane); }

static inline void mb_i8x16_eq(MbFunc* f)    { mb_simd_op(f, 0x23u); }
static inline void mb_i8x16_ne(MbFunc* f)    { mb_simd_op(f, 0x24u); }
static inline void mb_i8x16_lt_s(MbFunc* f)  { mb_simd_op(f, 0x25u); }
static inline void mb_i8x16_lt_u(MbFunc* f)  { mb_simd_op(f, 0x26u); }
static inline void mb_i8x16_gt_s(MbFunc* f)  { mb_simd_op(f, 0x27u); }
static inline void mb_i8x16_gt_u(MbFunc* f)  { mb_simd_op(f, 0x28u); }
static inline void mb_i8x16_le_s(MbFunc* f)  { mb_simd_op(f, 0x29u); }
static inline void mb_i8x16_le_u(MbFunc* f)  { mb_simd_op(f, 0x2Au); }
static inline void mb_i8x16_ge_s(MbFunc* f)  { mb_simd_op(f, 0x2Bu); }
static inline void mb_i8x16_ge_u(MbFunc* f)  { mb_simd_op(f, 0x2Cu); }

static inline void mb_i16x8_eq(MbFunc* f)    { mb_simd_op(f, 0x2Du); }
static inline void mb_i16x8_ne(MbFunc* f)    { mb_simd_op(f, 0x2Eu); }
static inline void mb_i16x8_lt_s(MbFunc* f)  { mb_simd_op(f, 0x2Fu); }
static inline void mb_i16x8_lt_u(MbFunc* f)  { mb_simd_op(f, 0x30u); }
static inline void mb_i16x8_gt_s(MbFunc* f)  { mb_simd_op(f, 0x31u); }
static inline void mb_i16x8_gt_u(MbFunc* f)  { mb_simd_op(f, 0x32u); }
static inline void mb_i16x8_le_s(MbFunc* f)  { mb_simd_op(f, 0x33u); }
static inline void mb_i16x8_le_u(MbFunc* f)  { mb_simd_op(f, 0x34u); }
static inline void mb_i16x8_ge_s(MbFunc* f)  { mb_simd_op(f, 0x35u); }
static inline void mb_i16x8_ge_u(MbFunc* f)  { mb_simd_op(f, 0x36u); }

static inline void mb_f64x2_eq(MbFunc* f)    { mb_simd_op(f, 0x47u); }
static inline void mb_f64x2_ne(MbFunc* f)    { mb_simd_op(f, 0x48u); }
static inline void mb_f64x2_lt(MbFunc* f)    { mb_simd_op(f, 0x49u); }
static inline void mb_f64x2_gt(MbFunc* f)    { mb_simd_op(f, 0x4Au); }
static inline void mb_f64x2_le(MbFunc* f)    { mb_simd_op(f, 0x4Bu); }
static inline void mb_f64x2_ge(MbFunc* f)    { mb_simd_op(f, 0x4Cu); }

static inline void mb_f32x4_demote_f64x2_zero(MbFunc* f) { mb_simd_op(f, 0x5Eu); }
static inline void mb_f64x2_promote_low_f32x4(MbFunc* f) { mb_simd_op(f, 0x5Fu); }

static inline void mb_i8x16_abs(MbFunc* f)       { mb_simd_op(f, 0x60u); }
static inline void mb_i8x16_neg(MbFunc* f)       { mb_simd_op(f, 0x61u); }
static inline void mb_i8x16_popcnt(MbFunc* f)    { mb_simd_op(f, 0x62u); }
static inline void mb_i8x16_all_true(MbFunc* f)  { mb_simd_op(f, 0x63u); }
static inline void mb_i8x16_bitmask(MbFunc* f)   { mb_simd_op(f, 0x64u); }
static inline void mb_i8x16_shl(MbFunc* f)       { mb_simd_op(f, 0x6Bu); }
static inline void mb_i8x16_shr_s(MbFunc* f)     { mb_simd_op(f, 0x6Cu); }
static inline void mb_i8x16_shr_u(MbFunc* f)     { mb_simd_op(f, 0x6Du); }
static inline void mb_i8x16_add(MbFunc* f)       { mb_simd_op(f, 0x6Eu); }
static inline void mb_i8x16_add_sat_s(MbFunc* f) { mb_simd_op(f, 0x6Fu); }
static inline void mb_i8x16_add_sat_u(MbFunc* f) { mb_simd_op(f, 0x70u); }
static inline void mb_i8x16_sub(MbFunc* f)       { mb_simd_op(f, 0x71u); }
static inline void mb_i8x16_sub_sat_s(MbFunc* f) { mb_simd_op(f, 0x72u); }
static inline void mb_i8x16_sub_sat_u(MbFunc* f) { mb_simd_op(f, 0x73u); }
static inline void mb_f64x2_ceil(MbFunc* f)      { mb_simd_op(f, 0x74u); }
static inline void mb_f64x2_floor(MbFunc* f)     { mb_simd_op(f, 0x75u); }
static inline void mb_i8x16_min_s(MbFunc* f)     { mb_simd_op(f, 0x76u); }
static inline void mb_i8x16_min_u(MbFunc* f)     { mb_simd_op(f, 0x77u); }
static inline void mb_i8x16_max_s(MbFunc* f)     { mb_simd_op(f, 0x78u); }
static inline void mb_i8x16_max_u(MbFunc* f)     { mb_simd_op(f, 0x79u); }
static inline void mb_f64x2_trunc(MbFunc* f)     { mb_simd_op(f, 0x7Au); }
static inline void mb_i8x16_avgr_u(MbFunc* f)    { mb_simd_op(f, 0x7Bu); }

static inline void mb_i16x8_extadd_pairwise_i8x16_s(MbFunc* f) { mb_simd_op(f, 0x7Cu); }
static inline void mb_i16x8_extadd_pairwise_i8x16_u(MbFunc* f) { mb_simd_op(f, 0x7Du); }
static inline void mb_i32x4_extadd_pairwise_i16x8_s(MbFunc* f) { mb_simd_op(f, 0x7Eu); }
static inline void mb_i32x4_extadd_pairwise_i16x8_u(MbFunc* f) { mb_simd_op(f, 0x7Fu); }

static inline void mb_i16x8_abs(MbFunc* f)                { mb_simd_op(f, 0x80u); }
static inline void mb_i16x8_neg(MbFunc* f)                { mb_simd_op(f, 0x81u); }
static inline void mb_i16x8_q15mulr_sat_s(MbFunc* f)      { mb_simd_op(f, 0x82u); }
static inline void mb_i16x8_all_true(MbFunc* f)           { mb_simd_op(f, 0x83u); }
static inline void mb_i16x8_bitmask(MbFunc* f)            { mb_simd_op(f, 0x84u); }
static inline void mb_i16x8_extend_low_i8x16_s(MbFunc* f) { mb_simd_op(f, 0x87u); }
static inline void mb_i16x8_extend_high_i8x16_s(MbFunc* f){ mb_simd_op(f, 0x88u); }
static inline void mb_i16x8_extend_low_i8x16_u(MbFunc* f) { mb_simd_op(f, 0x89u); }
static inline void mb_i16x8_extend_high_i8x16_u(MbFunc* f){ mb_simd_op(f, 0x8Au); }
static inline void mb_i16x8_shl(MbFunc* f)                { mb_simd_op(f, 0x8Bu); }
static inline void mb_i16x8_shr_s(MbFunc* f)              { mb_simd_op(f, 0x8Cu); }
static inline void mb_i16x8_shr_u(MbFunc* f)              { mb_simd_op(f, 0x8Du); }
static inline void mb_i16x8_add(MbFunc* f)                { mb_simd_op(f, 0x8Eu); }
static inline void mb_i16x8_add_sat_s(MbFunc* f)          { mb_simd_op(f, 0x8Fu); }
static inline void mb_i16x8_add_sat_u(MbFunc* f)          { mb_simd_op(f, 0x90u); }
static inline void mb_i16x8_sub(MbFunc* f)                { mb_simd_op(f, 0x91u); }
static inline void mb_i16x8_sub_sat_s(MbFunc* f)          { mb_simd_op(f, 0x92u); }
static inline void mb_i16x8_sub_sat_u(MbFunc* f)          { mb_simd_op(f, 0x93u); }
static inline void mb_f64x2_nearest(MbFunc* f)            { mb_simd_op(f, 0x94u); }
static inline void mb_i16x8_mul(MbFunc* f)                { mb_simd_op(f, 0x95u); }
static inline void mb_i16x8_min_s(MbFunc* f)              { mb_simd_op(f, 0x96u); }
static inline void mb_i16x8_min_u(MbFunc* f)              { mb_simd_op(f, 0x97u); }
static inline void mb_i16x8_max_s(MbFunc* f)              { mb_simd_op(f, 0x98u); }
static inline void mb_i16x8_max_u(MbFunc* f)              { mb_simd_op(f, 0x99u); }
static inline void mb_i16x8_avgr_u(MbFunc* f)             { mb_simd_op(f, 0x9Bu); }
static inline void mb_i16x8_extmul_low_i8x16_s(MbFunc* f) { mb_simd_op(f, 0x9Cu); }
static inline void mb_i16x8_extmul_high_i8x16_s(MbFunc* f){ mb_simd_op(f, 0x9Du); }
static inline void mb_i16x8_extmul_low_i8x16_u(MbFunc* f) { mb_simd_op(f, 0x9Eu); }
static inline void mb_i16x8_extmul_high_i8x16_u(MbFunc* f){ mb_simd_op(f, 0x9Fu); }

static inline void mb_i32x4_extend_low_i16x8_s(MbFunc* f) { mb_simd_op(f, 0xA7u); }
static inline void mb_i32x4_extend_high_i16x8_s(MbFunc* f){ mb_simd_op(f, 0xA8u); }
static inline void mb_i32x4_extend_low_i16x8_u(MbFunc* f) { mb_simd_op(f, 0xA9u); }
static inline void mb_i32x4_extend_high_i16x8_u(MbFunc* f){ mb_simd_op(f, 0xAAu); }
static inline void mb_i32x4_dot_i16x8_s(MbFunc* f)        { mb_simd_op(f, 0xBAu); }
static inline void mb_i32x4_extmul_low_i16x8_s(MbFunc* f) { mb_simd_op(f, 0xBCu); }
static inline void mb_i32x4_extmul_high_i16x8_s(MbFunc* f){ mb_simd_op(f, 0xBDu); }
static inline void mb_i32x4_extmul_low_i16x8_u(MbFunc* f) { mb_simd_op(f, 0xBEu); }
static inline void mb_i32x4_extmul_high_i16x8_u(MbFunc* f){ mb_simd_op(f, 0xBFu); }

static inline void mb_i64x2_abs(MbFunc* f)                { mb_simd_op(f, 0xC0u); }
static inline void mb_i64x2_neg(MbFunc* f)                { mb_simd_op(f, 0xC1u); }
static inline void mb_i64x2_all_true(MbFunc* f)           { mb_simd_op(f, 0xC3u); }
static inline void mb_i64x2_bitmask(MbFunc* f)            { mb_simd_op(f, 0xC4u); }
static inline void mb_i64x2_extend_low_i32x4_s(MbFunc* f) { mb_simd_op(f, 0xC7u); }
static inline void mb_i64x2_extend_high_i32x4_s(MbFunc* f){ mb_simd_op(f, 0xC8u); }
static inline void mb_i64x2_extend_low_i32x4_u(MbFunc* f) { mb_simd_op(f, 0xC9u); }
static inline void mb_i64x2_extend_high_i32x4_u(MbFunc* f){ mb_simd_op(f, 0xCAu); }
static inline void mb_i64x2_shl(MbFunc* f)                { mb_simd_op(f, 0xCBu); }
static inline void mb_i64x2_shr_s(MbFunc* f)              { mb_simd_op(f, 0xCCu); }
static inline void mb_i64x2_shr_u(MbFunc* f)              { mb_simd_op(f, 0xCDu); }
static inline void mb_i64x2_add(MbFunc* f)                { mb_simd_op(f, 0xCEu); }
static inline void mb_i64x2_sub(MbFunc* f)                { mb_simd_op(f, 0xD1u); }
static inline void mb_i64x2_mul(MbFunc* f)                { mb_simd_op(f, 0xD5u); }
static inline void mb_i64x2_eq(MbFunc* f)                 { mb_simd_op(f, 0xD6u); }
static inline void mb_i64x2_ne(MbFunc* f)                 { mb_simd_op(f, 0xD7u); }
static inline void mb_i64x2_lt_s(MbFunc* f)               { mb_simd_op(f, 0xD8u); }
static inline void mb_i64x2_gt_s(MbFunc* f)               { mb_simd_op(f, 0xD9u); }
static inline void mb_i64x2_le_s(MbFunc* f)               { mb_simd_op(f, 0xDAu); }
static inline void mb_i64x2_ge_s(MbFunc* f)               { mb_simd_op(f, 0xDBu); }
static inline void mb_i64x2_extmul_low_i32x4_s(MbFunc* f) { mb_simd_op(f, 0xDCu); }
static inline void mb_i64x2_extmul_high_i32x4_s(MbFunc* f){ mb_simd_op(f, 0xDDu); }
static inline void mb_i64x2_extmul_low_i32x4_u(MbFunc* f) { mb_simd_op(f, 0xDEu); }
static inline void mb_i64x2_extmul_high_i32x4_u(MbFunc* f){ mb_simd_op(f, 0xDFu); }

static inline void mb_f32x4_pmin(MbFunc* f) { mb_simd_op(f, 0xEAu); }
static inline void mb_f32x4_pmax(MbFunc* f) { mb_simd_op(f, 0xEBu); }
static inline void mb_f64x2_abs(MbFunc* f)  { mb_simd_op(f, 0xECu); }
static inline void mb_f64x2_neg(MbFunc* f)  { mb_simd_op(f, 0xEDu); }
static inline void mb_f64x2_sqrt(MbFunc* f) { mb_simd_op(f, 0xEFu); }
static inline void mb_f64x2_add(MbFunc* f)  { mb_simd_op(f, 0xF0u); }
static inline void mb_f64x2_sub(MbFunc* f)  { mb_simd_op(f, 0xF1u); }
static inline void mb_f64x2_mul(MbFunc* f)  { mb_simd_op(f, 0xF2u); }
static inline void mb_f64x2_div(MbFunc* f)  { mb_simd_op(f, 0xF3u); }
static inline void mb_f64x2_min(MbFunc* f)  { mb_simd_op(f, 0xF4u); }
static inline void mb_f64x2_max(MbFunc* f)  { mb_simd_op(f, 0xF5u); }
static inline void mb_f64x2_pmin(MbFunc* f) { mb_simd_op(f, 0xF6u); }
static inline void mb_f64x2_pmax(MbFunc* f) { mb_simd_op(f, 0xF7u); }

static inline void mb_i32x4_trunc_sat_f64x2_s_zero(MbFunc* f) { mb_simd_op(f, 0xFCu); }
static inline void mb_i32x4_trunc_sat_f64x2_u_zero(MbFunc* f) { mb_simd_op(f, 0xFDu); }
static inline void mb_f64x2_convert_low_i32x4_s(MbFunc* f)    { mb_simd_op(f, 0xFEu); }
static inline void mb_f64x2_convert_low_i32x4_u(MbFunc* f)    { mb_simd_op(f, 0xFFu); }

#endif /* MBIN_MBIN_OPS_H */
