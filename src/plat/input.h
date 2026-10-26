/* plat/input.h — JS platform katmanı ile paylaşılan girdi durumu.
 * JS olay dinleyicileri doğrudan bu yapıya yazar; C motoru her karede okur.
 * Bellek yerleşimi JS tarafıyla birebir aynıdır (toplam 412 byte).
 */
#ifndef PLAT_INPUT_H
#define PLAT_INPUT_H

#include "core/types.h"

#define INPUT_MAX_CHARS 32

typedef struct {
    u8  keys[256];            /* KeyboardEvent.keyCode -> 0/1 */
    i32 mouse_x, mouse_y;     /* framebuffer koordinatında */
    i32 mouse_dx, mouse_dy;   /* frame başına birikmiş; C okuyunca kendisi sıfırlar */
    u32 mouse_buttons;        /* bit0 sol, bit1 sağ, bit2 orta */
    i32 wheel;
    u32 chars[INPUT_MAX_CHARS]; /* frame başına birikmiş UTF-32 kod noktaları */
    u32 char_count;           /* C okuyunca kendisi sıfırlar */
} InputState;

_Static_assert(sizeof(InputState) == 412, "InputState boyutu 412 byte olmali");

#endif
