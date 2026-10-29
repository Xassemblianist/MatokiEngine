/* tests/test_phys2_common.h — phys2 testlerinin ortak yardımcıları (test_phys2_common.c).
 * test_arena(): her testin başında sıfırlanan 256 MB arena; test_note(): ölçüm/not satırı (PHYS2_TEST_VERBOSE=1
 * ile yazdırılır); p2t_sb_*: libc'siz kodun metin oluşturucusuna benzeyen küçük tampon (not satırları için).
 * Testler SamurayBalgami/Matoki'den uyarlandı (izinle).
 */
#ifndef TESTS_TEST_PHYS2_COMMON_H
#define TESTS_TEST_PHYS2_COMMON_H

#include "core/types.h"
#include "core/arena.h"

Arena* test_arena(void);
void   test_arena_reset(void);
void   test_note(const char* label, f64 value, u32 decimals);

typedef struct P2tBuf {
    char* p;
    u32   len;
    u32   cap;
} P2tBuf;

void p2t_sb_init(P2tBuf* s, char* mem, u32 cap);
void p2t_sb_char(P2tBuf* s, char c);
void p2t_sb_cstr(P2tBuf* s, const char* str);
void p2t_sb_u64(P2tBuf* s, u64 v);
void p2t_sb_i64(P2tBuf* s, i64 v);
void p2t_sb_f64(P2tBuf* s, f64 v, u32 decimals);
void p2t_sb_hex32(P2tBuf* s, u32 v);
void p2t_log(const P2tBuf* s);

#endif
