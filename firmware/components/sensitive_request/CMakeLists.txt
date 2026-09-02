idf_component_register(SRCS "sensitive_request.cpp" INCLUDE_DIRS "include"
                       REQUIRES mbedtls secure_memory)

target_compile_features(${COMPONENT_LIB} PRIVATE cxx_std_20)
