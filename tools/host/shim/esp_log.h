#pragma once
#include <stdio.h>
#define ESP_LOGI(t, f, ...) printf("%s " f "\n", t, ##__VA_ARGS__)
#define ESP_LOGW(t, f, ...) printf("%s " f "\n", t, ##__VA_ARGS__)
#define ESP_LOGE(t, f, ...) printf("%s " f "\n", t, ##__VA_ARGS__)
