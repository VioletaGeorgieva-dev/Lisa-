/* Lisa - voice-controlled story player.
 *
 *   I2S mic --> mic_task (VAD endpointing) --job--> rec_task
 *                                                    (speech gate, Whistle encoder, DTW)
 *                                                      |  accepted command
 *                                                      v
 *   SD stories --> I2S speaker <-- ctrl_task (FSM + player, pumps audio)
 *
 * Only one task (ctrl_task) touches the FSM and the player. The recognizer
 * runs on its own task so that a story keeps playing while a command is
 * being recognised.
 */
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <sys/stat.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"

#include "esp_log.h"
#include "esp_timer.h"
#include "esp_heap_caps.h"
#include "esp_partition.h"
#include <math.h>

#include "lisa_hardware.h"
#include "lisa_player.h"
#include "lisa_audio_in.h"
#include "lisa_recognizer.h"
#include "lisa_controller.h"
#include "esp_aec_wrapper.h"

static const char* TAG = "lisa";

#ifndef CONFIG_LISA_MAX_UTTERANCE_MS
#define CONFIG_LISA_MAX_UTTERANCE_MS 3000
#endif
#ifndef CONFIG_LISA_VAD_START_DB
#define CONFIG_LISA_VAD_START_DB 10
#endif
#ifndef CONFIG_LISA_VAD_ABS_MIN_RMS_MILLI
#define CONFIG_LISA_VAD_ABS_MIN_RMS_MILLI 3
#endif
#ifndef CONFIG_LISA_VAD_HANGOVER_MS
#define CONFIG_LISA_VAD_HANGOVER_MS 600
#endif
#ifndef CONFIG_LISA_VAD_PREROLL_MS
#define CONFIG_LISA_VAD_PREROLL_MS 300
#endif
#ifndef CONFIG_LISA_WDT_PATH
#define CONFIG_LISA_WDT_PATH "/sdcard/commands.wdt"
#endif
#ifndef CONFIG_LISA_MODEL_PARTITION
#define CONFIG_LISA_MODEL_PARTITION "model"
#endif

#define MAX_SAMPLES ((size_t)CONFIG_LISA_MAX_UTTERANCE_MS * (LISA_MIC_SAMPLE_RATE / 1000))
#define PARTITION_SUBTYPE_MODEL 0x40

typedef struct {
    char command[32];
    lisa_state_t state;     /* FSM state when classification started */
} cmd_msg_t;

static lisa_controller_t g_ctrl;
static lisa_recognizer_t g_rec;
static lisa_vad_t g_vad;

static float* g_job_buf;
static volatile size_t g_job_len;
static volatile float g_job_floor;
static volatile bool g_job_busy;
static portMUX_TYPE g_job_mux = portMUX_INITIALIZER_UNLOCKED;

static SemaphoreHandle_t g_job_sem;
static QueueHandle_t g_cmd_q;

/* Direct ESP-SR AEC state. The frame length is queried from the library at
 * runtime; FD_LOW_COST is expected to return 512 samples at 16 kHz. */
static aec_handle_t *g_aec;
static size_t g_aec_chunk;
static int16_t *g_aec_work;
static int16_t *g_aec_mic;
static int16_t *g_aec_ref;
static int16_t *g_aec_out;

/* Timestamped history of completed speaker DMA blocks. The TX callback
 * supplies the exact PCM copied to I2S and its completion timestamp. */
#define LISA_AEC_REF_HISTORY_BLOCKS 24
#define LISA_AEC_REF_BLOCK_MAX_SAMPLES 512

typedef struct {
    int16_t samples[LISA_AEC_REF_BLOCK_MAX_SAMPLES];
    size_t count;
    int64_t end_time_us;
    bool valid;
} lisa_aec_ref_block_t;

static lisa_aec_ref_block_t g_aec_ref_history[LISA_AEC_REF_HISTORY_BLOCKS];
static size_t g_aec_ref_history_write;
static size_t g_aec_ref_history_count;
static int64_t g_aec_ref_latest_end_us;
static int16_t g_aec_ref_read_tmp[LISA_AEC_REF_BLOCK_MAX_SAMPLES];

