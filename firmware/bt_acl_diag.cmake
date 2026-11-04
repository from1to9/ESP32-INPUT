# The built-in VHCI driver has one host callback owner. Instrument the pinned
# HAL, BTU and SBC queues in the diagnostic build without replacing that callback
# or changing the shared SDK checkout.
idf_component_get_property(acl_bt_lib bt COMPONENT_LIB)
idf_component_get_property(acl_bt_dir bt COMPONENT_DIR)
set(acl_src_dir "${acl_bt_dir}/host/bluedroid")
set(acl_output_dir "${CMAKE_BINARY_DIR}/generated/a2dp_acl")
set(acl_patch "${CMAKE_CURRENT_LIST_DIR}/../patches/esp-idf-a2dp-acl-diagnostics.patch")
set(acl_hal "${acl_src_dir}/hci/hci_hal_h4.c")
set(acl_sink "${acl_src_dir}/btc/profile/std/a2dp/btc_a2dp_sink.c")
set(acl_btu "${acl_src_dir}/stack/btu/btu_task.c")
set(acl_btu_copy "${acl_output_dir}/btu_task.c")
set(acl_hal_copy "${acl_output_dir}/hci_hal_h4.c")
set(acl_sink_copy "${acl_output_dir}/btc_a2dp_sink.c")
find_package(Git REQUIRED)
add_custom_command(OUTPUT "${acl_hal_copy}" "${acl_sink_copy}" "${acl_btu_copy}"
    COMMAND "${CMAKE_COMMAND}" -E make_directory "${acl_output_dir}"
    COMMAND "${CMAKE_COMMAND}" -E copy "${acl_hal}" "${acl_hal_copy}"
    COMMAND "${CMAKE_COMMAND}" -E copy "${acl_sink}" "${acl_sink_copy}"
    COMMAND "${CMAKE_COMMAND}" -E copy "${acl_btu}" "${acl_btu_copy}"
    COMMAND "${CMAKE_COMMAND}" -E env "GIT_CEILING_DIRECTORIES=${CMAKE_BINARY_DIR}/generated"
        "${GIT_EXECUTABLE}" -C "${acl_output_dir}" apply --no-index "${acl_patch}"
    DEPENDS "${acl_hal}" "${acl_sink}" "${acl_btu}" "${acl_patch}"
    VERBATIM)
add_custom_target(input_a2dp_acl_sources DEPENDS "${acl_hal_copy}" "${acl_sink_copy}" "${acl_btu_copy}")
add_dependencies(${acl_bt_lib} input_a2dp_acl_sources)

get_target_property(acl_sources ${acl_bt_lib} SOURCES)
get_target_property(acl_base ${acl_bt_lib} SOURCE_DIR)
set(acl_build_sources)
set(acl_replaced 0)
foreach(source IN LISTS acl_sources)
    get_filename_component(absolute "${source}" ABSOLUTE BASE_DIR "${acl_base}")
    if(absolute STREQUAL acl_hal)
        list(APPEND acl_build_sources "${acl_hal_copy}")
    elseif(absolute STREQUAL acl_btu)
        list(APPEND acl_build_sources "${acl_btu_copy}")
    elseif(absolute STREQUAL acl_sink)
        list(APPEND acl_build_sources "${acl_sink_copy}")
    else()
        list(APPEND acl_build_sources "${source}")
        continue()
    endif()
    math(EXPR acl_replaced "${acl_replaced} + 1")
endforeach()
if(NOT acl_replaced EQUAL 3)
    message(FATAL_ERROR "Expected the pinned H4, BTU and SBC sink sources for ACL diagnostics")
endif()
set_property(TARGET ${acl_bt_lib} PROPERTY SOURCES "${acl_build_sources}")
target_include_directories(${acl_bt_lib} PRIVATE "${CMAKE_CURRENT_LIST_DIR}/main")
target_sources(${acl_bt_lib} PRIVATE "${CMAKE_CURRENT_LIST_DIR}/main/bt_acl_diag.c")
