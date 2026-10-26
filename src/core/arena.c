/* core/arena.c — Doğrusal bellek ayırıcı implementasyonu.
 * Hizalama ikinin kuvveti olmalıdır. Ayrılan bölge __builtin_memset ile sıfırlanır.
 * Kapasite yetersizliğinde PANIC ile program durdurulur.
 */
#include "core/arena.h"
#include "core/log.h"

void arena_init(Arena* a, void* base, usize cap) {
    a->base = (u8*)base;
    a->cap  = cap;
    a->used = 0;
}

void* arena_push(Arena* a, usize size, usize align) {
    ASSERT(IS_POW2(align));
    uptr base_addr = (uptr)a->base;
    uptr cur_addr  = base_addr + (uptr)a->used;
    uptr aligned   = ALIGN_UP(cur_addr, (uptr)align);
    usize pad      = (usize)(aligned - cur_addr);

    if (pad > a->cap - a->used || size > a->cap - a->used - pad) {
        PANIC("arena overflow: used=%zu, pad=%zu, req=%zu, cap=%zu",
              a->used, pad, size, a->cap);
    }

    usize offset = a->used + pad;
    a->used = offset + size;
    void* ptr = a->base + offset;
    __builtin_memset(ptr, 0, size);
    return ptr;
}

void arena_reset(Arena* a) {
    a->used = 0;
}

usize arena_mark(const Arena* a) {
    return a->used;
}

void arena_pop_to(Arena* a, usize mark) {
    ASSERT(mark <= a->used);
    a->used = mark;
}

Arena arena_sub(Arena* parent, usize cap) {
    void* base = arena_push(parent, cap, 16);
    Arena sub;
    arena_init(&sub, base, cap);
    return sub;
}
