idf_component_register(
    SRCS "hid_control_executor.cpp"
    INCLUDE_DIRS "include"
    REQUIRES freertos hid_runtime usb_lifecycle hid_route ble_lifecycle ble_pairing ble_security
)

target_compile_features(${COMPONENT_LIB} PRIVATE cxx_std_20)
