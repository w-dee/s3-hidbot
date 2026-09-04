cmake_minimum_required(VERSION 3.16)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(s3_hidbot_passive_usb_hid)

# The pinned TinyUSB variant honors this project-owned, printf-compatible
# no-console runtime diagnostic sink. Scope the override to TinyUSB itself.
idf_component_get_property(tinyusb_lib espressif__tinyusb COMPONENT_LIB)
target_compile_definitions(
    ${tinyusb_lib} PRIVATE
    CFG_TUSB_DEBUG_PRINTF_OVERRIDE=s3_hidbot_tinyusb_debug_printf
)
