idf_component_register(SRCS "secure_memory.cpp" INCLUDE_DIRS "include" REQUIRES heap mbedtls)

target_compile_features(${COMPONENT_LIB} PRIVATE cxx_std_20)
