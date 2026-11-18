#ifndef LISA_HARDWARE_H
#define LISA_HARDWARE_H

#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#define LISA_MIC_BCLK_GPIO          42
#define LISA_MIC_WS_GPIO             2
#define LISA_MIC_DATA_GPIO          41
#define LISA_MIC_SAMPLE_RATE    16000
#define LISA_MIC_DMA_FRAME_SAMPLES 320

#define LISA_SD_CMD_GPIO            38
#define LISA_SD_CLK_GPIO            39
#define LISA_SD_D0_GPIO             40
#define LISA_SD_MOUNT_POINT "/sdcard"

#define LISA_SPK_BCLK_GPIO            5
#define LISA_SPK_WS_GPIO             6
#define LISA_SPK_DATA_GPIO            4
#define LISA_SPK_SAMPLE_RATE     22050

bool lisa_hw_mount_sdcard(void);
bool lisa_hw_init_mic_i2s(void);
bool lisa_hw_init_speaker_i2s(void);

bool lisa_hw_read_mic(
    int32_t *dest_buffer,
    size_t samples_to_read,
    size_t *samples_read,
    uint32_t timeout_ms);

bool lisa_hw_read_mic_timed(
    int32_t *dest_buffer,
    size_t samples_to_read,
    size_t *samples_read,
    int64_t *block_end_time_us,
    uint32_t timeout_ms);

bool lisa_hw_write_speaker(
    const int16_t *src_buffer,
    size_t samples_to_write,
    size_t *samples_written,
    uint32_t timeout_ms);

size_t lisa_hw_read_aec_ref(int16_t *dest, size_t max_samples);

bool lisa_hw_read_aec_ref_block(
    int16_t *dest,
    size_t max_samples,
    size_t *samples_read,
    int64_t *block_end_time_us);

typedef struct {
    uint32_t rx_app_queue_drops;
    uint32_t rx_driver_queue_overflows;
    uint32_t tx_app_queue_drops;
    uint32_t tx_driver_queue_overflows;
    uint32_t rx_queue_depth;
    uint32_t tx_queue_depth;
} lisa_hw_audio_stats_t;

void lisa_hw_get_audio_stats(lisa_hw_audio_stats_t *stats);

#ifdef __cplusplus
}
#endif

#endif
