/* lisa_audio_in - utterance endpointing for the microphone stream.
 *
 * Takes 16 kHz mono float samples ([-1, 1]) and returns one utterance at a
 * time: from `preroll_ms` before the speech starts to `hangover_ms` after it
 * ends. The utterance is then trimmed more precisely by the engine's own
 * nd_whistle_find_speech() in the recognizer (that function is the existing
 * speech gate; this module only decides WHEN to hand a clip to it).
 *
 * The parameters below are engineering defaults, not calibrated values. They
 * must be checked on the real device with the real microphone. Pure C, no
 * RTOS, host-testable. */
#ifndef LISA_AUDIO_IN_H
#define LISA_AUDIO_IN_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define LISA_VAD_WINDOW 320   /* 20 ms at 16 kHz - same as nd_whistle_find_speech */

typedef struct {
    uint32_t preroll_ms;        /* audio kept before the speech start */
    uint32_t hangover_ms;       /* silence that ends an utterance */
    uint32_t min_speech_ms;     /* shorter bursts are discarded */
    float    start_db;          /* speech = this many dB above the noise floor */
    float    abs_min_rms;       /* ... and at least this RMS (full scale = 1) */
} lisa_vad_params_t;

typedef struct {
    lisa_vad_params_t p;

    float*  buf;                /* caller-owned, capacity_samples floats */
    size_t  capacity;
    size_t  len;

    float   win_acc[LISA_VAD_WINDOW];
    size_t  win_fill;

    bool    recording;
    bool    ready;
    uint32_t loud_run;          /* consecutive loud windows while idle */
    uint32_t quiet_run;         /* consecutive quiet windows while recording */
    uint32_t speech_windows;

    float   floor_rms;          /* running noise-floor estimate */
    bool    floor_init;

    size_t  keep_samples;       /* how much to keep while not recording */
} lisa_vad_t;

void lisa_vad_default_params(lisa_vad_params_t* p);

void lisa_vad_init(lisa_vad_t* v, const lisa_vad_params_t* p,
                   float* buffer, size_t capacity_samples);

/* Forget everything (also the noise floor? no - the floor is kept). */
void lisa_vad_reset(lisa_vad_t* v);

/* Feed samples. Returns true as soon as an utterance is complete; the rest
 * of that block is dropped, so feed blocks of one window (320 samples) or
 * smaller. The utterance is then v->buf[0 .. v->len) and v->floor_rms is
 * the noise-floor estimate to pass to nd_whistle_find_speech(). Call
 * lisa_vad_reset() after consuming it. */
bool lisa_vad_feed(lisa_vad_t* v, const float* x, size_t n);

#ifdef __cplusplus
}
#endif
#endif
