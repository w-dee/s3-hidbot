idf_component_register(
    SRCS "hid_control_executor.cpp"
    INCLUDE_DIRS "include"
    REQUIRES freertos hid_runtime usb_lifecycle hid_route
)

target_compile_features(${COMPONENT_LIB} PRIVATE cxx_std_20)
