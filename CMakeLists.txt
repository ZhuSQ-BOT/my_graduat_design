cmake_minimum_required(VERSION 3.16)
project(PsychServer VERSION 1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTOMOC ON)

find_package(Qt6 REQUIRED COMPONENTS Core Network Sql WebSockets)

file(GLOB_RECURSE SOURCES "src/*.cpp")
file(GLOB_RECURSE HEADERS "include/*.h" "src/*.h")

add_executable(psych_server ${SOURCES} ${HEADERS})

target_include_directories(psych_server PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${CMAKE_CURRENT_SOURCE_DIR}/src
)

target_link_libraries(psych_server PRIVATE
    Qt6::Core
    Qt6::Network
    Qt6::Sql
    Qt6::WebSockets
)
