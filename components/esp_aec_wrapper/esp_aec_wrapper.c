#include "esp_heap_caps.h"
#include <string.h>
#include "esp_aec_wrapper.h"

aec_handle_t *lisa_aec_create(void)
{
    aec_config_t config = {0};

    config.mic_num = 1;
    config.ref_num = 1;
    config.out_num = 1;
    config.filter_length = 4;
    config.sample_rate = 16000;
    config.caps = MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT;
    config.mode = AEC_MODE_FD_LOW_COST;
    config.nlp_level = AEC_NLP_LEVEL_NORMAL;

    return aec_create_from_config(&config);
}

int lisa_aec_get_chunksize(const aec_handle_t *handle)
{
    return handle ? aec_get_chunksize(handle) : 0;
}

void lisa_aec_process(const aec_handle_t *handle,
                      int16_t *mic,
                      int16_t *ref,
                      int16_t *out)
{
    if (handle && mic && ref && out) {
        aec_process(handle, mic, ref, out);
    }
}

void lisa_aec_destroy(aec_handle_t *handle)
{
    if (handle) {
        aec_destroy(handle);
    }
}
