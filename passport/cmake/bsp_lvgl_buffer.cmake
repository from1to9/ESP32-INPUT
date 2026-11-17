# The pinned BSP hardcodes its LVGL DMA buffer instead of exposing a setting.
# Compile a build-local copy with a smaller single buffer; retain upstream
# source, license, display setup, rounding and failure handling unchanged.
idf_component_get_property(passport_bsp_dir bsp COMPONENT_DIR)
idf_component_get_property(passport_bsp_lib bsp COMPONENT_LIB)
set(passport_bsp_lvgl "${passport_bsp_dir}/src/bsp_display_lvgl.c")
file(READ "${passport_bsp_lvgl}" passport_bsp_lvgl_source)
if(NOT passport_bsp_lvgl_source MATCHES "\n#define BSP_LVGL_DRAW_BUFFER_LINES 40\n")
    message(FATAL_ERROR "Pinned BSP draw-buffer definition changed; review the Passport RAM override")
endif()
get_target_property(passport_bsp_sources ${passport_bsp_lib} SOURCES)
list(FIND passport_bsp_sources "${passport_bsp_lvgl}" passport_bsp_lvgl_index)
if(passport_bsp_lvgl_index LESS 0)
    message(FATAL_ERROR "Pinned BSP LVGL source is missing from its component target")
endif()
string(REPLACE "#define BSP_LVGL_DRAW_BUFFER_LINES 40"
    "#define BSP_LVGL_DRAW_BUFFER_LINES 16"
    passport_bsp_lvgl_source "${passport_bsp_lvgl_source}")
set(passport_bsp_lvgl_copy "${CMAKE_CURRENT_BINARY_DIR}/passport_bsp/bsp_display_lvgl.c")
file(WRITE "${passport_bsp_lvgl_copy}" "${passport_bsp_lvgl_source}")
list(REMOVE_AT passport_bsp_sources ${passport_bsp_lvgl_index})
list(APPEND passport_bsp_sources "${passport_bsp_lvgl_copy}")
set_property(TARGET ${passport_bsp_lib} PROPERTY SOURCES "${passport_bsp_sources}")
target_include_directories(${passport_bsp_lib} PRIVATE "${passport_bsp_dir}/src")
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${passport_bsp_lvgl}")
