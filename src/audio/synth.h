/* audio/synth.h — Yazılım ses sentezleyici (chiptune tarzı): kare/üçgen/testere/sinüs/gürültü
 * osilatörleri, ADSR zarfı, üstel frekans kayması, vibrato, polyBLEP ile yumuşatılmış kare/testere,
 * 15-bit LFSR gürültü ve adım tabanlı müzik sıralayıcısı. Tamamen deterministik, libc'siz, mono f32.
 */
#ifndef AUDIO_SYNTH_H
#define AUDIO_SYNTH_H

#include "core/types.h"

#define SYNTH_MAX_VOICES  24
#define SYNTH_MAX_TRACKS  4
#define SYNTH_MAX_STEPS   128

typedef enum {
    WAVE_SQUARE   = 0,
    WAVE_TRIANGLE = 1,
    WAVE_SAW      = 2,
    WAVE_SINE     = 3,
    WAVE_NOISE    = 4
} SynthWave;

/* Bir ses efekti / nota tanımı. Süre = attack + decay + hold + release (saniye). */
typedef struct {
    SynthWave wave;
    f32 freq;           /* Hz, başlangıç */
    f32 freq_end;       /* Hz, bitiş (üstel kayma); <= 0 ise sabit */
    f32 duty;           /* kare dalga doluluk oranı (0..1), 0 → 0.5 */
    f32 attack, decay, sustain, hold, release;
    f32 volume;         /* 0..1 */
    f32 vibrato_hz, vibrato_depth;   /* depth: yarım ton cinsinden */
} SynthSfx;

typedef struct {
    b32 active;
    SynthWave wave;
    f32 phase;          /* 0..1 */
    f32 inc;            /* faz artışı / örnek */
    f32 inc_mul;        /* örnek başına üstel kayma çarpanı */
    f32 duty;
    f32 attack, decay, sustain, hold, release;
    f32 t;              /* saniye */
    f32 dt;             /* 1 / sample_rate */
    f32 volume;
    f32 vib_phase, vib_inc, vib_depth;
    u32 lfsr;
    f32 noise_out;
    u32 id;             /* çalma sırası (en eski ses çalınır) */
} SynthVoice;

/* Müzik: her parça bir nota dizisi (MIDI no, -1 = sus/uzat yok, -2 = tut) */
typedef struct {
    i8  notes[SYNTH_MAX_STEPS];
    u32 length;
    SynthSfx instrument;   /* freq alanı her notada nota frekansıyla değiştirilir */
} SynthTrack;

typedef struct {
    SynthTrack tracks[SYNTH_MAX_TRACKS];
    u32 track_count;
    f32 step_seconds;
    f32 acc;            /* bir sonraki adıma kalan süre */
    u32 step;
    b32 playing;
    f32 volume;
} SynthSong;

typedef struct {
    u32        sample_rate;
    SynthVoice voices[SYNTH_MAX_VOICES];
    u32        next_id;
    f32        master;
    SynthSong  song;
    u64        frames_rendered;
} Synth;

void synth_init(Synth* s, u32 sample_rate);
i32  synth_play(Synth* s, const SynthSfx* sfx);           /* ses indeksi; doluysa en eskiyi çalar */
void synth_stop_all(Synth* s);
void synth_render(Synth* s, f32* out, u32 frames);        /* mono, [-1, 1] (yumuşak kırpma) */
u32  synth_active_voices(const Synth* s);

/* "C4 E4 G4 . - A#3" biçiminde dizi: nota adı + oktav, '.' = önceki notayı tut, '-' = sus.
 * Dönüş: okunan adım sayısı. */
u32  synth_parse_pattern(const char* text, i8* out, u32 cap);
f32  synth_midi_to_hz(i32 midi);
void synth_song_start(Synth* s, f32 bpm, u32 steps_per_beat);
void synth_song_stop(Synth* s);
/* parça ekle (pattern synth_parse_pattern ile çözülür) */
b32  synth_song_add_track(Synth* s, const char* pattern, const SynthSfx* instrument);

#endif
