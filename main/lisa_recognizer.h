/* lisa_recognizer - one utterance in, one classification out:
 *   float PCM -> nd_whistle_find_speech (existing speech gate)
 *             -> nd_whistle_encode      (Whistle encoder, 512-d per frame)
 *             -> dtw_classify_state_aware (existing DTW classifier)
 * No second classifier and no second audio path: it only calls the existing
 * engine and command_dtw APIs. */
#ifndef LISA_RECOGNIZER_H
#define LISA_RECOGNIZER_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "nd_whistle.h"
#include "nd_command_dtw.h"
#include "command_dictionary.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    LISA_REC_OK = 0,            /* classification ran; see the result */
    LISA_REC_NO_SPEECH,         /* the speech gate found no speech */
    LISA_REC_TOO_LONG,          /* more encoder frames than MAX_FRAMES_LIMIT */
    LISA_REC_ENCODE_FAILED,
    LISA_REC_CLASSIFY_FAILED,
    LISA_REC_NO_MEMORY,
    LISA_REC_BAD_ARG
} lisa_rec_status_t;

typedef struct {
    float min_speech_db;        /* passed to nd_whistle_find_speech */
    uint32_t margin_ms;         /* context kept around the speech */
} lisa_rec_params_t;

typedef struct {
    nd_whistle w;
    dtw_dictionary_t dict;
    float* enc;                 /* encoder output buffer */
    size_t enc_capacity_frames;
    lisa_rec_params_t params;
    bool open;
} lisa_recognizer_t;

typedef struct {
    uint32_t speech_samples;    /* length of the cropped clip */
    uint32_t frames;            /* encoder frames classified */
    uint32_t encode_ms;
    uint32_t classify_ms;
} lisa_rec_stats_t;

void lisa_rec_default_params(lisa_rec_params_t* p);

/* `model_blob` is the whistle .cact used in place (mmap'd flash on the
 * device); `max_samples` is the longest clip that will be passed in. The
 * dictionary is loaded from `wdt_path`. */
bool lisa_rec_init(lisa_recognizer_t* r, const void* model_blob, size_t model_size,
                   const char* wdt_path, size_t max_samples,
                   const lisa_rec_params_t* params);

void lisa_rec_free(lisa_recognizer_t* r);

lisa_rec_status_t lisa_rec_classify(lisa_recognizer_t* r,
                                    const float* samples, size_t n,
                                    float floor_rms, lisa_state_t state,
                                    classification_result_t* out,
                                    lisa_rec_stats_t* stats);

const char* lisa_rec_status_str(lisa_rec_status_t s);

#ifdef __cplusplus
}
#endif
#endif
