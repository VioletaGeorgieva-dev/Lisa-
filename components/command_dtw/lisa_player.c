
#include "lisa_player.h"
#include "lisa_hardware.h"

#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <limits.h>

#define LISA_AUDIO_CHUNK_SAMPLES 256
#define LISA_SPEAKER_TIMEOUT_MS 50

static char s_base_dir[64] = LISA_SD_MOUNT_POINT;
static char s_current_path[128] = {0};

static bool s_is_playing = false;
static bool s_has_finished = false;

static FILE* s_story_file = NULL;

static uint32_t s_data_offset = 0;
static uint32_t s_data_size = 0;
static uint32_t s_data_remaining = 0;
static uint32_t s_current_position_bytes = 0;

static uint16_t read_le16(const uint8_t* p)
{
    return (uint16_t)p[0] | ((uint16_t)p[1] << 8);
}

static uint32_t read_le32(const uint8_t* p)
{
    return (uint32_t)p[0]
        | ((uint32_t)p[1] << 8)
        | ((uint32_t)p[2] << 16)
        | ((uint32_t)p[3] << 24);
}

static void close_story_file(void)
{
    if (s_story_file != NULL) {
        fclose(s_story_file);
        s_story_file = NULL;
    }
}

/*
 * Отваря WAV файл и проверява:
 * PCM, mono, 16-bit, 22050 Hz.
 *
 * start_position_bytes е позиция спрямо началото
 * на аудио данните, а не спрямо началото на файла.
 */
static bool open_wav_file(
    const char* filepath,
    uint32_t start_position_bytes)
{
    uint8_t riff[12];
    uint8_t chunk_header[8];
    uint8_t fmt[16];

    bool found_fmt = false;
    bool found_data = false;

    uint16_t audio_format = 0;
    uint16_t channels = 0;
    uint16_t bits_per_sample = 0;
    uint32_t sample_rate = 0;

    uint32_t data_offset = 0;
    uint32_t data_size = 0;

    FILE* f = fopen(filepath, "rb");

    if (!f) {
        return false;
    }

    if (fread(riff, 1, sizeof(riff), f) != sizeof(riff)) {
        fclose(f);
        return false;
    }

    if (memcmp(riff, "RIFF", 4) != 0 ||
        memcmp(riff + 8, "WAVE", 4) != 0) {
        fclose(f);
        return false;
    }

    while (fread(chunk_header, 1, 8, f) == 8) {

        uint32_t chunk_size = read_le32(chunk_header + 4);

        long chunk_data_position = ftell(f);

        if (chunk_data_position < 0 ||
            (uint64_t)chunk_data_position > UINT32_MAX) {
            fclose(f);
            return false;
        }

        if (memcmp(chunk_header, "fmt ", 4) == 0) {

            if (chunk_size < 16 ||
                fread(fmt, 1, 16, f) != 16) {
                fclose(f);
                return false;
            }

            audio_format = read_le16(fmt);
            channels = read_le16(fmt + 2);
            sample_rate = read_le32(fmt + 4);
            bits_per_sample = read_le16(fmt + 14);

            found_fmt = true;

        } else if (memcmp(chunk_header, "data", 4) == 0) {

            data_offset = (uint32_t)chunk_data_position;
            data_size = chunk_size;

            found_data = true;
        }

        uint64_t next_position =
            (uint64_t)chunk_data_position +
            chunk_size +
            (chunk_size & 1U);

        if (next_position > LONG_MAX ||
            fseek(f, (long)next_position, SEEK_SET) != 0) {
            fclose(f);
            return false;
        }

        if (found_fmt && found_data) {
            break;
        }
    }

    if (!found_fmt || !found_data ||
        audio_format != 1 ||
        channels != 1 ||
        sample_rate != LISA_SPK_SAMPLE_RATE ||
        bits_per_sample != 16 ||
        (data_size % sizeof(int16_t)) != 0 ||
        start_position_bytes > data_size ||
        (start_position_bytes % sizeof(int16_t)) != 0) {

        fclose(f);
        return false;
    }

    uint64_t absolute_position =
        (uint64_t)data_offset + start_position_bytes;

    if (absolute_position > LONG_MAX ||
        fseek(f, (long)absolute_position, SEEK_SET) != 0) {
        fclose(f);
        return false;
    }

    close_story_file();

    s_story_file = f;
    s_data_offset = data_offset;
    s_data_size = data_size;
    s_data_remaining = data_size - start_position_bytes;
    s_current_position_bytes = start_position_bytes;

    s_is_playing = true;
    s_has_finished = false;

    return true;
}

void lisa_player_set_base_dir(const char* base_dir)
{
    if (!base_dir || base_dir[0] == '\0') {
        return;
    }

    size_t len = strlen(base_dir);

    if (len >= sizeof(s_base_dir)) {
        return;
    }

    memcpy(s_base_dir, base_dir, len + 1);
}

