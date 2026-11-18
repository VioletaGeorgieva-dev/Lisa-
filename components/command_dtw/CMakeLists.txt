idf_component_register(
    SRCS
        "nd_command_dtw.c"
        "command_dictionary.c"
        "command_state.c"
        "lisa_hardware.c"
        "lisa_player.c"

    INCLUDE_DIRS
        "include"

    REQUIRES
        driver
        esp_timer
        fatfs
        heap
        log
        freertos
)
