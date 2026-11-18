#include "lisa_hardware.h"

#include <string.h>

#ifdef ESP_PLATFORM
#include "esp_log.h"
#include "esp_attr.h"
#include "esp_timer.h"
#include "esp_vfs_fat.h"
#include "driver/sdmmc_host.h"
#include "driver/i2s_std.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"

static const char *TAG = "lisa_hw";

#define LISA_RX_QUEUE_BLOCKS       8U
#define LISA_TX_QUEUE_BLOCKS       8U
#define LISA_TX_DMA_FRAME_SAMPLES  240U
#define LISA_TX_BLOCK_MAX_SAMPLES  512U
#define LISA_RX_BLOCK_BYTES \
    (LISA_MIC_DMA_FRAME_SAMPLES * sizeof(int32_t))
#define LISA_RX_BLOCK_DURATION_US \
    (((int64_t)LISA_MIC_DMA_FRAME_SAMPLES * 1000000LL) / LISA_MIC_SAMPLE_RATE)

typedef struct {
    int32_t samples[LISA_MIC_DMA_FRAME_SAMPLES];
    int64_t end_time_us;
    uint32_t sequence;
} lisa_rx_block_t;

typedef struct {
    int16_t samples[LISA_TX_BLOCK_MAX_SAMPLES];
    size_t count;
    int64_t end_time_us;
} lisa_tx_block_t;

static i2s_chan_handle_t s_rx_chan;
static i2s_chan_handle_t s_tx_chan;
static QueueHandle_t s_rx_queue;
static QueueHandle_t s_tx_queue;

/* Static ISR staging objects avoid putting >1 KB audio blocks on the
 * interrupt stack. RX and TX have separate staging storage. */
static lisa_rx_block_t s_rx_isr_block;
static lisa_tx_block_t s_tx_isr_block;

/* RX producer runs in the I2S ISR; the mic task is the sole consumer. */
static volatile uint32_t s_rx_next_sequence;
static uint32_t s_rx_expected_sequence;
static bool s_rx_expected_valid;
static int64_t s_rx_last_delivered_end_us;
static bool s_rx_pending_valid;
static lisa_rx_block_t s_rx_pending;

/* TX consumer is the AEC/mic task. The legacy sample reader shares this
 * pending block and must not be used concurrently with the block reader. */
static bool s_tx_pending_valid;
static size_t s_tx_pending_offset;
static lisa_tx_block_t s_tx_pending;

static portMUX_TYPE s_stats_mux = portMUX_INITIALIZER_UNLOCKED;
static volatile uint32_t s_rx_app_queue_drops;
static volatile uint32_t s_rx_driver_queue_overflows;
static volatile uint32_t s_tx_app_queue_drops;
static volatile uint32_t s_tx_driver_queue_overflows;

static bool IRAM_ATTR lisa_i2s_on_recv(
    i2s_chan_handle_t handle,
    i2s_event_data_t *event,
    void *user_ctx)
{
    (void)handle;
    (void)user_ctx;

    if (!event || !event->dma_buf || !s_rx_queue ||
        event->size != LISA_RX_BLOCK_BYTES) {
        s_rx_next_sequence++;
        portENTER_CRITICAL_ISR(&s_stats_mux);
        s_rx_app_queue_drops++;
        portEXIT_CRITICAL_ISR(&s_stats_mux);
        return false;
    }

    s_rx_isr_block.end_time_us = esp_timer_get_time();
    memcpy(s_rx_isr_block.samples, event->dma_buf, LISA_RX_BLOCK_BYTES);
    s_rx_isr_block.sequence = s_rx_next_sequence++;

    BaseType_t higher_priority_task_woken = pdFALSE;
    if (xQueueSendFromISR(s_rx_queue, &s_rx_isr_block,
                          &higher_priority_task_woken) != pdTRUE) {
        portENTER_CRITICAL_ISR(&s_stats_mux);
        s_rx_app_queue_drops++;
        portEXIT_CRITICAL_ISR(&s_stats_mux);
    }

    /* The I2S driver consumes this return value and performs the ISR yield
     * after its own DMA bookkeeping is complete. */
    return higher_priority_task_woken == pdTRUE;
}

