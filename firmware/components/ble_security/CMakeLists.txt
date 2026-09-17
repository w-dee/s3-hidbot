idf_component_register(
    SRCS "ble_security.cpp"
    INCLUDE_DIRS "include"
    REQUIRES ble_fixture_profile ble_lifecycle
)

target_compile_features(${COMPONENT_LIB} PRIVATE cxx_std_20)
