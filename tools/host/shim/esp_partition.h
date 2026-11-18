#pragma once
#include <stdint.h>
#include <stddef.h>
typedef int esp_err_t;
#define ESP_OK 0
typedef enum { ESP_PARTITION_TYPE_APP = 0, ESP_PARTITION_TYPE_DATA = 1 } esp_partition_type_t;
typedef int esp_partition_subtype_t;
typedef enum { ESP_PARTITION_MMAP_DATA, ESP_PARTITION_MMAP_INST } esp_partition_mmap_memory_t;
typedef uint32_t esp_partition_mmap_handle_t;
typedef struct { uint32_t size; uint32_t address; } esp_partition_t;
const esp_partition_t* esp_partition_find_first(esp_partition_type_t t, esp_partition_subtype_t s, const char* label);
esp_err_t esp_partition_mmap(const esp_partition_t* p, size_t off, size_t size, esp_partition_mmap_memory_t mem, const void** out, esp_partition_mmap_handle_t* h);