static bool IRAM_ATTR lisa_i2s_on_recv_q_ovf(
    i2s_chan_handle_t handle,
    i2s_event_data_t *event,
    void *user_ctx)
{
    (void)handle;
    (void)user_ctx;

    (void)event;

    /* This is the driver's i2s_channel_read descriptor queue, not the
     * callback-owned audio queue. The on_recv callback has already copied
     * the DMA samples, so this event must not advance the audio sequence. */
    portENTER_CRITICAL_ISR(&s_stats_mux);
    s_rx_driver_queue_overflows++;
    portEXIT_CRITICAL_ISR(&s_stats_mux);
    return false;
}

static bool IRAM_ATTR lisa_i2s_on_sent(
    i2s_chan_handle_t handle,
    i2s_event_data_t *event,
    void *user_ctx)
{
    (void)handle;
    (void)user_ctx;

    if (!event || !event->dma_buf || !s_tx_queue ||
        event->size == 0U || (event->size % sizeof(int16_t)) != 0U) {
        return false;
    }

    size_t samples = event->size / sizeof(int16_t);
    if (samples > LISA_TX_BLOCK_MAX_SAMPLES) {
        portENTER_CRITICAL_ISR(&s_stats_mux);
        s_tx_app_queue_drops++;
        portEXIT_CRITICAL_ISR(&s_stats_mux);
        return false;
    }

    s_tx_isr_block.end_time_us = esp_timer_get_time();
    memcpy(s_tx_isr_block.samples, event->dma_buf, event->size);
    s_tx_isr_block.count = samples;

    BaseType_t higher_priority_task_woken = pdFALSE;
    if (xQueueSendFromISR(s_tx_queue, &s_tx_isr_block,
                          &higher_priority_task_woken) != pdTRUE) {
        portENTER_CRITICAL_ISR(&s_stats_mux);
        s_tx_app_queue_drops++;
        portEXIT_CRITICAL_ISR(&s_stats_mux);
    }

    return higher_priority_task_woken == pdTRUE;
}

static bool IRAM_ATTR lisa_i2s_on_send_q_ovf(
    i2s_chan_handle_t handle,
    i2s_event_data_t *event,
    void *user_ctx)
{
    (void)handle;
    (void)event;
    (void)user_ctx;

    portENTER_CRITICAL_ISR(&s_stats_mux);
    s_tx_driver_queue_overflows++;
    portEXIT_CRITICAL_ISR(&s_stats_mux);
    return false;
}

bool lisa_hw_mount_sdcard(void)
{
    esp_vfs_fat_sdmmc_mount_config_t mount_config = {
        .format_if_mount_failed = false,
        .max_files = 5,
        .allocation_unit_size = 16 * 1024
    };
    sdmmc_card_t *card = NULL;
    sdmmc_host_t host = SDMMC_HOST_DEFAULT();
    sdmmc_slot_config_t slot_config = SDMMC_SLOT_CONFIG_DEFAULT();
    slot_config.width = 1;
    slot_config.cmd = LISA_SD_CMD_GPIO;
    slot_config.clk = LISA_SD_CLK_GPIO;
    slot_config.d0 = LISA_SD_D0_GPIO;
    slot_config.flags |= SDMMC_SLOT_FLAG_INTERNAL_PULLUP;

    esp_err_t ret = esp_vfs_fat_sdmmc_mount(
        LISA_SD_MOUNT_POINT, &host, &slot_config, &mount_config, &card);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "SD mount failed: %s", esp_err_to_name(ret));
        return false;
    }
    return true;
}

