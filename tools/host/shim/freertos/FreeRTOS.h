#pragma once
#include <stdint.h>
#include <stddef.h>
typedef uint32_t TickType_t; typedef int BaseType_t; typedef uint32_t UBaseType_t;
#define pdTRUE 1
#define pdFALSE 0
#define portMAX_DELAY 0xffffffffu
#define pdMS_TO_TICKS(ms) ((TickType_t)(ms))
/* Minimal portMUX stand-in for host syntax checks only. */
typedef int portMUX_TYPE;
#define portMUX_INITIALIZER_UNLOCKED 0
#define portENTER_CRITICAL(mux) ((void)(mux))
#define portEXIT_CRITICAL(mux) ((void)(mux))
