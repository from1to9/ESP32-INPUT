cmake_minimum_required(VERSION 3.16)
set(PROJECT_VER "0.1.0-dev")
set(EXTRA_COMPONENT_DIRS "${CMAKE_CURRENT_LIST_DIR}/../components/input_core"
                        "${CMAKE_CURRENT_LIST_DIR}/../components/hid_engine"
                        "${CMAKE_CURRENT_LIST_DIR}/../components/interaction"
                        "${CMAKE_CURRENT_LIST_DIR}/../components/device_settings"
                        "${CMAKE_CURRENT_LIST_DIR}/../components/haptics")
include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(esp32_input)
