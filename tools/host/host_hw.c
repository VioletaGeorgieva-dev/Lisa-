/* Host stand-in for lisa_hardware.c: the "speaker" just records what was
 * written, so tests can check that playback is complete and gap-free. */
#include "lisa_hardware.h"
#include <string.h>

uint32_t host_hw_written[65536];     /* count of samples written, by value */
uint32_t host_hw_zero_samples;
uint32_t host_hw_total_calls;

void host_hw_reset(void) { memset(host_hw_written, 0, sizeof(host_hw_written)); host_hw_zero_samples = 0; host_hw_total_calls = 0; }

bool lisa_hw_mount_sdcard(void) { return true; }
bool lisa_hw_init_mic_i2s(void) { return true; }
bool lisa_hw_init_speaker_i2s(void) { return true; }
bool lisa_hw_read_mic(int32_t* d, size_t n, size_t* r, uint32_t t) { (void)d; (void)n; (void)t; *r = 0; return false; }

bool lisa_hw_write_speaker(const int16_t* src, size_t n, size_t* w, uint32_t t)
{
    (void)t;
    host_hw_total_calls++;
    for (size_t i = 0; i < n; i++) {
        if (src[i] == 0) host_hw_zero_samples++;
        else host_hw_written[(uint16_t)src[i]]++;
    }
    *w = n;
    return true;
}