/* 22.05 kHz speaker reference -> 16 kHz microphone timeline. A 16-tap
 * windowed-sinc fractional-delay interpolator limits the reference bandwidth
 * below the 8 kHz Nyquist frequency of the AEC input. */
#define LISA_AEC_RESAMPLER_TAPS   32U
#define LISA_AEC_RESAMPLER_PHASES 128U
#define LISA_AEC_RESAMPLER_LEFT   15
#define LISA_AEC_RESAMPLER_FC    0.35f
static float g_aec_resampler_coeff[LISA_AEC_RESAMPLER_PHASES][LISA_AEC_RESAMPLER_TAPS];
static float g_aec_resampler_sum[LISA_AEC_RESAMPLER_PHASES];
static uint32_t g_aec_ref_uncovered_samples;
static uint32_t g_aec_ref_wait_timeouts;
static uint32_t g_aec_ref_wait_max_us;

static void aec_resampler_init(void)
{
    const float pi = 3.14159265358979323846f;

    for (size_t phase = 0; phase < LISA_AEC_RESAMPLER_PHASES; phase++) {
        float fraction = (float)phase / (float)LISA_AEC_RESAMPLER_PHASES;
        float sum = 0.0f;

        for (size_t tap = 0; tap < LISA_AEC_RESAMPLER_TAPS; tap++) {
            float offset = (float)((int)tap - LISA_AEC_RESAMPLER_LEFT);
            float x = offset - fraction;
            float arg = 2.0f * LISA_AEC_RESAMPLER_FC * x;
            float sinc = (fabsf(arg) < 1.0e-7f)
                ? 1.0f
                : sinf(pi * arg) / (pi * arg);
            float window = 0.42f +
                0.50f * cosf(pi * x / 16.0f) +
                0.08f * cosf(2.0f * pi * x / 16.0f);
            float coefficient =
                2.0f * LISA_AEC_RESAMPLER_FC * sinc * window;

            g_aec_resampler_coeff[phase][tap] = coefficient;
            sum += coefficient;
        }

        g_aec_resampler_sum[phase] = sum;
    }
}

