set(ND_ENGINE "${CMAKE_CURRENT_LIST_DIR}")

file(GLOB ND_SRC "${ND_ENGINE}/src/*.c")

idf_component_register(
    SRCS
        ${ND_SRC}

    INCLUDE_DIRS
        "include"

    REQUIRES
        esp_timer
        heap
        esp_common
)