bool lisa_hw_init_mic_i2s(void)
{
    if (s_rx_chan) {
        return true;
    }

    if (!s_rx_queue) {
        s_rx_queue = xQueueCreate(LISA_RX_QUEUE_BLOCKS, sizeof(lisa_rx_block_t));
        if (!s_rx_queue) {
            ESP_LOGE(TAG, "cannot allocate microphone DMA block queue");
            return false;
        }
    }

    i2s_chan_config_t chan_cfg =
        I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER);
    chan_cfg.dma_frame_num = LISA_MIC_DMA_FRAME_SAMPLES;

    esp_err_t ret = i2s_new_channel(&chan_cfg, NULL, &s_rx_chan);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "microphone I2S channel create failed: %s",
                 esp_err_to_name(ret));
        return false;
    }

    i2s_std_config_t std_cfg = {
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(LISA_MIC_SAMPLE_RATE),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(
            I2S_DATA_BIT_WIDTH_32BIT, I2S_SLOT_MODE_MONO),
        .gpio_cfg = {
            .mclk = I2S_GPIO_UNUSED,
            .bclk = LISA_MIC_BCLK_GPIO,
            .ws = LISA_MIC_WS_GPIO,
            .dout = I2S_GPIO_UNUSED,
            .din = LISA_MIC_DATA_GPIO,
        },
    };
    std_cfg.slot_cfg.slot_mask = I2S_STD_SLOT_LEFT;

    ret = i2s_channel_init_std_mode(s_rx_chan, &std_cfg);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "microphone I2S standard mode failed: %s",
                 esp_err_to_name(ret));
        i2s_del_channel(s_rx_chan);
        s_rx_chan = NULL;
        return false;
    }

    const i2s_event_callbacks_t callbacks = {
        .on_recv = lisa_i2s_on_recv,
        .on_recv_q_ovf = lisa_i2s_on_recv_q_ovf,
        .on_sent = NULL,
        .on_send_q_ovf = NULL,
    };
    ret = i2s_channel_register_event_callback(s_rx_chan, &callbacks, NULL);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "microphone I2S callback registration failed: %s",
                 esp_err_to_name(ret));
        i2s_del_channel(s_rx_chan);
        s_rx_chan = NULL;
        return false;
    }

    s_rx_next_sequence = 0;
    s_rx_expected_sequence = 0;
    s_rx_expected_valid = false;
    s_rx_pending_valid = false;
    s_rx_last_delivered_end_us = 0;
    xQueueReset(s_rx_queue);

    ret = i2s_channel_enable(s_rx_chan);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "microphone I2S enable failed: %s", esp_err_to_name(ret));
        i2s_del_channel(s_rx_chan);
        s_rx_chan = NULL;
        return false;
    }

    ESP_LOGI(TAG, "microphone I2S ready: %d Hz, %u-sample DMA blocks",
             LISA_MIC_SAMPLE_RATE, (unsigned)LISA_MIC_DMA_FRAME_SAMPLES);
    return true;
}

bool lisa_hw_init_speaker_i2s(void)
{
    if (s_tx_chan) {
        return true;
    }

    if (!s_tx_queue) {
        s_tx_queue = xQueueCreate(LISA_TX_QUEUE_BLOCKS, sizeof(lisa_tx_block_t));
        if (!s_tx_queue) {
            ESP_LOGE(TAG, "cannot allocate speaker reference queue");
            return false;
        }
    }

    i2s_chan_config_t chan_cfg =
        I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_1, I2S_ROLE_MASTER);
    chan_cfg.dma_frame_num = LISA_TX_DMA_FRAME_SAMPLES;

    esp_err_t ret = i2s_new_channel(&chan_cfg, &s_tx_chan, NULL);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "speaker I2S channel create failed: %s",
                 esp_err_to_name(ret));
        return false;
    }

    i2s_std_config_t std_cfg = {
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(LISA_SPK_SAMPLE_RATE),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(
            I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_MONO),
        .gpio_cfg = {
            .mclk = I2S_GPIO_UNUSED,
            .bclk = LISA_SPK_BCLK_GPIO,
            .ws = LISA_SPK_WS_GPIO,
            .dout = LISA_SPK_DATA_GPIO,
            .din = I2S_GPIO_UNUSED,
        },
    };

    ret = i2s_channel_init_std_mode(s_tx_chan, &std_cfg);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "speaker I2S standard mode failed: %s",
                 esp_err_to_name(ret));
        i2s_del_channel(s_tx_chan);
        s_tx_chan = NULL;
        return false;
    }

    const i2s_event_callbacks_t callbacks = {
        .on_recv = NULL,
        .on_recv_q_ovf = NULL,
        .on_sent = lisa_i2s_on_sent,
        .on_send_q_ovf = lisa_i2s_on_send_q_ovf,
    };
    ret = i2s_channel_register_event_callback(s_tx_chan, &callbacks, NULL);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "speaker I2S callback registration failed: %s",
                 esp_err_to_name(ret));
        i2s_del_channel(s_tx_chan);
        s_tx_chan = NULL;
        return false;
    }

    xQueueReset(s_tx_queue);
    s_tx_pending_valid = false;
    s_tx_pending_offset = 0;

    ret = i2s_channel_enable(s_tx_chan);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "speaker I2S enable failed: %s", esp_err_to_name(ret));
        i2s_del_channel(s_tx_chan);
        s_tx_chan = NULL;
        return false;
    }

    ESP_LOGI(TAG, "speaker I2S ready: %d Hz, %u-sample DMA blocks",
             LISA_SPK_SAMPLE_RATE, (unsigned)LISA_TX_DMA_FRAME_SAMPLES);
    return true;
}