static bool aec_init(void)
{
    aec_resampler_init();
    g_aec = lisa_aec_create();
    if (!g_aec) {
        ESP_LOGE(TAG, "AEC create failed; microphone will run without echo cancellation");
        return false;
    }

    int chunk = lisa_aec_get_chunksize(g_aec);
    if (chunk <= 0 || chunk > 2048) {
        ESP_LOGE(TAG, "AEC returned invalid frame size: %d", chunk);
        lisa_aec_destroy(g_aec);
        g_aec = NULL;
        return false;
    }

    g_aec_chunk = (size_t)chunk;
    size_t bytes = 3U * g_aec_chunk * sizeof(int16_t);
    g_aec_work = (int16_t *)heap_caps_aligned_alloc(
        16, bytes, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (!g_aec_work) {
        ESP_LOGE(TAG, "cannot allocate %u bytes for aligned AEC buffers",
                 (unsigned)bytes);
        lisa_aec_destroy(g_aec);
        g_aec = NULL;
        g_aec_chunk = 0;
        return false;
    }

    g_aec_ref_history_write = 0;
    g_aec_ref_history_count = 0;
    g_aec_ref_latest_end_us = 0;
    g_aec_ref_uncovered_samples = 0;
    g_aec_ref_wait_timeouts = 0;
    g_aec_ref_wait_max_us = 0;
    memset(g_aec_ref_history, 0, sizeof(g_aec_ref_history));

    g_aec_mic = g_aec_work;
    g_aec_ref = g_aec_work + g_aec_chunk;
    g_aec_out = g_aec_work + 2U * g_aec_chunk;
    memset(g_aec_work, 0, bytes);

    ESP_LOGI(TAG, "AEC ready: FD_LOW_COST, frame=%u samples (%.1f ms)",
             (unsigned)g_aec_chunk,
             1000.0f * (float)g_aec_chunk / (float)LISA_MIC_SAMPLE_RATE);
    return true;
}

/* Drain all completed TX DMA blocks into a bounded timestamped history. */
static void aec_drain_reference(void)
{
    for (;;) {
        size_t got = 0;
        int64_t end_time_us = 0;

        if (!lisa_hw_read_aec_ref_block(g_aec_ref_read_tmp,
                                        LISA_AEC_REF_BLOCK_MAX_SAMPLES,
                                        &got, &end_time_us) || got == 0) {
            break;
        }
        if (got > LISA_AEC_REF_BLOCK_MAX_SAMPLES) {
            ESP_LOGW(TAG, "AEC reference block too large: %u", (unsigned)got);
            continue;
        }

        lisa_aec_ref_block_t *b =
            &g_aec_ref_history[g_aec_ref_history_write];
        memcpy(b->samples, g_aec_ref_read_tmp, got * sizeof(int16_t));
        b->count = got;
        b->end_time_us = end_time_us;
        b->valid = true;

        g_aec_ref_history_write =
            (g_aec_ref_history_write + 1U) % LISA_AEC_REF_HISTORY_BLOCKS;
        if (g_aec_ref_history_count < LISA_AEC_REF_HISTORY_BLOCKS) {
            g_aec_ref_history_count++;
        }
        if (end_time_us > g_aec_ref_latest_end_us) {
            g_aec_ref_latest_end_us = end_time_us;
        }
    }
}

/* TX and RX timestamps share esp_timer's monotonic clock. While a story is
 * playing, allow the TX completion callback a short time to publish the
 * speaker block covering the end of the just-read microphone block. */
static void aec_collect_reference_until(int64_t mic_end_time_us)
{
    aec_drain_reference();

    if (!lisa_player_is_playing()) {
        return;
    }

    /* The 32-tap FIR needs up to sixteen future 22.05-kHz samples (~726 us)
     * for the final 16-kHz microphone sample. */
    const int64_t reference_lookahead_us = 800;
    int64_t required_end_us = mic_end_time_us + reference_lookahead_us;
    int64_t wait_start_us = esp_timer_get_time();
    int64_t deadline_us = wait_start_us + 15000;
    while (g_aec_ref_latest_end_us < required_end_us &&
           esp_timer_get_time() < deadline_us) {
        vTaskDelay(pdMS_TO_TICKS(1));
        aec_drain_reference();
    }

    uint32_t waited_us = (uint32_t)(esp_timer_get_time() - wait_start_us);
    if (waited_us > g_aec_ref_wait_max_us) {
        g_aec_ref_wait_max_us = waited_us;
    }
    if (g_aec_ref_latest_end_us < required_end_us) {
        g_aec_ref_wait_timeouts++;
    }
}

/* Timestamp of the first sample in a completed speaker DMA block. */
static int64_t aec_ref_block_start_us(const lisa_aec_ref_block_t *block)
{
    int64_t span_us =
        ((int64_t)(block->count - 1U) * 1000000LL) / LISA_SPK_SAMPLE_RATE;
    return block->end_time_us - span_us;
}

/* A discontinuity larger than 2 ms indicates a missing TX block, not ordinary
 * ISR timestamp jitter. FIR taps are not allowed to bridge such a gap. */
static bool aec_ref_blocks_contiguous(
    const lisa_aec_ref_block_t *left,
    const lisa_aec_ref_block_t *right)
{
    if (!left->valid || !right->valid || left->count == 0U || right->count == 0U) {
        return false;
    }

    int64_t expected_right_start = left->end_time_us +
        (1000000LL / LISA_SPK_SAMPLE_RATE);
    int64_t actual_right_start = aec_ref_block_start_us(right);
    int64_t delta = actual_right_start - expected_right_start;
    if (delta < 0) delta = -delta;
    return delta <= 2000;
}

/* Fetch a tap relative to the located reference block. The filter is only
 * 32 samples wide, so it can cross at most one normal 240-sample DMA boundary. */
static bool aec_ref_get_tap(
    size_t oldest,
    size_t block_count,
    size_t center_index,
    int sample_index,
    int16_t *sample_out)
{
    if (!sample_out || center_index >= block_count) {
        return false;
    }

    size_t current_index = center_index;
    const lisa_aec_ref_block_t *current =
        &g_aec_ref_history[(oldest + current_index) % LISA_AEC_REF_HISTORY_BLOCKS];

    while (sample_index < 0) {
        if (current_index == 0U) {
            return false;
        }
        const lisa_aec_ref_block_t *previous =
            &g_aec_ref_history[(oldest + current_index - 1U) % LISA_AEC_REF_HISTORY_BLOCKS];
        if (!aec_ref_blocks_contiguous(previous, current)) {
            return false;
        }
        current_index--;
        current = previous;
        sample_index += (int)current->count;
    }

    while (sample_index >= (int)current->count) {
        sample_index -= (int)current->count;
        if (current_index + 1U >= block_count) {
            return false;
        }
        const lisa_aec_ref_block_t *next =
            &g_aec_ref_history[(oldest + current_index + 1U) % LISA_AEC_REF_HISTORY_BLOCKS];
        if (!aec_ref_blocks_contiguous(current, next)) {
            return false;
        }
        current_index++;
        current = next;
    }

    *sample_out = current->samples[sample_index];
    return true;
}

/* Band-limited fractional-delay resampling from the timestamped 22.05-kHz
 * speaker timeline onto each 16-kHz microphone sample instant. */
static int16_t aec_reference_sample_at(int64_t sample_time_us)
{
    size_t block_count = g_aec_ref_history_count;
    if (block_count == 0U) {
        return 0;
    }

    size_t oldest = (g_aec_ref_history_write +
                     LISA_AEC_REF_HISTORY_BLOCKS - block_count) %
                    LISA_AEC_REF_HISTORY_BLOCKS;
    size_t center_index = block_count;
    double position = 0.0;

    for (size_t k = 0; k < block_count; k++) {
        const lisa_aec_ref_block_t *block =
            &g_aec_ref_history[(oldest + k) % LISA_AEC_REF_HISTORY_BLOCKS];
        if (!block->valid || block->count == 0U) {
            continue;
        }

        int64_t start_time_us = aec_ref_block_start_us(block);
        if (sample_time_us < start_time_us || sample_time_us > block->end_time_us) {
            continue;
        }

        position = (double)(sample_time_us - start_time_us) *
                   (double)LISA_SPK_SAMPLE_RATE / 1000000.0;
        center_index = k;
        break;
    }

    if (center_index >= block_count) {
        if (lisa_player_is_playing()) {
            g_aec_ref_uncovered_samples++;
        }
        return 0;
    }

    int base_index = (int)floor(position);
    float fraction = (float)(position - (double)base_index);
    size_t phase = (size_t)(fraction * (float)LISA_AEC_RESAMPLER_PHASES + 0.5f);
    if (phase >= LISA_AEC_RESAMPLER_PHASES) {
        phase = 0U;
        base_index++;
    }

    float accumulator = 0.0f;
    bool missing_tap = false;
    const float *coefficients = g_aec_resampler_coeff[phase];
    for (size_t tap = 0; tap < LISA_AEC_RESAMPLER_TAPS; tap++) {
        int offset = (int)tap - LISA_AEC_RESAMPLER_LEFT;
        int16_t source_sample = 0;
        if (aec_ref_get_tap(oldest, block_count, center_index,
                            base_index + offset, &source_sample)) {
            accumulator += (float)source_sample * coefficients[tap];
        } else {
            missing_tap = true;
        }
    }
    if (missing_tap && lisa_player_is_playing()) {
        g_aec_ref_uncovered_samples++;
    }

    float normalization = g_aec_resampler_sum[phase];
    if (fabsf(normalization) < 1.0e-6f) {
        return 0;
    }

    float output = accumulator / normalization;
    if (output > 32767.0f) output = 32767.0f;
    if (output < -32768.0f) output = -32768.0f;
    return (int16_t)lrintf(output);
}

static void publish_vad_if_ready(void)
{
    bool taken = false;

    portENTER_CRITICAL(&g_job_mux);
    if (!g_job_busy) {
        g_job_busy = true;
        taken = true;
    }
    portEXIT_CRITICAL(&g_job_mux);

    if (taken) {
        memcpy(g_job_buf, g_vad.buf, g_vad.len * sizeof(float));
        g_job_len = g_vad.len;
        g_job_floor = g_vad.floor_rms;
        xSemaphoreGive(g_job_sem);
    } else {
        ESP_LOGW(TAG, "utterance dropped: recognizer busy");
    }

    lisa_vad_reset(&g_vad);
}

static uint64_t now_ms(void)
{
    return (uint64_t)(esp_timer_get_time() / 1000);
}

static void halt(const char* why)
{
    ESP_LOGE(TAG, "FATAL: %s", why);

    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}

/* One line with free memory and this task's stack headroom, at most every
 * `every_ms`. uxTaskGetStackHighWaterMark() is in bytes on ESP-IDF. Used to
 * measure the real margins on the first runs instead of guessing. */
static void log_health(const char* who, uint32_t every_ms, uint32_t* last_ms)
{
    uint32_t t = (uint32_t)now_ms();

    if (every_ms && (uint32_t)(t - *last_ms) < every_ms) {
        return;
    }

    *last_ms = t;

    ESP_LOGI(TAG, "health %s: stack_min_free=%u B, free internal=%u B (largest %u), "
                  "free psram=%u B (largest %u)",
             who,
             (unsigned)uxTaskGetStackHighWaterMark(NULL),
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL),
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM));
}

