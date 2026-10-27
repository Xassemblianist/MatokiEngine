/* img/jpeg.c — Sıfırdan Baseline (SOF0/SOF1) ve Progressive (SOF2) JPEG çözücü.
 * Desteklenenler: SOI/APPn/DQT/SOF0/SOF1/SOF2/DHT/DRI/SOS/EOI, 9-bit hızlı Huffman tablosu,
 * FF00 bayt doldurma, RST0..RST7 yeniden başlatma, 8-bit 1 (gri) ve 3 bileşen (YCbCr),
 * 4:4:4 / 4:2:2 / 4:2:0 ve genel h/v örnekleme, sabit noktalı LLM IDCT, çift-doğrusal kroma yükseltme,
 * EXIF yön okuma. CMYK/Adobe, 12-bit ve aritmetik kodlama hata döner. Tüm okumalar sınır denetimlidir.
 */
#include "img/jpeg.h"
#include "raster/framebuffer.h"

#define JPEG_MAX_DIM   16384u
#define JPEG_MAX_PX    (64u * 1024u * 1024u)
#define JPEG_MAX_COMPS 3u
#define JPEG_MAX_SCANS 128u
#define HUFF_FAST_BITS 9u
#define HUFF_FAST_SIZE (1u << HUFF_FAST_BITS)

static const char* g_jpeg_err = "";
static u32         g_jpeg_orient = 1u;

const char* jpeg_last_error(void) {
    return g_jpeg_err;
}

u32 jpeg_orientation(void) {
    return g_jpeg_orient;
}

static inline void* jpeg_fail(Arena* arena, usize mark, const char* msg) {
    g_jpeg_err = msg;
    if (arena) arena_pop_to(arena, mark);
    return NULL;
}

static inline u16 read_be16(const u8* p) {
    return (u16)(((u16)p[0] << 8u) | (u16)p[1]);
}

static inline u16 read_le16(const u8* p) {
    return (u16)(((u16)p[1] << 8u) | (u16)p[0]);
}

static inline u32 read_be32(const u8* p) {
    return ((u32)p[0] << 24u) | ((u32)p[1] << 16u) | ((u32)p[2] << 8u) | (u32)p[3];
}

static inline u32 read_le32(const u8* p) {
    return ((u32)p[3] << 24u) | ((u32)p[2] << 16u) | ((u32)p[1] << 8u) | (u32)p[0];
}

/* Zigzag sıra indeksinden (0..63) 8x8 doğal blok indeksine (row*8 + col) dönüşüm tablosu */
static const u8 JPEG_UNZIGZAG[64] = {
     0,  1,  8, 16,  9,  2,  3, 10,
    17, 24, 32, 25, 18, 11,  4,  5,
    12, 19, 26, 33, 40, 48, 41, 34,
    27, 20, 13,  6,  7, 14, 21, 28,
    35, 42, 49, 56, 57, 50, 43, 36,
    29, 22, 15, 23, 30, 37, 44, 51,
    58, 59, 52, 45, 38, 31, 39, 46,
    53, 60, 61, 54, 47, 55, 62, 63
};

/* ============================================================================
 * SABİT NOKTALI LLM TAMSAYI IDCT & YCbCr -> RGBA8 (Skaler Referans)
 * ============================================================================ */

static inline u8 clamp_u8_i32(i32 x) {
    if (x < 0) return 0u;
    if (x > 255) return 255u;
    return (u8)x;
}

void jpeg_idct_block_scalar(const i16* block_zz, const u16* quant_zz, u8* out, u32 stride) {
    i32 d[64];
    for (u32 i = 0; i < 64; i++) d[i] = 0;
    for (u32 k = 0; k < 64; k++) {
        i32 c = (i32)block_zz[k] * (i32)quant_zz[k];
        d[JPEG_UNZIGZAG[k]] = CLAMP(c, -8192, 8192);
    }

    i32 tmp[64];
    /* 1. Geçiş: Sütunlar (CONST_BITS = 13, PASS1_BITS = 2, sağa kaydırma = 11) */
    for (u32 col = 0; col < 8; col++) {
        i32 s0 = d[0 * 8 + col];
        i32 s1 = d[1 * 8 + col];
        i32 s2 = d[2 * 8 + col];
        i32 s3 = d[3 * 8 + col];
        i32 s4 = d[4 * 8 + col];
        i32 s5 = d[5 * 8 + col];
        i32 s6 = d[6 * 8 + col];
        i32 s7 = d[7 * 8 + col];

        i32 p2 = s2;
        i32 p3 = s6;
        i32 p1 = (p2 + p3) * 4433;
        i32 t2 = p1 + p3 * (-15137);
        i32 t3 = p1 + p2 * 6270;

        p2 = s0;
        p3 = s4;
        i32 t0 = (i32)((u32)(p2 + p3) << 13u);
        i32 t1 = (i32)((u32)(p2 - p3) << 13u);

        i32 x0 = t0 + t3 + 1024;
        i32 x3 = t0 - t3 + 1024;
        i32 x1 = t1 + t2 + 1024;
        i32 x2 = t1 - t2 + 1024;

        t0 = s7;
        t1 = s5;
        t2 = s3;
        t3 = s1;

        p3 = t0 + t2;
        i32 p4 = t1 + t3;
        p1 = t0 + t3;
        p2 = t1 + t2;
        i32 p5 = (p3 + p4) * 9633;

        t0 = t0 * 2446;
        t1 = t1 * 16819;
        t2 = t2 * 25172;
        t3 = t3 * 12299;

        p1 = p5 + p1 * (-7373);
        p2 = p5 + p2 * (-20995);
        p3 = p3 * (-16069);
        p4 = p4 * (-3196);

        t0 += p1 + p3;
        t1 += p2 + p4;
        t2 += p2 + p3;
        t3 += p1 + p4;

        tmp[0 * 8 + col] = CLAMP((x0 + t3) >> 11, -8192, 8192);
        tmp[7 * 8 + col] = CLAMP((x0 - t3) >> 11, -8192, 8192);
        tmp[1 * 8 + col] = CLAMP((x1 + t2) >> 11, -8192, 8192);
        tmp[6 * 8 + col] = CLAMP((x1 - t2) >> 11, -8192, 8192);
        tmp[2 * 8 + col] = CLAMP((x2 + t1) >> 11, -8192, 8192);
        tmp[5 * 8 + col] = CLAMP((x2 - t1) >> 11, -8192, 8192);
        tmp[3 * 8 + col] = CLAMP((x3 + t0) >> 11, -8192, 8192);
        tmp[4 * 8 + col] = CLAMP((x3 - t0) >> 11, -8192, 8192);
    }

    /* 2. Geçiş: Satırlar (sağa kaydırma = 18, +128 seviye kaydırması) */
    const i32 row_bias = 131072 + (128 << 18);
    for (u32 row = 0; row < 8; row++) {
        const i32* r = tmp + row * 8;
        i32 s0 = r[0], s1 = r[1], s2 = r[2], s3 = r[3];
        i32 s4 = r[4], s5 = r[5], s6 = r[6], s7 = r[7];

        i32 p2 = s2;
        i32 p3 = s6;
        i32 p1 = (p2 + p3) * 4433;
        i32 t2 = p1 + p3 * (-15137);
        i32 t3 = p1 + p2 * 6270;

        p2 = s0;
        p3 = s4;
        i32 t0 = (i32)((u32)(p2 + p3) << 13u);
        i32 t1 = (i32)((u32)(p2 - p3) << 13u);

        i32 x0 = t0 + t3 + row_bias;
        i32 x3 = t0 - t3 + row_bias;
        i32 x1 = t1 + t2 + row_bias;
        i32 x2 = t1 - t2 + row_bias;

        t0 = s7;
        t1 = s5;
        t2 = s3;
        t3 = s1;

        p3 = t0 + t2;
        i32 p4 = t1 + t3;
        p1 = t0 + t3;
        p2 = t1 + t2;
        i32 p5 = (p3 + p4) * 9633;

        t0 = t0 * 2446;
        t1 = t1 * 16819;
        t2 = t2 * 25172;
        t3 = t3 * 12299;

        p1 = p5 + p1 * (-7373);
        p2 = p5 + p2 * (-20995);
        p3 = p3 * (-16069);
        p4 = p4 * (-3196);

        t0 += p1 + p3;
        t1 += p2 + p4;
        t2 += p2 + p3;
        t3 += p1 + p4;

        u8* dst = out + row * stride;
        dst[0] = clamp_u8_i32((x0 + t3) >> 18);
        dst[7] = clamp_u8_i32((x0 - t3) >> 18);
        dst[1] = clamp_u8_i32((x1 + t2) >> 18);
        dst[6] = clamp_u8_i32((x1 - t2) >> 18);
        dst[2] = clamp_u8_i32((x2 + t1) >> 18);
        dst[5] = clamp_u8_i32((x2 - t1) >> 18);
        dst[3] = clamp_u8_i32((x3 + t0) >> 18);
        dst[4] = clamp_u8_i32((x3 - t0) >> 18);
    }
}

