idf_component_register(
    SRCS "uart_control_transport.cpp"
    INCLUDE_DIRS "include"
    REQUIRES control_framing control_protocol esp_driver_uart esp_hw_support esp_timer
             bootloader_support mbedtls secure_memory sensitive_request
)

target_compile_features(${COMPONENT_LIB} PRIVATE cxx_std_20)
