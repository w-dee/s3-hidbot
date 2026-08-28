idf_component_register(
    SRCS "uart_control_transport.cpp"
    INCLUDE_DIRS "include"
    REQUIRES control_framing esp_driver_uart
)

target_compile_features(${COMPONENT_LIB} PRIVATE cxx_std_20)
