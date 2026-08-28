idf_component_register(SRCS "blink.cpp" INCLUDE_DIRS ".")

target_compile_features(${COMPONENT_LIB} PRIVATE cxx_std_20)
