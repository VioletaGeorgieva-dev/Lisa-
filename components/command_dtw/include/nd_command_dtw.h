
#ifndef ND_COMMAND_DTW_H
#define ND_COMMAND_DTW_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#include "command_state.h"

#ifdef __cplusplus
extern "C" {
#endif

#define EMBEDDING_DIM 512
#define TOP_K_DEFAULT 3
#define DTW_BAND_RATIO_DEFAULT 0.25f

/* Максимален брой embedding кадри в една команда. */
#define MAX_FRAMES_LIMIT 256

typedef struct {
    float* frames;       /* T x 512 L2-нормализирани кадъра */
    int num_frames;
} dtw_template_t;

typedef struct {
    char name[32];
    int num_templates;
    dtw_template_t* templates;

    float threshold_distance;
    float threshold_margin;

    /* true само за калибрираните оперативни команди */
    bool has_threshold;
} dtw_class_t;

typedef struct {
    int num_classes;
    dtw_class_t* classes;
} dtw_dictionary_t;

typedef struct {
    /* Оперативни резултати — само калибрирани класове */
    char prediction_7[32];
    char best_command_7[32];
    char second_command_7[32];

    float best_distance_7;
    float second_distance_7;
    float margin_7;

    bool accepted_7;
    char reason_7[32];

    /* Диагностични резултати — всички класове */
    char diag_best_9[32];
    char diag_second_9[32];

    float diag_best_dist_9;
    float diag_second_dist_9;
    float diag_margin_9;
} classification_result_t;

void dtw_normalize_frame(float* frame, int dim);

float dtw_cosine_distance(
    const float* a,
    const float* b,
    int dim
);

float dtw_calculate_distance(
    const float* seq1,
    int n1,
    const float* seq2,
    int n2
);

/* Класификация според текущото състояние на Лиса. */
bool dtw_classify_state_aware(
    const float* input_frames,
    int num_frames,
    const dtw_dictionary_t* dict,
    lisa_state_t current_state,
    classification_result_t* out_res
);

/* Съвместим интерфейс — класификация в IDLE състояние. */
bool dtw_classify(
    const float* input_frames,
    int num_frames,
    const dtw_dictionary_t* dict,
    classification_result_t* out_res
);

#ifdef __cplusplus
}
#endif

#endif