void jpeg_ycbcr_to_rgba_row_scalar(const u8* y_row, const u8* cb_row, const u8* cr_row, u32* dst, u32 w) {
    for (u32 x = 0; x < w; x++) {
        i32 y  = (i32)y_row[x];
        i32 cb = (i32)cb_row[x] - 128;
        i32 cr = (i32)cr_row[x] - 128;

        i32 r = y + ((22970 * cr + 8192) >> 14);
        i32 g = y + ((-5638 * cb - 11700 * cr + 8192) >> 14);
        i32 b = y + ((29032 * cb + 8192) >> 14);

        dst[x] = FB_RGBA(clamp_u8_i32(r), clamp_u8_i32(g), clamp_u8_i32(b), 255u);
    }
}

#if !defined(__wasm_simd128__)
void jpeg_idct_block(const i16* block_zz, const u16* quant_zz, u8* out, u32 stride) {
    jpeg_idct_block_scalar(block_zz, quant_zz, out, stride);
}

void jpeg_ycbcr_to_rgba_row(const u8* y_row, const u8* cb_row, const u8* cr_row, u32* dst, u32 w) {
    jpeg_ycbcr_to_rgba_row_scalar(y_row, cb_row, cr_row, dst, w);
}
#endif

/* ============================================================================
 * HUFFMAN TABLOSU VE BİT OKUYUCU
 * ============================================================================ */

typedef struct {
    b32 valid;
    u16 fast[HUFF_FAST_SIZE];
    i32 maxcode[18];
    i32 valoffset[18];
    u8  symbols[256];
    u32 num_syms;
} JpegHuffTable;

static b32 jpeg_build_huff(JpegHuffTable* ht, const u8* bits, const u8* syms, u32 total_syms, b32 is_dc) {
    if (total_syms == 0 || total_syms > 256u) return FALSE;
    for (u32 i = 0; i < total_syms; i++) {
        if (is_dc && syms[i] > 15u) return FALSE;
        ht->symbols[i] = syms[i];
    }
    ht->num_syms = total_syms;
    for (u32 i = 0; i < HUFF_FAST_SIZE; i++) ht->fast[i] = 0u;

    u32 code = 0u;
    u32 k = 0u;
    for (u32 l = 1u; l <= 16u; l++) {
        u32 cnt = bits[l - 1u];
        if (cnt == 0u) {
            ht->maxcode[l] = -1;
            ht->valoffset[l] = 0;
        } else {
            if (code + cnt > (1u << l)) return FALSE; /* Kraft eşitsizliği aşımı */
            ht->valoffset[l] = (i32)k - (i32)code;
            for (u32 i = 0; i < cnt; i++) {
                u8 sym = syms[k];
                if (l <= HUFF_FAST_BITS) {
                    u32 prefix = code << (HUFF_FAST_BITS - l);
                    u32 fill = 1u << (HUFF_FAST_BITS - l);
                    u16 entry = (u16)((l << 8u) | (u16)sym);
                    for (u32 f = 0; f < fill; f++) {
                        ht->fast[prefix + f] = entry;
                    }
                }
                code++;
                k++;
            }
            ht->maxcode[l] = (i32)(code - 1u);
        }
        code <<= 1u;
    }
    ht->valid = TRUE;
    return TRUE;
}

typedef struct {
    const u8* data;
    usize     len;
    usize     pos;
    u64       bitbuf;
    u32       bitcnt;
    u8        marker;
    b32       eof;
    b32       err;
} JpegBitReader;

static void jbr_fill(JpegBitReader* br) {
    while (br->bitcnt <= 56u && !br->marker && !br->eof) {
        if (br->pos >= br->len) {
            br->eof = TRUE;
            break;
        }
        u8 b = br->data[br->pos++];
        if (b == 0xFFu) {
            usize ff_pos = br->pos - 1u;
            if (br->pos >= br->len) {
                br->eof = TRUE;
                break;
            }
            u8 next = br->data[br->pos++];
            if (next == 0x00u) {
                b = 0xFFu;
            } else {
                while (next == 0xFFu) {
                    ff_pos = br->pos - 1u;
                    if (br->pos >= br->len) {
                        br->eof = TRUE;
                        break;
                    }
                    next = br->data[br->pos++];
                }
                if (br->eof) break;
                if (next == 0x00u) {
                    b = 0xFFu;
                } else {
                    br->pos = ff_pos;
                    br->marker = next;
                    break;
                }
            }
        }
        br->bitbuf = (br->bitbuf << 8u) | (u64)b;
        br->bitcnt += 8u;
    }
}

