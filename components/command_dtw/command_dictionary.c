#include "command_dictionary.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

#ifdef ESP_PLATFORM
#include <esp_log.h>
#include <esp_heap_caps.h>
static const char* TAG = "wdt_dict";
#define WDT_CALLOC(n, sz) heap_caps_calloc((n), (sz), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT)
#define WDT_MALLOC(sz)    heap_caps_malloc((sz), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT)
#define WDT_FREE(ptr)     heap_caps_free(ptr)
#else
static const char* TAG = "wdt_dict";
#define ESP_LOGE(tag, fmt, ...) fprintf(stderr, "[%s] ERROR: " fmt "\n", tag, ##__VA_ARGS__)
#define ESP_LOGI(tag, fmt, ...) fprintf(stdout, "[%s] INFO: " fmt "\n", tag, ##__VA_ARGS__)
#define WDT_CALLOC(n, sz) calloc((n), (sz))
#define WDT_MALLOC(sz)    malloc(sz)
#define WDT_FREE(ptr)     free(ptr)
#endif

#pragma pack(push, 1)
typedef struct {
    char magic[4];         // "WDT1"
    uint32_t version;      // 1
    uint32_t num_classes;  // 9
    uint32_t embedding_dim;// 512
    uint32_t total_size;   // Total expected file size in bytes
} wdt_header_t;

typedef struct {
    char name[32];
    uint8_t has_threshold;
    uint8_t reserved[3];
    float threshold_distance;
    float threshold_margin;
    uint32_t num_templates;
} wdt_class_header_t;
#pragma pack(pop)

void wdt_dictionary_free(dtw_dictionary_t* dict) {
    if (!dict) return;
    if (dict->classes) {
        for (int c = 0; c < dict->num_classes; c++) {
            dtw_class_t* cls = &dict->classes[c];
            if (cls->templates) {
                for (int t = 0; t < cls->num_templates; t++) {
                    if (cls->templates[t].frames) {
                        WDT_FREE(cls->templates[t].frames);
                        cls->templates[t].frames = NULL;
                    }
                }
                WDT_FREE(cls->templates);
                cls->templates = NULL;
            }
        }
        WDT_FREE(dict->classes);
        dict->classes = NULL;
    }
    dict->num_classes = 0;
}

bool wdt_dictionary_load(const char* filepath, dtw_dictionary_t* out_dict) {
    if (!filepath || !out_dict) return false;
    memset(out_dict, 0, sizeof(dtw_dictionary_t));

    FILE* f = fopen(filepath, "rb");
    if (!f) {
        ESP_LOGE(TAG, "Cannot open dictionary file: %s", filepath);
        return false;
    }

    fseek(f, 0, SEEK_END);
    long file_size = ftell(f);
    fseek(f, 0, SEEK_SET);

    if (file_size < (long)sizeof(wdt_header_t)) {
        ESP_LOGE(TAG, "File too small: %ld bytes", file_size);
        fclose(f);
        return false;
    }

    wdt_header_t hdr;
    if (fread(&hdr, sizeof(wdt_header_t), 1, f) != 1) {
        ESP_LOGE(TAG, "Failed reading header");
        fclose(f);
        return false;
    }

    if (memcmp(hdr.magic, WDT_MAGIC, 4) != 0 ||
        hdr.version != WDT_VERSION ||
        hdr.embedding_dim != EMBEDDING_DIM ||
        hdr.num_classes == 0 ||
        hdr.num_classes > 64 ||
        (long)hdr.total_size != file_size) {
        ESP_LOGE(TAG, "Invalid WDT header or file size mismatch (expected %u, actual %ld)",
                 hdr.total_size, file_size);
        fclose(f);
        return false;
    }

    out_dict->num_classes = (int)hdr.num_classes;
    out_dict->classes = (dtw_class_t*)WDT_CALLOC(hdr.num_classes, sizeof(dtw_class_t));
    if (!out_dict->classes) {
        ESP_LOGE(TAG, "OOM allocating classes");
        fclose(f);
        return false;
    }

    for (uint32_t c = 0; c < hdr.num_classes; c++) {
        wdt_class_header_t chdr;
        if (fread(&chdr, sizeof(wdt_class_header_t), 1, f) != 1) {
            ESP_LOGE(TAG, "Failed reading class header %u", c);
            wdt_dictionary_free(out_dict);
            fclose(f);
            return false;
        }

        dtw_class_t* cls = &out_dict->classes[c];
        memcpy(cls->name, chdr.name, sizeof(cls->name));
        cls->name[sizeof(cls->name) - 1] = '\0';
        cls->has_threshold = (chdr.has_threshold != 0);
        cls->threshold_distance = chdr.threshold_distance;
        cls->threshold_margin = chdr.threshold_margin;
        cls->num_templates = (int)chdr.num_templates;

        if (chdr.num_templates == 0 || chdr.num_templates > 256) {
            ESP_LOGE(TAG, "Invalid template count %u for class %s", chdr.num_templates, cls->name);
            wdt_dictionary_free(out_dict);
            fclose(f);
            return false;
        }

        cls->templates = (dtw_template_t*)WDT_CALLOC(chdr.num_templates, sizeof(dtw_template_t));
        if (!cls->templates) {
            ESP_LOGE(TAG, "OOM allocating templates for %s", cls->name);
            wdt_dictionary_free(out_dict);
            fclose(f);
            return false;
        }

        for (uint32_t t = 0; t < chdr.num_templates; t++) {
            uint32_t num_frames = 0;
            if (fread(&num_frames, sizeof(uint32_t), 1, f) != 1 || num_frames == 0 || num_frames > 2000) {
                ESP_LOGE(TAG, "Invalid num_frames %u in %s[%u]", num_frames, cls->name, t);
                wdt_dictionary_free(out_dict);
                fclose(f);
                return false;
            }

            size_t float_count = (size_t)num_frames * EMBEDDING_DIM;
            size_t byte_count = float_count * sizeof(float);
            float* buf = (float*)WDT_MALLOC(byte_count);
            if (!buf) {
                ESP_LOGE(TAG, "OOM allocating %zu bytes for template", byte_count);
                wdt_dictionary_free(out_dict);
                fclose(f);
                return false;
            }

            if (fread(buf, 1, byte_count, f) != byte_count) {
                ESP_LOGE(TAG, "Truncated template data in %s[%u]", cls->name, t);
                WDT_FREE(buf);
                wdt_dictionary_free(out_dict);
                fclose(f);
                return false;
            }

            // Validate finite values and L2 normalize
            for (size_t i = 0; i < float_count; i++) {
                if (!isfinite(buf[i])) {
                    ESP_LOGE(TAG, "NaN/Inf detected in template %s[%u]", cls->name, t);
                    WDT_FREE(buf);
                    wdt_dictionary_free(out_dict);
                    fclose(f);
                    return false;
                }
            }
            for (uint32_t fr = 0; fr < num_frames; fr++) {
                dtw_normalize_frame(&buf[fr * EMBEDDING_DIM], EMBEDDING_DIM);
            }

            cls->templates[t].num_frames = (int)num_frames;
            cls->templates[t].frames = buf;
        }
    }

    long end_pos = ftell(f);
    fclose(f);
    if (end_pos != file_size) {
        ESP_LOGE(TAG, "Extra trailing bytes in dictionary (%ld != %ld)", end_pos, file_size);
        wdt_dictionary_free(out_dict);
        return false;
    }

    ESP_LOGI(TAG, "Loaded WDT dictionary (%d classes, %ld bytes)", out_dict->num_classes, file_size);
    return true;
}
