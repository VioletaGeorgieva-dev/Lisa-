#pragma once

#include <stdint.h>
#include "esp_aec.h"

#ifdef __cplusplus
extern "C" {
#endif

aec_handle_t *lisa_aec_create(void);
int lisa_aec_get_chunksize(const aec_handle_t *handle);
void lisa_aec_process(const aec_handle_t *handle,
                      int16_t *mic,
                      int16_t *ref,
                      int16_t *out);
void lisa_aec_destroy(aec_handle_t *handle);

#ifdef __cplusplus
}
#endif
