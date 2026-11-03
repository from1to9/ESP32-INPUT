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

# The bounded Classic command adapter uses the pinned BTC task and profile
# message ABI. Keep its private includes inside the SDK's component.
if(CONFIG_BT_CLASSIC_ENABLED AND CONFIG_BT_A2DP_ENABLE AND CONFIG_BT_HFP_CLIENT_ENABLE)
    idf_component_get_property(input_classic_bt bt COMPONENT_LIB)
    target_sources(${input_classic_bt} PRIVATE "${CMAKE_CURRENT_LIST_DIR}/main/bt_audio_btc.c")
endif()

# Enable UAC2 consistently in the one managed TinyUSB stack without editing
# managed dependency files. PUBLIC propagates the same class configuration to
# esp_tinyusb and application consumers, preventing descriptor/driver mismatch.
idf_component_get_property(input_tinyusb_lib espressif__tinyusb COMPONENT_LIB)
target_compile_options(${input_tinyusb_lib} PUBLIC
    "-include${CMAKE_CURRENT_LIST_DIR}/main/usb_audio_config.h")

# Keep the managed dependency pristine and compile only the exact reviewed
# generated audio-class repair with the existing TinyUSB target configuration.
include("${CMAKE_CURRENT_LIST_DIR}/tinyusb_audio_patch.cmake")
