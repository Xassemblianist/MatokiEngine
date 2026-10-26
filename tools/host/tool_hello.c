/* tools/host/tool_hello.c — genel araç kuralının duman testi; build/host/hello üretir.
 * Gerçek araçlar (matc, masm) eklenince silinebilir.
 */
#include <stdio.h>
#include "core/log.h"

int main(void) {
    char b[64];
    fmt_format(b, sizeof b, "matoki host tool ok (%d)", 42);
    puts(b);
    return 0;
}
