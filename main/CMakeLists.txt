idf_component_register(
    SRCS
        "main.c"
        "lisa_audio_in.c"
        "lisa_recognizer.c"
        "lisa_controller.c"

    INCLUDE_DIRS
        "."

    REQUIRES
        command_dtw
        engine
        esp_aec_wrapper
        esp_partition
        esp_timer
        esp_psram
        freertos
        heap
        log
)
