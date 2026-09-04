idf_component_register(
    SRCS "main.cpp" "firmware_identity_adapter.cpp"
    INCLUDE_DIRS "."
    REQUIRES uart_control_transport hid_runtime hid_control_executor firmware_identity esp_app_format ble_transport ble_hid_service
)

target_compile_features(${COMPONENT_LIB} PRIVATE cxx_std_20)
target_link_options(${COMPONENT_LIB} INTERFACE
    "-Wl,--wrap=tud_descriptor_string_cb"
)
