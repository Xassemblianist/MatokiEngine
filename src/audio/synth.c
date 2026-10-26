/* audio/synth.c — Osilatörler, zarf, ses tahsisi, karıştırma ve müzik sıralayıcısı.
 * Kare/testere dalgalarındaki süreksizlikler polyBLEP ile yumuşatılır (aliasing azalır).
 * Karışım master ile ölçeklenip rasyonel tanh yaklaşımıyla yumuşak kırpılır.
 */
#include "audio/synth.h"
#include "core/math.h"

void synth_init(Synth* s, u32 sample_rate) {
    __builtin_memset(s, 0, sizeof(*s));
    s->sample_rate = sample_rate ? sample_rate : 44100u;
    s->master      = 0.6f;
    s->song.volume = 1.0f;
}

f32 synth_midi_to_hz(i32 midi) {
    return 440.0f * m_exp2((f32)(midi - 69) / 12.0f);
}

i32 synth_play(Synth* s, const SynthSfx* x) {
    i32 slot = -1;
    u32 oldest = 0xFFFFFFFFu;
    for (u32 i = 0; i < SYNTH_MAX_VOICES; i++) {
        if (!s->voices[i].active) { slot = (i32)i; break; }
        if (s->voices[i].id < oldest) { oldest = s->voices[i].id; slot = (i32)i; }
    }
    SynthVoice* v = &s->voices[slot];
    f32 sr = (f32)s->sample_rate;
    v->active    = TRUE;
    v->wave      = x->wave;
    v->phase     = 0.0f;
    v->inc       = x->freq / sr;
    v->duty      = (x->duty > 0.0f && x->duty < 1.0f) ? x->duty : 0.5f;
    v->attack    = m_max(x->attack, 0.0f);
    v->decay     = m_max(x->decay, 0.0f);
    v->sustain   = m_clamp(x->sustain, 0.0f, 1.0f);
    v->hold      = m_max(x->hold, 0.0f);
    v->release   = m_max(x->release, 0.001f);
    v->t         = 0.0f;
    v->dt        = 1.0f / sr;
    v->volume    = m_clamp(x->volume, 0.0f, 1.0f);
    v->vib_phase = 0.0f;
    v->vib_inc   = x->vibrato_hz / sr;
    v->vib_depth = x->vibrato_depth;
    v->lfsr      = 0x7FFFu;
    v->noise_out = 1.0f;
    v->id        = s->next_id++;
    f32 dur = v->attack + v->decay + v->hold + v->release;
    v->inc_mul = 1.0f;
    if (x->freq_end > 0.0f && x->freq > 0.0f && dur > 0.0f) {
        v->inc_mul = m_exp2(m_log2(x->freq_end / x->freq) / (dur * sr));
    }
    return slot;
}

void synth_stop_all(Synth* s) {
    for (u32 i = 0; i < SYNTH_MAX_VOICES; i++) s->voices[i].active = FALSE;
}

u32 synth_active_voices(const Synth* s) {
    u32 n = 0;
    for (u32 i = 0; i < SYNTH_MAX_VOICES; i++) n += s->voices[i].active ? 1u : 0u;
    return n;
}

static inline f32 env_level(const SynthVoice* v, b32* done) {
    f32 t = v->t;
    if (t < v->attack) return t / v->attack;
    t -= v->attack;
    if (t < v->decay) return 1.0f - (1.0f - v->sustain) * (t / v->decay);
    t -= v->decay;
    if (t < v->hold) return v->sustain;
    t -= v->hold;
    if (t < v->release) return v->sustain * (1.0f - t / v->release);
    *done = TRUE;
    return 0.0f;
}

static inline f32 poly_blep(f32 t, f32 dt) {
    if (t < dt) {
        t /= dt;
        return t + t - t * t - 1.0f;
    }
    if (t > 1.0f - dt) {
        t = (t - 1.0f) / dt;
        return t * t + t + t + 1.0f;
    }
    return 0.0f;
}

static inline f32 voice_sample(SynthVoice* v) {
    f32 inc = v->inc;
    if (v->vib_depth != 0.0f) {
        inc *= m_exp2(v->vib_depth * (1.0f / 12.0f) * m_sin(v->vib_phase * M_TAU));
        v->vib_phase += v->vib_inc;
        if (v->vib_phase >= 1.0f) v->vib_phase -= 1.0f;
    }
    inc = m_min(inc, 0.5f);   /* Nyquist üstüne çıkma */
    f32 p = v->phase, o;
    switch (v->wave) {
    case WAVE_SQUARE: {
        o = (p < v->duty) ? 1.0f : -1.0f;
        o += poly_blep(p, inc);
        f32 p2 = p + 1.0f - v->duty;
        if (p2 >= 1.0f) p2 -= 1.0f;
        o -= poly_blep(p2, inc);
    } break;
    case WAVE_TRIANGLE:
        o = 1.0f - 4.0f * m_abs(p - 0.5f);
        break;
    case WAVE_SAW:
        o = 2.0f * p - 1.0f - poly_blep(p, inc);
        break;
    case WAVE_SINE:
        o = m_sin(p * M_TAU);
        break;
    case WAVE_NOISE:
    default:
        o = v->noise_out;
        break;
    }
    p += inc;
    if (p >= 1.0f) {
        p -= m_floor(p);
        if (v->wave == WAVE_NOISE) {
            u32 bit = (v->lfsr ^ (v->lfsr >> 1)) & 1u;
            v->lfsr = (v->lfsr >> 1) | (bit << 14);
            v->noise_out = (v->lfsr & 1u) ? 1.0f : -1.0f;
        }
    }
    v->phase = p;
    v->inc *= v->inc_mul;
    return o;
}

