/* core/log.h — mini printf, LOG/ASSERT/PANIC.
 * Desteklenen biçimler: %d %i %u %x %X %c %s %p %% , uzunluk: l ll z ,
 * %f / %.Nf (N <= 9), genişlik + '0' dolgu (ör. %08x, %5d, %-8s).
 */
#ifndef CORE_LOG_H
#define CORE_LOG_H

#include "core/types.h"

typedef __builtin_va_list VaList;
#define VA_START(ap, last) __builtin_va_start(ap, last)
#define VA_END(ap)         __builtin_va_end(ap)
#define VA_ARG(ap, T)      __builtin_va_arg(ap, T)

/* buf her zaman NUL ile biter; dönüş: yazılan karakter sayısı (NUL hariç, kırpılmış) */
u32  fmt_vformat(char* buf, u32 cap, const char* fmt, VaList ap);
u32  fmt_format(char* buf, u32 cap, const char* fmt, ...);

void log_printf(const char* fmt, ...);
_Noreturn void log_panic(const char* file, i32 line, const char* fmt, ...);

#define LOG(...)   log_printf(__VA_ARGS__)
#define PANIC(...) log_panic(__FILE__, __LINE__, __VA_ARGS__)
#define ASSERT(c)  do { if (!(c)) log_panic(__FILE__, __LINE__, "ASSERT: %s", #c); } while (0)

#endif
