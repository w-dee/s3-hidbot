idf_component_register(
    SRCS "ble_transport.cpp"
    INCLUDE_DIRS "include"
    REQUIRES bt nvs_flash esp_timer hid_control_executor ble_lifecycle
)

target_compile_features(${COMPONENT_LIB} PRIVATE cxx_std_20)