bool lisa_player_start_story(
    int story_idx,
    uint32_t start_position_bytes)
{
    if (story_idx < 0 || story_idx > 999) {
        return false;
    }

    char filepath[128];

    int len = snprintf(
        filepath,
        sizeof(filepath),
        "%s/stories/%03d.wav",
        s_base_dir,
        story_idx + 1
    );

    if (len < 0 || (size_t)len >= sizeof(filepath)) {
        return false;
    }

    close_story_file();

    s_is_playing = false;
    s_has_finished = false;
    s_current_position_bytes = 0;

    if (!open_wav_file(filepath, start_position_bytes)) {
        return false;
    }

    memcpy(s_current_path, filepath, (size_t)len + 1);

    return true;
}

bool lisa_player_start_question(
    int story_idx,
    uint32_t start_position_bytes)
{
    if (story_idx < 0 || story_idx > 999) {
        return false;
    }

    char filepath[128];

    int len = snprintf(
        filepath,
        sizeof(filepath),
        "%s/%03d_question.wav",
        s_base_dir,
        story_idx + 1
    );

    if (len < 0 || (size_t)len >= sizeof(filepath)) {
        return false;
    }

    close_story_file();

    s_is_playing = false;
    s_has_finished = false;
    s_current_position_bytes = 0;

    if (!open_wav_file(filepath, start_position_bytes)) {
        return false;
    }

    memcpy(s_current_path, filepath, (size_t)len + 1);

    return true;
}

bool lisa_player_start_prompt(uint32_t start_position_bytes)
{
    char filepath[128];

    int len = snprintf(
        filepath,
        sizeof(filepath),
        "%s/izberi.wav",
        s_base_dir
    );

    if (len < 0 || (size_t)len >= sizeof(filepath)) {
        return false;
    }

    close_story_file();

    s_is_playing = false;
    s_has_finished = false;
    s_current_position_bytes = 0;

    if (!open_wav_file(filepath, start_position_bytes)) {
        return false;
    }

    memcpy(s_current_path, filepath, (size_t)len + 1);

    return true;
}

bool lisa_player_pause(uint32_t* out_position_bytes)
{
    if (!s_is_playing || !s_story_file) {
        return false;
    }

    long position = ftell(s_story_file);

    if (position < 0 ||
        (uint64_t)position < s_data_offset) {
        return false;
    }

    uint64_t relative_position =
        (uint64_t)position - s_data_offset;

    if (relative_position > s_data_size) {
        return false;
    }

    s_current_position_bytes = (uint32_t)relative_position;

    s_is_playing = false;
    close_story_file();

    if (out_position_bytes) {
        *out_position_bytes = s_current_position_bytes;
    }

    return true;
}

bool lisa_player_resume(uint32_t saved_position_bytes)
{
    if (s_is_playing || s_current_path[0] == '\0') {
        return false;
    }

    return open_wav_file(
        s_current_path,
        saved_position_bytes
    );
}

bool lisa_player_stop(void)
{
    s_is_playing = false;
    s_has_finished = false;

    s_current_position_bytes = 0;
    s_data_remaining = 0;

    close_story_file();

    return true;
}

bool lisa_player_is_playing(void)
{
    return s_is_playing;
}

bool lisa_player_has_finished(void)
{
    return s_has_finished;
}

uint32_t lisa_player_get_position_bytes(void)
{
    if (s_story_file != NULL) {

        long position = ftell(s_story_file);

        if (position >= 0 &&
            (uint64_t)position >= s_data_offset) {

            uint64_t relative =
                (uint64_t)position - s_data_offset;

            if (relative <= s_data_size) {
                s_current_position_bytes = (uint32_t)relative;
            }
        }
    }

    return s_current_position_bytes;
}

bool lisa_player_process_audio_chunk(void)
{
    if (!s_is_playing || !s_story_file) {
        return false;
    }

    if (s_data_remaining == 0) {
        s_is_playing = false;
        s_has_finished = true;
        close_story_file();
        return false;
    }

    int16_t pcm_buffer[LISA_AUDIO_CHUNK_SAMPLES];

    size_t max_samples = LISA_AUDIO_CHUNK_SAMPLES;

    size_t available_samples =
        s_data_remaining / sizeof(int16_t);

    if (max_samples > available_samples) {
        max_samples = available_samples;
    }

    size_t samples_read = fread(
        pcm_buffer,
        sizeof(int16_t),
        max_samples,
        s_story_file
    );

    if (samples_read == 0) {
        lisa_player_stop();
        return false;
    }

    s_data_remaining -=
        (uint32_t)(samples_read * sizeof(int16_t));

    size_t total_samples_written = 0;

    while (total_samples_written < samples_read) {

        size_t samples_written = 0;

        bool write_ok = lisa_hw_write_speaker(
            &pcm_buffer[total_samples_written],
            samples_read - total_samples_written,
            &samples_written,
            LISA_SPEAKER_TIMEOUT_MS
        );

        if (samples_written >
            samples_read - total_samples_written) {
            lisa_player_stop();
            return false;
        }

        total_samples_written += samples_written;

        if (!write_ok || samples_written == 0) {
            lisa_player_stop();
            return false;
        }
    }

    s_current_position_bytes =
        s_data_size - s_data_remaining;

    if (s_data_remaining == 0) {
        s_is_playing = false;
        s_has_finished = true;
        close_story_file();
        return false;
    }

    return true;
}
