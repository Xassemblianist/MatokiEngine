/* plat/input_ring.c — Girdi olay halkası gerçeklemesi.
 * JS olay dinleyicileri ve wasm motoru arasında SPSC halka tampon işlemleri.
 * Atomics/bariyerler thread/atomic.h üzerinden taşınır.
 */
#include "plat/input_ring.h"
#include "thread/atomic.h"

static InputRing g_input_ring;

void input_ring_init(InputRing* ring) {
    if (!ring) return;
    atomic_store_relaxed_u32(&ring->write_pos, 0u);
    atomic_store_relaxed_u32(&ring->read_pos,  0u);
    ring->capacity = INPUT_RING_CAP;
    atomic_store_relaxed_u32(&ring->dropped, 0u);
    __builtin_memset(ring->events, 0, sizeof(ring->events));
}

b32 input_ring_push(InputRing* ring, const InputEvent* ev) {
    if (!ring || !ev) return FALSE;
    u32 write_pos = atomic_load_relaxed_u32(&ring->write_pos);
    u32 read_pos  = atomic_load_acquire_u32(&ring->read_pos);
    if ((write_pos - read_pos) >= ring->capacity) {
        atomic_add_u32(&ring->dropped, 1u);
        return FALSE;
    }
    ring->events[write_pos & INPUT_RING_MASK] = *ev;
    atomic_store_release_u32(&ring->write_pos, write_pos + 1u);
    return TRUE;
}

b32 input_ring_pop(InputRing* ring, InputEvent* out_ev) {
    if (!ring || !out_ev) return FALSE;
    u32 read_pos  = atomic_load_relaxed_u32(&ring->read_pos);
    u32 write_pos = atomic_load_acquire_u32(&ring->write_pos);
    if (read_pos == write_pos) {
        return FALSE;
    }
    *out_ev = ring->events[read_pos & INPUT_RING_MASK];
    atomic_store_release_u32(&ring->read_pos, read_pos + 1u);
    return TRUE;
}

u32 input_ring_count(const InputRing* ring) {
    if (!ring) return 0;
    u32 write_pos = atomic_load_acquire_u32(&ring->write_pos);
    u32 read_pos  = atomic_load_acquire_u32(&ring->read_pos);
    return (write_pos >= read_pos) ? (write_pos - read_pos) : 0;
}

void input_ring_clear(InputRing* ring) {
    if (!ring) return;
    u32 write_pos = atomic_load_acquire_u32(&ring->write_pos);
    atomic_store_release_u32(&ring->read_pos, write_pos);
}

EXPORT(engine_input_ring) InputRing* engine_input_ring(void) {
    return &g_input_ring;
}