static inline i32 jbr_huff_decode(JpegBitReader* br, const JpegHuffTable* ht) {
    if (br->bitcnt < 16u) jbr_fill(br);
    if (br->bitcnt >= HUFF_FAST_BITS) {
        u32 peek = (u32)((br->bitbuf >> (br->bitcnt - HUFF_FAST_BITS)) & (HUFF_FAST_SIZE - 1u));
        u16 fast = ht->fast[peek];
        if (fast != 0u) {
            u32 l = (u32)(fast >> 8u);
            br->bitcnt -= l;
            return (i32)(fast & 0xFFu);
        }
    } else if (br->bitcnt > 0u) {
        u32 peek = (u32)((br->bitbuf << (HUFF_FAST_BITS - br->bitcnt)) & (HUFF_FAST_SIZE - 1u));
        u16 fast = ht->fast[peek];
        if (fast != 0u) {
            u32 l = (u32)(fast >> 8u);
            if (l <= br->bitcnt) {
                br->bitcnt -= l;
                return (i32)(fast & 0xFFu);
            }
            br->err = TRUE;
            return -1;
        }
    } else {
        br->err = TRUE;
        return -1;
    }

    for (u32 l = HUFF_FAST_BITS + 1u; l <= 16u; l++) {
        if (br->bitcnt < l) {
            br->err = TRUE;
            return -1;
        }
        i32 code = (i32)((br->bitbuf >> (br->bitcnt - l)) & ((1ULL << l) - 1ULL));
        if (code <= ht->maxcode[l]) {
            i32 idx = ht->valoffset[l] + code;
            if (idx < 0 || (u32)idx >= ht->num_syms) {
                br->err = TRUE;
                return -1;
            }
            br->bitcnt -= l;
            return (i32)ht->symbols[idx];
        }
    }
    br->err = TRUE;
    return -1;
}

static inline u32 jbr_get_bits(JpegBitReader* br, u32 n) {
    if (n == 0u) return 0u;
    if (br->bitcnt < n) jbr_fill(br);
    if (br->bitcnt < n) {
        br->err = TRUE;
        return 0u;
    }
    br->bitcnt -= n;
    return (u32)((br->bitbuf >> br->bitcnt) & ((1ULL << n) - 1ULL));
}

static inline i32 jpeg_extend(u32 v, u32 n) {
    if (n == 0u) return 0;
    u32 vt = 1u << (n - 1u);
    if (v < vt) {
        return (i32)v - (i32)((1u << n) - 1u);
    }
    return (i32)v;
}

static b32 jbr_consume_restart(JpegBitReader* br, u32 expected_rst) {
    if (!br->marker) jbr_fill(br);
    while (!br->marker && br->pos < br->len) {
        u8 b = br->data[br->pos++];
        if (b == 0xFFu) {
            while (br->pos < br->len && br->data[br->pos] == 0xFFu) br->pos++;
            if (br->pos >= br->len) return FALSE;
            u8 next = br->data[br->pos];
            if (next != 0x00u) {
                br->pos--; /* 0xFF üzerine geri gel */
                br->marker = next;
                break;
            }
        }
    }
    u8 want = (u8)(0xD0u + (expected_rst & 7u));
    if (br->marker != want) return FALSE;
    if (br->pos + 2u > br->len) return FALSE;
    br->pos += 2u; /* 0xFF ve RSTn baytlarını atla */
    br->marker = 0u;
    br->bitbuf = 0u;
    br->bitcnt = 0u;
    br->eof = FALSE;
    return TRUE;
}

/* ============================================================================
 * EXIF YÖN (ORIENTATION) OKUMA (APP1)
 * ============================================================================ */

static void jpeg_parse_exif(const u8* seg, usize seg_len) {
    /* "Exif\0\0" (6 bayt) + TIFF başlığı (8 bayt) = en az 14 bayt */
    if (seg_len < 14u) return;
    if (seg[0] != 'E' || seg[1] != 'x' || seg[2] != 'i' || seg[3] != 'f' ||
        seg[4] != 0u  || seg[5] != 0u) {
        return;
    }
    const u8* tiff = seg + 6u;
    usize tiff_len = seg_len - 6u;

    b32 le = FALSE;
    if (tiff[0] == 'I' && tiff[1] == 'I') le = TRUE;
    else if (tiff[0] == 'M' && tiff[1] == 'M') le = FALSE;
    else return;

    u16 magic = le ? read_le16(tiff + 2u) : read_be16(tiff + 2u);
    if (magic != 42u) return;

    u32 ifd_off = le ? read_le32(tiff + 4u) : read_be32(tiff + 4u);
    if (ifd_off < 8u || (usize)ifd_off + 2u > tiff_len) return;

    u16 count = le ? read_le16(tiff + ifd_off) : read_be16(tiff + ifd_off);
    usize entries_off = (usize)ifd_off + 2u;
    for (u32 i = 0; i < count; i++) {
        usize epos = entries_off + (usize)i * 12u;
        if (epos + 12u > tiff_len) break;
        const u8* entry = tiff + epos;
        u16 tag  = le ? read_le16(entry + 0u) : read_be16(entry + 0u);
        u16 type = le ? read_le16(entry + 2u) : read_be16(entry + 2u);
        u32 cnt  = le ? read_le32(entry + 4u) : read_be32(entry + 4u);
        if (tag == 0x0112u && cnt >= 1u) {
            u32 val = 0u;
            if (type == 3u) {
                val = le ? read_le16(entry + 8u) : read_be16(entry + 8u);
            } else if (type == 4u) {
                val = le ? read_le32(entry + 8u) : read_be32(entry + 8u);
            }
            if (val >= 1u && val <= 8u) {
                g_jpeg_orient = val;
            }
            return;
        }
    }
}

/* ============================================================================
 * JPEG ÇÖZÜCÜ DURUMU VE TARAMA (SCAN) İŞLEMCİLERİ
 * ============================================================================ */

typedef struct {
    u8   id;
    u8   h_samp;
    u8   v_samp;
    u8   tq;
    u8   td;
    u8   ta;
    i32  dc_pred;
    u32  comp_w;
    u32  comp_h;
    u32  blocks_w;
    u32  blocks_h;
    u32  scan_blocks_w;
    u32  scan_blocks_h;
    i16* coeffs; /* [blocks_h * blocks_w * 64], zigzag sırada */
    u8*  plane;  /* [blocks_h * 8 * blocks_w * 8] IDCT çıktısı */
} JpegComp;

typedef struct {
    u32           w;
    u32           h;
    u32           num_comps;
    b32           progressive;
    u8            h_max;
    u8            v_max;
    u32           mcu_w;
    u32           mcu_h;
    u32           mcu_cols;
    u32           mcu_rows;
    u32           restart_interval;
    b32           quant_valid[4];
    u16           quant[4][64]; /* zigzag sırada */
    JpegHuffTable dc_huff[4];
    JpegHuffTable ac_huff[4];
    JpegComp      comps[JPEG_MAX_COMPS];
} JpegState;

static b32 jpeg_decode_block_seq(JpegBitReader* br, JpegComp* comp,
                                 const JpegHuffTable* dc_ht, const JpegHuffTable* ac_ht,
                                 i16* block) {
    i32 s = jbr_huff_decode(br, dc_ht);
    if (s < 0 || s > 15 || br->err) return FALSE;
    i32 diff = 0;
    if (s > 0) {
        u32 bits = jbr_get_bits(br, (u32)s);
        if (br->err) return FALSE;
        diff = jpeg_extend(bits, (u32)s);
    }
    i64 dc = (i64)comp->dc_pred + (i64)diff;
    if (dc < -32768 || dc > 32767) return FALSE;
    comp->dc_pred = (i32)dc;
    block[0] = (i16)dc;

    u32 k = 1u;
    while (k < 64u) {
        i32 rs = jbr_huff_decode(br, ac_ht);
        if (rs < 0 || br->err) return FALSE;
        u32 r = (u32)(rs >> 4u);
        u32 sz = (u32)(rs & 0x0Fu);
        if (sz == 0u) {
            if (r == 15u) {
                k += 16u;
                if (k > 64u) return FALSE;
            } else {
                break; /* EOB */
            }
        } else {
            if (sz > 10u) return FALSE;
            k += r;
            if (k >= 64u) return FALSE;
            u32 bits = jbr_get_bits(br, sz);
            if (br->err) return FALSE;
            block[k] = (i16)jpeg_extend(bits, sz);
            k++;
        }
    }
    return TRUE;
}

