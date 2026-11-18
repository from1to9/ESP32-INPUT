cmake_minimum_required(VERSION 3.16)
set(IDF_TARGET "esp32c3")
set(EXTRA_COMPONENT_DIRS "../components/input_core" "../components/hid_engine" "../components/interaction" "../components/control_protocol")
include($ENV{IDF_PATH}/tools/cmake/project.cmake)
idf_build_set_property(MINIMAL_BUILD ON)
idf_build_set_property(COMPILE_DEFINITIONS "CP_PASSPORT_BROWSER_LEASE_MS=30000U;CP_MAX_RESULTS=4U;CJ_CAPACITY=4096U;CJ_TOKENS=192U;APP_REMOTE_INPUT_RECENT=4U" APPEND)
project(passport_c3)
# sdkconfig is available after project(). Keep the existing two jobs for a
# single-command channel and reserve the third job only on a three-wide build.
set(passport_input_capacity 2)
if(CONFIG_CONTROL_MOTION_WINDOW_MAX EQUAL 3)
    set(passport_input_capacity 3)
endif()
idf_build_set_property(COMPILE_DEFINITIONS
    "CP_MAX_ACTIVE_REQUESTS=${passport_input_capacity}U;APP_REMOTE_INPUT_JOBS=${passport_input_capacity}U" APPEND)
include(cmake/bsp_lvgl_buffer.cmake)
include(cmake/button_repeat.cmake)
