# create buildOptions
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/cmake/BuildOptions.h.in")
    configure_file(
        ${CMAKE_CURRENT_SOURCE_DIR}/cmake/BuildOptions.h.in
        ${CMAKE_CURRENT_BINARY_DIR}/cmake/BuildOptions.h
        @ONLY)
endif()