static b32 jpeg_decode_block_prog_dc_first(JpegBitReader* br, JpegComp* comp,
                                           const JpegHuffTable* dc_ht, u32 al, i16* block) {
    i32 s = jbr_huff_decode(br, dc_ht);
    if (s < 0 || s > 15 || br->err) return FALSE;
    i32 diff = 0;
    if (s > 0) {
        u32 bits = jbr_get_bits(br, (u32)s);
        if (br->err) return FALSE;
        diff = jpeg_extend(bits, (u32)s);
    }
    i64 dc = (i64)comp->dc_pred + (i64)diff;
    if (dc < -16384 || dc > 16383) return FALSE;
    comp->dc_pred = (i32)dc;
    i64 shifted = dc * (i64)(1u << al);
    if (shifted < -32768 || shifted > 32767) return FALSE;
    block[0] = (i16)shifted;
    return TRUE;
}

static b32 jpeg_decode_block_prog_dc_refine(JpegBitReader* br, u32 al, i16* block) {
    u32 bit = jbr_get_bits(br, 1u);
    if (br->err) return FALSE;
    if (bit) {
        block[0] = (i16)((u16)block[0] | (u16)(1u << al));
    }
    return TRUE;
}

static b32 jpeg_decode_block_prog_ac_first(JpegBitReader* br, const JpegHuffTable* ac_ht,
                                           u32 ss, u32 se, u32 al, u32* eobrun, i16* block) {
    if (*eobrun > 0u) {
        (*eobrun)--;
        return TRUE;
    }
    for (u32 k = ss; k <= se; k++) {
        i32 rs = jbr_huff_decode(br, ac_ht);
        if (rs < 0 || br->err) return FALSE;
        u32 r  = (u32)(rs >> 4u);
        u32 sz = (u32)(rs & 0x0Fu);
        if (sz == 0u) {
            if (r == 15u) {
                k += 15u;
                if (k > se) return FALSE;
            } else {
                u32 eob = 1u << r;
                if (r > 0u) {
                    u32 extra = jbr_get_bits(br, r);
                    if (br->err) return FALSE;
                    eob += extra;
                }
                *eobrun = eob - 1u;
                break;
            }
        } else {
            if (sz > 10u) return FALSE;
            k += r;
            if (k > se) return FALSE;
            u32 bits = jbr_get_bits(br, sz);
            if (br->err) return FALSE;
            i64 v = (i64)jpeg_extend(bits, sz) * (i64)(1u << al);
            if (v < -32768 || v > 32767) return FALSE;
            block[k] = (i16)v;
        }
    }
    return TRUE;
}

static b32 jpeg_decode_block_prog_ac_refine(JpegBitReader* br, const JpegHuffTable* ac_ht,
                                            u32 ss, u32 se, u32 al, u32* eobrun, i16* block) {
    i32 p1 = (i32)(1u << al);
    i32 m1 = -p1;
    u32 k = ss;

    if (*eobrun == 0u) {
        while (k <= se) {
            i32 rs = jbr_huff_decode(br, ac_ht);
            if (rs < 0 || br->err) return FALSE;
            u32 r  = (u32)(rs >> 4u);
            u32 sz = (u32)(rs & 0x0Fu);
            i32 new_val = 0;

            if (sz == 0u) {
                if (r < 15u) {
                    u32 eob = 1u << r;
                    if (r > 0u) {
                        u32 extra = jbr_get_bits(br, r);
                        if (br->err) return FALSE;
                        eob += extra;
                    }
                    *eobrun = eob;
                    break;
                }
                /* r == 15 (ZRL): 16 sıfır atla */
            } else if (sz == 1u) {
                u32 sign = jbr_get_bits(br, 1u);
                if (br->err) return FALSE;
                new_val = sign ? p1 : m1;
            } else {
                return FALSE;
            }

            for (; k <= se; k++) {
                i32 coef = block[k];
                if (coef != 0) {
                    u32 bit = jbr_get_bits(br, 1u);
                    if (br->err) return FALSE;
                    if (bit && (coef & p1) == 0) {
                        i32 upd = coef + (coef > 0 ? p1 : m1);
                        if (upd < -32768 || upd > 32767) return FALSE;
                        block[k] = (i16)upd;
                    }
                } else {
                    if (r == 0u) break;
                    r--;
                }
            }
            if (k > se) return FALSE;
            if (sz == 1u) {
                block[k] = (i16)new_val;
            }
            k++;
        }
    }

    if (*eobrun > 0u) {
        for (; k <= se; k++) {
            i32 coef = block[k];
            if (coef != 0) {
                u32 bit = jbr_get_bits(br, 1u);
                if (br->err) return FALSE;
                if (bit && (coef & p1) == 0) {
                    i32 upd = coef + (coef > 0 ? p1 : m1);
                    if (upd < -32768 || upd > 32767) return FALSE;
                    block[k] = (i16)upd;
                }
            }
        }
        (*eobrun)--;
    }
    return TRUE;
}

