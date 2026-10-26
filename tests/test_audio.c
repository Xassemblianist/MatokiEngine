/* tests/test_audio.c — Synth ve ses halka tamponu testleri: determinizm, [-1,1] sınırı ve NaN yokluğu,
 * zarf süresi, frekans doğruluğu (sıfır geçişi sayımı), üstel kayma, desen ayrıştırıcı ve
 * halka tamponun (sarma dahil) doğrudan render ile örnek-örnek aynı akışı üretmesi.
 */
#include "test.h"
#include "core/math.h"
#include "audio/synth.h"
#include "audio/audio.h"

static f32 g_buf[44100 * 2];
static f32 g_buf2[44100 * 2];

static SynthSfx tone(SynthWave w, f32 hz, f32 sec) {
    SynthSfx s;
    __builtin_memset(&s, 0, sizeof(s));
    s.wave = w;
    s.freq = hz;
    s.attack = 0.0f;
    s.decay = 0.0f;
    s.sustain = 1.0f;
    s.hold = sec;
    s.release = 0.001f;
    s.volume = 0.5f;
    return s;
}

static u32 zero_crossings(const f32* x, u32 n) {
    u32 c = 0;
    for (u32 i = 1; i < n; i++) c += (x[i - 1] < 0.0f) != (x[i] < 0.0f);
    return c;
}

TEST(synth_frequency_and_envelope) {
    static Synth s;
    static const SynthWave waves[] = { WAVE_SQUARE, WAVE_TRIANGLE, WAVE_SAW, WAVE_SINE };
    for (u32 k = 0; k < ARRAY_LEN(waves); k++) {
        synth_init(&s, 44100);
        SynthSfx t = tone(waves[k], 441.0f, 1.0f);
        synth_play(&s, &t);
        synth_render(&s, g_buf, 44100);
        u32 zc = zero_crossings(g_buf, 44100);
        CHECK(zc >= 878 && zc <= 886);          /* 441 Hz → saniyede 882 geçiş */
        synth_render(&s, g_buf, 441);            /* hold + release bitti */
        CHECK(synth_active_voices(&s) == 0);
    }
    /* Attack: ilk 5 ms'nin tepe genliği, 50 ms sonrakinden belirgin küçük */
    synth_init(&s, 44100);
    SynthSfx a = tone(WAVE_SQUARE, 220.0f, 0.2f);
    a.attack = 0.05f;
    synth_play(&s, &a);
    synth_render(&s, g_buf, 8820);
    f32 early = 0.0f, late = 0.0f;
    for (u32 i = 0; i < 220; i++) early = m_max(early, m_abs(g_buf[i]));
    for (u32 i = 4410; i < 8820; i++) late = m_max(late, m_abs(g_buf[i]));
    CHECK(early < late * 0.2f);
}

TEST(synth_slide_and_bounds) {
    static Synth s;
    synth_init(&s, 44100);
    SynthSfx t = tone(WAVE_SINE, 200.0f, 1.0f);
    t.freq_end = 800.0f;
    synth_play(&s, &t);
    synth_render(&s, g_buf, 44100);
    u32 zc_first = zero_crossings(g_buf, 4410);
    u32 zc_last  = zero_crossings(g_buf + 39690, 4410);
    CHECK(zc_last > zc_first * 3);               /* 2 oktav yükseldi */

    /* Tüm sesler + gürültü + vibrato tam güçte: çıktı [-1,1], NaN yok */
    synth_init(&s, 48000);
    for (u32 i = 0; i < SYNTH_MAX_VOICES + 4; i++) {
        SynthSfx x = tone((SynthWave)(i % 5), 50.0f + 97.0f * (f32)i, 0.5f);
        x.volume = 1.0f;
        x.vibrato_hz = 6.0f;
        x.vibrato_depth = 0.5f;
        synth_play(&s, &x);
    }
    CHECK(synth_active_voices(&s) == SYNTH_MAX_VOICES);
    synth_render(&s, g_buf, 48000);
    u32 bad = 0;
    for (u32 i = 0; i < 48000; i++) bad += !(g_buf[i] >= -1.0f && g_buf[i] <= 1.0f);
    CHECK(bad == 0);
}

TEST(synth_pattern_and_determinism) {
    i8 n[16];
    u32 c = synth_parse_pattern("C4 E4 . - A#3 Bb2 | G9", n, 16);
    CHECK(c == 7);
    CHECK(n[0] == 60 && n[1] == 64 && n[2] == -2 && n[3] == -1);
    CHECK(n[4] == 58 && n[5] == 46 && n[6] == 127);

    static Synth s1, s2;
    SynthSfx inst = tone(WAVE_SQUARE, 0.0f, 0.08f);
    inst.duty = 0.25f;
    for (u32 k = 0; k < 2; k++) {
        Synth* s = k ? &s2 : &s1;
        synth_init(s, 44100);
        synth_song_add_track(s, "C4 E4 G4 C5 . G4 E4 -", &inst);
        synth_song_add_track(s, "C2 - G2 -", &inst);
        synth_song_start(s, 140.0f, 4);
    }
    synth_render(&s1, g_buf, 44100);
    synth_render(&s2, g_buf2, 44100);
    u32 diff = 0;
    f32 energy = 0.0f;
    for (u32 i = 0; i < 44100; i++) {
        diff += (g_buf[i] != g_buf2[i]);
        energy += g_buf[i] * g_buf[i];
    }
    CHECK(diff == 0);
    CHECK(energy > 100.0f);
}

/* Halka tampon: worklet benzeri tüketici ile okunan akış = doğrudan render (sarma dahil) */
TEST(audio_ring_stream_matches_direct) {
    AudioRing* ring = engine_audio_init(44100);
    Synth* s = audio_synth();
    SynthSfx t = tone(WAVE_SAW, 330.0f, 2.0f);
    synth_play(s, &t);

    static Synth ref;
    synth_init(&ref, 44100);
    synth_play(&ref, &t);

    u32 got = 0, mismatches = 0;
    for (u32 frame = 0; frame < 120; frame++) {
        audio_pump(0.07f);                                   /* ~3087 kare hedef */
        u32 fill = ring->write_pos - ring->read_pos;
        CHECK(fill <= ring->capacity);
        u32 take = 735;                                      /* 60 fps'de bir karelik tüketim */
        if (take > fill) take = fill;
        synth_render(&ref, g_buf, take);
        for (u32 i = 0; i < take; i++) {
            f32 v = ring->data[(ring->read_pos + i) & (ring->capacity - 1u)];
            mismatches += (v != g_buf[i]);
        }
        ring->read_pos += take;
        got += take;
    }
    CHECK(got > AUDIO_RING_FRAMES * 4u);   /* birkaç kez sarıldı */
    CHECK(mismatches == 0);
}
