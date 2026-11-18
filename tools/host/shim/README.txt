Minimal stand-ins for a few ESP-IDF / FreeRTOS headers, written from memory
of the IDF 5.x API. They exist ONLY so `gcc -fsyntax-only -DESP_PLATFORM`
can type-check main/*.c on a machine without ESP-IDF. A pass here does NOT
prove the code builds against the real IDF.
