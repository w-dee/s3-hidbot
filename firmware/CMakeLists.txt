cmake_minimum_required(VERSION 3.16)

find_package(Python3 REQUIRED COMPONENTS Interpreter)
set(production_sdk_verifier "${CMAKE_CURRENT_LIST_DIR}/../tools/prepare_production_sdk.py")
execute_process(
    COMMAND "${Python3_EXECUTABLE}" "${production_sdk_verifier}" --verify "$ENV{IDF_PATH}"
    RESULT_VARIABLE production_sdk_result
)
if(NOT production_sdk_result EQUAL 0)
    message(FATAL_ERROR "Prepare the exact isolated production DLE-LAST SDK before building")
endif()

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(s3_hidbot_passive_usb_hid)

# Recheck even for incremental builds, not merely initial configuration.
add_custom_target(verify_production_sdk ALL
    COMMAND "${Python3_EXECUTABLE}" "${production_sdk_verifier}" --verify "$ENV{IDF_PATH}"
    VERBATIM)

# The pinned TinyUSB variant honors this project-owned, printf-compatible
# no-console runtime diagnostic sink. Scope the override to TinyUSB itself.
idf_component_get_property(tinyusb_lib espressif__tinyusb COMPONENT_LIB)
target_compile_definitions(
    ${tinyusb_lib} PRIVATE
    CFG_TUSB_DEBUG_PRINTF_OVERRIDE=s3_hidbot_tinyusb_debug_printf
)
