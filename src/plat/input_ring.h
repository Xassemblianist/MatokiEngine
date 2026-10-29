/* plat/input_ring.h — JS platform katmanından gelen kare damgalı SPSC girdi olay halkası.
 * Fare ve klavye olaylarını kayıpsız iletmek, deterministik simülasyon ve rollback
 * girdi geçmişi oluşturmak için tek üreticili / tek tüketicili halka tampon sağlar.
 * JS (üretici) yazar, C motoru (tüketici) her karede tüketir.
 */
#ifndef PLAT_INPUT_RING_H
#define PLAT_INPUT_RING_H

#include "core/types.h"
#include "plat/platform.h"

/* Olay türleri */
typedef enum {
    INPUT_EVENT_NONE        = 0,
    INPUT_EVENT_KEY_DOWN    = 1,
    INPUT_EVENT_KEY_UP      = 2,
    INPUT_EVENT_MOUSE_MOVE  = 3,
    INPUT_EVENT_MOUSE_DOWN  = 4,
    INPUT_EVENT_MOUSE_UP    = 5,
    INPUT_EVENT_MOUSE_WHEEL = 6,
    INPUT_EVENT_CHAR        = 7,
} InputEventType;

/* Tekil girdi olayı (tam 64 bayt, 8 bayt hizalı) */
typedef struct {
    u32 type;        /* InputEventType */
    u32 frame;       /* rAF / olay kare numarası */
    f64 time_ms;     /* Zaman damgası (performance.now()) */
    i32 mouse_x;     /* Framebuffer piksel koordinatı */
    i32 mouse_y;
    i32 mouse_dx;    /* Fare delta x (veya kilitli movementX) */
    i32 mouse_dy;    /* Fare delta y (veya kilitli movementY) */
    u32 key;         /* KeyboardEvent.keyCode (0..255) */
    u32 buttons;     /* Fare tuş maskesi: bit0 sol, bit1 sağ, bit2 orta */
    i32 wheel;       /* Tekerlek delta */
    u32 char_code;   /* UTF-32 kod noktası */
    u32 modifiers;   /* bit0: shift, bit1: ctrl, bit2: alt, bit3: meta */
    u32 _pad[3];     /* 64 bayt tamamlayıcı */
} InputEvent;

_Static_assert(sizeof(InputEvent) == 64, "InputEvent boyutu 64 bayt olmali");

/* Halka tampon kapasitesi (2'nin kuvveti olmalı) */
#define INPUT_RING_CAP  256u
#define INPUT_RING_MASK (INPUT_RING_CAP - 1u)

/* JS ile paylaşılan SPSC halka tampon yapısı */
typedef struct {
    _Alignas(4) u32 write_pos; /* Yalnızca JS (üretici) yazar / artırır */
    _Alignas(4) u32 read_pos;  /* Yalnızca C (tüketici) yazar / artırır */
    u32 capacity;              /* INPUT_RING_CAP (256) */
    u32 dropped;               /* Halka doluyken düşürülen olay sayısı */
    InputEvent events[INPUT_RING_CAP];
} InputRing;

#define INPUT_RING_OFF_WRITE   0
#define INPUT_RING_OFF_READ    4
#define INPUT_RING_OFF_CAP     8
#define INPUT_RING_OFF_DROPPED 12
#define INPUT_RING_OFF_EVENTS  16

_Static_assert(__builtin_offsetof(InputRing, write_pos) == INPUT_RING_OFF_WRITE, "input ring abi write");
_Static_assert(__builtin_offsetof(InputRing, read_pos)  == INPUT_RING_OFF_READ, "input ring abi read");
_Static_assert(__builtin_offsetof(InputRing, capacity)  == INPUT_RING_OFF_CAP, "input ring abi cap");
_Static_assert(__builtin_offsetof(InputRing, dropped)   == INPUT_RING_OFF_DROPPED, "input ring abi dropped");
_Static_assert(__builtin_offsetof(InputRing, events)    == INPUT_RING_OFF_EVENTS, "input ring abi events");

/* C arayüzü */
void input_ring_init(InputRing* ring);
b32  input_ring_push(InputRing* ring, const InputEvent* ev);
b32  input_ring_pop(InputRing* ring, InputEvent* out_ev);
u32  input_ring_count(const InputRing* ring);
void input_ring_clear(InputRing* ring);

/* Global motor girdi halkası export'u */
EXPORT(engine_input_ring) InputRing* engine_input_ring(void);

#endif /* PLAT_INPUT_RING_H */
