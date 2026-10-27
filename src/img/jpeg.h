/* img/jpeg.h — Sıfırdan Baseline (SOF0/SOF1) ve Progressive (SOF2) JPEG çözücü.
 * 8-bit gri (1 bileşen) ve YCbCr (3 bileşen; 4:4:4, 4:2:2, 4:2:0 ve genel h/v örnekleme),
 * hızlı 9-bit önbellekli Huffman, DRI/RSTn yeniden başlatma, EXIF yön okuma, sabit noktalı
 * LLM tamsayı IDCT ve çift-doğrusal (fancy) kroma yükseltme içerir. Dış kütüphane/libc yoktur.
 */
#ifndef IMG_JPEG_H
#define IMG_JPEG_H

#include "core/types.h"
#include "core/arena.h"

/* JPEG ikili akışını çözer ve RGBA8 (u32, FB_RGBA düzeni) piksel dizisini Arena'dan ayırıp döner.
 * Varsayılan olarak çift-doğrusal (fancy) kroma yükseltme kullanır.
 * Başarıda *w ve *h yazılır; hatada NULL döner ve hata açıklaması jpeg_last_error() ile alınır. */
u32* jpeg_decode(const u8* data, usize len, u32* w, u32* h, Arena* arena);

/* Kroma yükseltme seçeneği ile JPEG çözme (fancy_upsample != 0 ise çift-doğrusal, 0 ise en yakın komşu). */
u32* jpeg_decode_ex(const u8* data, usize len, u32* w, u32* h, Arena* arena, b32 fancy_upsample);

/* Son çözülen JPEG'deki EXIF Orientation etiketini (1..8, yoksa 1) döner; pikseller döndürülmez. */
u32  jpeg_orientation(void);

/* Son jpeg_decode çağrısının hata mesajını döner (başarılıysa boş dize ""). */
const char* jpeg_last_error(void);

/* İç çekirdekler: 8x8 sabit nokta LLM IDCT ve YCbCr -> RGBA8 satır dönüşümü (skaler ve wasm SIMD). */
void jpeg_idct_block(const i16* block_zz, const u16* quant_zz, u8* out, u32 stride);
void jpeg_idct_block_scalar(const i16* block_zz, const u16* quant_zz, u8* out, u32 stride);
void jpeg_ycbcr_to_rgba_row(const u8* y_row, const u8* cb_row, const u8* cr_row, u32* dst, u32 w);
void jpeg_ycbcr_to_rgba_row_scalar(const u8* y_row, const u8* cb_row, const u8* cr_row, u32* dst, u32 w);

#endif
