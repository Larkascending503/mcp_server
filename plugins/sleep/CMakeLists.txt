cmake_minimum_required(VERSION 3.10)

add_library(sleep SHARED
        ${PROJECT_SOURCE_DIR}/plugins/sleep/Sleep.cpp
)

set_target_properties(sleep PROPERTIES POSITION_INDEPENDENT_CODE ON)

find_package(Threads REQUIRED)
target_link_libraries(sleep PRIVATE Threads::Threads)

target_compile_definitions(sleep PRIVATE SLEEP_EXPORTS)
target_include_directories(sleep PRIVATE ${PROJECT_SOURCE_DIR}/include ${PROJECT_SOURCE_DIR}/src/interface)
