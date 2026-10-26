/* matoki/lower.h — Matoki AST'den skaler SSA IR'a (IrFunc) indirme arayüzü.
 * Vektör ve matrisleri skaler bileşenlere (SoA) ayırır, swizzle işlemlerini
 * sıfır maliyetli indeks yeniden eşlemesiyle çözer, yardımcı fonksiyonları inline eder
 * ve if/else dallarında değişen değişkenleri IR_SELECT ile birleştirir.
 */
#ifndef MATOKI_LOWER_H
#define MATOKI_LOWER_H

#include "core/types.h"
#include "core/arena.h"
#include "matoki/ast.h"
#include "matoki/ir.h"
#include "matoki/matoki.h"

b32 lower_ast(const AstProgram* prog, const MtkLayout* layout, Arena* arena, IrFunc* out_ir);

#endif
