idf_component_register(
    SRCS "hid_sequence.cpp"
    INCLUDE_DIRS "include"
    REQUIRES freertos esp_timer
)

target_compile_features(${COMPONENT_LIB} PRIVATE cxx_std_20)
