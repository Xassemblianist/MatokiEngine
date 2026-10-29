/* tests/test_input_ring.c — Girdi olay halkası birim testleri.
 * SPSC halka tampon başlatma, olay sırası, kapasite sınırı, taşma ve sarma testleri.
 */
#include "test.h"
#include "plat/input_ring.h"

TEST(input_ring_basic) {
    InputRing ring;
    input_ring_init(&ring);

    CHECK(ring.capacity == INPUT_RING_CAP);
    CHECK(ring.write_pos == 0);
    CHECK(ring.read_pos == 0);
    CHECK(ring.dropped == 0);
    CHECK(input_ring_count(&ring) == 0);

    InputEvent out;
    CHECK(!input_ring_pop(&ring, &out));

    /* Tek bir klavye olayı ekle */
    InputEvent ev1 = {
        .type      = INPUT_EVENT_KEY_DOWN,
        .frame     = 10,
        .time_ms   = 123.456,
        .mouse_x   = 100,
        .mouse_y   = 200,
        .mouse_dx  = 5,
        .mouse_dy  = -2,
        .key       = 65, /* 'A' */
        .buttons   = 1,
        .wheel     = 0,
        .char_code = 'a',
        .modifiers = 2, /* ctrl */
    };
    CHECK(input_ring_push(&ring, &ev1));
    CHECK(input_ring_count(&ring) == 1);
    CHECK(ring.write_pos == 1);
    CHECK(ring.read_pos == 0);

    /* Olayı oku ve doğrula */
    CHECK(input_ring_pop(&ring, &out));
    CHECK(out.type == INPUT_EVENT_KEY_DOWN);
    CHECK(out.frame == 10);
    CHECK_NEAR(out.time_ms, 123.456, 1e-6);
    CHECK(out.mouse_x == 100);
    CHECK(out.mouse_y == 200);
    CHECK(out.mouse_dx == 5);
    CHECK(out.mouse_dy == -2);
    CHECK(out.key == 65);
    CHECK(out.buttons == 1);
    CHECK(out.wheel == 0);
    CHECK(out.char_code == 'a');
    CHECK(out.modifiers == 2);

    CHECK(input_ring_count(&ring) == 0);
    CHECK(!input_ring_pop(&ring, &out));
}

TEST(input_ring_capacity_and_drop) {
    InputRing ring;
    input_ring_init(&ring);

    /* Tam kapasite (256 olay) doldur */
    for (u32 i = 0; i < INPUT_RING_CAP; i++) {
        InputEvent ev = {
            .type  = INPUT_EVENT_MOUSE_MOVE,
            .frame = i,
            .key   = i,
        };
        CHECK(input_ring_push(&ring, &ev));
    }

    CHECK(input_ring_count(&ring) == INPUT_RING_CAP);
    CHECK(ring.dropped == 0);

    /* 257. olay kapasite aşımı nedeniyle düşmeli */
    InputEvent overflow_ev = { .type = INPUT_EVENT_KEY_UP, .frame = 999 };
    CHECK(!input_ring_push(&ring, &overflow_ev));
    CHECK(ring.dropped == 1);
    CHECK(input_ring_count(&ring) == INPUT_RING_CAP);

    /* Bir olay tüket */
    InputEvent out;
    CHECK(input_ring_pop(&ring, &out));
    CHECK(out.frame == 0);
    CHECK(input_ring_count(&ring) == INPUT_RING_CAP - 1);

    /* Artık bir boşluk var, yeni olay eklenebilmeli */
    CHECK(input_ring_push(&ring, &overflow_ev));
    CHECK(input_ring_count(&ring) == INPUT_RING_CAP);
}

TEST(input_ring_wraparound) {
    InputRing ring;
    input_ring_init(&ring);

    /* Kapasitenin katları kadar ekle ve çıkar */
    const u32 total_events = 2000;
    u32 push_idx = 0;
    u32 pop_idx = 0;

    while (pop_idx < total_events) {
        /* Halka kapasitesi elverdiğince ekle (1..16 arası) */
        u32 to_push = ((push_idx * 7u + 3u) % 16u) + 1u;
        for (u32 i = 0; i < to_push && push_idx < total_events; i++) {
            if (input_ring_count(&ring) >= ring.capacity) break;
            InputEvent ev = {
                .type  = INPUT_EVENT_KEY_DOWN,
                .frame = push_idx,
                .key   = push_idx & 0xff,
            };
            CHECK(input_ring_push(&ring, &ev));
            push_idx++;
        }

        /* Birkaç tane çıkar */
        u32 to_pop = ((pop_idx * 5u + 1u) % 16u) + 1u;
        for (u32 i = 0; i < to_pop && pop_idx < push_idx; i++) {
            InputEvent out;
            CHECK(input_ring_pop(&ring, &out));
            CHECK(out.frame == pop_idx);
            CHECK(out.key == (pop_idx & 0xff));
            pop_idx++;
        }
    }

    CHECK(push_idx == total_events);
    CHECK(pop_idx == total_events);
    CHECK(input_ring_count(&ring) == 0);
    CHECK(ring.dropped == 0);
}

TEST(input_ring_clear) {
    InputRing ring;
    input_ring_init(&ring);

    for (u32 i = 0; i < 50; i++) {
        InputEvent ev = { .type = INPUT_EVENT_CHAR, .frame = i };
        input_ring_push(&ring, &ev);
    }
    CHECK(input_ring_count(&ring) == 50);

    input_ring_clear(&ring);
    CHECK(input_ring_count(&ring) == 0);

    InputEvent out;
    CHECK(!input_ring_pop(&ring, &out));
}
