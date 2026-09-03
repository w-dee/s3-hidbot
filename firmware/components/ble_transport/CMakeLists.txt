idf_component_register(
    SRCS "ble_transport.cpp"
    INCLUDE_DIRS "include"
    REQUIRES bt nvs_flash esp_timer heap log mbedtls hid_control_executor ble_hid_service ble_lifecycle ble_pairing ble_security secure_memory
)

target_compile_features(${COMPONENT_LIB} PRIVATE cxx_std_20)
