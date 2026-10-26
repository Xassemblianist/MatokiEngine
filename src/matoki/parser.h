/* matoki/parser.h — Matoki sözdizimi çözümleyicisi (parser) arayüzü.
 * Elle yazılmış özyinelemeli iniş (bildirim/deyim) ve Pratt öncelik tırmanma (ifade) kullanır.
 * Hata durumunda ';' veya '}' sınırına kadar senkronize olarak en fazla 20 hatayı toplar.
 */
#ifndef MATOKI_PARSER_H
#define MATOKI_PARSER_H

#include "core/types.h"
#include "core/arena.h"
#include "matoki/ast.h"
#include "matoki/matoki.h"

b32 parser_parse(const char* src, u32 len, const char* filename,
                 Arena* arena, AstProgram* out_prog, MtkDiag* diag);

#endif