static void lisa_hw_drain_i2s_rx_descriptor_queue(void)
{
    /* The I2S driver keeps its own DMA-descriptor queue for
     * i2s_channel_read(). We copy the actual audio in on_recv, but still have
     * to consume this descriptor queue so the driver does not report an
     * overflow merely because this application uses callback-captured data. */
    static int32_t discard[LISA_MIC_DMA_FRAME_SAMPLES];

    if (!s_rx_chan) {
        return;
    }

    for (;;) {
        size_t bytes_read = 0;
        esp_err_t ret = i2s_channel_read(
            s_rx_chan, discard, sizeof(discard), &bytes_read, 0);
        if (ret != ESP_OK || bytes_read == 0U) {
            break;
        }
    }
}

static bool lisa_hw_get_next_rx_block(uint32_t timeout_ms)
{
    if (s_rx_pending_valid) {
        return true;
    }
    if (!s_rx_queue) {
        return false;
    }

    if (xQueueReceive(s_rx_queue, &s_rx_pending,
                      pdMS_TO_TICKS(timeout_ms)) != pdTRUE) {
        return false;
    }
    s_rx_pending_valid = true;
    return true;
}

bool lisa_hw_read_mic_timed(
    int32_t *dest_buffer,
    size_t samples_to_read,
    size_t *samples_read,
    int64_t *block_end_time_us,
    uint32_t timeout_ms)
{
    if (!s_rx_chan || !dest_buffer || !samples_read ||
        samples_to_read != LISA_MIC_DMA_FRAME_SAMPLES) {
        return false;
    }
    *samples_read = 0;

    /* Keep the driver's own descriptor queue drained independently of the
     * callback-owned audio queue, including while synthesizing lost blocks. */
    lisa_hw_drain_i2s_rx_descriptor_queue();

    for (;;) {
        if (!lisa_hw_get_next_rx_block(timeout_ms)) {
            return false;
        }

        if (!s_rx_expected_valid) {
            /* No earlier delivered audio exists to reconstruct before this block. */
            s_rx_expected_sequence = s_rx_pending.sequence;
            s_rx_expected_valid = true;
        }

        int32_t sequence_delta =
            (int32_t)(s_rx_pending.sequence - s_rx_expected_sequence);

        if (sequence_delta > 0) {
            /* Keep the audio timeline continuous after a dropped DMA block. A
             * synthetic zero block is emitted for each missing sequence number;
             * the real queued block remains pending for the next read. */
            memset(dest_buffer, 0, LISA_RX_BLOCK_BYTES);
            s_rx_last_delivered_end_us += LISA_RX_BLOCK_DURATION_US;
            if (block_end_time_us) {
                *block_end_time_us = s_rx_last_delivered_end_us;
            }
            s_rx_expected_sequence++;
            *samples_read = LISA_MIC_DMA_FRAME_SAMPLES;
            return true;
        }

        if (sequence_delta < 0) {
            /* Discard stale callback data rather than pairing it with a newer
             * timestamp. Continue until the expected sequence is reached. */
            s_rx_pending_valid = false;
            continue;
        }

        memcpy(dest_buffer, s_rx_pending.samples, LISA_RX_BLOCK_BYTES);
        s_rx_last_delivered_end_us = s_rx_pending.end_time_us;
        s_rx_expected_sequence++;
        if (block_end_time_us) {
            *block_end_time_us = s_rx_pending.end_time_us;
        }
        s_rx_pending_valid = false;
        *samples_read = LISA_MIC_DMA_FRAME_SAMPLES;
        return true;
    }
}

