idf_component_register(
    SRCS "usb_exposure_control.cpp"
    INCLUDE_DIRS "include"
    REQUIRES freertos hid_runtime usb_lifecycle
)

target_compile_features(${COMPONENT_LIB} PRIVATE cxx_std_20)