static b32 jpeg_process_scan(JpegState* st, const u8* scan_comp_indices, u32 ns,
                             u32 ss, u32 se, u32 ah, u32 al, JpegBitReader* br) {
    /* Tablo geçerliliklerini doğrula */
    for (u32 i = 0; i < ns; i++) {
        JpegComp* c = &st->comps[scan_comp_indices[i]];
        if (!st->quant_valid[c->tq]) return FALSE;
        if (ss == 0u && ah == 0u && !st->dc_huff[c->td].valid) return FALSE;
        if (se > 0u && !st->ac_huff[c->ta].valid) return FALSE;
        c->dc_pred = 0;
    }

    u32 eobrun = 0u;
    u32 rst_todo = st->restart_interval;
    u32 rst_next = 0u;

    if (ns == 1u) {
        /* Etkileşimsiz (non-interleaved) tarama: yalnız aktif 8x8 bloklar */
        JpegComp* c = &st->comps[scan_comp_indices[0]];
        const JpegHuffTable* dc_ht = &st->dc_huff[c->td];
        const JpegHuffTable* ac_ht = &st->ac_huff[c->ta];
        u32 total_mcus = c->scan_blocks_w * c->scan_blocks_h;
        u32 mcu_idx = 0u;

        for (u32 by = 0; by < c->scan_blocks_h; by++) {
            for (u32 bx = 0; bx < c->scan_blocks_w; bx++) {
                i16* block = c->coeffs + ((usize)by * c->blocks_w + bx) * 64u;
                b32 ok = FALSE;
                if (!st->progressive) {
                    ok = jpeg_decode_block_seq(br, c, dc_ht, ac_ht, block);
                } else if (ss == 0u) {
                    if (ah == 0u) ok = jpeg_decode_block_prog_dc_first(br, c, dc_ht, al, block);
                    else          ok = jpeg_decode_block_prog_dc_refine(br, al, block);
                } else {
                    if (ah == 0u) ok = jpeg_decode_block_prog_ac_first(br, ac_ht, ss, se, al, &eobrun, block);
                    else          ok = jpeg_decode_block_prog_ac_refine(br, ac_ht, ss, se, al, &eobrun, block);
                }
                if (!ok) return FALSE;

                mcu_idx++;
                if (st->restart_interval > 0u && mcu_idx < total_mcus) {
                    rst_todo--;
                    if (rst_todo == 0u) {
                        if (!jbr_consume_restart(br, rst_next)) return FALSE;
                        rst_next = (rst_next + 1u) & 7u;
                        rst_todo = st->restart_interval;
                        c->dc_pred = 0;
                        eobrun = 0u;
                    }
                }
            }
        }
    } else {
        /* Etkileşimli (interleaved) tarama */
        u32 total_mcus = st->mcu_cols * st->mcu_rows;
        u32 mcu_idx = 0u;

        for (u32 my = 0; my < st->mcu_rows; my++) {
            for (u32 mx = 0; mx < st->mcu_cols; mx++) {
                for (u32 ci = 0; ci < ns; ci++) {
                    JpegComp* c = &st->comps[scan_comp_indices[ci]];
                    const JpegHuffTable* dc_ht = &st->dc_huff[c->td];
                    const JpegHuffTable* ac_ht = &st->ac_huff[c->ta];

                    for (u32 vy = 0; vy < c->v_samp; vy++) {
                        for (u32 hx = 0; hx < c->h_samp; hx++) {
                            u32 bx = mx * c->h_samp + hx;
                            u32 by = my * c->v_samp + vy;
                            i16* block = c->coeffs + ((usize)by * c->blocks_w + bx) * 64u;
                            b32 ok = FALSE;
                            if (!st->progressive) {
                                ok = jpeg_decode_block_seq(br, c, dc_ht, ac_ht, block);
                            } else {
                                if (ah == 0u) ok = jpeg_decode_block_prog_dc_first(br, c, dc_ht, al, block);
                                else          ok = jpeg_decode_block_prog_dc_refine(br, al, block);
                            }
                            if (!ok) return FALSE;
                        }
                    }
                }

                mcu_idx++;
                if (st->restart_interval > 0u && mcu_idx < total_mcus) {
                    rst_todo--;
                    if (rst_todo == 0u) {
                        if (!jbr_consume_restart(br, rst_next)) return FALSE;
                        rst_next = (rst_next + 1u) & 7u;
                        rst_todo = st->restart_interval;
                        for (u32 ci = 0; ci < ns; ci++) {
                            st->comps[scan_comp_indices[ci]].dc_pred = 0;
                        }
                        eobrun = 0u;
                    }
                }
            }
        }
    }
    return TRUE;
}

/* ============================================================================
 * KROMA YÜKSELTME (CHROMA UPSAMPLING)
 * ============================================================================ */

static void jpeg_upsample_row(const JpegComp* c, u32 y, u32 dst_w, u32 dst_h,
                              u8 h_max, u8 v_max, b32 fancy, u8* out_row) {
    u32 stride = c->blocks_w * 8u;
    u32 cw = c->comp_w;
    u32 ch = c->comp_h;

    if (c->h_samp == h_max && c->v_samp == v_max) {
        const u8* src = c->plane + (usize)y * stride;
        for (u32 x = 0; x < dst_w; x++) out_row[x] = src[x];
        return;
    }

    u32 rx = (u32)h_max / (u32)c->h_samp;
    u32 ry = (u32)v_max / (u32)c->v_samp;

    if (!fancy) {
        u32 sy = (y * c->v_samp) / v_max;
        if (sy >= ch) sy = ch - 1u;
        const u8* src = c->plane + (usize)sy * stride;
        for (u32 x = 0; x < dst_w; x++) {
            u32 sx = (x * c->h_samp) / h_max;
            if (sx >= cw) sx = cw - 1u;
            out_row[x] = src[sx];
        }
        return;
    }

    /* Çift-doğrusal (fancy) 2x1 (4:2:2), 1x2 ve 2x2 (4:2:0) kroma yükseltme */
    if (rx == 2u && ry == 1u && dst_w == cw * 2u) {
        u32 sy = MIN(y, ch - 1u);
        const u8* in = c->plane + (usize)sy * stride;
        if (cw == 1u) {
            for (u32 x = 0; x < dst_w; x++) out_row[x] = in[0];
            return;
        }
        out_row[0] = in[0];
        for (u32 x = 0; x + 1u < cw; x++) {
            u32 dx0 = 2u * x + 1u;
            u32 dx1 = 2u * x + 2u;
            if (dx0 < dst_w) out_row[dx0] = (u8)((3u * (u32)in[x] + (u32)in[x + 1u] + 2u) >> 2u);
            if (dx1 < dst_w) out_row[dx1] = (u8)((3u * (u32)in[x + 1u] + (u32)in[x] + 1u) >> 2u);
        }
        u32 last_dx = 2u * cw - 1u;
        for (u32 dx = last_dx; dx < dst_w; dx++) out_row[dx] = in[cw - 1u];
        return;
    }

    if (rx == 1u && ry == 2u && dst_h == ch * 2u) {
        u32 sy0 = y >> 1u;
        if (sy0 >= ch) sy0 = ch - 1u;
        u32 sy1;
        if (y & 1u) sy1 = (sy0 + 1u < ch) ? (sy0 + 1u) : (ch - 1u);
        else        sy1 = (sy0 > 0u) ? (sy0 - 1u) : 0u;
        const u8* r_near = c->plane + (usize)sy0 * stride;
        const u8* r_far  = c->plane + (usize)sy1 * stride;
        for (u32 x = 0; x < dst_w; x++) {
            u32 sx = MIN(x, cw - 1u);
            out_row[x] = (u8)((3u * (u32)r_near[sx] + (u32)r_far[sx] + 2u) >> 2u);
        }
        return;
    }

    if (rx == 2u && ry == 2u && dst_w == cw * 2u && dst_h == ch * 2u) {
        u32 sy0 = y >> 1u;
        if (sy0 >= ch) sy0 = ch - 1u;
        u32 sy1;
        if (y & 1u) sy1 = (sy0 + 1u < ch) ? (sy0 + 1u) : (ch - 1u);
        else        sy1 = (sy0 > 0u) ? (sy0 - 1u) : 0u;
        const u8* r_near = c->plane + (usize)sy0 * stride;
        const u8* r_far  = c->plane + (usize)sy1 * stride;

        if (cw == 1u) {
            u8 v = (u8)((3u * (u32)r_near[0] + (u32)r_far[0] + 2u) >> 2u);
            for (u32 x = 0; x < dst_w; x++) out_row[x] = v;
            return;
        }

        u32 c0 = 3u * (u32)r_near[0] + (u32)r_far[0];
        out_row[0] = (u8)((c0 + 2u) >> 2u);
        for (u32 x = 0; x + 1u < cw; x++) {
            u32 ca = 3u * (u32)r_near[x]      + (u32)r_far[x];
            u32 cb = 3u * (u32)r_near[x + 1u] + (u32)r_far[x + 1u];
            u32 dx0 = 2u * x + 1u;
            u32 dx1 = 2u * x + 2u;
            if (dx0 < dst_w) out_row[dx0] = (u8)((3u * ca + cb + 8u) >> 4u);
            if (dx1 < dst_w) out_row[dx1] = (u8)((3u * cb + ca + 7u) >> 4u);
        }
        u32 clast = 3u * (u32)r_near[cw - 1u] + (u32)r_far[cw - 1u];
        u8  vlast = (u8)((clast + 2u) >> 2u);
        for (u32 dx = 2u * cw - 1u; dx < dst_w; dx++) out_row[dx] = vlast;
        return;
    }

    /* Genel h/v örnekleme için sabit nokta çift-doğrusal interpolasyon */
    for (u32 x = 0; x < dst_w; x++) {
        /* Merkez hizalı koordinat: ((2*x + 1)*cw - dst_w) / (2*dst_w) */
        i32 num_x = (i32)((2u * x + 1u) * cw) - (i32)dst_w;
        i32 den_x = (i32)(2u * dst_w);
        i32 sx0 = (num_x >= 0) ? (num_x / den_x) : -1;
        i32 fx  = (num_x >= 0) ? ((num_x % den_x) * 256 / den_x) : 0;
        i32 sx1 = sx0 + 1;
        sx0 = CLAMP(sx0, 0, (i32)cw - 1);
        sx1 = CLAMP(sx1, 0, (i32)cw - 1);

        i32 num_y = (i32)((2u * y + 1u) * ch) - (i32)dst_h;
        i32 den_y = (i32)(2u * dst_h);
        i32 sy0 = (num_y >= 0) ? (num_y / den_y) : -1;
        i32 fy  = (num_y >= 0) ? ((num_y % den_y) * 256 / den_y) : 0;
        i32 sy1 = sy0 + 1;
        sy0 = CLAMP(sy0, 0, (i32)ch - 1);
        sy1 = CLAMP(sy1, 0, (i32)ch - 1);

        const u8* row0 = c->plane + (usize)sy0 * stride;
        const u8* row1 = c->plane + (usize)sy1 * stride;
        i32 v0 = (i32)row0[sx0] * (256 - fx) + (i32)row0[sx1] * fx;
        i32 v1 = (i32)row1[sx0] * (256 - fx) + (i32)row1[sx1] * fx;
        out_row[x] = (u8)((v0 * (256 - fy) + v1 * fy + 32768) >> 16);
    }
}

