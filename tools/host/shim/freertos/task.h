#pragma once
#include "FreeRTOS.h"
typedef void* TaskHandle_t; typedef void (*TaskFunction_t)(void*);
void vTaskDelay(TickType_t t);
BaseType_t xTaskCreatePinnedToCore(TaskFunction_t f, const char* n, uint32_t stack, void* a, UBaseType_t prio, TaskHandle_t* h, BaseType_t core);
UBaseType_t uxTaskGetStackHighWaterMark(TaskHandle_t t);
#define pdPASS 1
