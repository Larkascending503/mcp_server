cmake_minimum_required(VERSION 3.20.0)
if(POLICY CMP0167)
    cmake_policy(SET CMP0167 NEW)
endif()
project(mcp_server)

include(CTest)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

set(PROJECT_VERSION_MAJOR 0)
set(PROJECT_VERSION_MINOR 7)
set(PROJECT_VERSION_PATCH 0)
set(PROJECT_VERSION "${PROJECT_VERSION_MAJOR}.${PROJECT_VERSION_MINOR}.${PROJECT_VERSION_PATCH}")

configure_file(
    "${CMAKE_CURRENT_SOURCE_DIR}/version.h.in"
    "${CMAKE_CURRENT_BINARY_DIR}/version.h"
)

find_package(Threads REQUIRED)
find_package(Boost 1.81 REQUIRED)

# for ASAN
option(USE_ASAN "Enable AddressSanitizer for memory debugging" OFF)

if(USE_ASAN)
    message(STATUS "Build with AddressSanitizer: ENABLED")
    
    add_compile_options(-fsanitize=address -fno-omit-frame-pointer -g)
    
    add_link_options(-fsanitize=address)
endif()


set(SOURCES
    src/main.cpp
    src/server/Server.cpp
    src/transport/StdioTransport.cpp
    src/transport/HttpStreamTransport.cpp
    src/transport/SseTransport.cpp
    src/transport/StreamableTransport.cpp
    src/loader/PluginsLoader.cpp
)

add_executable(${PROJECT_NAME} ${SOURCES})
target_include_directories(${PROJECT_NAME} PRIVATE
    include
    src
    src/transport
    src/interface
    src/utils
    libs_tier_01/aixlog-1.5.0/include
    ${CMAKE_CURRENT_BINARY_DIR}
)

target_link_libraries(${PROJECT_NAME} PRIVATE Threads::Threads Boost::headers)
target_compile_definitions(${PROJECT_NAME} PRIVATE BOOST_ERROR_CODE_HEADER_ONLY)

if(CMAKE_COMPILER_IS_GNUCC OR CMAKE_CXX_COMPILER_ID MATCHES "Clang")
    if(UNIX AND NOT APPLE)
        target_link_options(${PROJECT_NAME} PRIVATE "-static-libgcc" "-static-libstdc++")
    endif()
endif()

add_subdirectory(plugins/sleep)
add_subdirectory(plugins/notification)
add_subdirectory(plugins/weather)
add_subdirectory(plugins/code-review)

if(BUILD_TESTING)
    add_test(
        NAME streamable_http_integration
        COMMAND bash "${CMAKE_CURRENT_SOURCE_DIR}/tests/streamable_http_integration.sh" $<TARGET_FILE:mcp_server>
    )
    set_tests_properties(streamable_http_integration PROPERTIES
        TIMEOUT 30
        RUN_SERIAL TRUE
    )
endif()
