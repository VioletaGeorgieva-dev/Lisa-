
#include "nd_command_dtw.h"

#include <math.h>
#include <string.h>
#include <stdlib.h>
#include <float.h>
#include <stdio.h>

#ifdef ESP_PLATFORM
#include <esp_heap_caps.h>

#define DTW_MALLOC(sz) dtw_malloc(sz)
#define DTW_FREE(ptr) heap_caps_free(ptr)

static void* dtw_malloc(size_t size)
{
    void* ptr = heap_caps_malloc(
        size,
        MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT
    );

    if (!ptr) {
        ptr = heap_caps_malloc(size, MALLOC_CAP_8BIT);
    }

    return ptr;
}
#else
#define DTW_MALLOC(sz) malloc(sz)
#define DTW_FREE(ptr) free(ptr)
#endif

typedef struct {
    int class_idx;
    float score;
} class_score_entry_t;


/* ---------------------------------------------------------
   Помощни функции
   --------------------------------------------------------- */

static void copy_name(char* dst, size_t dst_size, const char* src)
{
    if (!dst || dst_size == 0) {
        return;
    }

    if (!src) {
        dst[0] = '\0';
        return;
    }

    snprintf(dst, dst_size, "%s", src);
}


static int py_round_half_to_even(double val)
{
    double f = floor(val);
    double diff = val - f;

    if (diff < 0.5) {
        return (int)f;
    }

    if (diff > 0.5) {
        return (int)(f + 1.0);
    }

    long long fi = (long long)f;

    return (fi % 2 == 0)
        ? (int)fi
        : (int)(fi + 1);
}


static int is_base_cmd(const char* cmd_name, const char* base)
{
    if (!cmd_name || !base) {
        return 0;
    }

    if (strcmp(cmd_name, base) == 0) {
        return 1;
    }

    size_t len = strlen(base);

    return (
        strncmp(cmd_name, base, len) == 0 &&
        strcmp(cmd_name + len, "_kids") == 0
    );
}


static int is_command_allowed_in_state(
    const char* cmd_name,
    lisa_state_t state
)
{
    if (!cmd_name) {
        return 0;
    }

    switch (state) {

        case LISA_STATE_IDLE:
        case LISA_STATE_SLEEP:
            return is_base_cmd(cmd_name, "hey_lisa");

        case LISA_STATE_QUESTION:
            /*
     * da/ne се обработват отделно в QUESTION блока.
     * Тук стандартният класификатор разрешава само zaspi.
     */
            return is_base_cmd(cmd_name, "zaspi");

        case LISA_STATE_PLAYING:
            return (
                is_base_cmd(cmd_name, "pauziraj") ||
                is_base_cmd(cmd_name, "spri") ||
                is_base_cmd(cmd_name, "predishna") ||
                is_base_cmd(cmd_name, "sledvashta") ||
                is_base_cmd(cmd_name, "zaspi")
            );

        case LISA_STATE_PAUSED:
            return (
                is_base_cmd(cmd_name, "napred") ||
                is_base_cmd(cmd_name, "spri") ||
                is_base_cmd(cmd_name, "zaspi")
            );

        default:
            return 0;
    }
}


static int compare_scores(
    const void* a,
    const void* b
)
{
    const class_score_entry_t* ca =
        (const class_score_entry_t*)a;

    const class_score_entry_t* cb =
        (const class_score_entry_t*)b;

    if (ca->score < cb->score) return -1;
    if (ca->score > cb->score) return 1;

    return 0;
}


static void init_reject_result(
    classification_result_t* out,
    const char* reason
)
{
    if (!out) {
        return;
    }

    memset(out, 0, sizeof(*out));

    copy_name(out->prediction_7, sizeof(out->prediction_7), "REJECT");
    copy_name(out->best_command_7, sizeof(out->best_command_7), "NONE");
    copy_name(out->second_command_7, sizeof(out->second_command_7), "NONE");

    copy_name(out->diag_best_9, sizeof(out->diag_best_9), "NONE");
    copy_name(out->diag_second_9, sizeof(out->diag_second_9), "NONE");

    out->best_distance_7 = FLT_MAX;
    out->second_distance_7 = FLT_MAX;

    out->diag_best_dist_9 = FLT_MAX;
    out->diag_second_dist_9 = FLT_MAX;

    out->margin_7 = 0.0f;
    out->diag_margin_9 = 0.0f;

    out->accepted_7 = false;

    copy_name(out->reason_7, sizeof(out->reason_7), reason);
}