static void* psram_alloc(size_t bytes)
{
    void* p = heap_caps_malloc(bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);

    return p ? p : heap_caps_malloc(bytes, MALLOC_CAP_8BIT);
}

static int count_stories(void)
{
    int n = 0;

    for (int i = 1; i <= 999; i++) {
        char path[96];
        struct stat st;

        snprintf(path, sizeof(path), LISA_SD_MOUNT_POINT "/stories/%03d.wav", i);

        if (stat(path, &st) != 0) {
            break;
        }

        n++;
    }

    return n;
}

/* ---------------------------------------------------------------- mic */

static void mic_task(void* arg)
{
    (void)arg;

    static int32_t raw[LISA_VAD_WINDOW];
    static float x[LISA_VAD_WINDOW];
    static float pending[2560];

    float prev_in = 0.0f;
    float prev_out = 0.0f;
    size_t aec_fill = 0;
    size_t pending_count = 0;
    bool aec_frame_contains_muted_audio = false;
    bool was_muted = false;
    uint32_t health_t = 0;
    uint32_t audio_stats_t = 0;

    for (;;) {
        size_t got = 0;
        int64_t mic_end_time_us = 0;

        log_health("mic", 30000, &health_t);

        uint32_t stats_now = now_ms();
        if ((uint32_t)(stats_now - audio_stats_t) >= 10000U) {
            lisa_hw_audio_stats_t audio_stats = {0};
            lisa_hw_get_audio_stats(&audio_stats);
            ESP_LOGI(TAG,
                     "I2S/AEC stats: RX q=%u drop=%u drv_ovf=%u; "
                     "TX q=%u drop=%u drv_ovf=%u; ref_uncovered=%u "
                     "wait_max=%u us wait_timeout=%u",
                     (unsigned)audio_stats.rx_queue_depth,
                     (unsigned)audio_stats.rx_app_queue_drops,
                     (unsigned)audio_stats.rx_driver_queue_overflows,
                     (unsigned)audio_stats.tx_queue_depth,
                     (unsigned)audio_stats.tx_app_queue_drops,
                     (unsigned)audio_stats.tx_driver_queue_overflows,
                     (unsigned)g_aec_ref_uncovered_samples,
                     (unsigned)g_aec_ref_wait_max_us,
                     (unsigned)g_aec_ref_wait_timeouts);
            audio_stats_t = stats_now;
        }

        if (!lisa_hw_read_mic_timed(raw, LISA_VAD_WINDOW, &got,
                                    &mic_end_time_us, 100) || got == 0) {
            continue;
        }

        bool listening = lisa_ctrl_listening(&g_ctrl, now_ms());

        /* Preserve the original prompt-mute behavior for VAD. Do not reset
         * the AEC frame accumulator: AEC must keep seeing the continuous
         * microphone/reference stream while Lisa speaks, otherwise its
         * adaptive state becomes stale exactly at the mute boundary. */
        if (!listening) {
            if (!was_muted) {
                lisa_vad_reset(&g_vad);
                pending_count = 0;
            }
            was_muted = true;
        } else if (was_muted) {
            lisa_vad_reset(&g_vad);
            pending_count = 0;
            was_muted = false;
        }

        if (!g_aec || !g_aec_chunk) {
            /* Safe fallback if the DSP library cannot allocate its state. */
            if (!listening) {
                continue;
            }

            for (size_t i = 0; i < got; i++) {
                float in = (float)raw[i] * (1.0f / 2147483648.0f);
                float out = in - prev_in + 0.995f * prev_out;
                prev_in = in;
                prev_out = out;
                x[i] = out;
            }

            if (lisa_vad_feed(&g_vad, x, got)) {
                publish_vad_if_ready();
            }
            continue;
        }

        /* Obtain the TX block covering the end of this RX block. The bounded
         * wait accounts for the next speaker DMA completion; missing history
         * is represented as zero reference, never as stale audio. */
        aec_collect_reference_until(mic_end_time_us);

        for (size_t i = 0; i < got; i++) {
            /* INMP441 delivers signed 24-bit data left-aligned in int32.
             * Divide by 2^16 to convert it to signed PCM16. */
            int32_t mic16 = raw[i] / 65536;
            if (mic16 > 32767) mic16 = 32767;
            if (mic16 < -32768) mic16 = -32768;

            int64_t sample_time_us = mic_end_time_us -
                ((int64_t)(got - 1U - i) * 1000000LL) / LISA_MIC_SAMPLE_RATE;

            g_aec_mic[aec_fill] = (int16_t)mic16;
            g_aec_ref[aec_fill] = aec_reference_sample_at(sample_time_us);
            if (!listening) {
                aec_frame_contains_muted_audio = true;
            }
            aec_fill++;

            if (aec_fill == g_aec_chunk) {
                lisa_aec_process(g_aec, g_aec_mic, g_aec_ref, g_aec_out);

                /* A frame crossing a prompt-mute boundary is discarded as a
                 * whole. The AEC state still receives it, but VAD never sees
                 * prompt audio or a partial frame contaminated by the prompt. */
                if (listening && !aec_frame_contains_muted_audio) {
                    if (pending_count + g_aec_chunk >
                        (sizeof(pending) / sizeof(pending[0]))) {
                        ESP_LOGE(TAG, "AEC output FIFO overflow; dropping pending audio");
                        pending_count = 0;
                    }

                    for (size_t j = 0; j < g_aec_chunk; j++) {
                        pending[pending_count++] =
                            (float)g_aec_out[j] * (1.0f / 32768.0f);
                    }
                }

                aec_fill = 0;
                aec_frame_contains_muted_audio = false;
            }
        }

        /* AEC uses its own frame length (normally 512 samples), while the
         * existing VAD consumes 320-sample windows. Keep the remainder so no
         * AEC output samples are duplicated or silently lost. */
        if (listening) {
            while (pending_count >= LISA_VAD_WINDOW) {
                for (size_t i = 0; i < LISA_VAD_WINDOW; i++) {
                    float in = pending[i];
                    float out = in - prev_in + 0.995f * prev_out;
                    prev_in = in;
                    prev_out = out;
                    x[i] = out;
                }

                if (lisa_vad_feed(&g_vad, x, LISA_VAD_WINDOW)) {
                    publish_vad_if_ready();
                }

                pending_count -= LISA_VAD_WINDOW;
                if (pending_count > 0) {
                    memmove(pending, pending + LISA_VAD_WINDOW,
                            pending_count * sizeof(float));
                }
            }
        } else {
            pending_count = 0;
        }
    }
}

