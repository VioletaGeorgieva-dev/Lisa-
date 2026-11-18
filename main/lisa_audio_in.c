#include "lisa_audio_in.h"

#include <math.h>
#include <string.h>

#define LISA_VAD_SAMPLE_RATE 16000
#define MS_TO_WINDOWS(ms) (((ms) * (LISA_VAD_SAMPLE_RATE / 1000)) / LISA_VAD_WINDOW)
#define START_WINDOWS 5   /* 100 ms, the engine's minimum speech length */

void lisa_vad_default_params(lisa_vad_params_t* p)
{
    if (!p) {
        return;
    }

    p->preroll_ms = 300;
    p->hangover_ms = 600;
    p->min_speech_ms = 200;
    p->start_db = 10.0f;
    p->abs_min_rms = 0.003f;
}

void lisa_vad_init(lisa_vad_t* v, const lisa_vad_params_t* p,
                   float* buffer, size_t capacity_samples)
{
    if (!v) {
        return;
    }

    memset(v, 0, sizeof(*v));

    if (p) {
        v->p = *p;
    } else {
        lisa_vad_default_params(&v->p);
    }

    v->buf = buffer;
    v->capacity = capacity_samples;

    v->keep_samples = (size_t)(MS_TO_WINDOWS(v->p.preroll_ms) + START_WINDOWS)
                      * LISA_VAD_WINDOW;

    /* The pre-roll plus the start window must leave room to record. */
    if (v->keep_samples + LISA_VAD_WINDOW > v->capacity) {
        v->keep_samples = v->capacity > 2 * LISA_VAD_WINDOW
                              ? v->capacity - 2 * LISA_VAD_WINDOW
                              : 0;
    }
}

void lisa_vad_reset(lisa_vad_t* v)
{
    if (!v) {
        return;
    }

    v->len = 0;
    v->win_fill = 0;
    v->recording = false;
    v->ready = false;
    v->loud_run = 0;
    v->quiet_run = 0;
    v->speech_windows = 0;
}

static float window_rms(const float* w)
{
    float e = 0.0f;

    for (int i = 0; i < LISA_VAD_WINDOW; i++) {
        e += w[i] * w[i];
    }

    return sqrtf(e / (float)LISA_VAD_WINDOW);
}

static float threshold(const lisa_vad_t* v)
{
    float t = v->floor_rms * powf(10.0f, v->p.start_db / 20.0f);

    return t > v->p.abs_min_rms ? t : v->p.abs_min_rms;
}

/* Returns true when the utterance is complete. */
static bool process_window(lisa_vad_t* v, const float* w)
{
    float rms = window_rms(w);

    if (!v->floor_init) {
        v->floor_rms = rms > 1e-6f ? rms : 1e-6f;
        v->floor_init = true;
    }

    bool loud = rms > threshold(v);

    /* Append the window. */
    if (v->len + LISA_VAD_WINDOW > v->capacity) {

        if (v->recording) {
            /* Utterance longer than the buffer: hand over what we have
             * (the caller still checks the minimum speech length). */
            return true;
        }

        /* Not recording: drop the oldest audio (pre-roll). */
        size_t drop = v->len + LISA_VAD_WINDOW - v->capacity;
        memmove(v->buf, v->buf + drop, (v->len - drop) * sizeof(float));
        v->len -= drop;
    }

    memcpy(v->buf + v->len, w, LISA_VAD_WINDOW * sizeof(float));
    v->len += LISA_VAD_WINDOW;

    if (!v->recording) {

        if (!loud) {
            /* Noise floor follows the quiet signal slowly. */
            v->floor_rms = 0.95f * v->floor_rms + 0.05f * rms;
            if (v->floor_rms < 1e-6f) {
                v->floor_rms = 1e-6f;
            }
            v->loud_run = 0;
        } else {
            v->loud_run++;
        }

        /* Keep only the pre-roll plus the windows that may start speech. */
        if (v->len > v->keep_samples) {
            size_t drop = v->len - v->keep_samples;
            memmove(v->buf, v->buf + drop, (v->len - drop) * sizeof(float));
            v->len -= drop;
        }

        if (v->loud_run >= START_WINDOWS) {
            v->recording = true;
            v->speech_windows = v->loud_run;
            v->quiet_run = 0;
        }

        return false;
    }

    /* Recording. */
    if (loud) {
        v->quiet_run = 0;
        v->speech_windows++;
    } else {
        v->quiet_run++;
    }

    if (v->quiet_run >= MS_TO_WINDOWS(v->p.hangover_ms)) {
        return true;
    }

    return false;
}

bool lisa_vad_feed(lisa_vad_t* v, const float* x, size_t n)
{
    if (!v || !x || !v->buf) {
        return false;
    }

    if (v->ready) {
        return true;
    }

    for (size_t i = 0; i < n; i++) {

        v->win_acc[v->win_fill++] = x[i];

        if (v->win_fill < LISA_VAD_WINDOW) {
            continue;
        }

        v->win_fill = 0;

        if (process_window(v, v->win_acc)) {

            bool long_enough =
                v->speech_windows >= MS_TO_WINDOWS(v->p.min_speech_ms);

            if (long_enough) {
                v->ready = true;
                return true;
            }

            /* Too short (a click, a cough): forget it, keep the floor. */
            lisa_vad_reset(v);
            return false;
        }
    }

    return false;
}
