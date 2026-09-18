cmake_minimum_required(VERSION 3.16)
project(streammonitor CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release)
endif()
add_compile_options(-Wall -Wextra -Wpedantic)

find_package(CURL REQUIRED)
find_package(Threads REQUIRED)
find_path(RAPIDJSON_INCLUDE_DIR rapidjson/document.h REQUIRED)
find_library(SQLITE3_LIB sqlite3 REQUIRED)
find_path(SQLITE3_INCLUDE_DIR sqlite3.h REQUIRED)

add_library(sm_common INTERFACE)
target_include_directories(sm_common INTERFACE
    ${CMAKE_CURRENT_SOURCE_DIR} ${RAPIDJSON_INCLUDE_DIR} ${SQLITE3_INCLUDE_DIR})
target_link_libraries(sm_common INTERFACE ${SQLITE3_LIB} Threads::Threads)

add_executable(sm_collector collector/main.cpp)
target_link_libraries(sm_collector PRIVATE sm_common CURL::libcurl)

add_executable(sm_push push/main.cpp)
target_link_libraries(sm_push PRIVATE sm_common)

install(TARGETS sm_collector sm_push RUNTIME DESTINATION bin)
