idf_component_register(
    SRCS "ble_hid_service.cpp"
    INCLUDE_DIRS "include"
    REQUIRES bt ble_fixture_profile hid_control_executor
)

target_compile_features(${COMPONENT_LIB} PRIVATE cxx_std_20)
