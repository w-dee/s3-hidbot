set(_source_revision "$ENV{S3_HIDBOT_SOURCE_REVISION}")
set(_source_revision_present "false")

if(NOT "${_source_revision}" STREQUAL "")
    string(LENGTH "${_source_revision}" _source_revision_length)
    if(NOT _source_revision_length EQUAL 40 OR NOT "${_source_revision}" MATCHES "^[0-9a-f]+$")
        message(FATAL_ERROR "S3_HIDBOT_SOURCE_REVISION must be exactly 40 lowercase hexadecimal characters")
    endif()
    set(_source_revision_present "true")
endif()

set(_generated_directory "${CMAKE_CURRENT_BINARY_DIR}/generated")
file(MAKE_DIRECTORY "${_generated_directory}")
configure_file(
    "${CMAKE_CURRENT_LIST_DIR}/identity_build_config.hpp.in"
    "${_generated_directory}/identity_build_config.hpp"
    @ONLY
)

idf_component_register(
    SRCS "firmware_identity.cpp"
    INCLUDE_DIRS "include"
    PRIV_INCLUDE_DIRS "${_generated_directory}"
)

target_compile_features(${COMPONENT_LIB} PRIVATE cxx_std_20)
