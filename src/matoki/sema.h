/* matoki/sema.h — Matoki anlamsal çözümleyici (sema) arayüzü.
 * İsim çözümleme, kapsam yönetimi, katı tip denetimi (örtük dönüşüm yok),
 * swizzle doğrulama, yerleşik fonksiyon aşırı yükleme çözümlemesi,
 * uniform/varying tekdüzelik çıkarımı ve özyineleme/frag kontrollerini yapar.
 */
#ifndef MATOKI_SEMA_H
#define MATOKI_SEMA_H

#include "core/types.h"
#include "core/arena.h"
#include "matoki/ast.h"
#include "matoki/matoki.h"

b32 sema_check(AstProgram* prog, const char* src, u32 src_len, const char* filename,
               Arena* arena, MtkLayout* out_layout, MtkDiag* diag);

#endif
