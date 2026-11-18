set(ESP_SR_LIB_DIR "${CMAKE_CURRENT_LIST_DIR}/../../managed_components/espressif__esp-sr/lib/esp32s3")
set(ESP_SR_INCLUDE_DIR "${CMAKE_CURRENT_LIST_DIR}/../../managed_components/espressif__esp-sr/include/esp32s3")

idf_component_register(
    WHOLE_ARCHIVE
    SRCS "esp_aec_wrapper.c"
    INCLUDE_DIRS "include" "${ESP_SR_INCLUDE_DIR}"
    REQUIRES espressif__dl_fft esp_timer heap log
)

add_prebuilt_library(esp_audio_processor
    "${ESP_SR_LIB_DIR}/libesp_audio_processor.a"
    REQUIRES espressif__dl_fft esp_timer heap log
)

add_prebuilt_library(esp_dl_lib
    "${ESP_SR_LIB_DIR}/libdl_lib.a"
    REQUIRES heap log
)

target_link_libraries(${COMPONENT_LIB} PRIVATE
    esp_audio_processor
    esp_dl_lib
)
