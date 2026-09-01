idf_component_register(
    SRCS "main.cpp" "firmware_identity_adapter.cpp"
    INCLUDE_DIRS "."
    REQUIRES uart_control_transport hid_runtime hid_control_executor firmware_identity esp_app_format
)

target_compile_features(${COMPONENT_LIB} PRIVATE cxx_std_20)
