#ifndef COMMAND_DICTIONARY_H
#define COMMAND_DICTIONARY_H

#include "nd_command_dtw.h"
#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define WDT_MAGIC "WDT1"
#define WDT_VERSION 1

// Loads binary dictionary from SD card / filesystem into PSRAM/RAM.
// Returns true on success; on failure frees all allocated memory and returns false.
bool wdt_dictionary_load(const char* filepath, dtw_dictionary_t* out_dict);

// Frees all allocated memory inside dtw_dictionary_t.
void wdt_dictionary_free(dtw_dictionary_t* dict);

#ifdef __cplusplus
}
#endif

#endif // COMMAND_DICTIONARY_H
