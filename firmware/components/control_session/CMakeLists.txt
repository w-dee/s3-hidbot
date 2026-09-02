idf_component_register(SRCS "control_session.cpp" INCLUDE_DIRS "include"
                       REQUIRES secure_memory sensitive_request)

target_compile_features(${COMPONENT_LIB} PRIVATE cxx_std_20)