bool lisa_hw_read_mic(
    int32_t *dest_buffer,
    size_t samples_to_read,
    size_t *samples_read,
    uint32_t timeout_ms)
{
    int64_t ignored_end_time_us = 0;
    return lisa_hw_read_mic_timed(dest_buffer, samples_to_read, samples_read,
                                  &ignored_end_time_us, timeout_ms);
}

bool lisa_hw_write_speaker(
    const int16_t *src_buffer,
    size_t samples_to_write,
    size_t *samples_written,
    uint32_t timeout_ms)
{
    if (!s_tx_chan || !src_buffer || !samples_written) {
        return false;
    }

    size_t bytes_written = 0;
    esp_err_t ret = i2s_channel_write(
        s_tx_chan, src_buffer, samples_to_write * sizeof(int16_t),
        &bytes_written, pdMS_TO_TICKS(timeout_ms));
    *samples_written = bytes_written / sizeof(int16_t);
    return ret == ESP_OK;
}

static bool lisa_hw_get_next_tx_block(uint32_t timeout_ms)
{
    if (s_tx_pending_valid) {
        return true;
    }
    if (!s_tx_queue ||
        xQueueReceive(s_tx_queue, &s_tx_pending,
                      pdMS_TO_TICKS(timeout_ms)) != pdTRUE) {
        return false;
    }
    s_tx_pending_valid = true;
    s_tx_pending_offset = 0;
    return true;
}

bool lisa_hw_read_aec_ref_block(
    int16_t *dest,
    size_t max_samples,
    size_t *samples_read,
    int64_t *block_end_time_us)
{
    if (!dest || !samples_read || max_samples == 0U ||
        !lisa_hw_get_next_tx_block(0)) {
        return false;
    }

    size_t remaining = s_tx_pending.count - s_tx_pending_offset;
    if (remaining > max_samples) {
        return false;
    }

    memcpy(dest, s_tx_pending.samples + s_tx_pending_offset,
           remaining * sizeof(int16_t));
    *samples_read = remaining;
    if (block_end_time_us) {
        *block_end_time_us = s_tx_pending.end_time_us;
    }

    s_tx_pending_valid = false;
    s_tx_pending_offset = 0;
    return true;
}

size_t lisa_hw_read_aec_ref(int16_t *dest, size_t max_samples)
{
    if (!dest || max_samples == 0U || !lisa_hw_get_next_tx_block(0)) {
        return 0U;
    }

    size_t remaining = s_tx_pending.count - s_tx_pending_offset;
    size_t count = remaining < max_samples ? remaining : max_samples;
    memcpy(dest, s_tx_pending.samples + s_tx_pending_offset,
           count * sizeof(int16_t));
    s_tx_pending_offset += count;

    if (s_tx_pending_offset >= s_tx_pending.count) {
        s_tx_pending_valid = false;
        s_tx_pending_offset = 0;
    }
    return count;
}

void lisa_hw_get_audio_stats(lisa_hw_audio_stats_t *stats)
{
    if (!stats) {
        return;
    }

    portENTER_CRITICAL(&s_stats_mux);
    stats->rx_app_queue_drops = s_rx_app_queue_drops;
    stats->rx_driver_queue_overflows = s_rx_driver_queue_overflows;
    stats->tx_app_queue_drops = s_tx_app_queue_drops;
    stats->tx_driver_queue_overflows = s_tx_driver_queue_overflows;
    portEXIT_CRITICAL(&s_stats_mux);

    stats->rx_queue_depth = s_rx_queue ? (uint32_t)uxQueueMessagesWaiting(s_rx_queue) : 0U;
    stats->tx_queue_depth = s_tx_queue ? (uint32_t)uxQueueMessagesWaiting(s_tx_queue) : 0U;
}

#else
/* Host builds intentionally provide no ESP32 hardware implementation. */
#endif
