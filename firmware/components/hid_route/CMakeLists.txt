idf_component_register(
    SRCS "hid_route.cpp"
    INCLUDE_DIRS "include"
)

target_compile_features(${COMPONENT_LIB} PRIVATE cxx_std_20)
