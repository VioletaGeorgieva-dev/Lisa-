#include "lisa_recognizer.h"

#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#ifdef ESP_PLATFORM
#include <esp_log.h>
#include <esp_timer.h>
#include <esp_heap_caps.h>
static const char* TAG = "lisa_rec";
#define NOW_US() ((uint64_t)esp_timer_get_time())
#define REC_ALLOC(n) heap_caps_malloc((n), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT)
#define REC_FREE(p)  heap_caps_free(p)
#else
#include <time.h>
#define ESP_LOGI(tag, fmt, ...) printf("[%s] " fmt "\n", tag, ##__VA_ARGS__)
#define ESP_LOGW(tag, fmt, ...) printf("[%s] WARN " fmt "\n", tag, ##__VA_ARGS__)
#define ESP_LOGE(tag, fmt, ...) printf("[%s] ERROR " fmt "\n", tag, ##__VA_ARGS__)
static const char* TAG = "lisa_rec";
static uint64_t NOW_US(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (uint64_t)t.tv_sec * 1000000ULL + (uint64_t)t.tv_nsec / 1000ULL;
}
#define REC_ALLOC(n) malloc(n)
#define REC_FREE(p)  free(p)
#endif

void lisa_rec_default_params(lisa_rec_params_t* p)
{
    if (!p) {
        return;
    }

    p->min_speech_db = 10.0f;   /* same as the VAD start threshold */
    p->margin_ms = 150;
}

bool lisa_rec_init(lisa_recognizer_t* r, const void* model_blob, size_t model_size,
                   const char* wdt_path, size_t max_samples,
                   const lisa_rec_params_t* params)
{
    if (!r || !model_blob || !wdt_path || max_samples == 0) {
        return false;
    }

    memset(r, 0, sizeof(*r));

    if (params) {
        r->params = *params;
    } else {
        lisa_rec_default_params(&r->params);
    }

    int rc = nd_whistle_open(&r->w, model_blob, model_size);

    if (rc != 0) {
        ESP_LOGE(TAG, "nd_whistle_open failed (%d): not a Whistle .cact v4?", rc);
        return false;
    }

    if (!wdt_dictionary_load(wdt_path, &r->dict)) {
        ESP_LOGE(TAG, "cannot load dictionary %s", wdt_path);
        nd_whistle_close(&r->w);
        return false;
    }

    /* Encoder output for the longest clip. */
    size_t frames = nd_whistle_frames(&r->w, (uint32_t)max_samples);

    if (frames == 0) {
        frames = 1;
    }

    r->enc = (float*)REC_ALLOC(frames * EMBEDDING_DIM * sizeof(float));

    if (!r->enc) {
        ESP_LOGE(TAG, "no memory for encoder output (%u frames)", (unsigned)frames);
        wdt_dictionary_free(&r->dict);
        nd_whistle_close(&r->w);
        return false;
    }

    if (r->w.d != EMBEDDING_DIM) {
        ESP_LOGE(TAG, "encoder width %u != EMBEDDING_DIM %d", (unsigned)r->w.d, EMBEDDING_DIM);
        REC_FREE(r->enc);
        wdt_dictionary_free(&r->dict);
        nd_whistle_close(&r->w);
        return false;
    }

    r->enc_capacity_frames = frames;
    r->open = true;

    ESP_LOGI(TAG, "ready: %d classes, up to %u encoder frames",
             r->dict.num_classes, (unsigned)frames);

    return true;
}

void lisa_rec_free(lisa_recognizer_t* r)
{
    if (!r || !r->open) {
        return;
    }

    REC_FREE(r->enc);
    wdt_dictionary_free(&r->dict);
    nd_whistle_close(&r->w);
    memset(r, 0, sizeof(*r));
}

lisa_rec_status_t lisa_rec_classify(lisa_recognizer_t* r,
                                    const float* samples, size_t n,
                                    float floor_rms, lisa_state_t state,
                                    classification_result_t* out,
                                    lisa_rec_stats_t* stats)
{
    if (!r || !r->open || !samples || n == 0 || !out) {
        return LISA_REC_BAD_ARG;
    }

    if (stats) {
        memset(stats, 0, sizeof(*stats));
    }

    uint32_t s0 = 0;
    uint32_t s1 = 0;

    if (!nd_whistle_find_speech(samples, (uint32_t)n, floor_rms,
                                r->params.min_speech_db, r->params.margin_ms,
                                &s0, &s1)) {
        return LISA_REC_NO_SPEECH;
    }

    uint32_t len = s1 - s0;
    uint32_t frames = nd_whistle_frames(&r->w, len);

    if (frames == 0) {
        return LISA_REC_NO_SPEECH;
    }

    if (frames > MAX_FRAMES_LIMIT || frames > r->enc_capacity_frames) {
        return LISA_REC_TOO_LONG;
    }

#ifdef ESP_PLATFORM
    /* nd_whistle_encode() does not check ND_ALLOC for NULL (nd_whistle.c:857,
     * 863, 870), so refuse to start when it clearly cannot succeed.
     * Measured with the real model (host, same code): 1.6-1.7 MB of PSRAM in
     * total for 1-3 s of audio, the largest single block 737,280 B, and a
     * 58,368 B contiguous block of internal RAM (the MLP weights). */
    if (heap_caps_get_free_size(MALLOC_CAP_SPIRAM) < (size_t)(2 * 1024 * 1024) ||
        heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM) < (size_t)(768 * 1024) ||
        heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL) < (size_t)(80 * 1024)) {
        ESP_LOGE(TAG, "not enough free memory for the encoder");
        return LISA_REC_NO_MEMORY;
    }
#endif

    uint64_t t0 = NOW_US();

    if (nd_whistle_encode(&r->w, samples + s0, len, r->enc) != 0) {
        return LISA_REC_ENCODE_FAILED;
    }

    uint64_t t1 = NOW_US();

    bool ok = dtw_classify_state_aware(r->enc, (int)frames, &r->dict, state, out);

    uint64_t t2 = NOW_US();

    if (stats) {
        stats->speech_samples = len;
        stats->frames = frames;
        stats->encode_ms = (uint32_t)((t1 - t0) / 1000);
        stats->classify_ms = (uint32_t)((t2 - t1) / 1000);
    }

    return ok ? LISA_REC_OK : LISA_REC_CLASSIFY_FAILED;
}

const char* lisa_rec_status_str(lisa_rec_status_t s)
{
    switch (s) {
    case LISA_REC_OK:              return "OK";
    case LISA_REC_NO_SPEECH:       return "NO_SPEECH";
    case LISA_REC_TOO_LONG:        return "TOO_LONG";
    case LISA_REC_ENCODE_FAILED:   return "ENCODE_FAILED";
    case LISA_REC_CLASSIFY_FAILED: return "CLASSIFY_FAILED";
    case LISA_REC_NO_MEMORY:       return "NO_MEMORY";
    case LISA_REC_BAD_ARG:         return "BAD_ARG";
    default:                       return "UNKNOWN";
    }
}
