# Compile a hash-checked generated copy; managed sources and their license stay intact.
idf_component_get_property(input_audio_usb_target espressif__tinyusb COMPONENT_LIB)
idf_component_get_property(input_audio_usb_dir espressif__tinyusb COMPONENT_DIR)
idf_build_get_property(input_audio_python PYTHON)
get_filename_component(input_audio_root "${CMAKE_CURRENT_LIST_DIR}/.." ABSOLUTE)
set(input_audio_generator "${input_audio_root}/tools/tinyusb_audio_patch.py")
set(input_audio_copy "${CMAKE_BINARY_DIR}/generated/tinyusb_audio/audio_device.c")
execute_process(COMMAND "${input_audio_python}" "${input_audio_generator}"
    --source-root "${input_audio_usb_dir}" --output "${input_audio_copy}"
    RESULT_VARIABLE input_audio_patch_result OUTPUT_VARIABLE input_audio_patch_metadata
    ERROR_VARIABLE input_audio_patch_error)
if(NOT input_audio_patch_result EQUAL 0)
    message(FATAL_ERROR "TinyUSB audio patch verification failed: ${input_audio_patch_error}")
endif()

get_target_property(input_audio_sources ${input_audio_usb_target} SOURCES)
get_target_property(input_audio_source_dir ${input_audio_usb_target} SOURCE_DIR)
set(input_audio_replacements 0)
set(input_audio_dcd_replacements 0)
set(input_audio_patched_sources)
foreach(input_audio_source IN LISTS input_audio_sources)
    get_filename_component(input_audio_absolute "${input_audio_source}" ABSOLUTE BASE_DIR "${input_audio_source_dir}")
    if(input_audio_absolute STREQUAL "${input_audio_usb_dir}/src/class/audio/audio_device.c")
        list(APPEND input_audio_patched_sources "${input_audio_copy}")
        math(EXPR input_audio_replacements "${input_audio_replacements} + 1")
    elseif(input_audio_absolute STREQUAL "${input_audio_usb_dir}/src/portable/synopsys/dwc2/dcd_dwc2.c")
        list(APPEND input_audio_patched_sources "${CMAKE_BINARY_DIR}/generated/tinyusb_audio/dcd_dwc2.c")
        math(EXPR input_audio_dcd_replacements "${input_audio_dcd_replacements} + 1")
    else()
        list(APPEND input_audio_patched_sources "${input_audio_source}")
    endif()
endforeach()
if(NOT input_audio_replacements EQUAL 1 OR NOT input_audio_dcd_replacements EQUAL 1)
    message(FATAL_ERROR "Expected exactly one pinned TinyUSB audio class and DCD source")
endif()
set_property(TARGET ${input_audio_usb_target} PROPERTY SOURCES "${input_audio_patched_sources}")
target_include_directories(${input_audio_usb_target} PRIVATE "${input_audio_usb_dir}/src/class/audio"
    "${input_audio_usb_dir}/src/portable/synopsys/dwc2" "${input_audio_root}/firmware/main")
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
    "${input_audio_generator}" "${input_audio_root}/patches/tinyusb-audio-streams.patch"
    "${input_audio_root}/patches/tinyusb-audio-streams.json" "${input_audio_root}/firmware/dependencies.lock"
    "${input_audio_usb_dir}/src/class/audio/audio_device.c"
    "${input_audio_usb_dir}/src/portable/synopsys/dwc2/dcd_dwc2.c"
    "${input_audio_root}/patches/tinyusb-dwc2-observer.patch"
    "${input_audio_root}/firmware/main/usb_audio_transport.h")
add_custom_target(input_tinyusb_audio_verify
    COMMAND "${input_audio_python}" "${input_audio_generator}" --source-root "${input_audio_usb_dir}"
        --output "${input_audio_copy}" --check
    VERBATIM)
add_dependencies(${input_audio_usb_target} input_tinyusb_audio_verify)