/* ---------------------------------------------------------
   Нормализация и cosine distance
   --------------------------------------------------------- */

void dtw_normalize_frame(float* frame, int dim)
{
    if (!frame || dim <= 0) {
        return;
    }

    double sum_sq = 0.0;

    for (int i = 0; i < dim; i++) {
        double v = (double)frame[i];

        if (!isfinite(v)) {
            v = 0.0;
            frame[i] = 0.0f;
        }

        sum_sq += v * v;
    }

    double norm = sqrt(sum_sq);

    if (norm > 0.0 && isfinite(norm)) {
        for (int i = 0; i < dim; i++) {
            frame[i] = (float)((double)frame[i] / norm);
        }
    } else {
        for (int i = 0; i < dim; i++) {
            frame[i] = 0.0f;
        }
    }
}


float dtw_cosine_distance(
    const float* a,
    const float* b,
    int dim
)
{
    if (!a || !b || dim <= 0) {
        return 1.0f;
    }

    double dot = 0.0;

    for (int i = 0; i < dim; i++) {
        dot += (double)a[i] * (double)b[i];
    }

    double dist = 1.0 - dot;

    if (dist < 0.0) {
        dist = 0.0;
    }

    if (!isfinite(dist)) {
        return FLT_MAX;
    }

    return (float)dist;
}


/* ---------------------------------------------------------
   Dynamic Time Warping
   --------------------------------------------------------- */

float dtw_calculate_distance(
    const float* seq1,
    int n1,
    const float* seq2,
    int n2
)
{
    if (
        !seq1 ||
        !seq2 ||
        n1 <= 0 ||
        n2 <= 0 ||
        n1 > MAX_FRAMES_LIMIT ||
        n2 > MAX_FRAMES_LIMIT
    ) {
        return FLT_MAX;
    }

    int abs_diff = abs(n1 - n2);
    int max_len = (n1 > n2) ? n1 : n2;

    int scaled_band = (int)ceil(
        (double)max_len * DTW_BAND_RATIO_DEFAULT
    );

    int band = (abs_diff > scaled_band)
        ? abs_diff
        : scaled_band;

    size_t count = (size_t)n2 + 1;

    size_t row_bytes_f = count * sizeof(float);
    size_t row_bytes_i = count * sizeof(int);

    float* prev_cost = (float*)DTW_MALLOC(row_bytes_f);
    int* prev_len = (int*)DTW_MALLOC(row_bytes_i);

    float* curr_cost = (float*)DTW_MALLOC(row_bytes_f);
    int* curr_len = (int*)DTW_MALLOC(row_bytes_i);

    if (
        !prev_cost ||
        !prev_len ||
        !curr_cost ||
        !curr_len
    ) {
        DTW_FREE(prev_cost);
        DTW_FREE(prev_len);
        DTW_FREE(curr_cost);
        DTW_FREE(curr_len);

        return NAN;
    }

    for (int j = 0; j <= n2; j++) {
        prev_cost[j] = FLT_MAX;
        prev_len[j] = 0;
    }

    prev_cost[0] = 0.0f;

    for (int i = 1; i <= n1; i++) {

        for (int j = 0; j <= n2; j++) {
            curr_cost[j] = FLT_MAX;
            curr_len[j] = 0;
        }

        int center = py_round_half_to_even(
            ((double)i * (double)n2) / (double)n1
        );

        int start = center - band;
        int end = center + band;

        if (start < 1) start = 1;
        if (end > n2) end = n2;

        const float* frame_a =
            &seq1[(size_t)(i - 1) * EMBEDDING_DIM];

        for (int j = start; j <= end; j++) {

            const float* frame_b =
                &seq2[(size_t)(j - 1) * EMBEDDING_DIM];

            float cost = dtw_cosine_distance(
                frame_a,
                frame_b,
                EMBEDDING_DIM
            );

            float c0 = prev_cost[j - 1];
            int l0 = prev_len[j - 1];

            float c1 = prev_cost[j];
            int l1 = prev_len[j];

            float c2 = curr_cost[j - 1];
            int l2 = curr_len[j - 1];

            float best_cost = c0;
            int best_len = l0;

            if (c1 < best_cost) {
                best_cost = c1;
                best_len = l1;
            }

            if (c2 < best_cost) {
                best_cost = c2;
                best_len = l2;
            }

            if (
                best_cost < FLT_MAX &&
                isfinite(best_cost)
            ) {
                curr_cost[j] = best_cost + cost;
                curr_len[j] = best_len + 1;
            }
        }

        memcpy(prev_cost, curr_cost, row_bytes_f);
        memcpy(prev_len, curr_len, row_bytes_i);
    }

    float total_cost = prev_cost[n2];
    int total_len = prev_len[n2];

    DTW_FREE(prev_cost);
    DTW_FREE(prev_len);
    DTW_FREE(curr_cost);
    DTW_FREE(curr_len);

    if (
        total_len <= 0 ||
        total_cost >= FLT_MAX ||
        !isfinite(total_cost)
    ) {
        return FLT_MAX;
    }

    return total_cost / (float)total_len;
}


