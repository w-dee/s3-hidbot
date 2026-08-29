idf_component_register(
    SRCS "hid_runtime.cpp"
    INCLUDE_DIRS "include"
    REQUIRES esp_tinyusb
)

target_compile_features(${COMPONENT_LIB} PRIVATE cxx_std_20)
