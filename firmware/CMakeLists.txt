cmake_minimum_required(VERSION 3.16)
set(PROJECT_VER "0.1.0-dev")
set(EXTRA_COMPONENT_DIRS "${CMAKE_CURRENT_LIST_DIR}/../components/input_core"
                        "${CMAKE_CURRENT_LIST_DIR}/../components/hid_engine"
                        "${CMAKE_CURRENT_LIST_DIR}/../components/interaction"
                        "${CMAKE_CURRENT_LIST_DIR}/../components/device_settings"
                        "${CMAKE_CURRENT_LIST_DIR}/../components/haptics"
                        "${CMAKE_CURRENT_LIST_DIR}/../components/audio_engine"
                        "${CMAKE_CURRENT_LIST_DIR}/../components/control_protocol"
                        "${CMAKE_CURRENT_LIST_DIR}/../components/spoken_letters")
include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(esp32_input)

# Enable UAC2 consistently in the one managed TinyUSB stack without editing
# managed dependency files. PUBLIC propagates the same class configuration to
# esp_tinyusb and application consumers, preventing descriptor/driver mismatch.
idf_component_get_property(input_tinyusb_lib espressif__tinyusb COMPONENT_LIB)
target_compile_options(${input_tinyusb_lib} PUBLIC
    "-include${CMAKE_CURRENT_LIST_DIR}/main/usb_audio_config.h")