/* ---------------------------------------------------------
   Python-compatible DTW for YES/NO only
   --------------------------------------------------------- */

static float dtw_yes_no_distance(
    const float* seq1,
    int n1,
    const float* seq2,
    int n2
)
{
    if (
        !seq1 || !seq2 ||
        n1 <= 0 || n2 <= 0 ||
        n1 > MAX_FRAMES_LIMIT ||
        n2 > MAX_FRAMES_LIMIT
    ) {
        return FLT_MAX;
    }

    int abs_diff = abs(n1 - n2);
    int max_len = (n1 > n2) ? n1 : n2;

    int band = (int)((double)max_len * 0.25);

    if (abs_diff > band) {
        band = abs_diff;
    }

    size_t count = (size_t)n2 + 1;

    size_t bytes_d = count * sizeof(double);
    size_t bytes_i = count * sizeof(int);

    double* prev_cost = (double*)DTW_MALLOC(bytes_d);
    double* curr_cost = (double*)DTW_MALLOC(bytes_d);
    int* prev_len = (int*)DTW_MALLOC(bytes_i);
    int* curr_len = (int*)DTW_MALLOC(bytes_i);

    if (!prev_cost || !curr_cost || !prev_len || !curr_len) {
        DTW_FREE(prev_cost);
        DTW_FREE(curr_cost);
        DTW_FREE(prev_len);
        DTW_FREE(curr_len);
        return NAN;
    }

    for (int j = 0; j <= n2; j++) {
        prev_cost[j] = DBL_MAX;
        prev_len[j] = 0;
    }

    prev_cost[0] = 0.0;

    for (int i = 1; i <= n1; i++) {

        for (int j = 0; j <= n2; j++) {
            curr_cost[j] = DBL_MAX;
            curr_len[j] = 0;
        }

        int start = i - band;
        int end = i + band;

        if (start < 1) start = 1;
        if (end > n2) end = n2;

        const float* frame_a =
            &seq1[(size_t)(i - 1) * EMBEDDING_DIM];

        for (int j = start; j <= end; j++) {

            const float* frame_b =
                &seq2[(size_t)(j - 1) * EMBEDDING_DIM];

            double cost = (double)dtw_cosine_distance(
                frame_a,
                frame_b,
                EMBEDDING_DIM
            );

            /* Python order: up, left, diagonal.
               Strict comparison preserves the first on ties. */

            double best_cost = prev_cost[j];
            int best_len = prev_len[j];

            if (curr_cost[j - 1] < best_cost) {
                best_cost = curr_cost[j - 1];
                best_len = curr_len[j - 1];
            }

            if (prev_cost[j - 1] < best_cost) {
                best_cost = prev_cost[j - 1];
                best_len = prev_len[j - 1];
            }

            if (best_cost < DBL_MAX && isfinite(best_cost)) {
                curr_cost[j] = best_cost + cost;
                curr_len[j] = best_len + 1;
            }
        }

        memcpy(prev_cost, curr_cost, bytes_d);
        memcpy(prev_len, curr_len, bytes_i);
    }

    double total_cost = prev_cost[n2];
    int total_len = prev_len[n2];

    DTW_FREE(prev_cost);
    DTW_FREE(curr_cost);
    DTW_FREE(prev_len);
    DTW_FREE(curr_len);

    if (
        total_len <= 0 ||
        total_cost >= DBL_MAX ||
        !isfinite(total_cost)
    ) {
        return FLT_MAX;
    }

    return (float)(total_cost / (double)total_len);
}


