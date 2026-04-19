cmake_minimum_required(VERSION 3.10)

add_library(codereview SHARED
        ${PROJECT_SOURCE_DIR}/plugins/code-review/CodeReview.cpp
)

set_target_properties(codereview PROPERTIES POSITION_INDEPENDENT_CODE ON)

find_package(Threads REQUIRED)
target_link_libraries(codereview PRIVATE Threads::Threads)

target_compile_definitions(codereview PRIVATE CODEREVIEW_EXPORTS)
target_include_directories(codereview PRIVATE ${PROJECT_SOURCE_DIR}/include ${PROJECT_SOURCE_DIR}/src/interface)
