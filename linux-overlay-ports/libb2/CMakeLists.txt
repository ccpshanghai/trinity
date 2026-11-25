cmake_minimum_required(VERSION 3.16)
project(libb2 LANGUAGES C)

add_library(b2
    src/blake2b-ref.c
    src/blake2s-ref.c
    src/blake2sp.c
    src/blake2bp.c
)
target_compile_definitions(b2 PRIVATE "SUFFIX=")
target_include_directories(b2 PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/src>
    $<INSTALL_INTERFACE:include>
)
set_target_properties(b2 PROPERTIES
    OUTPUT_NAME b2
    VERSION 0.98.1
    SOVERSION 1
    PUBLIC_HEADER src/blake2.h
)
install(TARGETS b2
    LIBRARY DESTINATION lib
    ARCHIVE DESTINATION lib
    RUNTIME DESTINATION bin
    PUBLIC_HEADER DESTINATION include
)