/* ---------------------------------------------------------
   Класификация
   --------------------------------------------------------- */

bool dtw_classify_state_aware(
    const float* input_frames,
    int num_frames,
    const dtw_dictionary_t* dict,
    lisa_state_t current_state,
    classification_result_t* out_res
)
{
    if (!out_res) {
        return false;
    }

    init_reject_result(out_res, "INVALID_INPUT");

    if (
        !input_frames ||
        num_frames <= 0 ||
        num_frames > MAX_FRAMES_LIMIT ||
        !dict ||
        dict->num_classes < 2 ||
        !dict->classes
    ) {
        return false;
    }

    size_t total_floats =
        (size_t)num_frames * EMBEDDING_DIM;

    for (size_t i = 0; i < total_floats; i++) {
        if (!isfinite(input_frames[i])) {
            copy_name(
                out_res->reason_7,
                sizeof(out_res->reason_7),
                "INVALID_EMBEDDING"
            );

            return false;
        }
    }

    float* norm_input = (float*)DTW_MALLOC(
        total_floats * sizeof(float)
    );

    class_score_entry_t* ranked_9 =
        (class_score_entry_t*)DTW_MALLOC(
            (size_t)dict->num_classes *
            sizeof(class_score_entry_t)
        );

    class_score_entry_t* ranked_7 =
        (class_score_entry_t*)DTW_MALLOC(
            (size_t)dict->num_classes *
            sizeof(class_score_entry_t)
        );

    if (!norm_input || !ranked_9 || !ranked_7) {

        DTW_FREE(norm_input);
        DTW_FREE(ranked_9);
        DTW_FREE(ranked_7);

        init_reject_result(out_res, "OOM");
        return false;
    }

    memcpy(
        norm_input,
        input_frames,
        total_floats * sizeof(float)
    );

    for (int f = 0; f < num_frames; f++) {
        dtw_normalize_frame(
            &norm_input[(size_t)f * EMBEDDING_DIM],
            EMBEDDING_DIM
        );
    }

    int count_9 = 0;
    int count_7 = 0;

    bool oom_occurred = false;
    bool invalid_dictionary = false;

    for (int c = 0; c < dict->num_classes; c++) {

        const dtw_class_t* cls = &dict->classes[c];

        if (
            cls->num_templates <= 0 ||
            !cls->templates ||
            cls->name[0] == '\0'
        ) {
            invalid_dictionary = true;
            break;
        }

        float* dists = (float*)DTW_MALLOC(
            (size_t)cls->num_templates * sizeof(float)
        );

        if (!dists) {
            oom_occurred = true;
            break;
        }

        int valid_dists = 0;

        for (int t = 0; t < cls->num_templates; t++) {

            const dtw_template_t* tmpl = &cls->templates[t];

            if (
                !tmpl->frames ||
                tmpl->num_frames <= 0 ||
                tmpl->num_frames > MAX_FRAMES_LIMIT
            ) {
                invalid_dictionary = true;
                break;
            }

            float d;

            if (
                current_state == LISA_STATE_QUESTION &&
                (
                    is_base_cmd(cls->name, "da") ||
                    is_base_cmd(cls->name, "ne")
                )
            ) {
                d = dtw_yes_no_distance(
                    norm_input,
                    num_frames,
                    tmpl->frames,
                    tmpl->num_frames
                );
            } else {
                d = dtw_calculate_distance(
                    norm_input,
                    num_frames,
                    tmpl->frames,
                    tmpl->num_frames
                );
            }

            if (isnan(d)) {
                oom_occurred = true;
                break;
            }

            dists[valid_dists++] = d;
        }

        if (oom_occurred || invalid_dictionary) {
            DTW_FREE(dists);
            break;
        }

        if (valid_dists <= 0) {
            DTW_FREE(dists);
            invalid_dictionary = true;
            break;
        }

        /* Сортиране на разстоянията в рамките на класа. */
        for (int i = 0; i < valid_dists - 1; i++) {
            for (int j = i + 1; j < valid_dists; j++) {

                if (dists[j] < dists[i]) {
                    float tmp = dists[i];
                    dists[i] = dists[j];
                    dists[j] = tmp;
                }
            }
        }

        int k = (valid_dists < TOP_K_DEFAULT)
            ? valid_dists
            : TOP_K_DEFAULT;

        double sum = 0.0;

        for (int i = 0; i < k; i++) {
            sum += (double)dists[i];
        }

        float class_score = (float)(sum / (double)k);

        DTW_FREE(dists);

        if (!isfinite(class_score)) {
            class_score = FLT_MAX;
        }

        ranked_9[count_9].class_idx = c;
        ranked_9[count_9].score = class_score;
        count_9++;

        if (cls->has_threshold) {
            ranked_7[count_7].class_idx = c;
            ranked_7[count_7].score = class_score;
            count_7++;
        }
    }

    DTW_FREE(norm_input);

    if (oom_occurred || invalid_dictionary) {

        DTW_FREE(ranked_9);
        DTW_FREE(ranked_7);

        init_reject_result(
            out_res,
            oom_occurred ? "OOM" : "INVALID_DICTIONARY"
        );

        return false;
    }

    if (count_9 < 2 || count_7 < 2) {

        DTW_FREE(ranked_9);
        DTW_FREE(ranked_7);

        init_reject_result(out_res, "NOT_ENOUGH_CLASSES");
        return false;
    }

    /* Подреждане на всички класове — диагностичен резултат. */
    qsort(
        ranked_9,
        (size_t)count_9,
        sizeof(class_score_entry_t),
        compare_scores
    );

    int diag_best_idx = ranked_9[0].class_idx;
    int diag_second_idx = ranked_9[1].class_idx;

    const char* diag_best_name =
        dict->classes[diag_best_idx].name;

    const char* diag_second_name =
        dict->classes[diag_second_idx].name;

    out_res->diag_best_dist_9 = ranked_9[0].score;
    out_res->diag_second_dist_9 = ranked_9[1].score;

    out_res->diag_margin_9 =
        ranked_9[1].score - ranked_9[0].score;

    copy_name(
        out_res->diag_best_9,
        sizeof(out_res->diag_best_9),
        diag_best_name
    );

    copy_name(
        out_res->diag_second_9,
        sizeof(out_res->diag_second_9),
        diag_second_name
    );


    /*
     * QUESTION:
     * da_kids и ne_kids се сравняват директно.
     * За този тест не използваме калибрирани прагове.
     */

    /*
     * QUESTION:
     * 1. Проверяваме седемте калибрирани команди със стандартния DTW.
     * 2. Разрешаваме zaspi, ако премине собствените си критерии.
     * 3. Не допускаме друга валидно разпозната команда да бъде
     *    приета погрешно като da/ne.
     * 4. Сравняваме da/ne само чрез специалния им DTW.
     */

    if (current_state == LISA_STATE_QUESTION) {

        /* Първо проверяваме седемте калибрирани класа. */
        qsort(
            ranked_7,
            (size_t)count_7,
            sizeof(class_score_entry_t),
            compare_scores
        );

        int standard_best_idx = ranked_7[0].class_idx;
        int standard_second_idx = ranked_7[1].class_idx;

        const dtw_class_t* standard_best_cls =
            &dict->classes[standard_best_idx];

        const char* standard_best_name =
            standard_best_cls->name;

        float standard_best_dist = ranked_7[0].score;
        float standard_second_dist = ranked_7[1].score;

        float standard_margin =
            standard_second_dist - standard_best_dist;

        bool standard_threshold_ok =
            standard_best_dist <= standard_best_cls->threshold_distance;

        bool standard_margin_ok =
            standard_margin >= standard_best_cls->threshold_margin;

        bool standard_candidate_valid =
            standard_threshold_ok && standard_margin_ok;

        /*
         * Специална команда zaspi.
         * Тя е единствената от седемте основни команди,
         * разрешена в QUESTION.
         */
        if (is_base_cmd(standard_best_name, "zaspi")) {

            if (standard_candidate_valid) {

                copy_name(
                    out_res->best_command_7,
                    sizeof(out_res->best_command_7),
                    standard_best_name
                );

                copy_name(
                    out_res->second_command_7,
                    sizeof(out_res->second_command_7),
                    dict->classes[standard_second_idx].name
                );

                out_res->best_distance_7 = standard_best_dist;
                out_res->second_distance_7 = standard_second_dist;
                out_res->margin_7 = standard_margin;
                out_res->accepted_7 = true;

                copy_name(
                    out_res->prediction_7,
                    sizeof(out_res->prediction_7),
                    standard_best_name
                );

                copy_name(
                    out_res->reason_7,
                    sizeof(out_res->reason_7),
                    "OK"
                );

                DTW_FREE(ranked_9);
                DTW_FREE(ranked_7);

                return true;
            }

            /*
             * zaspi е най-добрият стандартен кандидат,
             * но не покрива критериите си.
             * Не го заменяме с потенциално погрешно da/ne.
             */
            if (standard_threshold_ok) {

                copy_name(
                    out_res->best_command_7,
                    sizeof(out_res->best_command_7),
                    standard_best_name
                );

                out_res->best_distance_7 = standard_best_dist;
                out_res->second_distance_7 = standard_second_dist;
                out_res->margin_7 = standard_margin;
                out_res->accepted_7 = false;

                copy_name(
                    out_res->prediction_7,
                    sizeof(out_res->prediction_7),
                    "REJECT"
                );

                copy_name(
                    out_res->reason_7,
                    sizeof(out_res->reason_7),
                    "QUESTION_ZASPI_AMBIGUOUS"
                );

                DTW_FREE(ranked_9);
                DTW_FREE(ranked_7);

                return true;
            }
        }

        /*
         * Друга калибрирана команда е преминала
         * и двата си критерия.
         * Не позволяваме тя да бъде приета като da/ne.
         */
        if (
            !is_base_cmd(standard_best_name, "zaspi") &&
            standard_candidate_valid
        ) {

            copy_name(
                out_res->best_command_7,
                sizeof(out_res->best_command_7),
                standard_best_name
            );

            copy_name(
                out_res->second_command_7,
                sizeof(out_res->second_command_7),
                dict->classes[standard_second_idx].name
            );

            out_res->best_distance_7 = standard_best_dist;
            out_res->second_distance_7 = standard_second_dist;
            out_res->margin_7 = standard_margin;
            out_res->accepted_7 = false;

            copy_name(
                out_res->prediction_7,
                sizeof(out_res->prediction_7),
                "REJECT"
            );

            copy_name(
                out_res->reason_7,
                sizeof(out_res->reason_7),
                "QUESTION_OTHER_CLASS_REJECT"
            );

            DTW_FREE(ranked_9);
            DTW_FREE(ranked_7);

            return true;
        }

        /* Намираме специалните класове da и ne. */
        int da_idx = -1;
        int ne_idx = -1;

        float da_dist = FLT_MAX;
        float ne_dist = FLT_MAX;

        for (int i = 0; i < count_9; i++) {

            int idx = ranked_9[i].class_idx;
            const char* name = dict->classes[idx].name;

            if (is_base_cmd(name, "da")) {
                da_idx = idx;
                da_dist = ranked_9[i].score;
            }

            if (is_base_cmd(name, "ne")) {
                ne_idx = idx;
                ne_dist = ranked_9[i].score;
            }
        }

        if (da_idx < 0 || ne_idx < 0) {

            init_reject_result(
                out_res,
                "MISSING_YES_NO_CLASS"
            );

            DTW_FREE(ranked_9);
            DTW_FREE(ranked_7);

            return false;
        }

        /*
         * Не избираме произволно при равни разстояния.
         * Запазваме съществуващия праг 0.30.
         */
        bool accept_da = da_dist < ne_dist;
        bool accept_ne = ne_dist < da_dist;

        int best_idx = accept_da ? da_idx : ne_idx;
        int second_idx = accept_da ? ne_idx : da_idx;

        float best_dist = accept_da ? da_dist : ne_dist;
        float second_dist = accept_da ? ne_dist : da_dist;

        bool accepted =
            (accept_da || accept_ne) &&
            best_dist <= 0.30f;

        copy_name(
            out_res->best_command_7,
            sizeof(out_res->best_command_7),
            dict->classes[best_idx].name
        );

        copy_name(
            out_res->second_command_7,
            sizeof(out_res->second_command_7),
            dict->classes[second_idx].name
        );

        out_res->best_distance_7 = best_dist;
        out_res->second_distance_7 = second_dist;
        out_res->margin_7 = second_dist - best_dist;
        out_res->accepted_7 = accepted;

        copy_name(
            out_res->prediction_7,
            sizeof(out_res->prediction_7),
            accepted ? dict->classes[best_idx].name : "REJECT"
        );

        copy_name(
            out_res->reason_7,
            sizeof(out_res->reason_7),
            accepted ? "OK" : "QUESTION_DISTANCE_REJECT"
        );

        DTW_FREE(ranked_9);
        DTW_FREE(ranked_7);

        return true;
    }



    /*
     * Всички останали състояния:
     * използваме седемте калибрирани команди.
     */
    qsort(
        ranked_7,
        (size_t)count_7,
        sizeof(class_score_entry_t),
        compare_scores
    );

    int best_idx = ranked_7[0].class_idx;
    int second_idx = ranked_7[1].class_idx;

    const dtw_class_t* best_cls = &dict->classes[best_idx];

    const char* best_name = best_cls->name;
    const char* second_name = dict->classes[second_idx].name;

    float best_dist = ranked_7[0].score;
    float second_dist = ranked_7[1].score;
    float margin = second_dist - best_dist;

    copy_name(
        out_res->best_command_7,
        sizeof(out_res->best_command_7),
        best_name
    );

    copy_name(
        out_res->second_command_7,
        sizeof(out_res->second_command_7),
        second_name
    );

    out_res->best_distance_7 = best_dist;
    out_res->second_distance_7 = second_dist;
    out_res->margin_7 = margin;

    bool accepted = false;
    const char* reason = "THRESHOLD_REJECT";

    if (!is_command_allowed_in_state(best_name, current_state)) {

        reason = "STATE_REJECT";

    } else if (
        best_dist <= best_cls->threshold_distance &&
        margin >= best_cls->threshold_margin
    ) {

        accepted = true;
        reason = "OK";
    }

    out_res->accepted_7 = accepted;

    copy_name(
        out_res->prediction_7,
        sizeof(out_res->prediction_7),
        accepted ? best_name : "REJECT"
    );

    copy_name(
        out_res->reason_7,
        sizeof(out_res->reason_7),
        reason
    );

    DTW_FREE(ranked_9);
    DTW_FREE(ranked_7);

    return true;
}


/* ---------------------------------------------------------
   Съвместим интерфейс без зададено състояние
   --------------------------------------------------------- */

bool dtw_classify(
    const float* input_frames,
    int num_frames,
    const dtw_dictionary_t* dict,
    classification_result_t* out_res
)
{
    return dtw_classify_state_aware(
        input_frames,
        num_frames,
        dict,
        LISA_STATE_IDLE,
        out_res
    );
}
