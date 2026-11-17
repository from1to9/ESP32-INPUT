# Keep the pinned dependency intact; compile only its repaired build-local copy.
idf_component_get_property(passport_button_dir espressif__button COMPONENT_DIR)
idf_component_get_property(passport_button_lib espressif__button COMPONENT_LIB)
set(passport_button_source "${passport_button_dir}/iot_button.c")
set(passport_button_patch "${CMAKE_CURRENT_LIST_DIR}/../../tools/passport_button_patch.py")
set(passport_button_copy "${CMAKE_CURRENT_BINARY_DIR}/passport_button/iot_button.c")
execute_process(COMMAND "${PYTHON}" "${passport_button_patch}"
    "${passport_button_source}" "${passport_button_copy}"
    COMMAND_ERROR_IS_FATAL ANY)
get_target_property(passport_button_sources ${passport_button_lib} SOURCES)
list(FIND passport_button_sources "${passport_button_source}" passport_button_index)
if(passport_button_index LESS 0)
    message(FATAL_ERROR "Pinned button source is missing from its component target")
endif()
list(REMOVE_AT passport_button_sources ${passport_button_index})
list(APPEND passport_button_sources "${passport_button_copy}")
set_property(TARGET ${passport_button_lib} PROPERTY SOURCES "${passport_button_sources}")
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
    "${passport_button_source}" "${passport_button_patch}")