/* ---------------------------------------------------------- recognizer */

static void rec_task(void* arg)
{
    (void)arg;

    for (;;) {
        xSemaphoreTake(g_job_sem, portMAX_DELAY);

        lisa_state_t state = lisa_ctrl_get_state(&g_ctrl);

        classification_result_t res;
        lisa_rec_stats_t stats;

        /* Levels, to tune the VAD on the real microphone. */
        float peak = 0.0f;

        for (size_t i = 0; i < g_job_len; i++) {
            float a = fabsf(g_job_buf[i]);
            if (a > peak) {
                peak = a;
            }
        }

        ESP_LOGI(TAG, "utterance %u ms, peak %.1f dBFS, noise floor %.1f dBFS",
                 (unsigned)(g_job_len / (LISA_MIC_SAMPLE_RATE / 1000)),
                 peak > 0 ? 20.0f * log10f(peak) : -120.0f,
                 g_job_floor > 0 ? 20.0f * log10f(g_job_floor) : -120.0f);

        lisa_rec_status_t st = lisa_rec_classify(
            &g_rec, g_job_buf, g_job_len, g_job_floor, state, &res, &stats);

        portENTER_CRITICAL(&g_job_mux);
        g_job_busy = false;   /* the job buffer is free again */
        portEXIT_CRITICAL(&g_job_mux);

        {
            uint32_t unused = 0;

            log_health("rec", 0, &unused);   /* after every utterance */
        }

        if (st != LISA_REC_OK) {
            ESP_LOGI(TAG, "[%s] no classification: %s",
                     lisa_state_to_str(state), lisa_rec_status_str(st));
            continue;
        }

        ESP_LOGI(TAG,
                 "[%s] best=%s %.3f second=%s %.3f margin=%.3f -> %s (%s) "
                 "| %u frames, encode %u ms, dtw %u ms",
                 lisa_state_to_str(state),
                 res.best_command_7, res.best_distance_7,
                 res.second_command_7, res.second_distance_7, res.margin_7,
                 res.prediction_7, res.reason_7,
                 (unsigned)stats.frames, (unsigned)stats.encode_ms,
                 (unsigned)stats.classify_ms);

        if (res.accepted_7) {
            cmd_msg_t m;

            snprintf(m.command, sizeof(m.command), "%s", res.prediction_7);
            m.state = state;
            if (xQueueSend(g_cmd_q, &m, 0) != pdTRUE) {
                ESP_LOGW(TAG, "command queue full, %s dropped", m.command);
            }
        }
    }
}

