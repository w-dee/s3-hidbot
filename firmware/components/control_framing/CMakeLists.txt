idf_component_register(SRCS "control_framing.cpp" INCLUDE_DIRS "include")

target_compile_features(${COMPONENT_LIB} PRIVATE cxx_std_20)
