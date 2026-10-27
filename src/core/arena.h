/* core/arena.h — Doğrusal bellek ayırıcı (bump allocator).
 * Motor içinde dinamik bellek yalnızca Arena üzerinden ayrılır (malloc yok).
 * Tüm ayırmalar sıfırlanmış döner; kapasite aşımında PANIC çağrılır.
 */
#ifndef CORE_ARENA_H
#define CORE_ARENA_H

#include "core/types.h"

typedef struct {
    u8*   base;
    usize cap;
    usize used;
} Arena;

void  arena_init(Arena* a, void* base, usize cap);
void* arena_push(Arena* a, usize size, usize align);
/* Sıfırlamasız ayırma: içerik tanımsız; çağıran tamamını yazacaksa (büyük geçici diziler) */
void* arena_push_raw(Arena* a, usize size, usize align);
void  arena_reset(Arena* a);
usize arena_mark(const Arena* a);
void  arena_pop_to(Arena* a, usize mark);
Arena arena_sub(Arena* parent, usize cap);

#define ARENA_PUSH_STRUCT(a, T)    ((T*)arena_push((a), sizeof(T), _Alignof(T)))
#define ARENA_PUSH_ARRAY(a, T, n)  ((T*)arena_push((a), sizeof(T) * (usize)(n), _Alignof(T)))
#define ARENA_PUSH_ARRAY_RAW(a, T, n) ((T*)arena_push_raw((a), sizeof(T) * (usize)(n), _Alignof(T)))

#endif