/* ---------------------------------------------------------- controller */

static void ctrl_task(void* arg)
{
    (void)arg;

    uint32_t health_t = 0;

    for (;;) {
        cmd_msg_t m;

        log_health("ctrl", 30000, &health_t);

        TickType_t wait = lisa_player_is_playing() ? 0 : pdMS_TO_TICKS(20);

        if (xQueueReceive(g_cmd_q, &m, wait) == pdTRUE) {

            /* The command was recognised for the state it was heard in. If
             * the story ended or the state changed meanwhile, drop it. */
            if (m.state == lisa_ctrl_get_state(&g_ctrl)) {
                lisa_ctrl_on_command(&g_ctrl, m.command, now_ms());
            } else {
                ESP_LOGI(TAG, "stale command %s dropped", m.command);
            }
        }

        lisa_ctrl_poll(&g_ctrl, now_ms());
    }
}

/* ---------------------------------------------------------------- main */

void app_main(void)
{
    ESP_LOGI(TAG, "Lisa starting");

    if (!lisa_hw_mount_sdcard()) {
        halt("SD card mount failed");
    }

    if (!lisa_hw_init_speaker_i2s()) {
        halt("speaker I2S init failed");
    }

    if (!lisa_hw_init_mic_i2s()) {
        halt("microphone I2S init failed");
    }

    /* Whistle model: used in place from the flash partition. */
    const esp_partition_t* part = esp_partition_find_first(
        ESP_PARTITION_TYPE_DATA,
        (esp_partition_subtype_t)PARTITION_SUBTYPE_MODEL,
        CONFIG_LISA_MODEL_PARTITION);

    if (!part) {
        halt("model partition not found");
    }

    const void* model = NULL;
    esp_partition_mmap_handle_t model_handle;

    if (esp_partition_mmap(part, 0, part->size, ESP_PARTITION_MMAP_DATA,
                           &model, &model_handle) != ESP_OK) {
        halt("cannot map the model partition");
    }

    lisa_rec_params_t rp;
    lisa_rec_default_params(&rp);
    rp.min_speech_db = (float)CONFIG_LISA_VAD_START_DB;

    if (!lisa_rec_init(&g_rec, model, part->size, CONFIG_LISA_WDT_PATH,
                       MAX_SAMPLES, &rp)) {
        halt("recognizer init failed (model or dictionary)");
    }

    int stories = count_stories();

    if (stories == 0) {
        ESP_LOGW(TAG, "no stories found in " LISA_SD_MOUNT_POINT "/stories");
    }

    ESP_LOGI(TAG, "%d stories", stories);

    lisa_ctrl_init(&g_ctrl, stories, now_ms());

    float* vad_buf = (float*)psram_alloc(MAX_SAMPLES * sizeof(float));

    g_job_buf = (float*)psram_alloc(MAX_SAMPLES * sizeof(float));

    if (!vad_buf || !g_job_buf) {
        halt("out of memory for audio buffers");
    }

    lisa_vad_params_t vp;
    lisa_vad_default_params(&vp);
    vp.start_db = (float)CONFIG_LISA_VAD_START_DB;
    vp.abs_min_rms = (float)CONFIG_LISA_VAD_ABS_MIN_RMS_MILLI / 1000.0f;
    vp.hangover_ms = CONFIG_LISA_VAD_HANGOVER_MS;
    vp.preroll_ms = CONFIG_LISA_VAD_PREROLL_MS;

    lisa_vad_init(&g_vad, &vp, vad_buf, MAX_SAMPLES);

    g_job_sem = xSemaphoreCreateBinary();
    g_cmd_q = xQueueCreate(4, sizeof(cmd_msg_t));

    if (!g_job_sem || !g_cmd_q) {
        halt("cannot create RTOS objects");
    }

    /* Initialize AEC after the recognizer and application buffers have taken
     * their required memory, but before the microphone task starts. */
    (void)aec_init();

    /* ctrl on core 1 (steady audio), mic + recognizer on core 0.
     *
     * rec_task runs nd_whistle_encode() on its own stack: enc_block() alone
     * has 20,480 B of locals (float tmp[512], dw[9*512], nd_whistle.c) and,
     * with the serial nd_parallel_rows, mlp_rows (~10 KB) and the log-mel
     * (~5 KB) run on the same stack. 16 KB overflowed; the upstream demo
     * gives the equivalent task 48 KB. The stack comes from internal RAM. */
    if (xTaskCreatePinnedToCore(ctrl_task, "lisa_ctrl", 6144, NULL, 6, NULL, 1) != pdPASS ||
        xTaskCreatePinnedToCore(mic_task,  "lisa_mic",  4096, NULL, 5, NULL, 0) != pdPASS ||
        xTaskCreatePinnedToCore(rec_task,  "lisa_rec", 48 * 1024, NULL, 3, NULL, 0) != pdPASS) {
        halt("cannot create the tasks (out of internal RAM?)");
    }

    {
        uint32_t unused = 0;

        log_health("boot", 0, &unused);
    }

    ESP_LOGI(TAG, "ready - say the wake phrase");
}
