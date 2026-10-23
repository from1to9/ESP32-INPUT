# SPDX-License-Identifier: Apache-2.0
cmake_minimum_required(VERSION 3.18)
project(esp32_input_ui_host C)
set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
get_filename_component(REPOSITORY_ROOT "../.." ABSOLUTE BASE_DIR "${CMAKE_CURRENT_LIST_DIR}")
set(LVGL_SOURCE "${REPOSITORY_ROOT}/firmware/managed_components/lvgl__lvgl")
if(NOT EXISTS "${LVGL_SOURCE}/lvgl.h")
    message(FATAL_ERROR "Resolve pinned firmware dependencies first (see firmware/README.md)")
endif()
set(LV_BUILD_CONF_PATH "${CMAKE_CURRENT_LIST_DIR}/lv_conf.h" CACHE PATH "" FORCE)
set(CONFIG_LV_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(CONFIG_LV_BUILD_DEMOS OFF CACHE BOOL "" FORCE)
set(CONFIG_LV_USE_THORVG_INTERNAL OFF CACHE BOOL "" FORCE)
add_subdirectory("${LVGL_SOURCE}" "lvgl")
# Observe the allocator boundary while delegating to the unchanged CLIB backend.
# Only its three symbols are renamed; LVGL and application decisions are intact.
set_source_files_properties("${LVGL_SOURCE}/src/stdlib/clib/lv_mem_core_clib.c"
    TARGET_DIRECTORY lvgl PROPERTIES COMPILE_DEFINITIONS
    "lv_malloc_core=ui_clib_malloc_core;lv_realloc_core=ui_clib_realloc_core;lv_free_core=ui_clib_free_core")
add_executable(ui_visual ui_visual.c ui_memory.c ui_private_probe.c ui_trust_review_fixture.c
    "${REPOSITORY_ROOT}/firmware/main/app_bluetooth_management.c"
    "${REPOSITORY_ROOT}/components/control_protocol/control_registry.c")
target_include_directories(ui_visual PRIVATE "${LVGL_SOURCE}" "${REPOSITORY_ROOT}/tests/bt_audio/stubs"
    "${REPOSITORY_ROOT}/components/control_protocol/include"
    "${REPOSITORY_ROOT}/components/device_settings/include" "${REPOSITORY_ROOT}/components/audio_engine/include")
add_executable(ui_resources ui_resources.c ui_memory.c ui_private_probe.c)
foreach(target ui_visual ui_resources)
    target_include_directories(${target} PRIVATE "${REPOSITORY_ROOT}/firmware/main"
        "${REPOSITORY_ROOT}/components/input_core/include")
    target_compile_options(${target} PRIVATE -Wall -Wextra -Werror -Wpedantic)
    target_link_libraries(${target} PRIVATE lvgl m)
endforeach()
option(UI_SANITIZERS "Instrument the UI and LVGL with address/undefined sanitizers" ON)
if(UI_SANITIZERS AND CMAKE_C_COMPILER_ID MATCHES "Clang|GNU")
    foreach(target lvgl ui_visual ui_resources)
        target_compile_options(${target} PRIVATE -fsanitize=address,undefined -fno-omit-frame-pointer -g)
    endforeach()
    target_link_options(ui_visual PRIVATE -fsanitize=address,undefined)
    target_link_options(ui_resources PRIVATE -fsanitize=address,undefined)
endif()

add_executable(ui_presenter_integration ui_presenter_integration.c ui_memory.c
    "${REPOSITORY_ROOT}/firmware/main/app_ui.c"
    "${REPOSITORY_ROOT}/firmware/main/app_presenter.c"
    "${REPOSITORY_ROOT}/firmware/main/board_display_transfer.c")
target_include_directories(ui_presenter_integration PRIVATE "${LVGL_SOURCE}"
    "${REPOSITORY_ROOT}/firmware/main" "${REPOSITORY_ROOT}/components/input_core/include"
    "${REPOSITORY_ROOT}/tests/display_transfer/stubs" "${CMAKE_CURRENT_LIST_DIR}/presenter_stubs")
target_compile_options(ui_presenter_integration PRIVATE -Wall -Wextra -Werror -Wpedantic)
target_link_libraries(ui_presenter_integration PRIVATE lvgl m)
if(UI_SANITIZERS AND CMAKE_C_COMPILER_ID MATCHES "Clang|GNU")
    target_compile_options(ui_presenter_integration PRIVATE -fsanitize=address,undefined -fno-omit-frame-pointer -g)
    target_link_options(ui_presenter_integration PRIVATE -fsanitize=address,undefined)
endif()
