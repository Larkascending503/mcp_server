cmake_minimum_required(VERSION 3.10)

add_library(notification SHARED
        ${PROJECT_SOURCE_DIR}/plugins/notification/Notification.cpp
)

set_target_properties(notification PROPERTIES POSITION_INDEPENDENT_CODE ON)

find_package(Threads REQUIRED)
target_link_libraries(notification PRIVATE Threads::Threads)

target_compile_definitions(notification PRIVATE NOTIFICATION_EXPORTS)
target_include_directories(notification PRIVATE ${PROJECT_SOURCE_DIR}/include ${PROJECT_SOURCE_DIR}/src/interface)