/* ============================================================================
 * ANA ÇÖZÜCÜ (jpeg_decode_ex / jpeg_decode)
 * ============================================================================ */

u32* jpeg_decode_ex(const u8* data, usize len, u32* out_w, u32* out_h, Arena* arena, b32 fancy_upsample) {
    g_jpeg_err    = "";
    g_jpeg_orient = 1u;

    if (!data || !out_w || !out_h || !arena) {
        g_jpeg_err = "gecersiz arguman (NULL)";
        return NULL;
    }
    *out_w = 0u;
    *out_h = 0u;

    if (len < 4u || data[0] != 0xFFu || data[1] != 0xD8u) {
        g_jpeg_err = "SOI (FFD8) isareti yok";
        return NULL;
    }

    usize mark = arena_mark(arena);
    usize scratch_mark = mark;
    u32*  out_px = NULL;

    JpegState* st = ARENA_PUSH_STRUCT(arena, JpegState);
    b32 got_sof = FALSE;
    b32 got_sos = FALSE;
    b32 got_eoi = FALSE;
    u32 scan_count = 0u;

    usize pos = 2u;
    while (pos < len) {
        /* Sonraki 0xFF işaret başlangıcını bul */
        if (data[pos] != 0xFFu) {
            return (u32*)jpeg_fail(arena, mark, "beklenen isaret (0xFF) bulunamadi");
        }
        while (pos < len && data[pos] == 0xFFu) pos++;
        if (pos >= len) {
            return (u32*)jpeg_fail(arena, mark, "kesik isaret basligi");
        }
        u8 marker = data[pos++];

        if (marker == 0x00u || marker == 0xFFu) {
            return (u32*)jpeg_fail(arena, mark, "gecersiz sifir isareti");
        }
        if (marker == 0xD8u) {
            return (u32*)jpeg_fail(arena, mark, "yinelenen SOI isareti");
        }
        if (marker == 0xD9u) { /* EOI */
            got_eoi = TRUE;
            break;
        }
        if (marker == 0x01u || (marker >= 0xD0u && marker <= 0xD7u)) {
            continue;
        }

        /* Uzunluklu segment */
        if (pos + 2u > len) {
            return (u32*)jpeg_fail(arena, mark, "kesik segment uzunlugu");
        }
        u16 seg_len = read_be16(data + pos);
        if (seg_len < 2u || pos + (usize)seg_len > len) {
            return (u32*)jpeg_fail(arena, mark, "segment dosya boyutunu asiyor");
        }
        const u8* seg = data + pos + 2u;
        usize plen = (usize)seg_len - 2u;
        pos += (usize)seg_len;

        if (marker == 0xE1u) { /* APP1 (EXIF) */
            jpeg_parse_exif(seg, plen);
        } else if (marker == 0xEEu) { /* APP14 (Adobe) */
            if (plen >= 5u && seg[0] == 'A' && seg[1] == 'd' && seg[2] == 'o' &&
                seg[3] == 'b' && seg[4] == 'e') {
                return (u32*)jpeg_fail(arena, mark, "Adobe APP14 renk uzayi desteklenmez");
            }
        } else if ((marker >= 0xE0u && marker <= 0xEFu) || marker == 0xFEu) {
            /* Diğer APPn ve COM segmentlerini atla */
        } else if (marker == 0xDBu) { /* DQT */
            usize qpos = 0u;
            if (plen == 0u) return (u32*)jpeg_fail(arena, mark, "bos DQT segmenti");
            while (qpos < plen) {
                u8 pq_tq = seg[qpos++];
                u8 pq = pq_tq >> 4u;
                u8 tq = pq_tq & 0x0Fu;
                if (pq > 1u || tq >= 4u) {
                    return (u32*)jpeg_fail(arena, mark, "gecersiz DQT tablosu");
                }
                usize need = (pq == 0u) ? 64u : 128u;
                if (qpos + need > plen) {
                    return (u32*)jpeg_fail(arena, mark, "kesik DQT tablosu");
                }
                for (u32 k = 0; k < 64u; k++) {
                    u16 qv = (pq == 0u) ? (u16)seg[qpos + k] : read_be16(seg + qpos + k * 2u);
                    if (qv == 0u) {
                        return (u32*)jpeg_fail(arena, mark, "sifir DQT katsayisi gecersiz");
                    }
                    st->quant[tq][k] = qv;
                }
                st->quant_valid[tq] = TRUE;
                qpos += need;
            }
        } else if (marker == 0xC4u) { /* DHT */
            usize hpos = 0u;
            if (plen == 0u) return (u32*)jpeg_fail(arena, mark, "bos DHT segmenti");
            while (hpos < plen) {
                if (hpos + 17u > plen) {
                    return (u32*)jpeg_fail(arena, mark, "kesik DHT basligi");
                }
                u8 tc_th = seg[hpos++];
                u8 tc = tc_th >> 4u;
                u8 th = tc_th & 0x0Fu;
                if (tc > 1u || th >= 4u) {
                    return (u32*)jpeg_fail(arena, mark, "gecersiz DHT tablo kimligi");
                }
                const u8* bits = seg + hpos;
                hpos += 16u;
                u32 total = 0u;
                for (u32 i = 0; i < 16u; i++) total += bits[i];
                if (total == 0u || total > 256u || hpos + total > plen) {
                    return (u32*)jpeg_fail(arena, mark, "sifir veya gecersiz Huffman tablosu");
                }
                JpegHuffTable* ht = (tc == 0u) ? &st->dc_huff[th] : &st->ac_huff[th];
                if (!jpeg_build_huff(ht, bits, seg + hpos, total, tc == 0u)) {
                    return (u32*)jpeg_fail(arena, mark, "gecersiz Huffman kod agaci");
                }
                hpos += total;
            }
        } else if (marker == 0xDDu) { /* DRI */
            if (plen != 2u) return (u32*)jpeg_fail(arena, mark, "gecersiz DRI uzunlugu");
            st->restart_interval = read_be16(seg);
        } else if (marker == 0xC0u || marker == 0xC1u || marker == 0xC2u) { /* SOF0, SOF1, SOF2 */
            if (got_sof) return (u32*)jpeg_fail(arena, mark, "birden fazla SOF basligi");
            if (plen < 6u) return (u32*)jpeg_fail(arena, mark, "kesik SOF basligi");

            u8 prec = seg[0];
            if (prec != 8u) {
                return (u32*)jpeg_fail(arena, mark, "yalniz 8-bit desteklenir (12-bit desteklenmez)");
            }
            u32 h = read_be16(seg + 1u);
            u32 w = read_be16(seg + 3u);
            u32 nf = seg[5];

            if (w == 0u || h == 0u) {
                return (u32*)jpeg_fail(arena, mark, "sifir goruntu boyutu");
            }
            if (w > JPEG_MAX_DIM || h > JPEG_MAX_DIM || (u64)w * (u64)h > JPEG_MAX_PX) {
                return (u32*)jpeg_fail(arena, mark, "goruntu boyutu siniri asiyor");
            }
            if (nf != 1u && nf != 3u) {
                return (u32*)jpeg_fail(arena, mark, "desteklenmeyen bilesen sayisi (CMYK desteklenmez)");
            }
            if (plen != 6u + (usize)nf * 3u) {
                return (u32*)jpeg_fail(arena, mark, "SOF uzunlugu bilesen sayisiyla uyusmuyor");
            }

            st->w           = w;
            st->h           = h;
            st->num_comps   = nf;
            st->progressive = (marker == 0xC2u);
            st->h_max       = 1u;
            st->v_max       = 1u;

            u32 mcu_blocks_sum = 0u;
            for (u32 i = 0; i < nf; i++) {
                u8 cid = seg[6u + i * 3u + 0u];
                u8 hv  = seg[6u + i * 3u + 1u];
                u8 tq  = seg[6u + i * 3u + 2u];
                u8 hs  = hv >> 4u;
                u8 vs  = hv & 0x0Fu;
                if (hs == 0u || hs > 4u || vs == 0u || vs > 4u || tq >= 4u) {
                    return (u32*)jpeg_fail(arena, mark, "gecersiz SOF ornekleme veya DQT secicisi");
                }
                for (u32 j = 0; j < i; j++) {
                    if (st->comps[j].id == cid) {
                        return (u32*)jpeg_fail(arena, mark, "yinelenen SOF bilesen kimligi");
                    }
                }
                if (nf == 1u) {
                    hs = 1u;
                    vs = 1u;
                }
                st->comps[i].id     = cid;
                st->comps[i].h_samp = hs;
                st->comps[i].v_samp = vs;
                st->comps[i].tq     = tq;
                if (hs > st->h_max) st->h_max = hs;
                if (vs > st->v_max) st->v_max = vs;
                mcu_blocks_sum += (u32)hs * (u32)vs;
            }
            if (mcu_blocks_sum > 10u) {
                return (u32*)jpeg_fail(arena, mark, "MCU basina blok sayisi 10 sinirini asiyor");
            }
            /* RGB veya CMYK ASCII bileşen kimliklerini reddet (yalnız JFIF 1..3 veya YCbCr) */
            if (nf == 3u && st->comps[0].id == 'R' && st->comps[1].id == 'G' && st->comps[2].id == 'B') {
                return (u32*)jpeg_fail(arena, mark, "ham RGB JPEG desteklenmez");
            }

            st->mcu_w    = (u32)st->h_max * 8u;
            st->mcu_h    = (u32)st->v_max * 8u;
            st->mcu_cols = (w + st->mcu_w - 1u) / st->mcu_w;
            st->mcu_rows = (h + st->mcu_h - 1u) / st->mcu_h;

            /* Gereken toplam belleği önceden hesapla: başlıktaki boyut güvenilmez, arena taşması PANIC olurdu.
             * Çıktı + durum + bileşen başına (katsayı i16 + düzlem u8) + 3 satır tamponu + hizalama payı. */
            {
                u64 need = (u64)w * (u64)h * 4u + sizeof(JpegState) + 3u * ((u64)w + 16u) + 256u;
                for (u32 i = 0; i < nf; i++) {
                    const JpegComp* c = &st->comps[i];
                    u64 nb = (u64)st->mcu_cols * c->h_samp * (u64)st->mcu_rows * c->v_samp;
                    need += nb * 64u * 3u + 32u;
                }
                if (need > (u64)(arena->cap - mark)) {
                    return (u32*)jpeg_fail(arena, mark, "goruntu icin bellek yetersiz");
                }
            }

            /* Çıktı tamponunu en başta ayır, geçici tamponları scratch_mark sonrasına koy */
            JpegState saved_st = *st;
            arena_pop_to(arena, mark);
            out_px = (u32*)arena_push(arena, (usize)w * (usize)h * sizeof(u32), 16);
            scratch_mark = arena_mark(arena);
            st = ARENA_PUSH_STRUCT(arena, JpegState);
            *st = saved_st;

            for (u32 i = 0; i < nf; i++) {
                JpegComp* c = &st->comps[i];
                c->comp_w        = (u32)(((u64)w * c->h_samp + st->h_max - 1u) / st->h_max);
                c->comp_h        = (u32)(((u64)h * c->v_samp + st->v_max - 1u) / st->v_max);
                c->blocks_w      = st->mcu_cols * c->h_samp;
                c->blocks_h      = st->mcu_rows * c->v_samp;
                c->scan_blocks_w = (c->comp_w + 7u) / 8u;
                c->scan_blocks_h = (c->comp_h + 7u) / 8u;
                usize num_blocks = (usize)c->blocks_w * (usize)c->blocks_h;
                c->coeffs        = ARENA_PUSH_ARRAY(arena, i16, num_blocks * 64u);
                c->plane         = ARENA_PUSH_ARRAY(arena, u8,  num_blocks * 64u);
            }
            got_sof = TRUE;
        } else if (marker >= 0xC3u && marker <= 0xCFu) {
            return (u32*)jpeg_fail(arena, mark, "aritmetik veya kayipsiz JPEG desteklenmez");
        } else if (marker == 0xDAu) { /* SOS */
            if (!got_sof) return (u32*)jpeg_fail(arena, mark, "SOF olmadan SOS isareti");
            if (plen < 4u) return (u32*)jpeg_fail(arena, mark, "kesik SOS basligi");
            u8 ns = seg[0];
            if (ns == 0u || ns > st->num_comps || plen != 4u + (usize)ns * 2u) {
                return (u32*)jpeg_fail(arena, mark, "gecersiz SOS bilesen sayisi");
            }
            if (++scan_count > JPEG_MAX_SCANS) {
                return (u32*)jpeg_fail(arena, mark, "cok fazla SOS taramasi");
            }

            u8 scan_comps[JPEG_MAX_COMPS];
            for (u32 i = 0; i < ns; i++) {
                u8 cid   = seg[1u + i * 2u + 0u];
                u8 td_ta = seg[1u + i * 2u + 1u];
                u8 td    = td_ta >> 4u;
                u8 ta    = td_ta & 0x0Fu;
                if (td >= 4u || ta >= 4u) {
                    return (u32*)jpeg_fail(arena, mark, "gecersiz SOS Huffman tablo secicisi");
                }
                i32 found = -1;
                for (u32 j = 0; j < st->num_comps; j++) {
                    if (st->comps[j].id == cid) { found = (i32)j; break; }
                }
                if (found < 0) return (u32*)jpeg_fail(arena, mark, "SOS bilinmeyen bilesen kimligi");
                for (u32 j = 0; j < i; j++) {
                    if (scan_comps[j] == (u8)found) {
                        return (u32*)jpeg_fail(arena, mark, "SOS icinde yinelenen bilesen");
                    }
                }
                scan_comps[i] = (u8)found;
                st->comps[found].td = td;
                st->comps[found].ta = ta;
            }

            u8 ss    = seg[1u + ns * 2u + 0u];
            u8 se    = seg[1u + ns * 2u + 1u];
            u8 ah_al = seg[1u + ns * 2u + 2u];
            u8 ah    = ah_al >> 4u;
            u8 al    = ah_al & 0x0Fu;

            if (!st->progressive) {
                if (ss != 0u || se != 63u || ah != 0u || al != 0u) {
                    return (u32*)jpeg_fail(arena, mark, "baseline taramada gecersiz spektral secim");
                }
            } else {
                if (ss > se || se > 63u || ah > 13u || al > 13u) {
                    return (u32*)jpeg_fail(arena, mark, "progressive taramada gecersiz spektral aralik");
                }
                if (ss == 0u && se != 0u) {
                    return (u32*)jpeg_fail(arena, mark, "progressive DC taramasinda Se != 0");
                }
                if (ss > 0u && ns != 1u) {
                    return (u32*)jpeg_fail(arena, mark, "progressive AC taramasi tek bilesenli olmalidir");
                }
                if (ah != 0u && ah != al + 1u) {
                    return (u32*)jpeg_fail(arena, mark, "progressive iyilestirmede Ah - Al != 1");
                }
            }

            JpegBitReader br;
            br.data   = data;
            br.len    = len;
            br.pos    = pos;
            br.bitbuf = 0u;
            br.bitcnt = 0u;
            br.marker = 0u;
            br.eof    = FALSE;
            br.err    = FALSE;

            if (!jpeg_process_scan(st, scan_comps, ns, ss, se, ah, al, &br)) {
                return (u32*)jpeg_fail(arena, mark, "entropi cozme hatasi (bozuk veya kesik veri)");
            }
            pos = br.pos;
            /* Tarama sonunda olası dolgu baytlarını sonraki 0xFF işaretine kadar atla */
            while (pos < len && data[pos] != 0xFFu) pos++;
            got_sos = TRUE;
        }
    }

    if (!got_sof || !got_sos || !got_eoi) {
        return (u32*)jpeg_fail(arena, mark, "eksik JPEG yapisi (SOF/SOS/EOI)");
    }

    /* Tüm bloklar için ters nicemleme + 8x8 LLM IDCT */
    for (u32 ci = 0; ci < st->num_comps; ci++) {
        JpegComp* c = &st->comps[ci];
        if (!st->quant_valid[c->tq]) {
            return (u32*)jpeg_fail(arena, mark, "atanmamis DQT tablosu");
        }
        const u16* qt = st->quant[c->tq];
        u32 stride = c->blocks_w * 8u;
        u32 active_bw = (c->comp_w + 7u) / 8u;
        u32 active_bh = (c->comp_h + 7u) / 8u;

        for (u32 by = 0; by < active_bh; by++) {
            for (u32 bx = 0; bx < active_bw; bx++) {
                const i16* blk = c->coeffs + ((usize)by * c->blocks_w + bx) * 64u;
                u8* dst_blk = c->plane + (usize)(by * 8u) * stride + bx * 8u;
                jpeg_idct_block(blk, qt, dst_blk, stride);
            }
        }
    }

    /* Kroma yükseltme ve RGBA8 renk dönüşümü */
    u32 w = st->w;
    u32 h = st->h;

    if (st->num_comps == 1u) {
        const JpegComp* cy = &st->comps[0];
        u32 stride = cy->blocks_w * 8u;
        for (u32 y = 0; y < h; y++) {
            const u8* y_row = cy->plane + (usize)y * stride;
            u32* dst_row = out_px + (usize)y * w;
            for (u32 x = 0; x < w; x++) {
                u8 g = y_row[x];
                dst_row[x] = FB_RGBA(g, g, g, 255u);
            }
        }
    } else {
        u8* y_row  = ARENA_PUSH_ARRAY(arena, u8, w + 16u);
        u8* cb_row = ARENA_PUSH_ARRAY(arena, u8, w + 16u);
        u8* cr_row = ARENA_PUSH_ARRAY(arena, u8, w + 16u);

        for (u32 y = 0; y < h; y++) {
            jpeg_upsample_row(&st->comps[0], y, w, h, st->h_max, st->v_max, fancy_upsample, y_row);
            jpeg_upsample_row(&st->comps[1], y, w, h, st->h_max, st->v_max, fancy_upsample, cb_row);
            jpeg_upsample_row(&st->comps[2], y, w, h, st->h_max, st->v_max, fancy_upsample, cr_row);
            jpeg_ycbcr_to_rgba_row(y_row, cb_row, cr_row, out_px + (usize)y * w, w);
        }
    }

    /* Tüm çalışma belleğini serbest bırak, yalnız out_px kalsın */
    arena_pop_to(arena, scratch_mark);
    *out_w = w;
    *out_h = h;
    return out_px;
}

u32* jpeg_decode(const u8* data, usize len, u32* w, u32* h, Arena* arena) {
    return jpeg_decode_ex(data, len, w, h, arena, TRUE);
}
