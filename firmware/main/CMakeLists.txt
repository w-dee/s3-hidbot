idf_component_register(SRCS "blink.cpp" INCLUDE_DIRS "." REQUIRES uart_control_transport hid_runtime firmware_identity)

target_compile_features(${COMPONENT_LIB} PRIVATE cxx_std_20)