static void song_step(Synth* s) {
    SynthSong* g = &s->song;
    for (u32 k = 0; k < g->track_count; k++) {
        SynthTrack* tr = &g->tracks[k];
        if (tr->length == 0) continue;
        i32 n = tr->notes[g->step % tr->length];
        if (n < 0) continue;
        SynthSfx sfx = tr->instrument;
        f32 ratio = (sfx.freq_end > 0.0f && sfx.freq > 0.0f) ? sfx.freq_end / sfx.freq : 0.0f;
        sfx.freq     = synth_midi_to_hz(n);
        sfx.freq_end = ratio > 0.0f ? sfx.freq * ratio : 0.0f;
        sfx.volume  *= g->volume;
        synth_play(s, &sfx);
    }
    g->step++;
}

static inline f32 soft_clip(f32 x) {
    if (x > 3.0f) return 1.0f;
    if (x < -3.0f) return -1.0f;
    return m_clamp(x * (27.0f + x * x) / (27.0f + 9.0f * x * x), -1.0f, 1.0f);   /* yuvarlama taşmasına karşı */
}

void synth_render(Synth* s, f32* out, u32 frames) {
    f32 dt = 1.0f / (f32)s->sample_rate;
    for (u32 i = 0; i < frames; i++) {
        if (s->song.playing) {
            s->song.acc -= dt;
            if (s->song.acc <= 0.0f) {
                s->song.acc += s->song.step_seconds;
                song_step(s);
            }
        }
        f32 mix = 0.0f;
        for (u32 k = 0; k < SYNTH_MAX_VOICES; k++) {
            SynthVoice* v = &s->voices[k];
            if (!v->active) continue;
            b32 done = FALSE;
            f32 e = env_level(v, &done);
            if (done) { v->active = FALSE; continue; }
            mix += voice_sample(v) * e * v->volume;
            v->t += v->dt;
        }
        out[i] = soft_clip(mix * s->master);
    }
    s->frames_rendered += frames;
}

static i32 note_base(char c) {
    switch (c) {
    case 'C': return 0;  case 'D': return 2;  case 'E': return 4;  case 'F': return 5;
    case 'G': return 7;  case 'A': return 9;  case 'B': return 11;
    default:  return -1;
    }
}

u32 synth_parse_pattern(const char* text, i8* out, u32 cap) {
    u32 n = 0;
    const char* p = text;
    while (*p && n < cap) {
        while (*p == ' ' || *p == '\n' || *p == '\t' || *p == '|') p++;
        if (!*p) break;
        if (*p == '.') { out[n++] = -2; p++; continue; }
        if (*p == '-') { out[n++] = -1; p++; continue; }
        i32 b = note_base(*p);
        if (b < 0) { p++; continue; }   /* tanınmayan karakter: atla */
        p++;
        if (*p == '#') { b++; p++; }
        else if (*p == 'b') { b--; p++; }
        i32 oct = 4;
        if (*p >= '0' && *p <= '9') { oct = *p - '0'; p++; }
        i32 midi = (oct + 1) * 12 + b;
        out[n++] = (i8)CLAMP(midi, 0, 127);
    }
    return n;
}

void synth_song_start(Synth* s, f32 bpm, u32 steps_per_beat) {
    s->song.step_seconds = 60.0f / (bpm * (f32)(steps_per_beat ? steps_per_beat : 1u));
    s->song.acc          = 0.0f;
    s->song.step         = 0;
    s->song.playing      = TRUE;
}

void synth_song_stop(Synth* s) {
    s->song.playing = FALSE;
}

b32 synth_song_add_track(Synth* s, const char* pattern, const SynthSfx* instrument) {
    SynthSong* g = &s->song;
    if (g->track_count >= SYNTH_MAX_TRACKS) return FALSE;
    SynthTrack* tr = &g->tracks[g->track_count++];
    tr->length     = synth_parse_pattern(pattern, tr->notes, SYNTH_MAX_STEPS);
    tr->instrument = *instrument;
    return TRUE;
}
