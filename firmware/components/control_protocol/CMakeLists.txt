idf_component_register(
    SRCS "control_protocol.cpp"
    INCLUDE_DIRS "include"
    REQUIRES control_framing control_session json
)

target_compile_features(${COMPONENT_LIB} PRIVATE cxx_std_20)
